#!/usr/bin/env python3
"""tests/reference/check.py -- iris against a second implementation of its model.

This file implements the iris model again, in 64-bit floating point (binary64),
from the model iris.h describes in its comments -- the activation of PART 1,
the starting weights of PART 3, the scaling of PART 5, the forward pass of
PART 6, the update rule and stopping rules of PART 8, the file format of
PART 9 and the neighbour samplers of PART 10 -- and never from iris.h's code.
tests/reference/export.c runs the library and writes out what it computed;
this file recomputes it and compares.

iris computes in 32-bit floats (binary32), so it cannot agree with a binary64
replay to the bit. Each comparison therefore has a stated tolerance, derived
from binary32 rounding. u = 2^-24 is the largest relative error of one
correctly rounded binary32 operation.

  L0 exact       the random stream, the starting weights (computed here in
                 binary32, as the model defines them), every epoch's shuffle
                 order and the fitted ranges: bit for bit.
  L1 one epoch   each sampled epoch replayed in binary64 from iris's own state
                 at the end of the previous epoch: every weight and velocity
                 within 2 * n * u * max(|w|, 1), n the number of
                 demonstrations. Each weight is rounded once per demonstration
                 in an epoch, which gives n * u * |w| to first order; the
                 factor 2 covers the velocity and gradient roundings.
  L2 whole run   the whole run replayed from the starting weights with iris's
                 shuffle orders, in binary64 and again in binary32 (NumPy),
                 compared on a 41 x 41 grid of predictions at checkpoints.
                 Over thousands of epochs no a-priori bound is useful, so the
                 binary32 replay calibrates it: |iris - b64| must stay within
                 4 |b32 - b64| + 16 u * range, and always under 1e-4 * range.
  L3 a saved file   the file parsed here from the format table alone (the
                 checksum by zlib's CRC-32), then a binary64 forward pass on
                 the grid, each output within its a-priori rounding bound.
  L5 neighbours  nearest neighbour (1-NN): the same demonstration and the
                 same bits on every query whose two nearest demonstrations
                 are not tied; k-nearest-neighbour blend (k = 3): within
                 (2 n_in + 2 k + 15) u max|y| of the binary64 blend on every
                 query without a tie at the k-th neighbour; and the same with
                 scikit-learn's KNeighborsRegressor when it is installed.

The binary64 replay also applies iris's stopping rules to its own errors and
reports the epoch at which it would stop; that is reported, not asserted,
because a decision made on a threshold can differ by one window when the two
errors straddle it.

Run it through build.sh (sh build.sh reference); by hand:
  cc -std=c99 -O2 -I. -o build/reference_export tests/reference/export.c
  python3 tests/reference/check.py build/reference_export
It exits 1 if any layer fails.
"""
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

U = 2.0 ** -24
F32 = np.float32


def f32(x):
    """The binary32 value of a decimal constant, as a C compiler rounds 0.1f."""
    return float(F32(x))


# The model's constants are binary32 numbers (0.1f, not 0.1), so the binary64
# replay uses exactly those values: the output band of PART 5, the learning
# rate and momentum of PART 3, and the velocity flush of PART 8.
OUT_LO, OUT_HI = f32(0.1), f32(0.9)
LR, MOM, TINY = f32(0.10), f32(0.85), f32(1e-30)


def act(x):
    """PART 1: x(27 + x^2) / (27 + 9x^2), held to [-1, 1]; +-1 beyond +-1e9."""
    x = np.asarray(x, dtype=np.float64)
    with np.errstate(over="ignore", invalid="ignore"):
        x2 = x * x
        r = x * (27.0 + x2) / (27.0 + 9.0 * x2)
    r = np.where(x > 1e9, 1.0, np.where(x < -1e9, -1.0, r))
    return np.clip(r, -1.0, 1.0)


def sig(x):
    return 0.5 * (act(0.5 * x) + 1.0)


class Xorshift32:
    """PART 1: x ^= x << 13; x ^= x >> 17; x ^= x << 5; a zero state becomes 0x9E3779B9."""
    def __init__(self, seed):
        self.s = seed if seed else 1

    def u32(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x if x else 0x9E3779B9
        return self.s

    def sym(self):
        """uniform in [-1, 1), in binary32: (float)(int32_t)u * (1.0f / 2147483648.0f)"""
        u = self.u32()
        i = u - (1 << 32) if u >= (1 << 31) else u
        return F32(i) * F32(1.0 / 2147483648.0)


def ranges(ex, ni, no):
    """PART 5, in binary32: the smallest and largest of each column. An input
    whose width is at most max(1e-5 * magnitude, 1e-6) is still (zero width);
    an output narrower than max(1e-5 * |lo|, 1e-6) is widened to that."""
    lo, hi = ex.min(axis=0).astype(F32), ex.max(axis=0).astype(F32)
    ilo, ihi, olo, ohi = lo[:ni].copy(), hi[:ni].copy(), lo[ni:].copy(), hi[ni:].copy()
    for i in range(ni):
        neg = max(F32(max(abs(ilo[i]), abs(ihi[i]))) * F32(1e-5), F32(1e-6))
        if F32(ihi[i] - ilo[i]) <= neg:
            ihi[i] = ilo[i]
    for o in range(no):
        w = max(F32(abs(olo[o])) * F32(1e-5), F32(1e-6))
        if F32(ohi[o] - olo[o]) < w:
            ohi[o] = F32(olo[o] + w)
    return ilo, ihi, olo, ohi


def norm_in(v, lo, hi):
    w = hi.astype(np.float64) - lo.astype(np.float64)
    safe = np.where(w > 0, w, 1.0)
    return np.where(w > 0, 2.0 * (v - lo) / safe - 1.0, 0.0)


def read_traj(path):
    b = open(path, "rb").read()
    off = 0

    def take(fmt, n):
        nonlocal off
        a = np.frombuffer(b, dtype=fmt, count=n, offset=off)
        off += a.nbytes
        return a.copy()

    ni, nh, no, nex, seed = (int(v) for v in take("=i4", 5))
    sm = float(take("=f4", 1)[0])
    ex = take("=f4", nex * (ni + no)).reshape(nex, ni + no)

    def block():
        return [take("=f4", nh * ni).reshape(nh, ni), take("=f4", nh),
                take("=f4", no * nh).reshape(no, nh), take("=f4", no)]

    init = block()
    rng, epochs = None, []
    while off < len(b):
        if rng is None:
            rng = [take("=f4", ni), take("=f4", ni), take("=f4", no), take("=f4", no)]
        ep = int(take("=i4", 1)[0])
        order = take("=i4", nex)
        err = float(take("=f4", 1)[0])
        epochs.append((ep, order, err, block(), block()))
    return dict(ni=ni, nh=nh, no=no, nex=nex, seed=seed, sm=sm, ex=ex, init=init, rng=rng, epochs=epochs)


def epoch(W, V, X, T, order, dt, wd):
    """PART 8, one epoch in dtype dt: forward, the error, the backward pass,
    then the output layer's and the hidden layer's momentum updates with the
    velocity flush and the decoupled weight decay (weights only)."""
    w1, b1, w2, b2 = [a.astype(dt).copy() for a in W]
    v1, vb1, v2, vb2 = [a.astype(dt).copy() for a in V]
    lr, m, tiny, wd = dt(LR), dt(MOM), dt(TINY), dt(wd)
    one = dt(1)
    err = 0.0
    for r in order:
        x, t = X[r], T[r]
        a = act(b1 + w1 @ x).astype(dt)
        y = sig(b2 + w2 @ a).astype(dt)
        e = y - t
        err += float((e * e).sum())
        dout = e * y * (one - y)
        dhid = (w2.T @ dout) * (one - a * a)
        v2 = m * v2 - lr * np.outer(dout, a); v2[np.abs(v2) < tiny] = 0; w2 = w2 + v2; w2 = w2 - wd * w2
        vb2 = m * vb2 - lr * dout; vb2[np.abs(vb2) < tiny] = 0; b2 = b2 + vb2
        v1 = m * v1 - lr * np.outer(dhid, x); v1[np.abs(v1) < tiny] = 0; w1 = w1 + v1; w1 = w1 - wd * w1
        vb1 = m * vb1 - lr * dhid; vb1[np.abs(vb1) < tiny] = 0; b1 = b1 + vb1
    return [w1, b1, w2, b2], [v1, vb1, v2, vb2], err / (len(order) * T.shape[1])


FAILS = []


def verdict(ok, text):
    print(("  PASS  " if ok else "  FAIL  ") + text)
    if not ok:
        FAILS.append(text)


def trajectory(exe, work, sm, free_epochs):
    path = os.path.join(work, f"traj_{sm}.bin")
    subprocess.run([exe, "traj", path, str(sm)], check=True)
    tr = read_traj(path)
    ni, nh, no, nex, E = tr["ni"], tr["nh"], tr["no"], tr["nex"], tr["epochs"]
    print(f"\ntraining run: {ni}-{nh}-{no}, {nex} demonstrations, seed {tr['seed']}, "
          f"smoothing {tr['sm']}, {len(E)} epochs")

    # L0: the starting weights and every shuffle order, bit for bit
    g = Xorshift32(tr["seed"])
    s1, s2 = F32(1) / np.sqrt(F32(ni)), F32(1) / np.sqrt(F32(nh))
    w1 = np.array([g.sym() * s1 for _ in range(nh * ni)], dtype=F32).reshape(nh, ni)
    w2 = np.array([g.sym() * s2 for _ in range(no * nh)], dtype=F32).reshape(no, nh)
    same_init = (np.array_equal(w1.view(np.uint32), tr["init"][0].view(np.uint32))
                 and np.array_equal(w2.view(np.uint32), tr["init"][2].view(np.uint32))
                 and not tr["init"][1].any() and not tr["init"][3].any())
    verdict(same_init, "L0 the starting weights are the xorshift32 draws, bit for bit")
    order, bad = np.arange(nex), 0
    for (_, o, *_rest) in E:
        for i in range(nex - 1, 0, -1):
            j = g.u32() % (i + 1)
            order[i], order[j] = order[j], order[i]
        bad += not np.array_equal(order, o)
    verdict(bad == 0, f"L0 every epoch's shuffle order is the Fisher-Yates draw ({bad} of {len(E)} differ)")
    ilo, ihi, olo, ohi = ranges(tr["ex"], ni, no)
    same_rng = all(np.array_equal(a.view(np.uint32), b.view(np.uint32))
                   for a, b in zip((ilo, ihi, olo, ohi), tr["rng"]))
    verdict(same_rng, "L0 the fitted ranges, bit for bit")

    X = norm_in(tr["ex"][:, :ni].astype(np.float64), ilo, ihi)
    T = OUT_LO + (tr["ex"][:, ni:].astype(np.float64) - olo) / (ohi.astype(np.float64) - olo) * (OUT_HI - OUT_LO)
    l2 = min(max(tr["sm"], 0.0), 1.0) * f32(0.3)
    wd = float(F32(F32(l2) * F32(LR)) / F32(nex))

    # L1: one epoch at a time, from iris's own state
    zeros = [np.zeros_like(a) for a in tr["init"]]
    sample = sorted(set(list(range(1, min(51, len(E) + 1))) + list(range(100, len(E) + 1, 100)) + [len(E)]))
    worst, worst_ep = 0.0, 0
    for e in sample:
        Wp, Vp = (tr["init"], zeros) if e == 1 else (E[e - 2][3], E[e - 2][4])
        W, V, _ = epoch(Wp, Vp, X, T, E[e - 1][1], np.float64, wd)
        for a, b in zip(W + V, E[e - 1][3] + E[e - 1][4]):
            b64 = b.astype(np.float64)
            r = float((np.abs(a - b64) / np.maximum(np.abs(b64), 1.0)).max()) / U
            if r > worst:
                worst, worst_ep = r, e
    bound = 2.0 * nex
    verdict(worst <= bound, f"L1 {len(sample)} epochs replayed one at a time: worst |dw| / max(|w|, 1) "
                            f"= {worst:.1f} u at epoch {worst_ep} (bound {bound:.0f} u)")

    # L2: the whole run (or its first free_epochs), in binary64 and binary32
    n_free = min(len(E), free_epochs)
    marks = sorted({1, 10, 100, 1000, 2000, 4000, 8000, 16000, n_free} & set(range(1, n_free + 1)))
    grid = np.array([[a / 40.0, b / 40.0] for a in range(41) for b in range(41)])
    GX = norm_in(grid, ilo, ihi)
    rng_w = (ohi.astype(np.float64) - olo)

    def play(W):
        w1_, b1_, w2_, b2_ = [a.astype(np.float64) for a in W]
        y = sig(act(GX @ w1_.T + b1_) @ w2_.T + b2_)
        return np.clip(olo + (y - OUT_LO) / (OUT_HI - OUT_LO) * rng_w, olo, ohi)

    runs = {}
    for dt in (np.float64, np.float32):
        W = [a.astype(dt) for a in tr["init"]]
        V = [np.zeros_like(a, dtype=dt) for a in tr["init"]]
        Xd, Td, at, ref, stop = X.astype(dt), T.astype(dt), {}, 0.0, None
        for e in range(1, n_free + 1):
            W, V, err = epoch(W, V, Xd, Td, E[e - 1][1], dt, wd)
            if dt is np.float64 and stop is None:
                if err < 1e-6:
                    stop = e
                elif e % 2000 == 0:
                    if ref > 0 and (ref - err) <= 0.10 * ref:
                        stop = e
                    ref = err
            if e in marks:
                at[e] = play(W)
        runs[dt.__name__] = (at, stop)
    ok, rows = True, []
    for e in marks:
        pi = play(E[e - 1][3])
        d64 = np.abs(pi - runs["float64"][0][e]).max(axis=0)
        d32 = np.abs(runs["float32"][0][e] - runs["float64"][0][e]).max(axis=0)
        envelope = np.minimum(4 * d32 + 16 * U * rng_w, 1e-4 * rng_w)
        ok = ok and bool((d64 <= envelope).all())
        rows.append((e, float(d64.max()), float(d32.max()), float((d64 / envelope).max())))
    verdict(ok, f"L2 the first {n_free} epochs replayed freely: every checkpoint inside its envelope")
    for e, a, b, r in rows:
        print(f"        epoch {e:6d}  |iris - b64| {a:.2e}  |b32 - b64| {b:.2e}  of envelope {r:.2f}")
    stop = runs["float64"][1]
    if n_free == len(E):
        print(f"        iris stopped after {len(E)} epochs; the binary64 replay's own stopping "
              f"rules would stop at {stop if stop else 'no epoch in the run'}")


def saved_file(exe, work):
    path = os.path.join(work, "saved.bin")
    out = subprocess.run([exe, "save", path], check=True, capture_output=True, text=True).stdout
    blob = open(path, "rb").read()
    print(f"\na saved file: {len(blob)} bytes")
    magic, ver, hdr, flags, ni, nh, no, nex, seed, next_id, rng = struct.unpack("<4s10I", blob[:44])
    smooth = struct.unpack("<f", blob[44:48])[0]
    crc_ok = zlib.crc32(blob[:-4]) == struct.unpack("<I", blob[-4:])[0]
    size_ok = len(blob) == hdr + 4 * ((nh * ni + nh + no * nh + no) + 2 * ni + 2 * no
                                      + nex * (ni + no) + nex) + 4
    verdict(magic == b"IRIS" and ver == 7 and hdr == 48 and flags == 3 and crc_ok and size_ok
            and seed != 0 and rng != 0 and 0.0 <= smooth <= 1.0,
            f"L3 parsed from the format table: magic, version 7, header 48, flags {flags}, "
            f"shape {ni}-{nh}-{no}, {nex} demonstrations, next_id {next_id}, CRC-32 (zlib) {crc_ok}")
    f = np.frombuffer(blob, dtype="<f4", offset=48, count=(len(blob) - 52) // 4)
    o = 0

    def tk(k):
        nonlocal o
        a = f[o:o + k].astype(np.float64); o += k
        return a

    w1, b1, w2, b2 = tk(nh * ni).reshape(nh, ni), tk(nh), tk(no * nh).reshape(no, nh), tk(no)
    il, ih, ol, oh = tk(ni), tk(ni), tk(no), tk(no)
    viol, total, worst = 0, 0, 0.0
    for line in out.split("\n"):
        if not line.strip():
            continue
        v = [float.fromhex(z) for z in line.split()]
        x = norm_in(np.array(v[:2]), il.astype(F32), ih.astype(F32))
        dx = 4 * U * (np.abs(x) + 1)
        s = b1 + w1 @ x
        ds = (ni + 1) * U / (1 - (ni + 1) * U) * (np.abs(b1) + np.abs(w1) @ np.abs(x)) + np.abs(w1) @ dx
        a = act(s)
        da = ds + 7 * U
        z = b2 + w2 @ a
        dz = (nh + 1) * U / (1 - (nh + 1) * U) * (np.abs(b2) + np.abs(w2) @ np.abs(a)) + np.abs(w2) @ da
        y = sig(z)
        dy = 0.25 * dz + 4.5 * U
        rw = oh - ol
        want = np.clip(ol + (y - OUT_LO) / (OUT_HI - OUT_LO) * rw, ol, oh)
        bound = rw * (dy / (OUT_HI - OUT_LO) + 4 * U) + 2 * U * np.maximum(np.abs(ol), np.abs(oh))
        d = np.abs(np.array(v[2:5]) - want)
        viol += int((d > bound).sum()); total += d.size
        worst = max(worst, float((d / bound).max()))
    verdict(viol == 0 and total > 0, f"L3 a binary64 forward pass of the file: {total} outputs, {viol} "
                                     f"outside the rounding bound (worst {worst:.3f} of it)")


def neighbours(exe):
    out = subprocess.run([exe, "neighbours"], check=True, capture_output=True, text=True).stdout
    D = np.array([[float.fromhex(v) for v in l.split()[1:]] for l in out.split("\n") if l.startswith("D ")])
    Q = [l.split()[1:] for l in out.split("\n") if l.startswith("Q ")]
    q = np.array([[float.fromhex(r[0]), float.fromhex(r[1])] for r in Q])
    k3 = np.array([[float.fromhex(v) for v in r[2:5]] for r in Q])
    ids = np.array([int(r[5]) for r in Q])
    o1 = np.array([[float.fromhex(v) for v in r[6:9]] for r in Q])
    Xtr, Ytr = D[:, :2], D[:, 2:]
    ni, k = Xtr.shape[1], 3
    lo, hi = Xtr.min(0), Xtr.max(0)
    Z, Zq = (Xtr - lo) / (hi - lo), (q - lo) / (hi - lo)       # fractions of each range, PART 10
    d2 = ((Zq[:, None, :] - Z[None, :, :]) ** 2).sum(-1)
    ds = np.sort(d2, 1)
    free1 = (ds[:, 1] - ds[:, 0]) > 1e-4 * ds[:, 1]
    freek = (ds[:, k] - ds[:, k - 1]) > 1e-4 * ds[:, k]
    nearest = d2.argmin(1)
    print(f"\nneighbours: {len(D)} demonstrations, {len(q)} queries")
    same_id = bool((nearest + 1 == ids)[free1].all())       # identifiers start at 1, in recording order
    same_bits = np.array_equal(Ytr[nearest][free1].astype(F32).view(np.uint32),
                               o1[free1].astype(F32).view(np.uint32))
    verdict(same_id and same_bits, f"L5 nearest neighbour: the same demonstration and the same bits on "
                                   f"all {int(free1.sum())} queries without a tie")
    idx = np.argsort(d2, 1)[:, :k]
    w = 1.0 / (np.take_along_axis(d2, idx, 1) + 1e-9)          # weight 1/(d^2 + 1e-9), PART 10
    blend = (w[:, :, None] * Ytr[idx]).sum(1) / w.sum(1)[:, None]
    bound = (2 * ni + 2 * k + 15) * U * np.abs(Ytr).max()
    err = np.abs(blend - k3)[freek]
    verdict(bool((err <= bound).all()), f"L5 k-nearest-neighbour blend, k = 3: {int(freek.sum())} queries "
                                        f"without a tie, worst {err.max():.2e} (bound {bound:.2e})")
    try:
        from sklearn.neighbors import KNeighborsRegressor
    except ImportError:
        print("  note  scikit-learn is not installed: its comparison is not run")
        return
    sk = KNeighborsRegressor(n_neighbors=k, weights=lambda dd: 1.0 / (dd ** 2 + 1e-9), algorithm="brute")
    err = np.abs(sk.fit(Z, Ytr).predict(Zq) - k3)[freek]
    verdict(bool((err <= bound).all()), f"L5 scikit-learn KNeighborsRegressor, k = 3: worst {err.max():.2e} "
                                        f"(bound {bound:.2e})")


def main():
    if len(sys.argv) < 2:
        print(__doc__.split("\n\n")[-2])
        return 2
    exe = os.path.abspath(sys.argv[1])
    work = os.path.join(os.path.dirname(exe), "reference")
    os.makedirs(work, exist_ok=True)
    print(f"iris against a binary64 reference (NumPy {np.__version__})")
    trajectory(exe, work, 0.0, 2000)      # stops at the plateau, 18,000 epochs; replayed freely for 2,000
    trajectory(exe, work, 0.5, 60000)     # smoothing on: the weight decay path, replayed whole
    saved_file(exe, work)
    neighbours(exe)
    print(f"\n{'FAIL' if FAILS else 'PASS'}  reference: {len(FAILS)} layer(s) failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
