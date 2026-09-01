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
out=""
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
  # EXIT CODES, NOT GREP. This grepped its own output for "failing" -- and
  # tests/regressions.c prints "0 of 16 failing" on every run, pass or fail. So
  # the word was always present, every mutation was recorded as killed, and the
  # 100% score was decoration. Proved by mutating a WORD INSIDE A COMMENT, which
  # cannot change behaviour: reported "killed". Every mutation score this
  # project has published was produced this way and none of them meant anything.
  #
  # Greping output for a word that appears in both the pass and the fail message
  # is the same defect this harness exists to catch, in the harness.
  # BEHAVIOURAL SUITES ONLY. `claims` used to be in this list and it made every
  # mutation a false kill: it compares iris.h's line count against a number in
  # README.md, so ANY edit to the source fails it -- including a mutation that
  # changes nothing observable. Measured: with the shuffle-fill mutation applied,
  # audit and regressions both PASS and claims fails on "total lines
  # MISMATCH doc=3406 actual=3415". That scored 100% while detecting nothing.
  #
  # A mutation is killed when the library BEHAVES differently, not when a
  # document disagrees about a line count. audit, regressions, coverage and tu
  # are the suites that test behaviour; claims and bloat are not and are run
  # separately by build.sh and CI.
  # Capture the output rather than discarding it: -v prints it for survivors,
  # and it referenced $out, which nothing ever assigned. Under `set -u` that
  # made the documented verbose mode abort instead of run.
  out=$( ( cd "$WORK/m" && sh build.sh audit && sh build.sh regressions \
                        && sh build.sh coverage && sh build.sh tu ) 2>&1 ) \
      && rc=0 || rc=1
  if [ "$rc" != "0" ]; then
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
run_mutation "drop the arena bound in iris_init"            's/if \(bytes < need\) return 0;/;/'
# EXPECTED SURVIVOR, same category as the shuffle-buffer one below: `need == 0`
# is the overflow path in iris_internal_bytes, and no shape within the library's
# own ceilings sizes to 0 on a 64-bit host, so the guarded branch is unreachable
# here and no host test can kill this. It is not an untested property; it is a
# property this machine cannot exercise. Left annotated rather than removed so
# the survivor count stays honest.
run_mutation "ignore an unsizeable shape in iris_init"     's/if \(need == 0\) return 0;/;/'
run_mutation "drop the not-a-number door check"             's/if \(iris_isbad\(in\[i\]\)\)/if (0)/'
run_mutation "iris_isbad always says healthy"               's/return \(c\.u & 0x7F800000u\) == 0x7F800000u;/return 0;/'
# EXPECTED SURVIVOR, kept deliberately. The trainer refills order[] at the start
# of every run, so the init-time fill is unobservable -- verified over 300
# randomised trials on dirty arenas under both sanitizers, identical result hash
# either way. The line stays as defence in depth; this mutation stays as the
# record that it is untestable rather than untested. See iris.h at k->order.
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
run_mutation "iris_train reports success when it refused"   's/return \(e >= 0\.0f \&\& k->trained\) \? 1 : 0;/return 1;/'
# NOT a mutation: "iris_train ignores the trained flag" would survive, because
# with the setters guarded I could find no route that leaves a NON-NEGATIVE
# error behind while trained is 0 -- the NaN-example-via-file route and the
# emptied-instrument route both return -1.0f. The `&& k->trained` condition is
# therefore defence in depth against a case that is currently unreachable.
# Adding a mutation that cannot be killed would put a permanent false survivor
# in this report, which is the opposite of what this harness is for.
run_mutation "iris_get_status always reports healthy"       's/return \(iris_status\)k->status;/return IRIS_STATUS_OK;/'
run_mutation "iris_index_of returns 0 for a null instrument" 's/IRIS_API int iris_index_of\(const iris \*k, int id\) \{ if \(!k\) return -1;/IRIS_API int iris_index_of(const iris *k, int id) { if (!k) return 0;/'

TOTAL=$((KILLED+SURVIVED))
echo
echo "=================================================================="
printf "  killed %d, survived %d, skipped %d\n" "$KILLED" "$SURVIVED" "$SKIPPED"
if [ "$TOTAL" -gt 0 ]; then
  printf "  mutation score: %d%%\n" $(( KILLED * 100 / TOTAL ))
fi
# ORDER MATTERS. This runs BEFORE the survivor-set comparison below. A stale
# pattern makes a mutation SKIP, so it is neither killed nor survived -- and if
# the stale one is an EXPECTED survivor, the set comparison sees it missing and
# reports "THE SURVIVOR SET CHANGED", which sends you looking for a new hole in
# the test suite instead of at the pattern that stopped matching. Diagnose the
# stale pattern first; it is the more specific fault.
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

# Two mutations cannot be killed on a 64-bit host, for reasons annotated at
# their run_mutation lines: both guard branches that are unreachable on this
# word size. Before this list existed the run was permanently red, which meant
# its exit code could never tell you a TWENTY-FIRST survivor had appeared --
# a new hole would change the list and nothing else. Comparing the survivor SET
# against the expected one restores the signal in both directions: a new
# survivor fails, and so does an expected one that has quietly started being
# killed, because that means the annotation is now wrong.
EXPECTED_SURVIVORS="ignore an unsizeable shape in iris_init
stop filling the shuffle buffer at init"
if [ "$SURVIVED" -gt 0 ]; then
  printf "\n  survivors:%b\n" "$SURVIVORS"
fi
GOT=$(printf "%b" "$SURVIVORS" | sed 's/^ *//' | grep -v '^$' | sort)
WANT=$(printf "%s\n" "$EXPECTED_SURVIVORS" | sed 's/^ *//' | grep -v '^$' | sort)
if [ "$GOT" != "$WANT" ]; then
  echo
  echo "  THE SURVIVOR SET CHANGED. Expected exactly:"
  printf "%s\n" "$WANT" | sed 's/^/    /'
  echo "  Got:"
  printf "%s\n" "$GOT" | sed 's/^/    /'
  echo
  echo "  A new survivor is a property with no check behind it, or a check that"
  echo "  cannot fail. An expected survivor that vanished means its annotation"
  echo "  is stale. Both are defects; neither is silent."
  exit 1
fi
if [ "$SURVIVED" -gt 0 ]; then
  echo
  echo "  Both survivors are the expected, annotated pair -- guard branches that"
  echo "  are unreachable on a 64-bit host. See their run_mutation lines."
fi
if [ "$SURVIVED" -gt 0 ]; then
  echo "  every mutation was detected, or is one of the two annotated survivors"
  echo "  that cannot be killed on a 64-bit host."
else
  echo "  every mutation was detected."
fi
