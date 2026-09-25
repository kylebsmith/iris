# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Kyle Smith
"""Run iris and its binary64 reference side by side, and compare.

    python3 tests/reference/run.py            every check, then the task table
    python3 tests/reference/run.py --quick    every check, no task table

It builds tests/reference/export.c against the repository's iris.h, runs each
recipe below through it, replays every recipe in reference.py, and prints one
PASS or FAIL line per check with the number it rests on. The exit status is 1
if any check fails, 2 if the export could not be built or run. The task table
(scikit-learn's multilayer perceptron against iris on the same data) is a
report: it prints a band and whether iris sits inside it, and never fails the
run. README.md explains every check and derives every tolerance.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import warnings

sys.dont_write_bytecode = True     # leave no __pycache__ in the repository
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np                  # noqa: E402
import reference as ref             # noqa: E402

U = ref.U

# ---------------------------------------------------------------------------
# Tolerances. README.md, "Tolerances", derives each. The three that are
# calibrated rather than derived carry the measurement that sets them: the
# largest value over every recipe below, with numpy 2.2.4 on an Apple M4 Max.
# ---------------------------------------------------------------------------
# (a) one epoch replayed from iris's own state: every weight and velocity
# within ONE_EPOCH_C * n_ex * u * max(|w|, 1), plus n_ex * u * max(|w|, 1)
# more when weight decay is on (a second rounding per visit). Derived.
ONE_EPOCH_C = 2.0
# (a) the same replay's epoch error, against the error iris reports for the
# epoch: within ERR_C * u * (sqrt(err) + n_ex * n_out * err). Each error
# y - t is a few u from binary64's, which moves the mean of the squares by
# at most twice that times sqrt(err); adding n_ex * n_out squares in binary32
# adds up to n_ex * n_out * u relative. The form is derived, ERR_C
# calibrated: the worst is 1.39 (grid, near the error floor), 5.8 times
# inside 8.
ERR_C = 8.0
# (a) the free-running trajectory: after e epochs the largest weight
# difference, relative to max(1, largest weight), within
# min(TRAJ_C * e * roundings_per_epoch * u, TRAJ_CEILING).
# Measured: the ratio to e * roundings * u peaks at epoch 1, at 3.06e-4
# (the grid recipe); the largest relative difference is 1.8e-4 (noisy,
# epoch 12,050).
TRAJ_C = 2e-3
TRAJ_CEILING = 1e-3
# (a) predictions of the two trajectories' weights at checkpoints, as a
# fraction of each output's range: a quarter of one step of a 7-bit
# controller value (1/128). Measured: 5.7e-4 at most (noisy, epoch 12,050).
PREDICT_CEILING = 2e-3
# (a) a stopping decision that differs is accepted when the reference's
# margin to the threshold is within this multiple of how far the two runs'
# errors differ at that epoch.
STOP_MARGIN_K = 4.0
# (c) neighbour blends: relative to the largest magnitude each output was
# demonstrated at.
KNN_REL = 1e-6
# (c) a query counts as tie-free when its k-th and (k+1)-th nearest
# distances differ by more than this, relatively.
TIE_GAP = 1e-4

GOLDEN_HASH = "0x6805FB0D"


# ---------------------------------------------------------------------------
# Recipes. Every number is generated in binary32 here and travels to the
# export as C99 hexadecimal, so both sides hold the same bits.
# ---------------------------------------------------------------------------
F = np.float32


def tanh32(x):
    """The header's rational function, evaluated in binary32 in the order a C
    compiler evaluates x * (27 + x*x) / (27 + 9*x*x): it builds the golden
    recipe's demonstrations, which must be tests/audit.c's to the bit."""
    x = F(x)
    if x > F(1e9):
        return F(1.0)
    if x < F(-1e9):
        return F(-1.0)
    x2 = x * x
    p = x * (F(27.0) + x2) / (F(27.0) + F(9.0) * x2)
    return F(1.0) if p > F(1.0) else (F(-1.0) if p < F(-1.0) else p)


def truth32(x, y):
    """tests/audit.c's truth(): three outputs of two inputs in [0, 1]."""
    x, y = F(x), F(y)
    return [F(0.5) + F(0.45) * tanh32(F(3.0) * (x - F(0.5))),
            F(0.5) + F(0.40) * tanh32(F(2.5) * (y - F(0.5)) * (x + F(0.3))),
            F(0.2) + F(0.6) * (x * y)]


def golden_demos():
    """tests/audit.c's golden demonstrations: 20 scattered points of truth()."""
    rows = []
    for i in range(20):
        u = F((i * 7919) % 97) / F(97.0)
        v = F((i * 6131) % 89) / F(89.0)
        rows.append([u, v] + truth32(u, v))
    return np.array(rows, dtype=np.float32)


def random_truth_demos(n, noise, seed):
    """n uniform points of truth() with Gaussian noise of standard deviation
    `noise` on every output."""
    g = np.random.default_rng(seed)
    rows = []
    for _ in range(n):
        u, v = F(g.uniform()), F(g.uniform())
        out = [F(float(o) + noise * g.standard_normal()) for o in truth32(u, v)]
        rows.append([u, v] + out)
    return np.array(rows, dtype=np.float32)


def sensor_demos(n, seed):
    """Readings in their own units: a distance in millimetres, an
    accelerometer in g, and a third input held at 500 in every take (still,
    so the instrument must ignore it); out, a pitch in hertz, a controller
    value from 0 to 127, and a second controller left at 64 in every take (an
    output that never moved, so its range is given the documented floor)."""
    g = np.random.default_rng(seed)
    rows = []
    for _ in range(n):
        mm, acc = g.uniform(80.0, 1300.0), g.uniform(-2.0, 2.0)
        a, b = (mm - 80.0) / 1220.0, (acc + 2.0) / 4.0
        hz = 200.0 + 1800.0 * (0.5 + 0.45 * np.sin(2.6 * a + 1.3 * b))
        cc = 127.0 * b * (0.3 + 0.7 * a) + 2.0 * g.standard_normal()
        rows.append([mm, acc, 500.0, hz, cc, 64.0])
    return np.array(rows, dtype=np.float32)


def grid_demos():
    """Nine takes on a 3 x 3 grid of two inputs driving one output: few enough
    that the network fits them to the error floor."""
    g = (0.0, 0.5, 1.0)
    return np.array([[x, y, 0.2 + 0.3 * x + 0.4 * y * y] for x in g for y in g],
                    dtype=np.float32)


def classes_demos(seed):
    """40 takes for the neighbour samplers. In: a distance, an accelerometer,
    an input held at 3.3, and two inputs on either side of the still-input
    threshold (a width of 1e-5 of the magnitude): one drifting across 0.01
    around 500, about twice the threshold, so it moves; one across 0.001
    around 200, about half of it, so it is still. Out: a class label from 0 to
    3 (which quadrant of the first two inputs), two continuous outputs, and
    440 in every take."""
    g = np.random.default_rng(seed)
    rows = []
    for _ in range(40):
        mm, acc = g.uniform(0.0, 1300.0), g.uniform(-2.0, 2.0)
        label = float((mm > 650.0) * 2 + (acc > 0.0))
        rows.append([mm, acc, 3.3, 500.0 + g.uniform(-0.005, 0.005),
                     200.0 + g.uniform(-0.0005, 0.0005),
                     label, np.cos(mm / 400.0) * acc, 1e3 + mm * 0.5, 440.0])
    return np.array(rows, dtype=np.float32)


def grid_probes(demos, n_in, per_side, spread=0.25, seed=0):
    """Probes over the demonstrated input box widened by `spread` of its width
    on every side, so they include readings beyond anything demonstrated:
    a grid for one or two inputs, uniform random points for more. A still
    input is probed across values it never took, to show they are ignored."""
    X = demos[:, :n_in].astype(np.float64)
    lo, hi = X.min(axis=0), X.max(axis=0)
    w = np.where(hi > lo, hi - lo, 20.0)
    lo, hi = lo - spread * w, hi + spread * w
    if n_in == 1:
        P = np.linspace(lo[0], hi[0], per_side * per_side)[:, None]
    elif n_in == 2:
        a = np.linspace(lo[0], hi[0], per_side)
        b = np.linspace(lo[1], hi[1], per_side)
        P = np.array([[x, y] for x in a for y in b])
    else:
        g = np.random.default_rng(seed)
        P = g.uniform(lo, hi, size=(per_side * per_side, n_in))
    return P.astype(np.float32)


def recipes():
    golden = golden_demos()
    noisy = random_truth_demos(30, 0.05, seed=31)
    out = [
        dict(name="golden", demos=golden, shape=(2, 12, 3), seed=1234, smoothing=0.0,
             ceiling=800, warm=100,
             note="tests/audit.c's golden recipe: seed 1234, 20 demonstrations, 800 epochs"),
        dict(name="golden-plateau", demos=golden, shape=(2, 12, 3), seed=1234,
             smoothing=0.0, ceiling=0, warm=50,
             note="the same demonstrations through iris_train, to its plateau"),
        dict(name="noisy", demos=noisy, shape=(2, 12, 3), seed=77, smoothing=0.0,
             ceiling=0, warm=50, note="30 demonstrations, noise 0.05, iris_train"),
        dict(name="noisy-smoothing", demos=noisy, shape=(2, 12, 3), seed=77,
             smoothing=0.5, ceiling=0, warm=50,
             note="the same, smoothing 0.5 (weight decay 0.15)"),
        dict(name="plateau-edge", demos=random_truth_demos(20, 0.03, seed=33),
             shape=(2, 12, 3), seed=6, smoothing=0.0, ceiling=0, warm=0,
             note="20 takes, noise 0.03; two plateau tests land just above the tolerance"),
        dict(name="sensors", demos=sensor_demos(25, seed=4242), shape=(3, 16, 3),
             seed=4242, smoothing=0.15, ceiling=0, warm=50,
             note="raw units, a still input, a constant output, 3-16-3, smoothing 0.15"),
        dict(name="grid", demos=grid_demos(), shape=(2, 12, 1), seed=9, smoothing=0.0,
             ceiling=0, warm=50, note="nine takes on a 3x3 grid, 2-12-1, stops on the error floor"),
    ]
    for r in out:
        r["train"] = 1
        r["dump"] = 1
        r["probes"] = grid_probes(r["demos"], r["shape"][0], 41)
        r["ks"] = [1, 3, 5, 8]
    # The grid recipe also plays the quarter-step lattice over its takes: the
    # takes themselves, and points midway between two or four of them, where
    # the nearest take is an exact tie in binary32 as well.
    q = (0.0, 0.25, 0.5, 0.75, 1.0)
    grid = next(r for r in out if r["name"] == "grid")
    grid["probes"] = np.vstack([grid["probes"],
                                np.array([[a, b] for a in q for b in q], dtype=np.float32)])
    classes = classes_demos(seed=5)
    out.append(dict(name="classes", demos=classes, shape=(5, 12, 4), seed=1, smoothing=0.0,
                    ceiling=0, warm=0, train=0, dump=0,
                    probes=grid_probes(classes, 5, 45, spread=0.2, seed=6), ks=[1, 3, 5, 8],
                    note="40 takes, an untrained instrument, class labels in out[0]"))
    return out


def recipe_text(r):
    I, H, O = r["shape"]
    hx = lambda a: " ".join(float(v).hex() for v in np.asarray(a, dtype=np.float32).ravel())
    lines = [f"{I} {H} {O} {r['seed']} {float(F(r['smoothing'])).hex()}",
             f"{r['train']} {r['ceiling']} {r['dump']} {r['warm']}",
             f"{len(r['demos'])}", hx(r["demos"]),
             f"{len(r['probes'])}", hx(r["probes"]),
             f"{len(r['ks'])} " + " ".join(str(k) for k in r["ks"])]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Building and running the export.
# ---------------------------------------------------------------------------
def build_export(cc, workdir):
    exe = os.path.join(workdir, "export")
    cmd = cc.split() + ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-o", exe, os.path.join(HERE, "export.c")]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0 or res.stderr.strip():
        sys.stderr.write(" ".join(cmd) + "\n" + res.stderr)
        sys.exit(2)
    return exe


def run_export(exe, r, workdir):
    path = os.path.join(workdir, r["name"] + ".json")
    with open(path, "w") as out:
        res = subprocess.run([exe], input=recipe_text(r), stdout=out,
                             stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        sys.stderr.write(f"export failed on {r['name']}: {res.stderr}")
        sys.exit(2)
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Reporting.
# ---------------------------------------------------------------------------
class Report:
    def __init__(self):
        self.failed = []

    def check(self, name, ok, detail):
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<46} {detail}")
        if not ok:
            self.failed.append(name)


def fmt(x):
    return f"{x:.3g}"


# ---------------------------------------------------------------------------
# The exact facts.
# ---------------------------------------------------------------------------
def check_exact(rep, r, ex, tag):
    I, H, O = r["shape"]
    demos32 = np.asarray(r["demos"], dtype=np.float32)
    rep.check(f"{tag}: demonstrations arrive intact",
              np.array_equal(ref.float32s(ex["demos"]).view(np.uint32),
                             demos32.ravel().view(np.uint32)),
              f"{demos32.size} values, bit for bit")
    rg_ref = ref.fit_ranges32(demos32, I)
    rg = exported_ranges(ex)
    same = all(np.array_equal(getattr(rg_ref, f), getattr(rg, f))
               for f in ("in_lo", "in_hi", "out_lo", "out_hi"))
    rep.check(f"{tag}: fitted ranges", same,
              f"{I + O} channels, {int(rg.still.sum())} still input(s), bit for bit")
    if not r["train"]:
        return rg
    got = [float(ref.float32s(ex[k])[0]) for k in ("lr", "momentum", "l2")]
    want = [ref.LEARNING_RATE, ref.MOMENTUM, ref.weight_decay(r["smoothing"])]
    rep.check(f"{tag}: learning rate, momentum, decay", got == want,
              "iris holds " + ", ".join(f"{v:.9g}" for v in got)
              + "; the description gives " + ", ".join(f"{v:.9g}" for v in want))
    init = ref.float32s(ex["initial"]["w"])
    w_recip, state = ref.reseed32(r["seed"], I, H, O, "reciprocal")
    w_quot, _ = ref.reseed32(r["seed"], I, H, O, "quotient")
    n_recip = int((w_recip.view(np.uint32) == init.view(np.uint32)).sum())
    n_quot = int((w_quot.view(np.uint32) == init.view(np.uint32)).sum())
    rep.check(f"{tag}: starting weights from the seed",
              n_recip == init.size and state == ex["rng_after_reseed"]
              and not ref.floats(ex["initial"]["v"]).any(),
              f"draw*(1/sqrt n) matches {n_recip}/{init.size}, "
              f"draw/sqrt n {n_quot}/{init.size}; generator state and zero velocities")
    # the shuffle, re-derived: identity at the start of each session,
    # carried across the epochs of a session, one session per warm call
    n = len(demos32)
    order = list(range(n))
    bad = 0
    for ep in ex["epochs"]:
        state = ref.shuffle(order, state)
        bad += order != ep["order"]
    for ep in ex["warm"]:
        order = list(range(n))
        state = ref.shuffle(order, state)
        bad += order != ep["order"]
    total = len(ex["epochs"]) + len(ex["warm"])
    rep.check(f"{tag}: shuffle order of every epoch", bad == 0,
              f"{total - bad}/{total} epochs re-derived from the seed")
    rep.check(f"{tag}: one-epoch slices equal the blocking call",
              ex["end"]["blocking_identical"],
              "saved files byte-identical" if ex["end"]["blocking_identical"]
              else "saved files differ")
    if r["name"] == "golden":
        rep.check(f"{tag}: the golden instrument hash", ex["end"]["hash"] == GOLDEN_HASH,
                  f"{ex['end']['hash']} (tests/audit.c pins {GOLDEN_HASH})")
    return rg


def exported_ranges(ex):
    R = ex["ranges"]
    return ref.Ranges(ref.floats(R["in_lo"]), ref.floats(R["in_hi"]),
                      ref.floats(R["out_lo"]), ref.floats(R["out_hi"]))


# ---------------------------------------------------------------------------
# (a) The training trajectory.
# ---------------------------------------------------------------------------
def float32_plateau_fires(ref_err, err):
    """The plateau comparison as binary32 evaluates it, for testing iris's
    decisions against its own exported errors."""
    return F(F(ref_err) - F(err)) <= F(F(ref.CONV_TOL) * F(ref_err))


def iris_rule_epoch(errs, conv, ceiling, statuses):
    """Where the documented rule, applied to iris's OWN per-epoch errors,
    says the run ends, and why; (None, ...) if it does not end it within
    the epochs iris ran."""
    refv = 0.0
    for e, err in enumerate(errs, start=1):
        if statuses[e - 1] == 1:
            return e, "diverged"
        if err < ref.ERROR_FLOOR:
            return e, "floor"
        if conv and e % ref.CONV_WINDOW == 0:
            if refv > 0.0 and float32_plateau_fires(refv, err):
                return e, "plateau"
            refv = err
        if e >= ceiling:
            return e, "ceiling"
    return None, f"not within {len(errs)} epochs"


def check_trajectory(rep, r, ex, rg, tag, stats):
    I, H, O = r["shape"]
    shape = ref.Shape(I, H, O)
    demos = np.asarray(r["demos"], dtype=np.float64)
    X = ref.normalise_inputs(demos[:, :I], rg)
    T = ref.normalise_targets(demos[:, I:], rg)
    lr, mom, l2 = ref.LEARNING_RATE, ref.MOMENTUM, ref.weight_decay(r["smoothing"])
    n = len(demos)
    wd = l2 * lr / n
    eps = ex["epochs"] + ex["warm"]
    E_main = len(ex["epochs"])
    Wx = np.array([ref.floats(ep["w"]) for ep in eps])
    Vx = np.array([ref.floats(ep["v"]) for ep in eps])
    orders = np.array([ep["order"] for ep in eps])
    errs = np.array([float(ref.float32s(ep["err"])[0]) for ep in eps])
    statuses = [ep["status"] for ep in eps]

    # --- one epoch at a time, from iris's own state ---------------------
    W0 = np.vstack([ref.floats(ex["initial"]["w"]), Wx[:-1]])
    V0 = np.vstack([ref.floats(ex["initial"]["v"]), Vx[:-1]])
    t0 = time.perf_counter()
    W1s, V1s, E1s = ref.replay_epochs_batched(shape, W0, V0, orders, X, T, lr, mom, wd)
    scale = np.maximum(np.abs(Wx), 1.0)
    c_eff = ONE_EPOCH_C + (1.0 if wd > 0 else 0.0)
    bound = c_eff * n * U * scale
    ratio_w = np.abs(W1s - Wx) / bound
    ratio_v = np.abs(V1s - Vx) / bound
    worst = max(ratio_w.max(), ratio_v.max())
    per_visit = max((np.abs(W1s - Wx) / (U * scale)).max(), (np.abs(V1s - Vx) / (U * scale)).max()) / n
    stats["one_epoch"].append((tag, worst, per_visit))
    rep.check(f"{tag}: every epoch, replayed from iris's state", worst <= 1.0,
              f"{len(eps)} epochs; worst {fmt(worst)} of the bound "
              f"{fmt(c_eff)}*n*u*max(|w|,1) ({fmt(per_visit)}u per visit); "
              f"{fmt(time.perf_counter() - t0)} s")
    # the epoch's error: what iris_continue returns, and what the error floor
    # and the plateau test read
    ratio_e = np.abs(errs - E1s) / (ERR_C * U * (np.sqrt(E1s) + n * O * E1s))
    stats["epoch_error"].append((tag, ratio_e.max()))
    rep.check(f"{tag}: every epoch's error, replayed", ratio_e.max() <= 1.0,
              f"{len(eps)} epochs; worst {fmt(ratio_e.max())} of "
              f"{ERR_C:g}*u*(sqrt(err)+n*outputs*err) (epoch {int(ratio_e.argmax()) + 1})")

    # --- the whole run, free-running in binary64 ------------------------
    # A second binary64 run starts from the same weights with w1[0] moved by
    # one binary32 unit in the last place. How far it strays measures how
    # strongly the training dynamics themselves amplify a small difference,
    # which is what the free-running difference between iris and the
    # reference is made of (README.md, "Tolerances").
    t0 = time.perf_counter()
    w_init = ref.floats(ex["initial"]["w"])
    net = ref.Net(shape, w_init)
    w_nudged = w_init.copy()
    w_nudged[0] = float(np.nextafter(np.float32(w_init[0]), np.float32(np.inf)))
    nudge = ref.Net(shape, w_nudged)
    nudge_min, amplification, amp_epoch = np.inf, 1.0, 0
    P = n * ref.roundings_per_visit(shape)
    rule = ref.StoppingRule(conv=1, ceiling=ex["ceiling"])
    ref_stop, tests = None, {}
    worst_rel, worst_c, worst_pred, worst_epoch, worst_d = 0.0, 0.0, 0.0, 0, 0.0
    err_rel = np.zeros(len(eps))
    checkpoints = set(range(1000, len(eps) + 1, 1000)) | {E_main, len(eps)}
    for e in range(1, len(eps) + 1):
        err = ref.train_epoch(net, X, T, orders[e - 1], lr, mom, wd)
        diverged = ref.divergence_guard(net)
        ref.train_epoch(nudge, X, T, orders[e - 1], lr, mom, wd)
        ref.divergence_guard(nudge)
        err_rel[e - 1] = abs(errs[e - 1] - err) / err
        if e <= E_main:
            why = rule.after_epoch(err, diverged)
            if rule.last and rule.last[0] == e:
                tests[e] = rule.last
            if why and ref_stop is None:
                ref_stop = (e, why)
        w = net.weights()
        big = max(1.0, np.abs(w).max())
        d = np.abs(Wx[e - 1] - w).max() / big
        dn = np.abs(nudge.weights() - w).max() / big
        nudge_min = min(nudge_min, dn)
        if nudge_min > 0 and dn / nudge_min > amplification:
            amplification, amp_epoch = dn / nudge_min, e
        tol = min(TRAJ_C * e * P * U, TRAJ_CEILING)
        worst_c = max(worst_c, d / (e * P * U))
        worst_d = max(worst_d, d)
        if d / tol > worst_rel:
            worst_rel, worst_epoch = d / tol, e
        if e in checkpoints:
            probes = r["probes"].astype(np.float64)
            a = ref.predict(ref.Net(shape, Wx[e - 1]), rg, probes)
            b = ref.predict(net, rg, probes)
            worst_pred = max(worst_pred, (np.abs(a - b) / rg.out_width).max())
    stats["trajectory"].append((tag, worst_c, worst_rel, worst_pred, amplification, worst_d))
    rep.check(f"{tag}: whole run, free-running binary64",
              worst_rel <= 1.0 and worst_pred <= PREDICT_CEILING,
              f"{len(eps)} epochs; weights at worst {fmt(worst_rel)} of "
              f"min({TRAJ_C:g}*e*{P}*u, {TRAJ_CEILING:g}) (epoch {worst_epoch}); "
              f"predictions {fmt(worst_pred)} of range (ceiling {PREDICT_CEILING:g}); "
              f"a one-unit nudge grew {fmt(amplification)}x by epoch {amp_epoch}; "
              f"{fmt(time.perf_counter() - t0)} s")
    stats["error_rel"].append((tag, err_rel.max()))

    # --- the stopping decision ------------------------------------------
    E_rule, why_rule = iris_rule_epoch(errs[:E_main], 1, ex["ceiling"], statuses[:E_main])
    rep.check(f"{tag}: iris stops where its rule says", E_rule == E_main,
              f"stopped at epoch {E_main}; the documented rule on iris's own errors "
              f"stops " + (f"at {E_rule} ({why_rule})" if E_rule else why_rule))
    E_ref, why_ref = ref_stop if ref_stop else (None, "not by epoch %d" % E_main)
    if E_ref == E_main:
        rep.check(f"{tag}: the reference stops at the same epoch", True,
                  f"epoch {E_main} ({why_ref})")
    else:
        e = min(E_ref or E_main, E_main)
        test = tests.get(e)
        margin = abs(test[3]) if test else np.inf
        spread = err_rel[e - 1] + (err_rel[e - 1 - ref.CONV_WINDOW]
                                   if e > ref.CONV_WINDOW else 0.0)
        ok = margin <= STOP_MARGIN_K * spread
        rep.check(f"{tag}: the reference's stopping decision", ok,
                  f"iris stops at {E_main}, the reference at {E_ref} ({why_ref}); "
                  f"its margin {fmt(margin)} against {STOP_MARGIN_K:g}x the errors' "
                  f"relative difference {fmt(spread)}")
    return net


# ---------------------------------------------------------------------------
# (b) The forward pass on the final weights.
# ---------------------------------------------------------------------------
def check_forward(rep, r, ex, rg, tag, stats):
    I, H, O = r["shape"]
    net = ref.Net(ref.Shape(I, H, O), ref.floats(ex["final"]["w"]))
    probes = ref.float32s(ex["probes"]).astype(np.float64).reshape(-1, I)
    got = ref.floats(ex["predict"]).reshape(-1, O)
    want = ref.predict(net, rg, probes)
    bound = ref.forward_bound(net, rg, probes)
    ratio = (np.abs(got - want) / bound).max()
    ulps = (np.abs(got - want) / (U * np.maximum(np.abs(want), 1e-30))).max()
    stats["forward"].append((tag, ratio))
    rep.check(f"{tag}: forward pass on the final weights", ratio <= 1.0,
              f"{got.size} outputs; worst {fmt(ratio)} of the rounding bound "
              f"(largest difference {fmt(np.abs(got - want).max())}, {fmt(ulps)} units of u relative)")


# ---------------------------------------------------------------------------
# (c) The neighbour samplers against scikit-learn.
# ---------------------------------------------------------------------------
def check_neighbours(rep, r, ex, rg, tag, stats):
    from sklearn.neighbors import KNeighborsClassifier, KNeighborsRegressor
    I, H, O = r["shape"]
    demos = np.asarray(r["demos"], dtype=np.float64)
    Q = ref.float32s(ex["probes"]).astype(np.float64).reshape(-1, I)
    # Each input as a fraction of its range, counted from the low end, as
    # iris counts it. The offset matters to scikit-learn alone: its brute-force
    # distance expands |a - b|^2 as |a|^2 - 2 a.b + |b|^2, which loses digits
    # when the coordinates sit far from zero (an input drifting across 0.01
    # around 500 becomes about 51,000 when merely scaled, and the blends then
    # came out 2e-3 away from reference.py's own).
    s = ref.neighbour_scale(rg)
    Xs, Qs = (demos[:, :I] - rg.in_lo) * s, (Q - rg.in_lo) * s
    n = len(demos)
    dist = np.sqrt(np.sort(ref.neighbour_distances2(demos, I, rg, Q), axis=1))

    def tie_free(k):
        if k >= n:
            return np.ones(len(Q), dtype=bool)
        return dist[:, k] - dist[:, k - 1] > TIE_GAP * dist[:, k]

    guard = ref.KNN_GUARD
    # each output's scale: the largest magnitude it was demonstrated at
    scale = np.maximum(np.abs(demos[:, I:]).max(axis=0), 1e-30)
    for k in r["ks"]:
        got = ref.floats(ex["knn"][str(k)]).reshape(-1, O)
        sk = KNeighborsRegressor(n_neighbors=min(k, n), algorithm="brute",
                                 weights=lambda d: 1.0 / (d * d + guard))
        want = sk.fit(Xs, demos[:, I:]).predict(Qs)
        own, idx = ref.knn_predict(demos, I, rg, Q, k)
        free = tie_free(min(k, n))
        rel = (np.abs(got - want) / scale)[free].max()
        agree = np.abs(own - want)[free].max()
        stats["knn"].append((tag, k, rel))
        rep.check(f"{tag}: {k}-nearest blend against scikit-learn", rel <= KNN_REL,
                  f"{int(free.sum())}/{len(Q)} tie-free queries; worst {fmt(rel)} of the "
                  f"output's largest magnitude (tolerance {KNN_REL:g}); "
                  f"scikit-learn and reference.py differ by {fmt(agree)}")

    ids = np.array(ex["nn1_id"])
    outs = ref.float32s(ex["nn1_out"]).reshape(-1, O)
    free = tie_free(1)
    nn = KNeighborsRegressor(n_neighbors=1, algorithm="brute").fit(Xs, demos[:, I:])
    idx = nn.kneighbors(Qs, return_distance=False)[:, 0]
    same_id = (ids[free] == idx[free] + 1).all()
    verbatim = np.array_equal(outs.view(np.uint32),
                              np.asarray(r["demos"], np.float32)[ids - 1, I:].view(np.uint32))
    detail = f"{int(free.sum())}/{len(Q)} tie-free queries name scikit-learn's nearest; " \
             f"outputs bit for bit"
    ok = same_id and verbatim
    if r["name"] == "classes":
        clf = KNeighborsClassifier(n_neighbors=1, algorithm="brute").fit(Xs, demos[:, I])
        labels = clf.predict(Qs)
        ok = ok and np.array_equal(outs[free, 0].astype(np.float64), labels[free])
        detail += "; class labels identical"
    rep.check(f"{tag}: nearest demonstration against scikit-learn", ok, detail)

    # Every query, ties included: neighbours chosen by the documented rule on
    # the binary32 distances iris compares, blended in binary64.
    demos32 = np.asarray(r["demos"], dtype=np.float32)
    d2_32 = ref.neighbour_distances2_32(demos32, I, rg, ref.float32s(ex["probes"]).reshape(-1, I))
    ranked = np.sort(d2_32, axis=1)
    worst, ties = 0.0, np.zeros(len(Q), dtype=bool)
    for k in r["ks"]:
        got = ref.floats(ex["knn"][str(k)]).reshape(-1, O)
        own, _ = ref.knn_predict(demos, I, rg, Q, k, rank_by=d2_32)
        worst = max(worst, (np.abs(got - own) / scale).max())
        kk = min(k, n, ref.KNN_MAXK)
        if kk < n:
            ties |= ranked[:, kk - 1] == ranked[:, kk]
    first = np.argmin(d2_32, axis=1)          # the first of equal minima
    named = int((ids == first + 1).sum())
    rep.check(f"{tag}: neighbours on every query, ties included",
              worst <= KNN_REL and named == len(Q),
              f"{len(Q)} queries, {int(ties.sum())} with a binary32 tie at a neighbour "
              f"boundary; worst blend {fmt(worst)} of the output's largest magnitude; "
              f"{named}/{len(Q)} nearest demonstrations the earliest-recorded of the nearest")


# ---------------------------------------------------------------------------
# (d) Task-level calibration against scikit-learn's multilayer perceptron.
# ---------------------------------------------------------------------------
def calibration(exe, workdir, seeds, n_demos):
    """Held-out error of iris_train against scikit-learn's multilayer
    perceptron (MLPRegressor) on tests/audit.c's truth(), the golden recipe's
    function; on a noisy variant; and on the noisy variant with smoothing.
    SGD is stochastic gradient descent, one demonstration at a time; L-BFGS,
    the limited-memory Broyden-Fletcher-Goldfarb-Shanno method, fits the same
    model class close to its optimum. A report, not a check: see README.md,
    "The task table"."""
    from sklearn.exceptions import ConvergenceWarning
    from sklearn.neural_network import MLPRegressor
    g = np.linspace(0.0, 1.0, 21)
    grid = np.array([[x, y] for x in g for y in g], dtype=np.float32)
    clean = np.array([truth32(x, y) for x, y in grid], dtype=np.float64)
    momentum = ref.MOMENTUM
    rows = []
    for noise, smoothing in ((0.0, 0.0), (0.05, 0.0), (0.05, 0.5)):
        res = {"iris": [], "reroll": [], "sgd": [], "lbfgs": [], "epochs": []}
        # iris decays each weight by l2 * lr / n per visit, outside the
        # momentum; scikit-learn adds alpha * w to the gradient, inside it,
        # where momentum multiplies a steady push by 1 / (1 - momentum). The
        # same shrink per visit therefore needs alpha = l2 (1 - momentum) / n.
        # scikit-learn divides the penalty by the rows in each gradient step:
        # one for the SGD arm, all n for L-BFGS, which therefore takes n times
        # that alpha to be held as hard.
        l2 = ref.weight_decay(smoothing)
        alpha = l2 * (1.0 - momentum) / n_demos
        arm_alpha = {"sgd": alpha, "lbfgs": alpha * n_demos}
        t0 = time.perf_counter()
        for s in range(1, seeds + 1):
            demos = random_truth_demos(n_demos, noise, seed=1000 + s)
            fits = []
            for seed in (s, s + 1000):
                rc = dict(name=f"cal-{noise}-{smoothing}-{seed}", demos=demos,
                          shape=(2, 12, 3), seed=seed, smoothing=smoothing, train=1,
                          ceiling=0, dump=0, warm=0, probes=grid, ks=[])
                fits.append(run_export(exe, rc, workdir))
            pred = [ref.floats(f["predict"]).reshape(-1, 3) for f in fits]
            res["iris"].append(np.sqrt(((pred[0] - clean) ** 2).mean()))
            res["reroll"].append(np.sqrt(((pred[1] - clean) ** 2).mean()))
            epochs = fits[0]["end"]["epochs_done"]
            res["epochs"].append(epochs)
            # scikit-learn sees what iris's network sees: inputs to [-1, +1]
            # and targets to [0.1, 0.9] across the training ranges, and its
            # predictions are mapped back and held inside the demonstrated
            # range, as iris holds them.
            rg = ref.fit_ranges32(demos, 2)
            X = ref.normalise_inputs(demos[:, :2].astype(np.float64), rg)
            T = ref.normalise_targets(demos[:, 2:].astype(np.float64), rg)
            Xg = ref.normalise_inputs(grid.astype(np.float64), rg)
            for arm, kw in (("sgd", dict(solver="sgd", batch_size=1, learning_rate="constant",
                                         learning_rate_init=ref.LEARNING_RATE, momentum=momentum,
                                         nesterovs_momentum=False, max_iter=epochs, tol=0.0,
                                         n_iter_no_change=epochs + 1, shuffle=True)),
                            ("lbfgs", dict(solver="lbfgs", max_iter=5000, tol=1e-10))):
                m = MLPRegressor(hidden_layer_sizes=(12,), activation="tanh",
                                 alpha=arm_alpha[arm], random_state=s, **kw)
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", ConvergenceWarning)
                    m.fit(X, T)
                p = ref.denormalise(m.predict(Xg), rg)
                res[arm].append(np.sqrt(((p - clean) ** 2).mean()))
        rows.append((noise, smoothing, alpha, res, time.perf_counter() - t0))
    return rows


def geo_ci(ratios, rng, reps=4000):
    lr = np.log(ratios)
    boots = rng.choice(lr, size=(reps, len(lr))).mean(axis=1)
    return np.exp(lr.mean()), np.exp(np.percentile(boots, 2.5)), np.exp(np.percentile(boots, 97.5))


def print_calibration(rows, seeds, n_demos):
    rng = np.random.default_rng(0)
    print(f"\n  Task level: held-out root-mean-square error against the clean truth on "
          f"a 21x21 grid, {n_demos} demonstrations, {seeds} seeds")
    print("  ratio = iris / scikit-learn: geometric mean [95% bootstrap interval], "
          "below 1 favours iris")
    print(f"  {'noise':>5} {'smooth':>6} {'alpha':>8}  {'iris':>7} {'SGD':>7} {'LBFGS':>7}  "
          f"{'iris/SGD':>21}  {'iris/LBFGS':>21}  {'reroll':>6}  {'epochs':>6}  band")
    for noise, smoothing, alpha, res, secs in rows:
        a = {k: np.array(v, dtype=np.float64) for k, v in res.items()}
        sgd = geo_ci(a["iris"] / a["sgd"], rng)
        lb = geo_ci(a["iris"] / a["lbfgs"], rng)
        null_sd = np.log(a["iris"] / a["reroll"]).std(ddof=1)
        inside = all(CAL_BAND[0] <= r[1] and r[2] <= CAL_BAND[1] for r in (sgd, lb))
        gm = lambda v: np.exp(np.log(v).mean())
        print(f"  {noise:>5.2f} {smoothing:>6.2f} {alpha:>8.2g}  {gm(a['iris']):>7.4f} "
              f"{gm(a['sgd']):>7.4f} {gm(a['lbfgs']):>7.4f}  "
              f"{sgd[0]:>5.2f} [{sgd[1]:.2f}, {sgd[2]:.2f}]      "
              f"{lb[0]:>5.2f} [{lb[1]:.2f}, {lb[2]:.2f}]      {null_sd:>6.3f}  "
              f"{int(np.median(a['epochs'])):>6}  "
              f"{'inside' if inside else 'OUTSIDE'} [{CAL_BAND[0]:g}, {CAL_BAND[1]:g}]"
              f"  ({secs:.0f} s)")
    print("  SGD: stochastic gradient descent, one demonstration at a time, held to iris's "
          "epoch count; LBFGS: the limited-memory\n  Broyden-Fletcher-Goldfarb-Shanno method; "
          "reroll: standard deviation of log(iris / iris with another seed) on the same data;\n"
          f"  alpha: the SGD arm's, and L-BFGS takes {n_demos} times it; epochs: iris_train's median")


# The band both ratios' intervals should sit in: iris within a factor of 2,
# either way, of a standard implementation of the same model class. A
# factor of 2 is about what iris's one quality knob, smoothing, is worth at
# realistic noise (2.4x, the table above iris_set_smoothing), so an iris
# outside it would be further from a standard fit than its own knob moves
# it. A row outside the band is printed, and fails nothing.
CAL_BAND = (0.5, 2.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--quick", action="store_true", help="skip the task table")
    ap.add_argument("--seeds", type=int, default=8, help="seeds in the task table")
    ap.add_argument("--demos", type=int, default=20, help="demonstrations per task")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"), help="C compiler")
    ap.add_argument("--workdir", help="keep the export's JSON here")
    ap.add_argument("--only", help="run only the recipes whose names contain this")
    args = ap.parse_args()

    workdir = args.workdir or tempfile.mkdtemp(prefix="iris-reference-")
    os.makedirs(workdir, exist_ok=True)
    print(f"iris against its binary64 reference (numpy {np.__version__})")
    exe = build_export(args.cc, workdir)
    rep = Report()
    stats = {k: [] for k in ("one_epoch", "epoch_error", "trajectory", "error_rel", "forward",
                             "knn")}
    t_all = time.perf_counter()
    for r in recipes():
        if args.only and args.only not in r["name"]:
            continue
        tag = r["name"]
        print(f"\n{tag}: {r['note']}")
        ex = run_export(exe, r, workdir)
        rg = check_exact(rep, r, ex, tag)
        if r["train"]:
            check_trajectory(rep, r, ex, rg, tag, stats)
            check_forward(rep, r, ex, rg, tag, stats)
        check_neighbours(rep, r, ex, rg, tag, stats)

    if stats["trajectory"]:
        print("\n  Calibration of the trajectory envelope, across every recipe:")
        c_obs = max(t[1] for t in stats["trajectory"])
        print(f"    largest (weight difference) / (e * roundings per epoch * u): {c_obs:.3g}; "
              f"TRAJ_C = {TRAJ_C:g}, margin {TRAJ_C / c_obs:.3g}x")
        d_obs = max(t[5] for t in stats["trajectory"])
        print(f"    largest relative weight difference: {d_obs:.3g}; ceiling "
              f"{TRAJ_CEILING:g}, margin {TRAJ_CEILING / d_obs:.3g}x")
        p_obs = max(t[3] for t in stats["trajectory"])
        print(f"    largest prediction difference: {p_obs:.3g} of range; ceiling "
              f"{PREDICT_CEILING:g}, margin {PREDICT_CEILING / max(p_obs, 1e-300):.3g}x")
        o_obs = max(t[1] for t in stats["one_epoch"])
        v_obs = max(t[2] for t in stats["one_epoch"])
        print(f"    one epoch: worst {o_obs:.3g} of its bound, {v_obs:.3g}u per visit")
        e_obs = max(t[1] for t in stats["epoch_error"])
        print(f"    one epoch's error: worst {e_obs:.3g} of its bound; ERR_C = {ERR_C:g}, "
              f"margin {1.0 / e_obs:.3g}x")
        print(f"    epoch error, relative difference, largest: "
              f"{max(t[1] for t in stats['error_rel']):.3g}")
        print(f"    forward pass: worst {max(t[1] for t in stats['forward']):.3g} "
              f"of its bound")
    if stats["knn"]:
        print(f"    neighbour blends: worst {max(t[2] for t in stats['knn']):.3g} of the "
              f"output's largest magnitude, tolerance {KNN_REL:g}")
    print(f"\n  checks took {time.perf_counter() - t_all:.0f} s")

    if not args.quick:
        rows = calibration(exe, workdir, args.seeds, args.demos)
        print_calibration(rows, args.seeds, args.demos)

    if rep.failed:
        print(f"\nFAIL: {len(rep.failed)} check(s): " + ", ".join(rep.failed))
        return 1
    print("\nPASS: every check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
