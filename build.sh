#!/bin/sh
# build.sh -- every check iris has, one arm per test program.
#
#   sh build.sh              the same as sh build.sh test
#   sh build.sh test         every arm in the first two groups below, one
#                            after another (fuzz, sanitize and threads only
#                            where the compiler can link their sanitizer);
#                            it stops at the first arm that fails and names it
#   sh build.sh <arm> [...]  one arm
#
# CC chooses the compiler for every arm (CC=gcc-15 sh build.sh), cc when it
# is unset. Everything is built into build/, which nothing tracks.
#
# Terms, once. AddressSanitizer and UndefinedBehaviorSanitizer are compiler
# modes that stop a program at the first out-of-bounds access or undefined
# operation; ThreadSanitizer stops it at the first data race between
# threads. libFuzzer is clang's coverage-guided fuzzing engine: it mutates
# inputs to reach code no input has reached yet. Contraction is a compiler
# fusing a*b+c into one fused multiply-add instruction, which rounds once
# instead of twice and so changes the bits. GNU C (-std=gnu99) is C with the
# GNU compilers' extensions, the dialect Arduino builds use, and the one in
# which gcc contracts by default; ISO C (-std=c99) is the language as the
# International Organization for Standardization defines it. x87 is the 32-bit x86 floating-point unit,
# which computes in 80 bits and so cannot give iris's bits.
#
# THE TEST PROGRAMS, each its own arm, each built with -std=c99 -Wall -Wextra
#   audit        tests/audit.c, the pinned golden hash and exact checks; then
#                tests/guards_ab.c built with and without -DIRIS_NO_GUARDS:
#                every healthy line must match and every control line differ
#   regressions  tests/regressions.c, one test per reviewed defect
#   coverage     tests/coverage.c, the refusal paths (cov measures coverage)
#   load         tests/load.c, the save format attacked as a parser
#   train        tests/train.c, the trainers and their refusals
#   elm          tests/elm.c, the closed-form trainer
#   playing      tests/playing.c, prediction and the neighbour samplers
#   portability  tests/portability.c, the in-header square root and the rest
#   recipes      tests/starter_recipes.c, the starter kit's pinned hashes
#   tu           tests/tu/, two translation units with different maxima,
#                under AddressSanitizer where the compiler has it
#   fuzz [N]     tests/fuzz.c, N random call sequences (400) under
#                AddressSanitizer and UndefinedBehaviorSanitizer
#   examples     every examples/*.c, built with -Werror and run; each must
#                exit 0 and print no not-a-number or infinity
#   tiny         docs/tiny.c, iris_train written again by hand, which must
#                agree with iris.h to the bit
#   mpe, sinks   extras/tests/, every byte the output ports in extras/ emit
#                (polyphonic expression; control change and Open Sound
#                Control), built with -Werror
#
# THE WHOLE SUITE UNDER A TOOL
#   sanitize     every test program above under AddressSanitizer and
#                UndefinedBehaviorSanitizer, with float-divide-by-zero and
#                float-cast-overflow, stopping at the first report
#   threads      tests/threads.c and audit check 36 under ThreadSanitizer;
#                the same-instrument positive control must be reported
#   noheap       tests/noheap.c under the allocator interposer
#                tests/noheap_interpose.c: zero allocator calls from iris,
#                every control counted, and a refusal to run without it
#   freestanding tests/freestanding.sh, no C library needed on any target
#   targets      tests/targets.sh, builds for every processor iris is for,
#                and refuses 32-bit x86 with x87 arithmetic
#   pragma       tests/pragma_leak.sh, contraction off for iris's code only
#   determinism  the golden hash and the starter hashes at -O0 -O1 -O2 -O3
#                -Os, with contraction off, on and at the compiler's default,
#                in ISO C and, where the compiler is gcc, GNU C; then a
#                positive control: without its contraction pragmas iris.h
#                must lose the golden hash, wherever this processor can fuse
#
# NEEDS MORE THAN A C COMPILER, SO NOT IN test
#   fuzz-load [S]  tests/fuzz_load.c under libFuzzer for S seconds (60);
#                  needs a clang with libFuzzer
#   cov            line and branch coverage of iris.h over every test
#                  program, per source position (tools/coverage.py); fails
#                  under its thresholds; needs clang and its llvm-cov
#   mutate [...]   tools/mutate.py, seeded operator mutants of iris.h run
#                  against the fast arms; a report, never a gate; needs Python
#   reference      tests/reference/, iris against a second implementation
#                  of its model in 64-bit floating point, written in Python;
#                  needs NumPy, and skips with a message when it is missing
#                  (a failure when IRIS_REQUIRE_REFERENCE is set)
#   bench          the browser prototype, build/bench.html
#   clean          remove build/
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"
CC_GIVEN=${CC:+yes}
CC=${CC:-cc}
STD=-std=c99
WARN="-Wall -Wextra"
CFLAGS="$STD -O2 $WARN -I."
SAN="-fsanitize=address,undefined,float-divide-by-zero,float-cast-overflow -fno-sanitize-recover=all"
mkdir -p build

ARM=${1:-test}
if [ $# -gt 0 ]; then shift; fi
trap 'st=$?; [ "$st" = 0 ] || echo "FAIL  sh build.sh $ARM (exit status $st)"' EXIT

say()  { printf '%s\n' "$*"; }
fail() { say "FAIL  $*"; exit 1; }
# can_link <flags...>: does $CC build and link a trivial program with them?
can_link() {
  printf 'int main(void) { return 0; }\n' > build/probe.c
  "$CC" "$@" -o build/probe build/probe.c > /dev/null 2>&1
}
need_asan() {
  if can_link -fsanitize=address,undefined; then return 0; fi
  fail "$1 needs AddressSanitizer and UndefinedBehaviorSanitizer, which $CC cannot link (Homebrew gcc on macOS ships neither; use cc or clang there)"
}
# the functions a user calls: every IRIS_API definition not named iris_internal_
public_functions() {
  sed -n 's/^IRIS_API[^(]*[ *]\(iris_[a-z0-9_]*\) *(.*/\1/p' iris.h | grep -v '^iris_internal_' | sort -u
}
is_clang() { "$1" --version 2>/dev/null | grep -qi clang; }

# ---------------------------------------------------------------- the arms
arm_audit() {
  "$CC" $CFLAGS -pthread -o build/audit tests/audit.c -lm
  ./build/audit
  "$CC" $CFLAGS -o build/guards_ab tests/guards_ab.c -lm
  "$CC" $CFLAGS -DIRIS_NO_GUARDS -o build/guards_ab_ng tests/guards_ab.c -lm
  ./build/guards_ab > build/guards_ab.out
  ./build/guards_ab_ng > build/guards_ab_ng.out
  grep -v '^control' build/guards_ab.out > build/guards_ab.healthy
  grep -v '^control' build/guards_ab_ng.out > build/guards_ab_ng.healthy
  if ! cmp -s build/guards_ab.healthy build/guards_ab_ng.healthy; then
    diff build/guards_ab.healthy build/guards_ab_ng.healthy || true
    fail "guards are inert on healthy runs: the builds with and without the guards differ"
  fi
  say "PASS  guards are inert on healthy runs           $(wc -l < build/guards_ab.healthy | tr -d ' ') regions identical with and without them"
  grep '^control' build/guards_ab.out > build/guards_ab.control || true
  grep '^control' build/guards_ab_ng.out > build/guards_ab_ng.control || true
  n=$(wc -l < build/guards_ab.control | tr -d ' ')
  same=$(paste -d'|' build/guards_ab.control build/guards_ab_ng.control | awk -F'|' '$1 == $2' | wc -l | tr -d ' ')
  if [ "$n" = 0 ] || [ "$same" != 0 ]; then
    fail "the guards flag reaches the code: $same of $n controls identical in both builds"
  fi
  say "PASS  the guards flag actually reaches the code  $n of $n controls differ between the builds"
}
arm_regressions() { "$CC" $CFLAGS -o build/regressions tests/regressions.c -lm; ./build/regressions; }
arm_coverage()    { "$CC" $CFLAGS -o build/coverage tests/coverage.c -lm; ./build/coverage; }
arm_load() {
  "$CC" $CFLAGS -DLOAD_SMALL_UNIT -c tests/load.c -o build/load_small.o
  "$CC" $CFLAGS -c tests/load.c -o build/load_main.o
  "$CC" build/load_main.o build/load_small.o -o build/load -lm
  ./build/load
}
arm_train()       { "$CC" $CFLAGS -o build/train tests/train.c -lm; ./build/train; }
arm_elm()         { "$CC" $CFLAGS -o build/elm tests/elm.c -lm; ./build/elm; }
arm_playing()     { "$CC" $CFLAGS -o build/playing tests/playing.c -lm; ./build/playing; }
arm_portability() { "$CC" $CFLAGS -o build/portability tests/portability.c -lm; ./build/portability; }
arm_recipes()     { "$CC" $CFLAGS -o build/recipes tests/starter_recipes.c -lm; ./build/recipes; }
arm_tu() {
  X="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
  if ! can_link -fsanitize=address,undefined; then
    X=-O2; say "note  $CC cannot link AddressSanitizer; tu runs without it"
  fi
  "$CC" $STD $WARN $X -I. -c tests/tu/big.c -o build/tu_big.o
  "$CC" $STD $WARN $X -I. -c tests/tu/small.c -o build/tu_small.o
  "$CC" $X build/tu_small.o build/tu_big.o -o build/tu -lm
  ./build/tu
}
arm_fuzz() {
  need_asan fuzz
  "$CC" $STD $WARN -O1 -g $SAN -I. -o build/fuzz tests/fuzz.c -lm
  ./build/fuzz "${1:-400}"
}
arm_examples() {
  for f in examples/*.c; do
    n=$(basename "$f" .c)
    "$CC" $STD -O2 $WARN -Werror -I. -o "build/example_$n" "$f"
    "./build/example_$n" > "build/example_$n.out"
    if [ ! -s "build/example_$n.out" ]; then fail "examples/$n.c printed nothing"; fi
    if grep -Eq '(^|[^A-Za-z])-?(nan|inf)([^A-Za-z]|$)' "build/example_$n.out"; then
      fail "examples/$n.c printed a not-a-number or an infinity"
    fi
    say "PASS  examples/$n.c builds with -Werror, runs, exits 0 ($(wc -l < "build/example_$n.out" | tr -d ' ') lines)"
  done
}
arm_tiny() { "$CC" $CFLAGS -o build/tiny docs/tiny.c -lm; ./build/tiny; }

arm_sanitize() {
  need_asan sanitize
  X="$STD $WARN -O1 -g -fno-omit-frame-pointer $SAN -I."
  mkdir -p build/sanitize
  sanitized() {   # sanitized <name> <run arguments> -- <sources...>
    name=$1; shift; args=""
    while [ "$1" != "--" ]; do args="$args $1"; shift; done; shift
    "$CC" $X -pthread -o "build/sanitize/$name" "$@" -lm
    "./build/sanitize/$name" $args > "build/sanitize/$name.out" 2>&1 || {
      cat "build/sanitize/$name.out"; fail "sanitize: $name"; }
    say "PASS  sanitize  $name"
  }
  sanitized audit -- tests/audit.c
  sanitized guards_ab -- tests/guards_ab.c
  sanitized guards_ab_ng -- -DIRIS_NO_GUARDS tests/guards_ab.c
  sanitized regressions -- tests/regressions.c
  sanitized coverage -- tests/coverage.c
  "$CC" $X -DLOAD_SMALL_UNIT -c tests/load.c -o build/sanitize/load_small.o
  sanitized load -- tests/load.c build/sanitize/load_small.o
  sanitized train -- tests/train.c
  sanitized elm -- tests/elm.c
  sanitized playing -- tests/playing.c
  sanitized portability -- tests/portability.c
  sanitized recipes -- tests/starter_recipes.c
  sanitized tu -- tests/tu/big.c tests/tu/small.c
  sanitized fuzz 2000 -- tests/fuzz.c
  sanitized threads separate -- tests/threads.c
  sanitized tiny -- docs/tiny.c
}

arm_threads() {
  if ! can_link -fsanitize=thread -pthread; then fail "threads needs ThreadSanitizer, which $CC cannot link"; fi
  X="$STD $WARN -O1 -g -fsanitize=thread -pthread -I."
  "$CC" $X -o build/threads tests/threads.c
  "$CC" $X -o build/threads_audit tests/audit.c -lm
  if ! ./build/threads separate > build/threads_separate.out 2>&1; then
    cat build/threads_separate.out; fail "threads: eight instruments on eight threads"
  fi
  if grep -q ThreadSanitizer build/threads_separate.out || ! grep -q '^PASS' build/threads_separate.out; then
    cat build/threads_separate.out; fail "threads: eight instruments on eight threads"
  fi
  tail -1 build/threads_separate.out
  say "PASS  no ThreadSanitizer report from eight instruments on eight threads"
  if ./build/threads same > build/threads_same.out 2>&1; then st=0; else st=$?; fi
  races=$(grep -c 'WARNING: ThreadSanitizer: data race' build/threads_same.out || true)
  if [ "$st" = 0 ] || [ "$races" = 0 ]; then
    cat build/threads_same.out
    fail "threads: two threads on one instrument must be reported, and were not (exit $st, $races reports)"
  fi
  say "PASS  two threads on one instrument are reported  ($races data-race reports, exit status $st)"
  if ! ./build/threads_audit four > build/threads_audit.out 2>&1 \
     || grep -q ThreadSanitizer build/threads_audit.out; then
    cat build/threads_audit.out; fail "threads: audit check 36 under ThreadSanitizer"
  fi
  cat build/threads_audit.out
}

arm_noheap() {
  missing=""
  for f in $(public_functions); do grep -qw "$f" tests/noheap.c || missing="$missing $f"; done
  if [ -n "$missing" ]; then fail "noheap: tests/noheap.c never names:$missing"; fi
  say "PASS  tests/noheap.c names all $(public_functions | wc -l | tr -d ' ') public functions"
  case $(uname -s) in
    Darwin)
      "$CC" $STD -O1 $WARN -dynamiclib tests/noheap_interpose.c -o build/libnoheap.dylib
      "$CC" $CFLAGS -o build/noheap tests/noheap.c
      DYLD_INSERT_LIBRARIES="$ROOT/build/libnoheap.dylib" ./build/noheap ;;
    *)
      "$CC" $STD -O1 $WARN -shared -fPIC tests/noheap_interpose.c -o build/libnoheap.so
      "$CC" $CFLAGS -o build/noheap tests/noheap.c -ldl
      LD_PRELOAD="$ROOT/build/libnoheap.so" ./build/noheap ;;
  esac
  if ./build/noheap > build/noheap_alone.out; then st=0; else st=$?; fi
  if [ "$st" != 2 ]; then fail "noheap: without the interposer it must refuse with exit status 2, not $st"; fi
  say "PASS  without the interposer it refuses to report anything (exit status 2)"
}

arm_freestanding() { sh tests/freestanding.sh; }
arm_targets()      { sh tests/targets.sh; }
arm_pragma()       { sh tests/pragma_leak.sh; }

arm_determinism() {
  modes=$STD
  if ! is_clang "$CC"; then modes="$STD -std=gnu99"; fi   # gcc contracts in GNU C by default
  n=0
  for std in $modes; do
    for O in -O0 -O1 -O2 -O3 -Os; do
      for fc in off on default; do
        F="-ffp-contract=$fc"
        if [ "$fc" = default ]; then F=""; fi
        "$CC" $std $O $WARN $F -pthread -I. -o build/det_audit tests/audit.c -lm
        "$CC" $std $O $WARN $F -I. -o build/det_recipes tests/starter_recipes.c -lm
        ./build/det_audit golden > build/det_audit.out || { cat build/det_audit.out; fail "determinism: golden hash, $std $O contraction $fc"; }
        ./build/det_recipes > build/det_recipes.out || { cat build/det_recipes.out; fail "determinism: starter recipes, $std $O contraction $fc"; }
        say "PASS  $std $O contraction $fc: 0x6805FB0D, 0xB7FC47A0, 0x203834ED"
        n=$((n + 1))
      done
    done
  done
  say "PASS  determinism: the golden hash and both starter hashes in all $n builds"
  # The positive control: the same program against a copy of iris.h without
  # its contraction pragmas must lose the golden hash, or the builds above
  # could not have seen a contraction leak.
  can_fuse=no; mflag=""
  case $(uname -m) in
    arm64|aarch64) can_fuse=yes ;;
    x86_64|amd64)
      if grep -qw fma /proc/cpuinfo 2>/dev/null || sysctl -n machdep.cpu.features 2>/dev/null | grep -qw FMA; then
        can_fuse=yes; mflag=-mfma
      fi ;;
  esac
  if [ "$can_fuse" = no ]; then
    say "note  determinism control not run: this processor has no fused multiply-add"
    return 0
  fi
  mkdir -p build/det_control/tests
  sed -e '/#pragma STDC FP_CONTRACT OFF/d' -e '/#pragma GCC optimize ("fp-contract=off")/d' \
      iris.h > build/det_control/iris.h
  if cmp -s iris.h build/det_control/iris.h; then
    fail "determinism control: iris.h has no contraction pragma left to remove"
  fi
  cp tests/audit.c build/det_control/tests/audit.c
  cstd=$STD
  if ! is_clang "$CC"; then cstd=-std=gnu99; fi
  "$CC" $cstd -O2 $mflag -pthread -I build/det_control -o build/det_control/audit build/det_control/tests/audit.c -lm
  if ./build/det_control/audit golden > build/det_control/out.txt; then
    cat build/det_control/out.txt
    fail "determinism control: without the pragmas the golden hash still held, so this arm cannot see contraction"
  fi
  say "PASS  control: without its contraction pragmas iris.h gives $(sed -n 's/.*fnv1a \(0x[0-9A-F]*\).*/\1/p' build/det_control/out.txt), not 0x6805FB0D ($(echo $cstd -O2 $mflag))"
}

# The fuzzing arm needs clang's libFuzzer: CC when it was given, otherwise the
# first clang that can link it.
arm_fuzz_load() {
  secs=${1:-60}
  printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n);\nint LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) { (void)d; (void)n; return 0; }\n' > build/fuzz_probe.c
  FCC=""
  if [ -n "$CC_GIVEN" ]; then cands=$CC; else cands="clang /opt/homebrew/opt/llvm/bin/clang cc"; fi
  for c in $cands; do
    if command -v "$c" > /dev/null 2>&1 \
       && "$c" -fsanitize=fuzzer,address -o build/fuzz_probe build/fuzz_probe.c > /dev/null 2>&1; then
      FCC=$c; break
    fi
  done
  if [ -z "$FCC" ]; then fail "fuzz-load needs a clang with libFuzzer (Apple clang has none: brew install llvm, or CC=/path/to/clang)"; fi
  "$FCC" $STD -g -O1 -I. -fsanitize=fuzzer,address,undefined,float-divide-by-zero,float-cast-overflow \
    -fno-sanitize-recover=all -o build/fuzz_load tests/fuzz_load.c
  mkdir -p build/fuzz-load/corpus build/fuzz-load/artifacts
  say "fuzzing iris_load for $secs seconds with $FCC"
  ./build/fuzz_load -max_total_time="$secs" -max_len=16384 -print_final_stats=1 \
    -artifact_prefix=build/fuzz-load/artifacts/ build/fuzz-load/corpus
  say "PASS  fuzz-load: $secs seconds, no crash, no sanitizer report, no broken rule"
}

# Coverage: every test program built with clang's source-based coverage, the
# profiles merged per program, and the union taken per source position by
# tools/coverage.py. IRIS_API is redefined to force every function out in
# every program, so all programs report against the same lines.
# THE THRESHOLDS sit just under what the suite measures. Lines: 1,157 of 1,177
# (98.30%) with Apple clang 17, Homebrew clang 22 and Debian clang 19 alike.
# Branch outcomes depend on the LLVM version, which decides how many there
# are: 885 of 968 (91.43%) with Apple clang 17, 886 of 968 (91.53%) with
# clang 19, 909 of 1,012 (89.82%) with clang 22. So: lines at least 98.0%,
# branch outcomes at least 89.5%. COV_MIN_LINES and COV_MIN_BRANCHES
# override them.
arm_cov() {
  KC=""
  if [ -n "$CC_GIVEN" ]; then KC=$CC; else
    for c in cc clang /opt/homebrew/opt/llvm/bin/clang; do
      if command -v "$c" > /dev/null 2>&1 && is_clang "$c"; then KC=$c; break; fi
    done
  fi
  if [ -z "$KC" ] || ! is_clang "$KC"; then fail "cov needs clang (CC=clang)"; fi
  # llvm-profdata and llvm-cov of the same LLVM: beside the compiler, through
  # xcrun for Apple clang, or on the PATH with the version as a suffix
  real=$(command -v "$KC")
  real=$(readlink -f "$real" 2> /dev/null || echo "$real")
  PD=""; LC=""
  if [ -x "$(dirname "$real")/llvm-profdata" ]; then
    PD="$(dirname "$real")/llvm-profdata"; LC="$(dirname "$real")/llvm-cov"
  elif command -v xcrun > /dev/null 2>&1 && xcrun -f llvm-profdata > /dev/null 2>&1; then
    PD="xcrun llvm-profdata"; LC="xcrun llvm-cov"
  else
    v=$("$KC" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
    for s in "" "-$v"; do
      if command -v "llvm-profdata$s" > /dev/null 2>&1; then PD="llvm-profdata$s"; LC="llvm-cov$s"; break; fi
    done
  fi
  if [ -z "$PD" ]; then fail "cov needs llvm-profdata and llvm-cov to match $KC"; fi
  rm -rf build/cov
  mkdir -p build/cov/bin build/cov/prof
  printf '#define IRIS_API static inline __attribute__((used))\n' > build/cov/prelude.h
  printf '#include "../../tests/guards_ab.c"\n' > build/cov/guards_ab_ng.c
  printf '#define LOAD_SMALL_UNIT\n#include "../../tests/load.c"\n' > build/cov/load_small.c
  K="$STD -O2 $WARN -I. -fprofile-instr-generate -fcoverage-mapping -include build/cov/prelude.h"
  covbuild() { name=$1; shift; "$KC" $K -pthread -o "build/cov/bin/$name" "$@" -lm; }
  covbuild audit tests/audit.c
  covbuild guards_ab tests/guards_ab.c
  covbuild guards_ab_ng -DIRIS_NO_GUARDS build/cov/guards_ab_ng.c
  covbuild regressions tests/regressions.c
  covbuild coverage tests/coverage.c
  covbuild load tests/load.c build/cov/load_small.c
  covbuild train tests/train.c
  covbuild elm tests/elm.c
  covbuild playing tests/playing.c
  covbuild portability tests/portability.c
  covbuild recipes tests/starter_recipes.c
  covbuild tu tests/tu/big.c tests/tu/small.c
  covbuild fuzz tests/fuzz.c
  covbuild threads tests/threads.c
  covbuild tiny docs/tiny.c
  for p in audit guards_ab guards_ab_ng regressions coverage load train elm playing portability recipes tu fuzz threads tiny; do
    case $p in fuzz) args=400 ;; threads) args=separate ;; *) args="" ;; esac
    LLVM_PROFILE_FILE="build/cov/prof/$p-%p.profraw" "./build/cov/bin/$p" $args > "build/cov/$p.out" 2>&1 || {
      cat "build/cov/$p.out"; fail "cov: $p failed"; }
    $PD merge -sparse build/cov/prof/"$p"-*.profraw -o "build/cov/prof/$p.profdata"
  done
  python3 tools/coverage.py --llvm-cov "$LC" --source iris.h --dir build/cov \
    --min-lines "${COV_MIN_LINES:-98.0}" --min-branches "${COV_MIN_BRANCHES:-89.5}" \
    audit guards_ab guards_ab_ng regressions coverage load train elm playing portability recipes tu fuzz threads tiny
}

arm_mutate() { python3 tools/mutate.py "$@"; }

arm_reference() {
  PY=${PYTHON:-python3}
  if ! "$PY" -c 'import numpy' > /dev/null 2>&1; then
    say "SKIP  reference: NumPy is not installed for $PY (pip install -r tests/reference/requirements.txt)"
    [ -z "${IRIS_REQUIRE_REFERENCE:-}" ] || fail "reference: IRIS_REQUIRE_REFERENCE is set, so a skip is a failure"
    return 0
  fi
  "$CC" $CFLAGS -o build/reference_export tests/reference/export.c -lm
  "$PY" tests/reference/check.py build/reference_export
}

# The two port arms assert every byte their encoders emit. Their 32-bit
# structure sizes are _Static_asserts, checked by compiling for wasm32, a
# 32-bit target, with clang's front end only (any clang, Apple's included).
wasm_syntax() {
  WC=""
  for c in $CC clang /opt/homebrew/opt/llvm/bin/clang; do
    if command -v "$c" > /dev/null 2>&1 && is_clang "$c"; then WC=$c; break; fi
  done
  if [ -z "$WC" ]; then fail "the 32-bit size assertions need a clang for the wasm32 front end"; fi
  for f in "$@"; do "$WC" --target=wasm32 -std=c99 -Wall -Wextra -Werror -I. -fsyntax-only "$f"; done
}
arm_mpe() {
  "$CC" $CFLAGS -Werror -o build/mpe_test extras/tests/mpe_test.c extras/ports/mpe/iris_mpe.c \
    extras/ports/mpe/iris_mpe_wire.c -lm
  ./build/mpe_test
  wasm_syntax extras/ports/mpe/iris_mpe_wire.c
  say "PASS  32-bit sizes: bytes 8, desc 12, sink 36, voice 18, pool 132, mpe 276"
}
arm_sinks() {
  "$CC" $CFLAGS -Werror -o build/cc_test extras/tests/cc_test.c extras/ports/cc/iris_cc.c -lm
  ./build/cc_test
  "$CC" $CFLAGS -Werror -o build/osc_test extras/tests/osc_test.c extras/ports/osc/iris_osc.c -lm
  ./build/osc_test
  wasm_syntax extras/ports/cc/iris_cc.c extras/ports/osc/iris_osc.c extras/ports/template/iris_yoursink.c
  say "PASS  32-bit sizes: cc_cfg 28, cc 224, osc_cfg 20, osc 480; the template builds"
}

# The browser prototype needs a clang with the wasm32 code generator and
# wasm-ld; Apple clang has neither, so it is Homebrew's LLVM on macOS.
arm_bench() {
  BC=""
  for c in $CC clang /opt/homebrew/opt/llvm/bin/clang; do
    if echo 'int f(void){return 0;}' | "$c" --target=wasm32 -c -x c - -o /dev/null 2> /dev/null; then BC=$c; break; fi
  done
  if [ -z "$BC" ]; then fail "bench needs a clang with the wasm32 code generator (brew install llvm lld)"; fi
  if [ -d /opt/homebrew/opt/lld/bin ]; then PATH="/opt/homebrew/opt/lld/bin:$PATH"; fi
  "$BC" --target=wasm32 -O2 -nostdlib -ffreestanding \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
    -Wl,-z,stack-size=32768 -Wl,--initial-memory=1114112 \
    -o build/iris.wasm extras/ports/wasm/wasm_shim.c
  python3 -c "import base64;w=base64.b64encode(open('build/iris.wasm','rb').read()).decode();\
open('build/bench.html','w').write(open('extras/bench/page.html').read().replace('__WASM_B64__',w))"
  say "built build/bench.html; open it in a browser"
}
arm_clean() { rm -rf build; trap - EXIT; }

# The host arms `test` runs. The three that need a sanitizer runtime join
# only when this compiler can link one, and say so when it cannot.
HOST_ARMS="audit regressions coverage load train elm playing portability recipes tu examples tiny noheap determinism pragma freestanding targets mpe sinks"
arm_test() {
  arms=$HOST_ARMS
  if can_link -fsanitize=address,undefined; then arms="$arms fuzz sanitize"
  else say "note  $CC cannot link AddressSanitizer: fuzz and sanitize are not run"; fi
  if can_link -fsanitize=thread -pthread; then arms="$arms threads"
  else say "note  $CC cannot link ThreadSanitizer: threads is not run"; fi
  for a in $arms; do
    say ""
    say "==== sh build.sh $a"
    sh "$ROOT/build.sh" "$a"
  done
  say ""
  say "PASS  sh build.sh test: $(echo $arms | wc -w | tr -d ' ') arms"
}

case "$ARM" in
  test|audit|regressions|coverage|load|train|elm|playing|portability|recipes|tu|fuzz|examples|tiny|\
  sanitize|threads|noheap|freestanding|targets|pragma|determinism|cov|mutate|reference|mpe|sinks|bench|clean)
    "arm_$ARM" "$@" ;;
  fuzz-load) arm_fuzz_load "$@" ;;
  *) trap - EXIT
     say "usage: sh build.sh [test | audit | regressions | coverage | load | train | elm | playing |"
     say "       portability | recipes | tu | fuzz [N] | examples | tiny | sanitize | threads | noheap |"
     say "       freestanding | targets | pragma | determinism | fuzz-load [S] | cov | mutate [...] |"
     say "       reference | mpe | sinks | bench | clean]"
     exit 2 ;;
esac
