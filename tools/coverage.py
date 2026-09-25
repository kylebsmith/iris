#!/usr/bin/env python3
"""Line and branch coverage of iris.h, as the union over several test programs.

sh build.sh cov builds every test program with clang's source-based coverage
(-fprofile-instr-generate -fcoverage-mapping), runs it, and merges its profile.
This script reads those profiles and reports how much of iris.h the test
programs, taken together, execute.

Why not simply `llvm-cov report bin1 -object bin2 ...`? Because iris.h's
functions are `static inline`: every program carries its own copy of each one,
and llvm-cov's summary counts a line or branch as covered only as well as the
single best copy covers it, not as the union of what all the copies covered. A
branch taken only in tests/coverage.c and another taken only in tests/audit.c
would count as one. So this script exports every program's regions and
branches as JSON and takes the union itself:

  lines     the union of the lines llvm-cov marks as executed (its lcov "DA"
            records), keeping only lines that belong to iris.h's own functions;
  branches  each branch outcome (true and false counted separately), keyed by
            the function, its source position and its occurrence at that
            position, so a branch inside a macro is told apart by where the
            macro was expanded.

sh build.sh cov also redefines IRIS_API as `static inline
__attribute__((used))` in every program, so every function is compiled into
every program and all of them report against the same lines. Without that, a
function a program never calls leaves a placeholder spanning its whole body,
comments included, and merged line counts go wrong.

Usage:
  python3 tools/coverage.py --llvm-cov LLVM_COV --source iris.h --dir build/cov \\
      --min-lines 98.0 --min-branches 88.5 PROGRAM...
reads DIR/bin/PROGRAM and DIR/prof/PROGRAM.profdata for each PROGRAM, prints the
totals, each program's own share, and every line no program executed, and exits
1 when lines or branch outcomes fall under their minimum.
"""
import argparse
import collections
import json
import os
import shlex
import subprocess
import sys


def export(llvm_cov, binary, profile, source, fmt):
    cmd = llvm_cov + ["export", "-format=" + fmt, binary, "-instr-profile", profile, source]
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def base(name):
    """A static function's profile name carries its file: keep the function."""
    return name.split(":", 1)[1] if ":" in name else name


def keyed_branches(fn):
    """Yield (key, true_count, false_count) for each branch region of fn."""
    regs = fn["regions"]
    expansion = {}                       # expanded file id -> (span, parent file id)
    for r in regs:
        if r[7] == 1:                    # an expansion region: a macro used here
            expansion[r[6]] = ((r[0], r[1], r[2], r[3]), r[5])

    def chain(file_id, span):
        c = [span]
        while file_id != 0 and file_id in expansion:
            s, file_id = expansion[file_id]
            c.append(s)
        return tuple(reversed(c)) if file_id == 0 else None

    seen = collections.Counter()
    for b in fn["branches"]:
        ch = chain(b[6], (b[0], b[1], b[2], b[3]))
        if ch is None:
            continue
        # inside a macro, key by where it was expanded, not where it is defined
        k = ch if b[6] == 0 else ch[:-1] + ((-1, -1, -1, -1),)
        seen[k] += 1
        yield (base(fn["name"]), k, seen[k]), ch[0][0], b[4], b[5]


def load(llvm_cov, directory, program, source):
    binary = os.path.join(directory, "bin", program)
    profile = os.path.join(directory, "prof", program + ".profdata")
    data = json.loads(export(llvm_cov, binary, profile, source, "text"))["data"][0]
    lcov = export(llvm_cov, binary, profile, source, "lcov")
    branches, allowed, where = {}, set(), {}
    for fn in data["functions"]:
        if not fn["filenames"][0].endswith(os.path.basename(source)):
            continue
        for r in fn["regions"]:
            if r[7] in (0, 1, 3) and fn["filenames"][r[5]].endswith(os.path.basename(source)):
                allowed.update(range(r[0], r[2] + 1))
        for key, line, t, f in keyed_branches(fn):
            a, b = branches.get(key, (0, 0))
            branches[key] = (a + t, b + f)
            where[key] = (line, base(fn["name"]))
    lines, infile = {}, False
    for ln in lcov.split("\n"):
        if ln.startswith("SF:"):
            infile = ln[3:].endswith(os.path.basename(source))
        elif ln.startswith("DA:") and infile:
            n, c = ln[3:].split(",")[:2]
            lines[int(n)] = lines.get(int(n), 0) + int(c)
    # A test file that expands one of iris.h's macros in its own code makes
    # llvm-cov map the #define line to iris.h. That is not library code.
    lines = {n: c for n, c in lines.items() if n in allowed}
    return lines, branches, where


def pct(a, b):
    return f"{a}/{b} = {100.0 * a / b:.2f}%" if b else "n/a"


def ranges(nums):
    out, start, prev = [], None, None
    for n in nums:
        if start is None:
            start = prev = n
        elif n == prev + 1:
            prev = n
        else:
            out.append(f"{start}" if start == prev else f"{start}-{prev}")
            start = prev = n
    if start is not None:
        out.append(f"{start}" if start == prev else f"{start}-{prev}")
    return ", ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--llvm-cov", required=True, help="llvm-cov, possibly as 'xcrun llvm-cov'")
    ap.add_argument("--source", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--min-lines", type=float, required=True)
    ap.add_argument("--min-branches", type=float, required=True)
    ap.add_argument("programs", nargs="+")
    a = ap.parse_args()
    llvm_cov = shlex.split(a.llvm_cov)
    source = os.path.abspath(a.source)
    text = open(source).read().split("\n")

    all_lines, hit_lines = set(), set()
    all_branch, hit_branch, where = set(), set(), {}
    own = {}
    for p in a.programs:
        lines, branches, w = load(llvm_cov, a.dir, p, source)
        where.update(w)
        all_lines |= set(lines)
        hl = {n for n, c in lines.items() if c > 0}
        hit_lines |= hl
        hb = set()
        for k, (t, f) in branches.items():
            all_branch |= {k + ("true",), k + ("false",)}
            if t > 0:
                hb.add(k + ("true",))
            if f > 0:
                hb.add(k + ("false",))
        hit_branch |= hb
        own[p] = (len(hl), len(lines), len(hb), 2 * len(branches))

    print("coverage of iris.h, the union over %d test programs" % len(a.programs))
    for p in a.programs:
        l, ln, b, bn = own[p]
        print(f"  {p:14s} lines {pct(l, ln):>22s}   branch outcomes {pct(b, bn):>22s}")
    lp = 100.0 * len(hit_lines) / len(all_lines) if all_lines else 0.0
    bp = 100.0 * len(hit_branch) / len(all_branch) if all_branch else 0.0
    print(f"  {'union':14s} lines {pct(len(hit_lines), len(all_lines)):>22s}   "
          f"branch outcomes {pct(len(hit_branch), len(all_branch)):>22s}")

    missed = sorted(all_lines - hit_lines)
    print(f"\nlines no test program executes ({len(missed)}): {ranges(missed) or 'none'}")
    for n in missed:
        print(f"  iris.h:{n:<5d} {text[n - 1].strip()[:90]}")
    by_fn = collections.Counter(where[k[:3]][1] for k in all_branch - hit_branch if k[:3] in where)
    print(f"\nbranch outcomes never taken ({len(all_branch - hit_branch)}), by function:")
    for fn, c in sorted(by_fn.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {c:4d}  {fn}")

    ok = lp >= a.min_lines and bp >= a.min_branches
    print(f"\n{'PASS' if ok else 'FAIL'}  lines {lp:.2f}% (at least {a.min_lines}%), "
          f"branch outcomes {bp:.2f}% (at least {a.min_branches}%)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
