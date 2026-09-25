# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Kyle Smith
"""iris's model, written a second time, in 64-bit floating point.

This module is a model of the instrument built from the DESCRIPTION of the
model -- the masthead and part comments of iris.h and a written specification
of the model -- and not from iris.h's code. It computes in
NumPy's binary64 (the 64-bit floating-point format, with 53 significant bits),
where iris computes in binary32 (24 significant bits), so the two agree only
up to the rounding binary32 adds, and run.py measures that agreement against
bounds derived from it.

Three kinds of number appear here, and the difference matters:

  * The model's CONSTANTS -- the learning rate 0.10, the momentum 0.85, the
    output band 0.1 to 0.9, the plateau tolerance 0.10 -- are taken at their
    binary32 values, because those are the numbers iris is defined by: 0.1 in
    binary32 is 0.100000001490116..., 1.5e-8 of itself from the decimal.
    Measured, taking the decimals instead moves the comparison by under 4%
    (README.md), so this is about defining the model correctly, not about
    the tolerances.
  * The FACTS iris states exactly -- the random-number generator, the
    starting weights it draws, the shuffle, the fitted ranges -- are
    recomputed in binary32 (NumPy float32, whose every operation rounds
    correctly, as a binary32 processor does) and compared bit for bit.
  * Everything the network then computes is binary64.

The places the description left a choice open, and how each was settled by
running the library rather than by reading its code, are listed in README.md
under "What the description left open".
"""

import numpy as np

# The unit roundoff of binary32 with rounding to nearest: every correctly
# rounded binary32 operation has a relative error of at most U.
U = 2.0 ** -24


def f32(x):
    """The binary32 value nearest x, as a Python float (exact in binary64)."""
    return float(np.float32(x))


# ---------------------------------------------------------------------------
# The model's constants, at the values iris holds them.
# ---------------------------------------------------------------------------
LEARNING_RATE = f32(0.10)   # every instrument trains with these two
MOMENTUM = f32(0.85)
OUT_LO = f32(0.1)           # targets are scaled into [0.1, 0.9]
OUT_HI = f32(0.9)
W_LIMIT = 16.0              # a weight past this means the run diverged
TINY = f32(1e-30)           # a velocity smaller than this is set to zero
CONV_WINDOW = 2000          # epochs between plateau tests
CONV_TOL = f32(0.10)        # stop when a window bought 10% or less
ERROR_FLOOR = f32(1e-6)     # stop when the epoch's mean squared error is below
KNN_GUARD = f32(1e-9)       # added to every squared distance before 1/d^2
KNN_MAXK = 8                # at most this many neighbours
STILL_REL = f32(1e-5)       # an input is still when its width is at most
STILL_ABS = f32(1e-6)       # 1e-5 of its magnitude, or at most 1e-6


def weight_decay(smoothing):
    """Smoothing s in [0, 1] is stored as the weight-decay strength 0.3 s,
    rounded to binary32, as iris stores it."""
    s = np.float32(min(max(smoothing, 0.0), 1.0))
    return float(np.float32(s * np.float32(0.3)))


def floats(hexstr):
    """Decode export.c's array encoding: 8 hexadecimal digits per binary32
    bit pattern, most significant first. Returns binary64 values, exactly."""
    return np.frombuffer(bytes.fromhex(hexstr), dtype=">f4").astype(np.float64)


def float32s(hexstr):
    """The same decoding, kept in binary32 for bit-for-bit comparisons."""
    return np.frombuffer(bytes.fromhex(hexstr), dtype=">f4").astype(np.float32)


# ---------------------------------------------------------------------------
# The facts iris states exactly, recomputed in binary32.
# ---------------------------------------------------------------------------
MASK32 = 0xFFFFFFFF


def xorshift32(state):
    """One step of the generator: xorshift with shifts 13, 17 and 5. A zero
    state would stay zero for ever, so the description replaces a zero
    result with 0x9E3779B9; it cannot arise from a nonzero state."""
    x = state
    x ^= (x << 13) & MASK32
    x ^= x >> 17
    x ^= (x << 5) & MASK32
    return x if x else 0x9E3779B9


def rand_sym32(u):
    """A draw uniform on [-1, 1): the 32 bits read as a signed integer,
    converted to binary32 (one rounding), times 2^-31 (exact)."""
    signed = u - (1 << 32) if u >= (1 << 31) else u
    return np.float32(np.float32(signed) * np.float32(2.0 ** -31))


def reseed32(seed, n_in, n_hid, n_out, form="reciprocal"):
    """The starting weights a seed draws, in binary32.

    Seed 0 is taken as 1. The generator starts at the seed; the first
    n_hid*n_in draws, scaled by 1/sqrt(n_in), are w1 in storage order; the
    biases are zero; the next n_out*n_hid draws, scaled by 1/sqrt(n_hid),
    are w2. "Divided by the square root" admits two binary32 evaluations,
    draw / sqrt(n) and draw * (1 / sqrt(n)), which round differently;
    `form` selects one, and run.py reports which of the two the library
    matches (the reciprocal, on every recipe it runs).

    Returns (flat weight vector w1|b1|w2|b2 as float32, generator state)."""
    s = seed if seed else 1
    root_in = np.sqrt(np.float32(n_in))
    root_hid = np.sqrt(np.float32(n_hid))
    inv_in = np.float32(np.float32(1.0) / root_in)
    inv_hid = np.float32(np.float32(1.0) / root_hid)

    def draw(n, root, inv):
        nonlocal s
        out = np.empty(n, dtype=np.float32)
        for i in range(n):
            s = xorshift32(s)
            r = rand_sym32(s)
            out[i] = np.float32(r / root) if form == "quotient" else np.float32(r * inv)
        return out

    w1 = draw(n_hid * n_in, root_in, inv_in)
    w2 = draw(n_out * n_hid, root_hid, inv_hid)
    flat = np.concatenate([w1, np.zeros(n_hid, np.float32), w2, np.zeros(n_out, np.float32)])
    return flat, s


def shuffle(order, state):
    """One epoch's Fisher-Yates shuffle of `order`, in place: for i from the
    last position down to 1, swap position i with position (draw mod (i+1)).
    Returns the new generator state."""
    for i in range(len(order) - 1, 0, -1):
        state = xorshift32(state)
        j = state % (i + 1)
        order[i], order[j] = order[j], order[i]
    return state


class Ranges:
    """The per-channel ranges an instrument measures from its demonstrations.
    in_lo/in_hi/out_lo/out_hi are binary64 arrays holding binary32 values."""

    def __init__(self, in_lo, in_hi, out_lo, out_hi):
        self.in_lo = np.asarray(in_lo, dtype=np.float64)
        self.in_hi = np.asarray(in_hi, dtype=np.float64)
        self.out_lo = np.asarray(out_lo, dtype=np.float64)
        self.out_hi = np.asarray(out_hi, dtype=np.float64)

    @property
    def in_width(self):
        return self.in_hi - self.in_lo

    @property
    def out_width(self):
        return self.out_hi - self.out_lo

    @property
    def still(self):
        """Inputs stored with zero width, which the model ignores."""
        return self.in_hi <= self.in_lo


def fit_ranges32(demos32, n_in):
    """The documented range rule, evaluated in binary32 because the ranges
    are stored in binary32 and the still-input test is a comparison.

    Inputs: lo and hi are the smallest and largest demonstrated values. An
    input is still when hi - lo is at most max(1e-5 * max(|lo|, |hi|), 1e-6);
    a still input is stored with hi = lo.
    Outputs: lo and hi likewise, and an output whose width is below
    max(1e-5 * |lo|, 1e-6) is given exactly that width."""
    f = np.float32
    X = demos32[:, :n_in]
    Y = demos32[:, n_in:]
    in_lo, in_hi = X.min(axis=0), X.max(axis=0)
    width = (in_hi - in_lo).astype(f)
    mag = np.maximum(np.abs(in_lo), np.abs(in_hi))
    negligible = np.maximum((mag * f(STILL_REL)).astype(f), f(STILL_ABS))
    in_hi = np.where(width <= negligible, in_lo, in_hi)
    out_lo, out_hi = Y.min(axis=0), Y.max(axis=0)
    floor = np.maximum((np.abs(out_lo) * f(STILL_REL)).astype(f), f(STILL_ABS))
    narrow = (out_hi - out_lo).astype(f) < floor
    out_hi = np.where(narrow, (out_lo + floor).astype(f), out_hi)
    return Ranges(in_lo, in_hi, out_lo, out_hi)


# ---------------------------------------------------------------------------
# The model, in binary64.
# ---------------------------------------------------------------------------
def rational(s):
    """The hidden nonlinearity: p(s) = s(27 + s^2) / (27 + 9 s^2), held to
    [-1, 1]. p(3) = 1 exactly and p rises monotonically to it, so holding the
    result to [-1, 1] is the same function as holding the argument to
    [-3, 3]. Beyond |s| = 1e9 the description returns +-1 directly; holding
    the argument to +-1e9 first gives the same value and keeps s^2 finite."""
    s = np.clip(s, -1e9, 1e9)
    s2 = s * s
    return np.clip(s * (27.0 + s2) / (27.0 + 9.0 * s2), -1.0, 1.0)


def squash(z):
    """The output nonlinearity, built from the hidden one by the identity
    logistic(z) = (1 + tanh(z / 2)) / 2."""
    return 0.5 * (rational(0.5 * z) + 1.0)


def normalise_inputs(V, rg):
    """Each input to [-1, +1] across its range; a still input to 0."""
    V = np.asarray(V, dtype=np.float64)
    w = rg.in_width
    moving = w > 0
    safe = np.where(moving, w, 1.0)
    return np.where(moving, 2.0 * (V - rg.in_lo) / safe - 1.0, 0.0)


def normalise_targets(Y, rg):
    """Each output from its range into the band [0.1, 0.9]."""
    return OUT_LO + (np.asarray(Y, dtype=np.float64) - rg.out_lo) / rg.out_width * (OUT_HI - OUT_LO)


def denormalise(y, rg):
    """The band back to each output's range, then held inside that range."""
    out = rg.out_lo + (y - OUT_LO) / (OUT_HI - OUT_LO) * rg.out_width
    return np.clip(out, rg.out_lo, rg.out_hi)


class Shape:
    """The widths, and where w1, b1, w2 and b2 sit in the flat vector export.c
    prints them in."""

    def __init__(self, n_in, n_hid, n_out):
        self.n_in, self.n_hid, self.n_out = n_in, n_hid, n_out
        self.sizes = [n_hid * n_in, n_hid, n_out * n_hid, n_out]
        self.n_params = sum(self.sizes)
        edges = np.cumsum([0] + self.sizes)
        self.slices = [slice(edges[j], edges[j + 1]) for j in range(4)]


class Net:
    """One hidden layer: hidden = p(W1 x + b1), output = squash(W2 hidden + b2),
    with a momentum velocity for every weight and bias. W1[h, i] is iris's
    w1[h*n_in + i] and W2[o, h] its w2[o*n_hid + h]."""

    def __init__(self, shape, w, v=None):
        self.shape = shape
        H, I, O = shape.n_hid, shape.n_in, shape.n_out
        w = np.asarray(w, dtype=np.float64)
        v = np.zeros(shape.n_params) if v is None else np.asarray(v, dtype=np.float64)
        sl = shape.slices
        self.W1 = w[sl[0]].reshape(H, I).copy()
        self.b1 = w[sl[1]].copy()
        self.W2 = w[sl[2]].reshape(O, H).copy()
        self.b2 = w[sl[3]].copy()
        self.V1 = v[sl[0]].reshape(H, I).copy()
        self.c1 = v[sl[1]].copy()
        self.V2 = v[sl[2]].reshape(O, H).copy()
        self.c2 = v[sl[3]].copy()

    def weights(self):
        return np.concatenate([self.W1.ravel(), self.b1, self.W2.ravel(), self.b2])

    def forward(self, X):
        """Network outputs in the [0, 1] band for normalised inputs X (rows)."""
        a = rational(X @ self.W1.T + self.b1)
        return squash(a @ self.W2.T + self.b2)


def flush(v):
    """A velocity smaller than 1e-30 in magnitude becomes exactly zero."""
    v[np.abs(v) < TINY] = 0.0


def train_epoch(net, X, T, order, lr, momentum, wd):
    """One epoch of per-example gradient descent with classical momentum.

    For each demonstration, in `order`:
      forward pass; output error signal (y - t) * y * (1 - y), the derivative
      of the true logistic (the description calls it a surrogate gradient:
      the forward pass uses the rational function, the backward pass the
      logistic's and tanh's own derivatives); hidden error signal from the
      output weights BEFORE this example's update, times (1 - a^2); then the
      output layer's update, then the hidden layer's:
          v = flush(momentum * v - lr * signal * input);  w += v;  w -= wd * w
          b's velocity likewise, without decay:           b += v_b
    Returns the epoch's error: the mean over demonstrations and outputs of
    (y - t)^2, accumulated while the weights change (an online error)."""
    total = 0.0
    W1, b1, W2, b2 = net.W1, net.b1, net.W2, net.b2
    V1, c1, V2, c2 = net.V1, net.c1, net.V2, net.c2
    for r in order:
        x, t = X[r], T[r]
        a = rational(W1 @ x + b1)
        y = squash(W2 @ a + b2)
        e = y - t
        total += float(e @ e)
        g = e * y * (1.0 - y)
        gh = (g @ W2) * (1.0 - a * a)
        V2 *= momentum
        V2 -= np.outer(lr * g, a)
        flush(V2)
        W2 += V2
        W2 -= wd * W2
        c2 *= momentum
        c2 -= lr * g
        flush(c2)
        b2 += c2
        V1 *= momentum
        V1 -= np.outer(lr * gh, x)
        flush(V1)
        W1 += V1
        W1 -= wd * W1
        c1 *= momentum
        c1 -= lr * gh
        flush(c1)
        b1 += c1
    return total / (len(order) * net.shape.n_out)


def divergence_guard(net):
    """The once-an-epoch weight check: any weight or bias past +-16 is set to
    exactly +-16 and the run ends. Returns True if it fired."""
    fired = False
    for arr in (net.W1, net.b1, net.W2, net.b2):
        over = np.abs(arr) > W_LIMIT
        if over.any():
            arr[over] = np.sign(arr[over]) * W_LIMIT
            fired = True
    return fired


def replay_epochs_batched(shape, w, v, orders, X, T, lr, momentum, wd):
    """Many independent single epochs at once (layer 1): row e of w and v is
    a starting state, row e of orders the permutation that epoch uses.
    Returns (weights, velocities, errors) after one epoch from each start."""
    E = w.shape[0]
    H, I, O = shape.n_hid, shape.n_in, shape.n_out
    sl = shape.slices
    W1 = w[:, sl[0]].reshape(E, H, I).copy()
    b1 = w[:, sl[1]].copy()
    W2 = w[:, sl[2]].reshape(E, O, H).copy()
    b2 = w[:, sl[3]].copy()
    V1 = v[:, sl[0]].reshape(E, H, I).copy()
    c1 = v[:, sl[1]].copy()
    V2 = v[:, sl[2]].reshape(E, O, H).copy()
    c2 = v[:, sl[3]].copy()
    total = np.zeros(E)
    for step in range(orders.shape[1]):
        idx = orders[:, step]
        x, t = X[idx], T[idx]
        a = rational(np.einsum("ehi,ei->eh", W1, x) + b1)
        y = squash(np.einsum("eoh,eh->eo", W2, a) + b2)
        e = y - t
        total += np.einsum("eo,eo->e", e, e)
        g = e * y * (1.0 - y)
        gh = np.einsum("eoh,eo->eh", W2, g) * (1.0 - a * a)
        V2 = momentum * V2 - lr * g[:, :, None] * a[:, None, :]
        flush(V2)
        W2 += V2
        W2 -= wd * W2
        c2 = momentum * c2 - lr * g
        flush(c2)
        b2 += c2
        V1 = momentum * V1 - lr * gh[:, :, None] * x[:, None, :]
        flush(V1)
        W1 += V1
        W1 -= wd * W1
        c1 = momentum * c1 - lr * gh
        flush(c1)
        b1 += c1
    wout = np.concatenate([W1.reshape(E, -1), b1, W2.reshape(E, -1), b2], axis=1)
    vout = np.concatenate([V1.reshape(E, -1), c1, V2.reshape(E, -1), c2], axis=1)
    return wout, vout, total / (orders.shape[1] * O)


class StoppingRule:
    """The documented stopping rules, tested after every epoch in this order:
    the divergence guard; the error floor (mean squared error below 1e-6);
    then, on a plateau-seeking run, every 2,000th epoch of the session, the
    plateau test -- stop if a reference error exists and the window bought
    no more than 10% of it, (reference - error) <= 0.10 * reference,
    otherwise the error becomes the new reference; and the ceiling.

    `margin` after a test says how far the quantity tested was from its
    threshold, as a fraction of the threshold's scale, so run.py can tell a
    decision that was close from one that was not."""

    def __init__(self, conv, ceiling):
        self.conv, self.ceiling = conv, ceiling
        self.done = 0
        self.ref = 0.0
        self.last = None     # (epoch, rule, fired, margin) of the latest test

    def after_epoch(self, err, diverged):
        self.done += 1
        if diverged:
            self.last = (self.done, "diverged", True, np.inf)
            return "diverged"
        floor_margin = (err - ERROR_FLOOR) / ERROR_FLOOR
        if err < ERROR_FLOOR:
            self.last = (self.done, "floor", True, floor_margin)
            return "floor"
        self.last = (self.done, "floor", False, floor_margin)
        if self.conv and self.done % CONV_WINDOW == 0:
            if self.ref > 0.0:
                margin = ((self.ref - err) - CONV_TOL * self.ref) / self.ref
                fired = (self.ref - err) <= CONV_TOL * self.ref
                self.last = (self.done, "plateau", fired, margin)
                if fired:
                    return "plateau"
            self.ref = err
        if self.done >= self.ceiling:
            return "ceiling"
        return None


def recall_errors(shape, W, X, T):
    """The training error of each set of weights in W (one row per set, in
    the order export.c prints them): the mean over demonstrations and
    outputs of (y - t)^2, with y the network's output for normalised inputs X
    and t the normalised targets T. This is the documented iris_last_error,
    one forward pass with fixed weights, where train_epoch's error is added
    up while the weights move."""
    W = np.asarray(W, dtype=np.float64)
    H, I, O = shape.n_hid, shape.n_in, shape.n_out
    sl = shape.slices
    W1 = W[:, sl[0]].reshape(-1, H, I)
    W2 = W[:, sl[2]].reshape(-1, O, H)
    a = rational(np.einsum("ehi,ni->enh", W1, X) + W[:, None, sl[1]])
    y = squash(np.einsum("eoh,enh->eno", W2, a) + W[:, None, sl[3]])
    return ((y - T[None, :, :]) ** 2).mean(axis=(1, 2))


def predict(net, rg, V):
    """What the instrument plays for raw readings V (rows)."""
    return denormalise(net.forward(normalise_inputs(V, rg)), rg)


# ---------------------------------------------------------------------------
# The neighbour samplers, as documented.
# ---------------------------------------------------------------------------
def neighbour_scale(rg):
    """One multiplier per input, 1 / width, so a distance counts each input in
    fractions of its range; a still input gets 0 and counts not at all."""
    w = rg.in_width
    return np.where(w > 0, 1.0 / np.where(w > 0, w, 1.0), 0.0)


def neighbour_distances2(demos, n_in, rg, Q):
    """Squared scaled distances, queries by demonstrations."""
    s = neighbour_scale(rg)
    D = (demos[None, :, :n_in] - np.asarray(Q)[:, None, :]) * s
    return np.einsum("qni,qni->qn", D, D)


def neighbour_distances2_32(demos32, n_in, rg, Q32):
    """The same squared distances as binary32 computes them: the reciprocal of
    each width rounded once, then each input's difference times it, squared,
    and added input by input, every step rounded. Only the ranking uses these.
    The documented tie rule (the earliest recorded wins) applies to the
    distances iris compares, and two demonstrations at the same exact
    distance can be a rounding apart in binary32."""
    f = np.float32
    w = (rg.in_hi.astype(f) - rg.in_lo.astype(f)).astype(f)
    inv = np.where(w > 0, f(1.0) / np.where(w > 0, w, f(1.0)), f(0.0)).astype(f)
    t = ((demos32[None, :, :n_in] - Q32[:, None, :]).astype(f) * inv).astype(f)
    d2 = np.zeros(t.shape[:2], dtype=f)
    for i in range(n_in):
        d2 = (d2 + t[:, :, i] * t[:, :, i]).astype(f)
    return d2


def knn_predict(demos, n_in, rg, Q, k, rank_by=None):
    """k nearest demonstrations (at most 8 and at most the count; ties to the
    earliest recorded), each weighted 1 / (d^2 + 1e-9) with d^2 its squared
    scaled distance, blended as the weighted mean of their outputs. `rank_by`,
    when given, holds the distances to choose the neighbours by (the binary32
    ones, to break ties as iris does); the weights always use binary64."""
    n = demos.shape[0]
    k = max(1, min(k, n, KNN_MAXK))
    d2 = neighbour_distances2(demos, n_in, rg, Q)
    idx = np.argsort(d2 if rank_by is None else rank_by, axis=1, kind="stable")[:, :k]
    dk = np.take_along_axis(d2, idx, axis=1)
    w = 1.0 / (dk + KNN_GUARD)
    Y = demos[:, n_in:][idx]                       # (queries, k, n_out)
    return np.einsum("qk,qko->qo", w, Y) / w.sum(axis=1)[:, None], idx


# ---------------------------------------------------------------------------
# Rounding bounds: how far binary32 can land from binary64.
# ---------------------------------------------------------------------------
def gamma(n):
    """Higham's gamma_n = n u / (1 - n u): the relative error bound of a
    sequential sum of n rounded products (Accuracy and Stability of Numerical
    Algorithms, section 3.1)."""
    return n * U / (1.0 - n * U)


def forward_bound(net, rg, V):
    """A first-order bound on |iris_predict - binary64 forward pass| for each
    probe and output, given the same binary32 weights and ranges. Each line
    follows one stage of the playing chain:

      input      x = 2 (v - lo) / (hi - lo) - 1, three roundings in t and one
                 in the subtraction:          |dx| <= 4u (|x| + 1)
      hidden     s = b1 + sum w1 x, summed in order:
                 |ds| <= gamma(n_in + 1) (|b1| + sum |w1||x|) + sum |w1||dx|
      p(s)       six roundings, each relative to |p| <= 1, add at most 7u, and
                 p's slope is at most 1:      |da| <= |ds| + 7u
      output     z = b2 + sum w2 a:
                 |dz| <= gamma(n_hid + 1) (|b2| + sum |w2||a|) + sum |w2||da|
      squash     0.5 (p(z/2) + 1): slope at most 1/4, 3.5u from p, u from
                 the addition:                |dy| <= |dz| / 4 + 4.5u
      range      t = (y - 0.1) / (0.9 - 0.1), three roundings;
                 out = lo + t (hi - lo), three more:
                 |dout| <= width (|dy| / 0.8 + 5u max(|t|, 1)) + 2u max(|lo|, |hi|)
      clamp      holds the result in [lo, hi] and cannot increase a difference.
    """
    s1 = net.shape
    x = normalise_inputs(V, rg)
    moving = rg.in_width > 0
    dx = np.where(moving, 4.0 * U * (np.abs(x) + 1.0), 0.0)
    aW1, aW2 = np.abs(net.W1), np.abs(net.W2)
    s = x @ net.W1.T + net.b1
    ds = gamma(s1.n_in + 1) * (np.abs(net.b1) + np.abs(x) @ aW1.T) + dx @ aW1.T
    a = rational(s)
    da = ds + 7.0 * U
    z = a @ net.W2.T + net.b2
    dz = gamma(s1.n_hid + 1) * (np.abs(net.b2) + np.abs(a) @ aW2.T) + da @ aW2.T
    y = squash(z)
    dy = 0.25 * dz + 4.5 * U
    t = (y - OUT_LO) / (OUT_HI - OUT_LO)
    edge = np.maximum(np.abs(rg.out_lo), np.abs(rg.out_hi))
    return rg.out_width * (dy / (OUT_HI - OUT_LO) + 5.0 * U * np.maximum(np.abs(t), 1.0)) \
        + 2.0 * U * edge


def roundings_per_visit(shape):
    """How many binary32 roundings one demonstration visit performs, counted
    stage by stage from the description: scaling each input (4) and target
    (4); each hidden unit's sum (2 per input) and p (6); each output's sum
    (2 per hidden unit), squash (8) and error signal (5); each hidden unit's
    error signal (2 per output, and 3); and every weight's update (7: two
    products and a difference for the velocity, the addition, the decay's
    product and difference, and the gradient's product with the input) and
    every bias's (4)."""
    I, H, O = shape.n_in, shape.n_hid, shape.n_out
    return (4 * I + 4 * O + H * (2 * I + 6) + O * (2 * H + 8) + 5 * O
            + H * (2 * O + 3) + 7 * (H * I + O * H) + 4 * (H + O))
