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
# CONTRIBUTING.md states the same number and was NOT policed, so it went stale at 40
# while README stayed right. Any document that states the count is now checked.
want "audit count in CONTRIBUTING.md" "$(grep -oE '# [0-9]+ correctness checks' CONTRIBUTING.md | grep -oE '[0-9]+')" "$CHECKS"

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
# Not only .md. docs/tiny.c printed "tiny.c is 123 lines of code. iris.h is 2835
# lines." for months -- neither number ever true, neither computed by anything --
# and this scan could not see it because it only ever read Markdown.
LIVING="README.md CONTRIBUTING.md docs/SYSTEM-technical.md docs/SYSTEM-plain-english.md docs/DESIGN.md docs/FREEZE.md docs/tiny.c tests/audit.c build.sh"
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
# Scope this to THIS repository's own tracked files. A bare recursive grep
# counts whatever happens to be under the working directory -- and in CI the
# starter kit is checked out into the workspace, carrying ten vendored copies
# of iris.h, so this read 11 and failed every job. `git ls-files` cannot see
# them because they belong to a different repository. The non-git fallback
# keeps this runnable from a tarball.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  N=$(git ls-files '*.h' | xargs grep -l "IRIS_VERSION_STRING \"" 2>/dev/null | wc -l | tr -d ' ')
else
  N=$(grep -rl "IRIS_VERSION_STRING \"" --include='*.h' . | wc -l | tr -d ' ')
fi
want "version declared exactly once"  "1" "$N"
say  "version" "$V"

# The masthead comment states the version too, in prose. It drifted to 0.3.0
# while the macro said 0.4.0 and nothing noticed, because "declared exactly
# once" only ever counted the macro. Compare them.
MAST=$(grep -oE '^   v[0-9]+\.[0-9]+\.[0-9]+' iris.h | head -1 | tr -d ' v')
want "masthead version matches the macro"  "$V" "$MAST"

# Four other files state the version and none of them were compared against the
# macro. Every one of them could be set to 9.9.9 and this script still exited 0
# -- which is the mechanism behind three separate stale numbers found in the
# release audit, not a hypothetical. The version is a fact about the release;
# every file that repeats it is a place it can rot.
want "library.properties version" "$V" \
     "$(grep -oE '^version=[0-9]+\.[0-9]+\.[0-9]+' library.properties | cut -d= -f2)"
want "CITATION.cff version"       "$V" \
     "$(grep -oE '^version: [0-9]+\.[0-9]+\.[0-9]+' CITATION.cff | awk '{print $2}')"
want "README states the version"  "$V" \
     "$(grep -oE 'Version [0-9]+\.[0-9]+\.[0-9]+' README.md | head -1 | awk '{print $2}')"
want "CHANGELOG heads this version" "$V" \
     "$(grep -oE '^## [0-9]+\.[0-9]+\.[0-9]+' CHANGELOG.md | head -1 | awk '{print $2}')"

# The release date is a claim too, and CITATION.cff is the file an archive reads
# to mint citation metadata -- so a slipped date there has a downstream consumer.
# want() compares two strings, so two EMPTY strings match. If both greps stop
# finding anything -- a heading reformat, say -- the check would pass while
# measuring nothing. Require a date shape first, then compare.
CHDATE=$(grep -oE '^## [0-9]+\.[0-9]+\.[0-9]+ — [0-9]{4}-[0-9]{2}-[0-9]{2}' CHANGELOG.md | head -1 | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' || true)
CFDATE=$(grep -oE '^date-released: "[0-9]{4}-[0-9]{2}-[0-9]{2}"' CITATION.cff | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' || true)
want "CHANGELOG states a release date" "yes" "$([ -n "$CHDATE" ] && echo yes || echo no)"
want "CITATION.cff states a release date" "yes" "$([ -n "$CFDATE" ] && echo yes || echo no)"
want "CITATION.cff date matches the CHANGELOG entry" \
     "$(grep -oE '^## [0-9]+\.[0-9]+\.[0-9]+ — [0-9]{4}-[0-9]{2}-[0-9]{2}' CHANGELOG.md | head -1 | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}')" \
     "$(grep -oE '^date-released: "[0-9]{4}-[0-9]{2}-[0-9]{2}"' CITATION.cff | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}')"

# --- sizes stated in prose, measured rather than trusted --------------------
# `struct iris` grew by 8 bytes before 0.1.0 and FOUR published figures did not
# follow it: two in SYSTEM-technical.md, one in SYSTEM-plain-english.md, and two
# in the starter kit -- one of them inside a quotation attributed to the audit,
# which the audit does not emit. CONTRIBUTING.md names this exact trap. Naming a
# trap is not a guard, so measure the numbers and compare them to the prose.
cat > /tmp/_iris_sizes.c <<'SZPROBE'
#define IRIS_IMPLEMENTATION
#include "iris.h"
#include <stdio.h>
int main(void){ printf("%zu %d\n", sizeof(iris), (int)IRIS_ARENA(2,12,3,256)); return 0; }
SZPROBE
if ${CC:-cc} -std=c99 -I. -o /tmp/_iris_sizes /tmp/_iris_sizes.c -lm 2>/dev/null; then
  SIZES=$(/tmp/_iris_sizes)
  STRUCT_B=$(echo "$SIZES" | awk '{print $1}')
  ARENA_B=$(echo "$SIZES" | awk '{print $2}')
  ARENA_C=$(printf "%s" "$ARENA_B" | sed 's/\([0-9]\)\([0-9]\{3\}\)$/\1,\2/')
  want "struct iris size in SYSTEM-technical.md" \
       "$(grep -oE '`struct iris` is [0-9]+ bytes' docs/SYSTEM-technical.md | grep -oE '[0-9]+')" "$STRUCT_B"
  want "arena figure in SYSTEM-technical.md" \
       "$(grep -oE 'IRIS_ARENA\(2,12,3,256\)` is [0-9,]+ bytes' docs/SYSTEM-technical.md | grep -oE '[0-9,]+ bytes' | grep -oE '[0-9,]+')" "$ARENA_C"
  want "arena figure quoted from the audit" \
       "$(grep -oE 'macro [0-9]+ B, needed [0-9]+ B, slack 0 B' docs/SYSTEM-technical.md | head -1)" "macro $ARENA_B B, needed $ARENA_B B, slack 0 B"
  want "arena figure in SYSTEM-plain-english.md" \
       "$(grep -oE '^[0-9,]+ bytes — about nine kilobytes' docs/SYSTEM-plain-english.md | grep -oE '[0-9,]+')" "$ARENA_C"
else
  say "size probe" "FAIL: probe did not build"; fail=1
fi

# Every document that states the audit's check count, not just two of them.
want "audit count in SYSTEM-technical.md" \
     "$(grep -oE 'The audit prints [0-9]+ `PASS` lines' docs/SYSTEM-technical.md | grep -oE '[0-9]+')" "$CHECKS"
want "audit count in build.sh usage" \
     "$(grep -oE 'run the correctness checks \([0-9]+\)' build.sh | grep -oE '[0-9]+')" "$CHECKS"
want "audit count in the pull-request checklist" \
     "$(grep -oE '`sh build.sh audit` — [0-9]+ checks' CONTRIBUTING.md | grep -oE '[0-9]+')" "$CHECKS"

# extras/README.md states the size of extras/ports/ in prose. It is exact today
# and nothing measured it, which is how every other stale number here began.
if [ -f extras/README.md ]; then
  PORTS_L=$(find extras/ports -type f -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
  PORTS_C=$(printf "%s" "$PORTS_L" | sed 's/\([0-9]\)\([0-9]\{3\}\)$/\1,\2/')
  want "extras/ports line count in extras/README.md" \
       "$(grep -oE '[0-9,]+ lines' extras/README.md | head -1 | grep -oE '[0-9,]+')" "$PORTS_C"
fi

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
