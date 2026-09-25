#!/usr/bin/env python3
"""tools/mutate.py -- how many small bugs planted in iris.h do the tests notice?

Mutation testing plants one small deliberate bug (a mutant) in the code at a
time and runs the tests against it. A mutant the tests fail on is killed; one
they pass is a survivor, and every survivor names a change to the library that
no test would notice. This is a report, never a gate: sh build.sh mutate always
exits 0 unless this program itself breaks, and nothing in continuous
integration fails on a survivor.

THE MUTANTS. Every site of five standard operator classes inside the bodies of
iris.h's functions is enumerated:
  relational   <  <=  >  >=  ==  !=  replaced by another of the six
  arithmetic   + - * / (and += -= *= /=) replaced by another of the four
  constant     an integer literal plus or minus 1; a float literal halved or
               doubled (0.0 becomes 1.0)
  deletion     an expression statement that assigns (=, op=, ++, --) removed
  negation     the condition of an if or a while negated
Preprocessor lines (the bodies of macros), code compiled only with
-DIRIS_NO_GUARDS, comments and declarations outside functions are left alone.
Sites are sampled with a fixed seed, in proportion to each PART of the header
with at least --floor from each, so every part of the library is represented
and the same seed always plans the same mutants.

A mutant whose object code equals the original's (built from a translation
unit that forces every function out) cannot change behaviour; it is counted
as equivalent and not run. Each other mutant is copied into its own directory
under build/mutate/ and run against the fast arms of build.sh (--arms), in
order, stopping at the first that fails, each with a time limit (--timeout).

THE BASELINE. --baseline names a file of known survivors, one per line as
`function | class | original | mutated`, which --write-baseline rewrites from
this run. The report then lists survivors that are new, and baseline entries
this run killed or did not sample.

  python3 tools/mutate.py                       100 mutants, seed 20260924
  python3 tools/mutate.py --sample 400 --seed 7 --jobs 8
  python3 tools/mutate.py --list                plan only: print the sites
"""
import argparse
import collections
import concurrent.futures as futures
import hashlib
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TYPES = {"float", "int", "double", "char", "void", "unsigned", "signed", "long", "short", "const",
         "volatile", "static", "inline", "struct", "union", "enum", "uint32_t", "int32_t", "uint8_t",
         "uint16_t", "int16_t", "size_t", "uintptr_t", "iris", "iris_internal_rng", "iris_status",
         "iris_progress_fn", "register"}
KEYWORDS = {"return", "sizeof", "case", "if", "else", "for", "while", "do", "switch", "break",
            "continue", "goto", "default"}
OPS = ["<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
       "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|="]
REL = ["<", "<=", ">", ">=", "==", "!="]
ARI = ["+", "-", "*", "/"]
CAS = ["+=", "-=", "*=", "/="]
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "<<=", ">>="}
NUM_RE = re.compile(r"0[xX][0-9a-fA-F]+[uUlL]*|(\d+\.\d*|\.\d+)([eE][+-]?\d+)?[fFlL]?"
                    r"|\d+[eE][+-]?\d+[fFlL]?|\d+[uUlL]*")
DEFAULT_ARMS = "audit coverage tiny recipes playing elm load portability tu regressions train"


# ---------------------------------------------------------------- the sites
def tokenize(s):
    toks, i, n, line, bol = [], 0, len(s), 1, True
    while i < n:
        c = s[i]
        if c == "\n":
            toks.append(("ws", c, i, i + 1, line)); line += 1; i += 1; bol = True; continue
        if c in " \t\r\f\v":
            toks.append(("ws", c, i, i + 1, line)); i += 1; continue
        if c == "#" and bol:                  # a preprocessor line, continuations included
            j = i
            while j < n:
                if s[j] == "\n" and s[j - 1] != "\\":
                    break
                if s.startswith("/*", j):
                    j = s.find("*/", j + 2) + 2; continue
                j += 1
            toks.append(("pp", s[i:j], i, j, line)); line += s.count("\n", i, j); i = j; continue
        bol = False
        if s.startswith("/*", i):
            j = s.find("*/", i + 2) + 2
            toks.append(("comment", s[i:j], i, j, line)); line += s.count("\n", i, j); i = j; continue
        if s.startswith("//", i):
            j = s.find("\n", i); j = n if j < 0 else j
            toks.append(("comment", s[i:j], i, j, line)); i = j; continue
        if c in "\"'":
            j = i + 1
            while s[j] != c:
                j += 2 if s[j] == "\\" else 1
            toks.append(("str", s[i:j + 1], i, j + 1, line)); i = j + 1; continue
        if c.isdigit() or (c == "." and i + 1 < n and s[i + 1].isdigit()):
            m = NUM_RE.match(s, i)
            toks.append(("num", m.group(0), i, m.end(), line)); i = m.end(); continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (s[j].isalnum() or s[j] == "_"):
                j += 1
            toks.append(("id", s[i:j], i, j, line)); i = j; continue
        for o in OPS:
            if s.startswith(o, i):
                toks.append(("op", o, i, i + len(o), line)); i += len(o); break
        else:
            toks.append(("op", c, i, i + 1, line)); i += 1
    return toks


def parts(lines):
    marks = [(0, "top")]
    for i, l in enumerate(lines, 1):
        m = re.match(r"\s+PART (\S+) —", l)
        if m:
            marks.append((i, "PART " + m.group(1)))
    return marks


def part_of(line, marks):
    p = marks[0][1]
    for ln, name in marks:
        if line >= ln:
            p = name
    return p


def sites(src):
    sig, stack = [], []
    for t in tokenize(src):
        if t[0] == "pp":
            d = t[1].lstrip("#").strip()
            if re.match(r"ifndef\s+IRIS_NO_GUARDS\b", d): stack.append("guard")
            elif re.match(r"ifdef\s+IRIS_NO_GUARDS\b", d): stack.append("ng")
            elif d.startswith("if"): stack.append("other")
            elif d.startswith("else"):
                if stack: stack[-1] = {"guard": "ng", "ng": "guard"}.get(stack[-1], stack[-1])
            elif d.startswith("endif"):
                if stack: stack.pop()
            continue
        if t[0] in ("ws", "comment"):
            continue
        sig.append(dict(kind=t[0], text=t[1], a=t[2], b=t[3], line=t[4], ng=("ng" in stack)))
    depth, fname, infn = 0, None, False
    for idx, t in enumerate(sig):              # function bodies: '{' at depth 0 after ')'
        if t["text"] == "{":
            if depth == 0 and idx > 0 and sig[idx - 1]["text"] == ")":
                k, p = idx - 1, 0
                while k >= 0:
                    if sig[k]["text"] == ")": p += 1
                    elif sig[k]["text"] == "(":
                        p -= 1
                        if p == 0: break
                    k -= 1
                fname, infn = sig[k - 1]["text"], True
            depth += 1
        t["fn"] = fname if (infn and depth > 0) else None
        if t["text"] == "}":
            depth -= 1
            if depth == 0: infn = False
    body = [t for t in sig if t["fn"] and not t["ng"]]
    out = []
    for j, t in enumerate(body):
        prev = body[j - 1] if j else None
        nxt = body[j + 1] if j + 1 < len(body) else None
        if t["kind"] == "op" and t["text"] in REL:
            out.append(dict(cls="relational", a=t["a"], b=t["b"], orig=t["text"], line=t["line"], fn=t["fn"]))
        elif t["kind"] == "op" and t["text"] in ARI and prev is not None:
            pk, pt = prev["kind"], prev["text"]
            binary = pk == "num" or pt in (")", "]") or (pk == "id" and pt not in TYPES and pt not in KEYWORDS)
            if t["text"] == "*" and nxt is not None and nxt["text"] in (")", ","):
                binary = False
            if binary:
                out.append(dict(cls="arithmetic", a=t["a"], b=t["b"], orig=t["text"], line=t["line"], fn=t["fn"]))
        elif t["kind"] == "op" and t["text"] in CAS:
            out.append(dict(cls="arithmetic", a=t["a"], b=t["b"], orig=t["text"], line=t["line"], fn=t["fn"]))
        elif t["kind"] == "num":
            out.append(dict(cls="constant", a=t["a"], b=t["b"], orig=t["text"], line=t["line"], fn=t["fn"]))
        elif t["kind"] == "id" and t["text"] in ("if", "while") and nxt is not None and nxt["text"] == "(":
            k, p = j + 1, 0
            while True:
                if body[k]["text"] == "(": p += 1
                elif body[k]["text"] == ")":
                    p -= 1
                    if p == 0: break
                k += 1
            out.append(dict(cls="negation", a=body[j + 2]["a"], b=body[k - 1]["b"],
                            orig=src[body[j + 2]["a"]:body[k - 1]["b"]], line=t["line"], fn=t["fn"]))
    p, start = 0, None                          # deletion: assigning statements
    for j, t in enumerate(body):
        tx = t["text"]
        if tx == "(":
            p += 1
        elif tx == ")":
            p -= 1
        if p == 0 and tx in (";", "{", "}"):
            if tx == ";" and start is not None:
                st, k = body[start:j], 0
                while k < len(st):              # step past else / if (...) / for (...) / while (...)
                    if st[k]["text"] == "else": k += 1; continue
                    if st[k]["text"] in ("if", "for", "while") and k + 1 < len(st) and st[k + 1]["text"] == "(":
                        q, pp = k + 1, 0
                        while q < len(st):
                            if st[q]["text"] == "(": pp += 1
                            elif st[q]["text"] == ")":
                                pp -= 1
                                if pp == 0: break
                            q += 1
                        k = q + 1; continue
                    break
                st = st[k:]
                if st and st[0]["text"] not in TYPES and st[0]["text"] not in KEYWORDS:
                    q, assigns = 0, False
                    for u in st:
                        if u["text"] == "(": q += 1
                        elif u["text"] == ")": q -= 1
                        elif q == 0 and (u["text"] in ASSIGN or u["text"] in ("++", "--")): assigns = True
                    if assigns and st[0]["fn"] == st[-1]["fn"]:
                        out.append(dict(cls="deletion", a=st[0]["a"], b=st[-1]["b"],
                                        orig=src[st[0]["a"]:st[-1]["b"]], line=st[0]["line"], fn=st[0]["fn"]))
            start = j + 1
        elif start is None:
            start = j
    return out


def variant(site, rng):
    o, c = site["orig"], site["cls"]
    if c == "relational":
        return rng.choice([x for x in REL if x != o])
    if c == "arithmetic":
        return rng.choice([x for x in (CAS if o in CAS else ARI) if x != o])
    if c == "negation":
        return "!(" + o + ")"
    if c == "deletion":
        return "(void)0"
    m = re.match(r"(0[xX][0-9a-fA-F]+|[\d.]+(?:[eE][+-]?\d+)?)([uUlLfF]*)$", o)
    body, suf = m.group(1), m.group(2)
    if body.lower().startswith("0x"):
        v = int(body, 16) + rng.choice([1, -1])
        return (hex(v) if v >= 0 else "-" + hex(-v)) + suf
    if "." in body or "e" in body.lower() or "f" in suf.lower():
        v = float(body)
        r = repr(1.0 if v == 0.0 else v * rng.choice([0.5, 2.0]))
        if "e" not in r and "." not in r:
            r += ".0"
        return r + suf
    v = int(body) + rng.choice([1, -1])
    return (str(v) if v >= 0 else "(" + str(v) + ")") + suf


def plan(src, n, seed, floor):
    marks = parts(src.split("\n"))
    every = sites(src)
    for s in every:
        s["part"] = part_of(s["line"], marks)
    by = collections.defaultdict(list)
    for s in every:
        by[s["part"]].append(s)
    rng, chosen = random.Random(seed), []
    order = [m[1] for m in marks]
    for p in sorted(by, key=order.index):
        pool = sorted(by[p], key=lambda s: (s["a"], s["cls"]))
        take = min(len(pool), max(floor, round(n * len(pool) / len(every))))
        for s in sorted(rng.sample(pool, take), key=lambda s: s["a"]):
            s["new"] = variant(s, rng)
            chosen.append(s)
    for i, s in enumerate(chosen):
        s["id"] = i
    return every, by, chosen


def apply(src, m):
    """Replace the site, keeping the line count so reports keep their lines."""
    old = src[m["a"]:m["b"]]
    return src[:m["a"]] + m["new"] + "\n" * (old.count("\n") - m["new"].count("\n")) + src[m["b"]:]


# ---------------------------------------------------------------- running
def run(cmd, cwd, timeout, env=None):
    p = subprocess.Popen(cmd, cwd=cwd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True, env=env)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out.decode(errors="replace")
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.communicate()
        return "timeout", ""


def object_hash(d, cc):
    with open(os.path.join(d, "mutate_tce.c"), "w") as f:
        f.write('#define IRIS_API static inline __attribute__((used))\n#include "iris.h"\n')
    rc, _ = run(f"{cc} -std=c99 -O2 -c mutate_tce.c -o mutate_tce.o", d, 120)
    if rc != 0:
        return None
    with open(os.path.join(d, "mutate_tce.o"), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def tracked_files():
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True, check=True).stdout
        files = out.split()
        extra = subprocess.run(["git", "-C", ROOT, "ls-files", "--others", "--exclude-standard"],
                               capture_output=True, text=True, check=True).stdout.split()
        return [f for f in files + extra if os.path.isfile(os.path.join(ROOT, f))]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for base, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in (".git", "build")]
            files += [os.path.relpath(os.path.join(base, n), ROOT) for n in names]
        return files


def copy_tree(dest):
    for f in tracked_files():
        d = os.path.join(dest, f)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, f), d)


def one(m, work, src, base_hash, arms, timeout, cc, keep):
    d = os.path.join(work, "m%04d" % m["id"])
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(os.path.join(work, "template"), d, symlinks=True)
    with open(os.path.join(d, "iris.h"), "w") as f:
        f.write(apply(src, m))
    t0 = time.time()
    h = object_hash(d, cc)
    if h is None:
        verdict, by = "compile-error", ""
    elif h == base_hash:
        verdict, by = "equivalent", ""
    else:
        verdict, by = "survived", ""
        env = dict(os.environ, CC=cc)
        for arm in arms:
            rc, _ = run(f"sh build.sh {arm}", d, timeout, env)
            if rc == "timeout":
                verdict, by = "timeout", arm
                break
            if rc != 0:
                verdict, by = "killed", arm
                break
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    return dict(m, verdict=verdict, by=by, secs=round(time.time() - t0, 1))


def key(m):
    return f"{m['fn']} | {m['cls']} | {' '.join(m['orig'].split())} | {m['new']}"


def main():
    ap = argparse.ArgumentParser(description="seeded operator mutation of iris.h (advisory)")
    ap.add_argument("--sample", type=int, default=100, help="how many mutants to run (100)")
    ap.add_argument("--seed", type=int, default=20260924, help="the sampling seed (20260924)")
    ap.add_argument("--floor", type=int, default=3, help="at least this many from each PART (3)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="mutants run at once")
    ap.add_argument("--timeout", type=int, default=60, help="seconds allowed each arm (60)")
    ap.add_argument("--arms", default=DEFAULT_ARMS, help="build.sh arms run against each mutant")
    ap.add_argument("--baseline", default=os.path.join(ROOT, "tools", "mutation-baseline.txt"))
    ap.add_argument("--write-baseline", action="store_true", help="rewrite the baseline from this run")
    ap.add_argument("--list", action="store_true", help="print the planned mutants and stop")
    ap.add_argument("--keep", action="store_true", help="keep each mutant's directory")
    a = ap.parse_args()
    cc = os.environ.get("CC", "cc")
    src = open(os.path.join(ROOT, "iris.h")).read()
    every, by, chosen = plan(src, a.sample, a.seed, a.floor)
    print(f"{len(every)} mutation sites in iris.h; {len(chosen)} sampled with seed {a.seed}")
    for p, ss in by.items():
        c = collections.Counter(s["cls"] for s in ss)
        print(f"  {p:10s} {len(ss):5d} sites, {sum(1 for m in chosen if m['part'] == p):3d} sampled  {dict(c)}")
    if a.list:
        for m in chosen:
            print(f"  m{m['id']:04d} {m['part']:9s} iris.h:{m['line']:<5d} {m['fn']:30s} {m['cls']:10s} "
                  f"{' '.join(m['orig'].split())[:40]!r} -> {m['new'][:30]!r}")
        return 0

    work = os.path.join(ROOT, "build", "mutate")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    copy_tree(os.path.join(work, "template"))
    base_hash = object_hash(os.path.join(work, "template"), cc)
    if base_hash is None:
        print(f"FAIL  the unmutated iris.h does not compile with {cc}")
        return 1
    arms = a.arms.split()
    env = dict(os.environ, CC=cc)
    for arm in arms:                            # the unmutated library must pass
        rc, out = run(f"sh build.sh {arm}", os.path.join(work, "template"), a.timeout, env)
        if rc != 0:
            print(out[-2000:])
            print(f"FAIL  the unmutated library fails build.sh {arm}; nothing can be measured")
            return 1
    print(f"running each mutant against: {' '.join(arms)} (stopping at the first failure)")
    t0, results = time.time(), []
    with futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        todo = [ex.submit(one, m, work, src, base_hash, arms, a.timeout, cc, a.keep) for m in chosen]
        for i, f in enumerate(futures.as_completed(todo), 1):
            r = f.result()
            results.append(r)
            print(f"  [{i:3d}/{len(chosen)} {time.time() - t0:5.0f}s] m{r['id']:04d} {r['part']:9s} "
                  f"iris.h:{r['line']:<5d} {r['cls']:10s} {r['verdict']}{' by ' + r['by'] if r['by'] else ''}",
                  flush=True)
    results.sort(key=lambda r: r["id"])
    v = collections.Counter(r["verdict"] for r in results)
    k, s = v["killed"], v["survived"]
    print(f"\nkilled {k}, survived {s}, timeout {v['timeout']}, compile-error {v['compile-error']}, "
          f"equivalent {v['equivalent']}")
    print(f"killed / (killed + survived) = {k}/{k + s} = {100.0 * k / max(1, k + s):.1f}% "
          f"(a sample of {len(chosen)} of {len(every)} sites)")
    per = collections.defaultdict(collections.Counter)
    for r in results:
        per[r["part"]][r["verdict"]] += 1
    print("\nper PART        killed survived timeout")
    for p in by:
        c = per[p]
        print(f"  {p:12s} {c['killed']:6d} {c['survived']:8d} {c['timeout']:7d}")
    survivors = [r for r in results if r["verdict"] == "survived"]
    print(f"\nSURVIVORS ({len(survivors)}): a change to iris.h that no arm above notices")
    for r in survivors:
        print(f"  iris.h:{r['line']:<5d} {key(r)}")
    base = set()
    if os.path.exists(a.baseline):
        base = {l.strip() for l in open(a.baseline) if l.strip() and not l.startswith("#")}
    now = {key(r) for r in survivors}
    sampled = {key(r) for r in results}
    new = sorted(now - base)
    gone = sorted(b for b in base if b in sampled and b not in now)
    print(f"\nagainst the baseline {os.path.relpath(a.baseline, ROOT)} ({len(base)} known survivors):")
    print(f"  new survivors: {len(new)}")
    for n in new:
        print(f"    {n}")
    print(f"  baseline survivors this run killed: {len(gone)}")
    for g in gone:
        print(f"    {g}")
    if a.write_baseline:
        with open(a.baseline, "w") as f:
            f.write("# Known survivors of tools/mutate.py, one per line: function | class | original | mutated.\n")
            f.write(f"# Written with --sample {a.sample} --seed {a.seed}.\n")
            for n in sorted(now):
                f.write(n + "\n")
        print(f"  wrote {len(now)} survivors to {os.path.relpath(a.baseline, ROOT)}")
    print("\nThis is a report; survivors never fail the run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
