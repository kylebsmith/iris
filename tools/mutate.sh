#!/bin/sh
# MUTATION TESTING — does every check actually protect what it claims to?
#
# The method: take a copy of the library, deliberately break ONE thing, run the
# suite, and see whether anything goes red. A mutation that survives is a hole:
# some property is unprotected, or the check that should protect it cannot fail.
#
# This project needed this. Three separate checks have been found unable to
# fail: an A/B test comparing two identical builds, a fixture generator that
# destroyed the fixture it protected, and a continuous-integration job whose
# grep matched FAIL as happily as PASS. Each was found by accident. This finds
# them on purpose.
#
#   sh tools/mutate.sh          run every mutation
#   sh tools/mutate.sh -v       show the suite output for survivors
set -u
VERBOSE=${1:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
KILLED=0; SURVIVED=0; SKIPPED=0
SURVIVORS=""

# Each mutation: a label, then a perl expression that breaks one thing.
# The suite MUST go red for every one of them.
run_mutation() {
  label=$1; expr=$2
  rm -rf "$WORK/m"; mkdir -p "$WORK/m"
  ( cd "$ROOT" && tar cf - --exclude=build --exclude=.git . ) | ( cd "$WORK/m" && tar xf - )
  if ! perl -pi -e "$expr" "$WORK/m/iris.h" 2>/dev/null; then
    printf "  SKIP    %-52s (could not apply)\n" "$label"; SKIPPED=$((SKIPPED+1)); return
  fi
  if cmp -s "$ROOT/iris.h" "$WORK/m/iris.h"; then
    printf "  SKIP    %-52s (pattern did not match)\n" "$label"; SKIPPED=$((SKIPPED+1)); return
  fi
  out=$( cd "$WORK/m" && sh build.sh audit 2>&1; sh build.sh regressions 2>&1; sh build.sh claims 2>&1 )
  if printf '%s' "$out" | grep -qE "FAIL|SOME CHECKS|failing|CLAIMS CHECK FAILED|error:"; then
    printf "  killed  %-52s\n" "$label"; KILLED=$((KILLED+1))
  else
    printf "  SURVIVED %-51s <-- nothing detected this\n" "$label"
    SURVIVED=$((SURVIVED+1)); SURVIVORS="$SURVIVORS\n    $label"
    [ "$VERBOSE" = "-v" ] && printf '%s\n' "$out" | tail -20
  fi
}

echo "MUTATION TESTING — breaking one thing at a time, on a copy"
echo

echo "-- the numerics the golden hashes exist to protect --"
run_mutation "activation: 27 -> 26 in the numerator"        's/x \* \(27\.0f \+ x2\)/x * (26.0f + x2)/'
run_mutation "activation: 9 -> 8 in the denominator"        's/27\.0f \+ 9\.0f \* x2/27.0f + 8.0f * x2/'
run_mutation "momentum default 0.85 -> 0.80"                's/k->momentum = 0\.85f/k->momentum = 0.80f/'
run_mutation "learning rate default 0.10 -> 0.11"           's/k->lr = 0\.10f/k->lr = 0.11f/'
run_mutation "output band: IRIS_OUT_HI 0.9 -> 0.85"          's/#define IRIS_OUT_HI 0\.9f/#define IRIS_OUT_HI 0.85f/'
run_mutation "backward slope 1-a^2 -> 1-a^2 scaled 0.99"    's/acc \* \(1\.0f - a \* a\)/acc * 0.99f * (1.0f - a * a)/'

echo
echo "-- the safety properties --"
run_mutation "drop the arena bound in iris_init"            's/if \(bytes < iris_internal_bytes\(n_in, n_hid, n_out, cap\)\) return 0;/;/'
run_mutation "drop the not-a-number door check"             's/if \(iris_isbad\(in\[i\]\)\)/if (0)/'
run_mutation "iris_isbad always says healthy"               's/return \(c\.u & 0x7F800000u\) == 0x7F800000u;/return 0;/'
run_mutation "stop filling the shuffle buffer at init"      's/for \(int i = 0; i < cap; \+\+i\) k->order\[i\] = i;/;/'
run_mutation "hidden-width floor 8 -> 1 in iris_init"       's/if \(n_hid < 8 \|\| n_hid > IRIS_MAX_HID\) return 0;/if (n_hid < 1 || n_hid > IRIS_MAX_HID) return 0;/'
run_mutation "remove the degenerate-range floor"            's/if \(k->in_hi\[i\]  - k->in_lo\[i\]  < w\)/if (0)/'

echo
echo "-- the file format --"
run_mutation "skip the checksum on save"                    's/\*tail = iris_crc32\(buf, need - sizeof\(uint32_t\)\);/*tail = 0u;/'
run_mutation "skip checksum verification on load"           's/if \(iris_crc32\(buf, bytes - sizeof\(uint32_t\)\) != stored\) return 0;/;/'
run_mutation "stop writing the smoothing word"              's/\*sm = k->l2;/*sm = 0.0f;/'

echo
echo "-- the reporting, and the two return conventions --"
run_mutation "iris_record refuses with -1 instead of 0"     's/{ k->status = IRIS_STORE_FULL; return 0; }/{ k->status = IRIS_STORE_FULL; return -1; }/'
run_mutation "iris_train reports success when it refused"   's/return iris_train_converge\(k, 0, 0, 0\) >= 0\.0f \? 1 : 0;/return 1;/'
run_mutation "iris_get_status always reports healthy"       's/return \(iris_status\)k->status;/return IRIS_STATUS_OK;/'
run_mutation "iris_index_of returns 0 for a null instrument" 's/IRIS_API int iris_index_of\(const iris \*k, int id\) \{ if \(!k\) return -1;/IRIS_API int iris_index_of(const iris *k, int id) { if (!k) return 0;/'

TOTAL=$((KILLED+SURVIVED))
echo
echo "=================================================================="
printf "  killed %d, survived %d, skipped %d\n" "$KILLED" "$SURVIVED" "$SKIPPED"
if [ "$TOTAL" -gt 0 ]; then
  printf "  mutation score: %d%%\n" $(( KILLED * 100 / TOTAL ))
fi
if [ "$SURVIVED" -gt 0 ]; then
  printf "\n  UNPROTECTED — nothing in the suite noticed these:%b\n" "$SURVIVORS"
  echo
  echo "  Each survivor is a property with no check behind it, or a check that"
  echo "  cannot fail. Both are the same defect from the user's side."
  exit 1
fi
if [ "$SKIPPED" -gt 0 ]; then
  echo
  echo "  STALE — $SKIPPED mutation(s) did not match the code and did not run."
  echo
  echo "  A skipped mutation is not a passing one. The pattern stopped matching"
  echo "  because the line it edits was changed, so the check silently became a"
  echo "  no-op while this script kept printing 100%. That is exactly the defect"
  echo "  class this harness exists to catch, so it fails here too: resync the"
  echo "  pattern in this file against the current code, then run it again."
  exit 1
fi
echo "  every mutation was detected."
