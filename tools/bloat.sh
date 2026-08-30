#!/bin/sh
# A ratchet against accretion.
#
# Four audits in a row were rewarded for finding defects, and the honest
# consequence is that every round added a guard, a comment and a special case
# while nothing was ever removed. The suites all stayed green, because green is
# what they measure. This measures the other thing.
#
# It compares four numbers against tools/BLOAT-BASELINE and FAILS if any got
# worse. Improving a number is not enough on its own -- you must re-record it,
# which is what makes it a ratchet rather than a report:
#
#   sh tools/bloat.sh                 check
#   IRIS_REBASELINE=yes sh tools/bloat.sh   record the current numbers
#
# Re-baselining upward is allowed but never silent: it prints the regression and
# the reason must go in the commit message. That is the whole enforcement model
# -- a mechanism you have to lie in public to defeat.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE="$ROOT/tools/BLOAT-BASELINE"
H="$ROOT/iris.h"

# --- 1. comment lines per line of code ------------------------------------
CODE=$(awk '/^[[:space:]]*$/{next}
            blk==1 { if (/\*\//) blk=0; next }
            /^[[:space:]]*\/\*/ { if (!/\*\//) blk=1; next }
            {c++} END{print c+0}' "$H")
TOTAL=$(wc -l < "$H" | tr -d ' ')
BLANK=$(grep -c '^[[:space:]]*$' "$H" || true)
COMMENT=$((TOTAL - CODE - BLANK))
RATIO=$(awk -v c="$COMMENT" -v k="$CODE" 'BEGIN{printf "%.3f", c/k}')

# --- 2. how much prose sits in blocks of 25+ lines -------------------------
# Long blocks are where archaeology hides. Short ones next to code are teaching.
LONGP=$(awk '
  { s=$0; sub(/^[[:space:]]+/,"",s)
    isc = (blk==1) || (s ~ /^\/\*/) || (s ~ /^\*/) || (s ~ /^\/\//)
    if (blk==1 && s ~ /\*\//) blk=0
    else if (s ~ /^\/\*/ && s !~ /\*\//) blk=1
    if (isc) run++
    else { if (run>=25) tot+=run; run=0 } }
  END { if (run>=25) tot+=run; print tot+0 }' "$H")

# --- 3. public functions nothing outside the header ever calls -------------
# A function with no caller is either undocumented or should not exist.
# Take the name immediately before the '(' -- not any iris_* token on the line,
# which caught the RETURN TYPE of `IRIS_API iris_status iris_get_status(...)`
# and reported the type as an uncalled function. Internal helpers are excluded
# by name: they are not public surface and are not expected to have outside
# callers.
API=$(grep -oE "IRIS_API [a-z_0-9 ]*\**iris_[a-z_0-9]+\(" "$H" \
      | sed "s/.*[^a-z_0-9]\(iris_[a-z_0-9]*\)($/\1/" \
      | grep -v '^iris_internal_' | sort -u)
SEARCH=""
for d in "$ROOT/tests" "$ROOT/examples" "$ROOT/extras" "$ROOT/docs" \
         "$ROOT/../iris-esp32-starter"; do
  [ -d "$d" ] && SEARCH="$SEARCH $d"
done
ORPHAN=0
ORPHANS=""
if [ -n "$SEARCH" ]; then
  # grep -r over the directories, NOT `find | xargs grep`. xargs batches, grep
  # exits non-zero on any batch with no match, and the first version therefore
  # reported iris_sigmoid -- which has four real callers -- as uncalled.
  for f in $API; do
    n=$(grep -rlw --include='*.c' --include='*.h' --include='*.ino' \
          "$f" $SEARCH 2>/dev/null | grep -vc '/iris\.h$' || true)
    [ "${n:-0}" = "0" ] && { ORPHAN=$((ORPHAN+1)); ORPHANS="$ORPHANS $f"; }
  done
fi

# --- 4. total size ---------------------------------------------------------
if [ "${IRIS_REBASELINE:-}" = "yes" ]; then
  printf 'ratio %s\nlongprose %s\norphans %s\ntotal %s\n' \
         "$RATIO" "$LONGP" "$ORPHAN" "$TOTAL" > "$BASE"
  echo "recorded: ratio $RATIO, long prose $LONGP lines, $ORPHAN orphans, $TOTAL total"
  exit 0
fi
[ -f "$BASE" ] || { echo "no baseline; run: IRIS_REBASELINE=yes sh tools/bloat.sh"; exit 1; }

get() { awk -v k="$1" '$1==k{print $2}' "$BASE"; }
BR=$(get ratio); BL=$(get longprose); BO=$(get orphans); BT=$(get total)
fail=0
row() { # name current baseline  (numeric, lower is better)
  if awk -v a="$2" -v b="$3" 'BEGIN{exit !(a>b)}'; then
    printf "  WORSE   %-22s %s  (was %s)\n" "$1" "$2" "$3"; fail=1
  elif awk -v a="$2" -v b="$3" 'BEGIN{exit !(a<b)}'; then
    printf "  better  %-22s %s  (was %s)\n" "$1" "$2" "$3"
  else
    printf "  same    %-22s %s\n" "$1" "$2"
  fi
}
echo
echo "  accretion check -- lower is better on every line"
echo
row "comment:code ratio"   "$RATIO"  "$BR"
row "lines in 25+ blocks"  "$LONGP"  "$BL"
row "uncalled public fns"  "$ORPHAN" "$BO"
row "total lines"          "$TOTAL"  "$BT"
echo
[ "$ORPHAN" -gt 0 ] && echo "  uncalled:$ORPHANS" && echo
if [ "$fail" = "1" ]; then
  echo "  A number went up. That is allowed, but not silently:"
  echo "  delete something, or re-record with IRIS_REBASELINE=yes and say why"
  echo "  in the commit message."
  exit 1
fi
echo "  no accretion since the last baseline."
