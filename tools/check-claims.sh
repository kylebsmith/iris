#!/bin/sh
# check-claims.sh — verify that what the documentation SAYS matches what the
# code IS. Every number this project publishes has, at some point, been wrong:
# a line count stale within four hours, an audit-check count off by five, a
# scaling factor wrong by 8x, an accuracy bound wrong by 28x. Chasing those one
# at a time does not work. This does.
#
# Run it before every commit and in CI. Exit 1 means a document lies.
set -e
cd "$(dirname "$0")/.."
fail=0
say()  { printf '%-46s %s\n' "$1" "$2"; }
want() { # want <label> <expected> <actual>
  if [ "$2" = "$3" ]; then say "$1" "ok ($3)"
  else say "$1" "MISMATCH doc=$2 actual=$3"; fail=1; fi
}

# --- the numbers the docs state -------------------------------------------
LINES=$(wc -l < iris.h | tr -d ' ')
CODE=$(awk '
  /^[[:space:]]*$/ {next}
  blk==1 { if (/\*\//) blk=0; next }
  /^[[:space:]]*\/\*/ { if (!/\*\//) blk=1; next }
  {c++} END{print c}' iris.h)
CHECKS=$(sh build.sh audit 2>/dev/null | grep -c '^PASS' || true)

want "iris.h total lines"      "$(grep -oE '[0-9,]+ lines' README.md | head -1 | tr -d ' lines,')" "$LINES"
want "iris.h code lines"       "$(grep -oE '[0-9,]+ of them code' README.md | head -1 | grep -oE '^[0-9,]+' | tr -d ,)" "$CODE"
want "audit check count"       "$(grep -oE '# [0-9]+ correctness checks' README.md | grep -oE '[0-9]+')" "$CHECKS"

# --- the dependency claim, actually tested --------------------------------
cat > /tmp/_claim_probe.c <<'PROBE'
#include "iris.h"
static unsigned char m[IRIS_ARENA(2,12,3,64)];
int probe(void){ iris *k=iris_init(m,sizeof m,2,12,3,64,1234);
  /* No array initialisers: GNU compilers lower those to memcpy/memset calls,
     which would show up as undefined symbols belonging to this probe rather
     than to the library, and make this check fail for the wrong reason. */
  static float g[2], s[3];
  g[0]=.1f; g[1]=.2f; s[0]=.3f; s[1]=.4f; s[2]=.5f;
  iris_record(k,g,s); iris_train_converge(k,0,0,0); iris_predict(k,g,s);
  iris_save(k,0,0); return 0; }
PROBE
# Test EVERY compiler available, not just the default one. This check used to
# run `cc` only, and on the GNU compiler it fails: gcc assumes a square root
# sets errno on a negative input, so it emits the hardware instruction plus a
# call to sqrtf for that case. The ESP32 toolchain is a GNU compiler, so the
# claim was false on the library's own target. -fno-math-errno removes it.
# The no-dependency claim needs a few flags, none of which changes an output
# bit. Applied per compiler, because clang rejects the GNU-only ones -- and a
# rejected flag used to make the whole check silently SKIP that compiler,
# which is how "zero external symbols" passed while being false on the GNU
# compiler the ESP32 toolchain is built from.
BASE="-std=c99 -O2 -ffreestanding -fno-stack-protector"
for CCX in cc gcc-15 gcc clang; do
  command -v "$CCX" >/dev/null 2>&1 || continue
  if $CCX --version 2>&1 | grep -qi clang; then EXTRA=""
  else EXTRA="-fno-math-errno -fno-tree-loop-distribute-patterns"; fi
  if ! $CCX $BASE $EXTRA -I. -c /tmp/_claim_probe.c -o /tmp/_claim_probe.o 2>/dev/null; then
    say "zero external symbols ($CCX)" "FAIL: the probe did not build"; fail=1; continue
  fi
  U=$(nm -u /tmp/_claim_probe.o | tr -d ' ' | grep -v '^$' || true)
  if [ -z "$U" ]; then say "zero external symbols ($CCX)" "ok"
  else say "zero external symbols ($CCX)" "FAIL: $U"; fail=1; fi
done

# --- no LIVING document may restate a figure nothing checks -----------------
# The header's line count was stated in six documents. Five were stale, one by
# 750 lines. The fix was not to check five more places -- it was to stop saying
# it in five more places. README.md holds the number and the check above
# compares it against reality; everywhere else either does not mention it or is
# marked a historical record, which is a dated statement about the past and
# therefore cannot go stale.
#
# This rule keeps it that way: a living document that states a line count is a
# second copy waiting to drift.
LIVING="README.md CONTRIBUTING.md docs/SYSTEM-technical.md docs/SYSTEM-plain-english.md docs/DESIGN.md docs/FREEZE.md"
DUPES=0
for f in $LIVING; do
  [ -f "$f" ] || continue
  [ "$f" = "README.md" ] && continue          # the one place it belongs
  # Only OUR header's count. The first attempt at this matched any line count
  # anywhere and flagged a sentence about Wekinator's FastDTW being 6,662 lines
  # -- a check that fires on the wrong thing is no better than one that cannot
  # fire. Require the figure to be on a line that is talking about this header.
  if grep -qiE '(iris\.h|single header|one header|this header)[^.]*[0-9],?[0-9]{3} lines|[0-9],?[0-9]{3} lines[^.]*(iris\.h|single header|one header|this header)' "$f"; then
    say "line count restated in $f" "FAIL: remove it or mark the document historical"
    DUPES=$((DUPES+1)); fail=1
  fi
done
want "line count stated in exactly one living document"  "0" "$DUPES"

# --- version stated exactly once ------------------------------------------
V=$(grep -oE 'IRIS_VERSION_STRING "[^"]+"' iris.h | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
N=$(grep -rl "IRIS_VERSION_STRING \"" --include='*.h' . | wc -l | tr -d ' ')
want "version declared exactly once"  "1" "$N"
say  "version" "$V"

# The masthead comment states the version too, in prose. It drifted to 0.3.0
# while the macro said 0.4.0 and nothing noticed, because "declared exactly
# once" only ever counted the macro. Compare them.
MAST=$(grep -oE '^   v[0-9]+\.[0-9]+\.[0-9]+' iris.h | head -1 | tr -d ' v')
want "masthead version matches the macro"  "$V" "$MAST"

# The library was renamed from embwek. Include guards kept the old spelling for
# a while after; this stops that coming back.
LEFT=$(grep -rl "EMBWEK" --include='*.h' . | grep -v '^./docs/' | wc -l | tr -d ' ')
want "no EMBWEK identifiers in headers"  "0" "$LEFT"
if ! grep -q "^## $V" CHANGELOG.md; then say "CHANGELOG has an entry for $V" "MISSING"; fail=1
else say "CHANGELOG has an entry for $V" "ok"; fi

# --- forbidden phrasings ---------------------------------------------------
# These are claims prior audits established we cannot make. Grepping for them
# is cheaper than re-litigating them.
for bad in \
  "bit-identical to Weka" \
  "verified 441/441 against Weka" \
  "faster than Wekinator" \
  "better than Wekinator" \
  "first embedded" \
  "the only library"
do
  H=$(grep -rin "$bad" --include='*.md' --include='*.h' --include='*.c' . 2>/dev/null | grep -v 'check-claims' | grep -vi 'never\|forbidden\|not\b\|cannot' || true)
  if [ -n "$H" ]; then say "forbidden: \"$bad\"" "FOUND"; echo "$H" | head -2; fail=1
  else say "forbidden: \"$bad\"" "absent"; fi
done

echo
if [ "$fail" -eq 0 ]; then echo "CLAIMS CHECK PASSED — the docs match the code."
else echo "CLAIMS CHECK FAILED — a document states something untrue."; fi
exit $fail
