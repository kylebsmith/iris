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
  float g[2]={.1f,.2f},s[3]={.3f,.4f,.5f};
  iris_record(k,g,s); iris_train_converge(k,0,0,0); iris_predict(k,g,s);
  iris_save(k,0,0); return 0; }
PROBE
cc -std=c99 -O2 -ffreestanding -fno-stack-protector -I. -c /tmp/_claim_probe.c -o /tmp/_claim_probe.o 2>/dev/null
U=$(nm -u /tmp/_claim_probe.o | tr -d ' ' | grep -v '^$' || true)
if [ -z "$U" ]; then say "zero external symbols" "ok"
else say "zero external symbols" "FAIL: $U"; fail=1; fi

# --- version stated exactly once ------------------------------------------
V=$(grep -oE 'IRIS_VERSION_STRING "[^"]+"' iris.h | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
N=$(grep -rl "IRIS_VERSION_STRING \"" --include='*.h' . | wc -l | tr -d ' ')
want "version declared exactly once"  "1" "$N"
say  "version" "$V"
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
