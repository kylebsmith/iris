#!/bin/sh
# tests/pragma_leak.sh -- iris.h switches contraction off for its own code
# and for nobody else's.
#
# RUN (from anywhere; it writes only to a temporary directory):
#     sh tests/pragma_leak.sh
# The exit status is non-zero if any check fails. A compiler that is not
# installed prints SKIP; a SKIP is not a pass for that compiler.
#
# WHAT IT CHECKS. A fused multiply-add computes a*b+c with one rounding
# instead of two, so a build that fuses where another does not makes a
# different instrument. iris.h therefore switches contraction off -- clang's
# #pragma STDC FP_CONTRACT OFF, GCC's #pragma GCC optimize ("fp-contract=off")
# -- inside a push/pop pair, so that code after the #include is compiled as
# if the header had not been there. Clang honours its push/pop pair,
# float_control, only on some processors (64-bit ARM and x86 among them);
# on the rest (32-bit ARM among them) iris.h ends its scope with
# #pragma STDC FP_CONTRACT DEFAULT, the command line's setting, instead.
#
# This script compiles one translation unit to assembly: iris.h, then one
# wrapper per public function (the wrappers do no arithmetic of their own),
# then a user function
#     float user_fma(float a, float b, float c) { return a * b + c; }
# and counts fused multiply-add instructions in every function. PASS needs
#   - user_fma to contain one: the include left the compiler's own default
#     alone, and that default contracts a*b+c;
#   - every other function to contain none: no iris arithmetic is fused,
#     whether it was inlined into a wrapper (clang) or kept out of line
#     (GCC does not inline across a change of optimize settings);
#   - no warning of any kind from the compiler;
#   - where the push/pop pair is honoured (every GCC build, and clang on
#     this machine), a contraction-off pragma of the includer's own placed
#     BEFORE the #include to survive it: built that way, user_fma must
#     contain no fused instruction.
#
# THE BUILDS. Apple clang (cc), Homebrew clang 22 and gcc-15, at -O2, as C
# and as C++, on this machine's processor. Clang contracts a*b+c by default;
# GCC does in its GNU modes and in C++, so gcc-15 is run with -std=gnu99 and
# -std=gnu++17 (in ISO C, -std=c99, GCC leaves contraction off and user_fma
# could not show anything). On x86-64 the builds add -mfma, because the
# baseline x86-64 instruction set has no fused multiply-add to emit. When
# installed, the ESP32-S3 compiler (xtensa-esp32s3-elf-gcc, at -Os, the
# Arduino core's level) and a Cortex-M4 compiler (arm-none-eabi-gcc) are
# checked the same way, in the Arduino core's GNU modes, and so is clang
# for a Cortex-M7, a 32-bit ARM core with a fused multiply-add where clang
# ignores float_control.
#
# POSITIVE CONTROL. Every build is repeated against a copy of iris.h with its
# two contraction pragmas deleted, and there the iris functions MUST contain
# fused instructions. Without this, a scan that could not recognise the
# instruction would pass everything.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/stripped"
sed -e '/#pragma STDC FP_CONTRACT OFF/d' \
    -e '/#pragma GCC optimize ("fp-contract=off")/d' \
    "$ROOT/iris.h" > "$T/stripped/iris.h"
cat > "$T/unit.c" <<'UNIT'
#ifdef USER_OFF_FIRST
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#else
#pragma GCC optimize ("fp-contract=off")
#endif
#endif
#include "iris.h"
#define NI 2
#define NH 12
#define NO 3
#define CAP 16
static unsigned char mem[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char file[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char scratch[IRIS_ELM_SCRATCH(NH, NO) + IRIS_ARENA(NI, NH, NO, CAP)];
static iris_rng R;
static iris *K;
static float IN[NI], OUT[NO], F;
static int I;
static unsigned long U;
static size_t N;
#ifdef __cplusplus
extern "C" {
#endif
void w_math(void) {
  I = iris_isbad(F); F = iris_tanh(F); F = iris_sigmoid(F); F = iris_sqrt(F);
  F = iris_absf(F); F = iris_clampf(F, IN[0], IN[1]); F = iris_artanh(F);
  F = iris_logit(F); U = iris_rand_u32(&R); F = iris_rand_sym(&R);
}
void w_init(void)       { N = iris_size(NI, NH, NO, CAP);
                          K = iris_init(mem, sizeof mem, NI, NH, NO, CAP, 1u); }
void w_status(void)     { I = (int)iris_get_status(K); }
void w_reseed(void)     { iris_reseed(K, 7u); }
void w_learning(void)   { iris_internal_set_learning(K, IN[0], IN[1]); }
void w_l2(void)         { iris_internal_set_l2(K, F); F = iris_get_l2(K); }
void w_smoothing(void)  { iris_set_smoothing(K, F); F = iris_get_smoothing(K); }
void w_record(void)     { I = iris_record(K, IN, OUT); }
void w_store(void)      { I = iris_count(K); I = iris_capacity(K);
                          I = iris_index_of(K, I); I = iris_id_at(K, I);
                          I = iris_get(K, I, IN, OUT); }
void w_ranges(void)     { iris_fit_ranges(K); }
void w_norm(void)       { F = iris_norm_in(K, I, F); F = iris_norm_out(K, I, F);
                          F = iris_denorm_out(K, I, F); }
void w_forward(void)    { iris_forward_norm(K, IN); I = iris_shape_fits(K); }
void w_epochs(void)     { F = iris_train_epochs(K, I); }
void w_converge(void)   { F = iris_train_converge(K, I, 0, 0); }
void w_train(void)      { I = iris_train(K); }
void w_check(void)      { I = iris_internal_check_weights(K); }
void w_run(void)        { F = iris_internal_train_run(K, I, 1, 0, 0, 0); }
void w_slices(void)     { I = iris_train_begin(K, I); I = iris_train_slice(K, I);
                          F = iris_train_progress(K); I = iris_train_busy(K);
                          I = iris_train_epochs_done(K); }
void w_predict(void)    { iris_predict(K, IN, OUT); }
void w_novelty(void)    { F = iris_novelty(K, IN); }
void w_stress(void)     { F = iris_example_stress(K, I); I = iris_worst_example(K, &F);
                          I = iris_worst_example_id(K, &F); }
void w_warm(void)       { iris_zero_velocity(K); F = iris_correct(K, I); }
void w_state(void)      { I = iris_is_trained(K); F = iris_last_error(K); U = iris_seed(K); }
void w_loo(void)        { F = iris_loo_error(K, I); }
void w_suggest(void)    { F = iris_suggest_smoothing(K, scratch, sizeof scratch); }
void w_elm(void)        { I = iris_train_elm(K, F, scratch, sizeof scratch); }
void w_elm_ex(void)     { I = iris_train_elm_ex(K, F, IN[0], IN[1], scratch, sizeof scratch); }
void w_elm_new(void)    { I = iris_retrain_elm_new(K, 9u, F, scratch, sizeof scratch); }
void w_knn(void)        { iris_knn_predict(K, IN, OUT, I); }
void w_1nn(void)        { I = iris_classify_1nn(K, IN, OUT); }
void w_save(void)       { N = iris_save_size(K); N = iris_save(K, file, sizeof file);
                          U = iris_crc32(file, N); }
void w_load(void)       { I = iris_load(K, file, N); }
void w_retrain(void)    { F = iris_retrain_new(K, 3u, I); }
void w_delete(void)     { I = iris_delete_nearest(K, IN); I = iris_delete_index(K, I);
                          I = iris_delete_id(K, I); I = iris_delete_last(K); iris_clear(K); }

/* The includer's own code. The compiler's default contracts this. */
float user_fma(float a, float b, float c) { return a * b + c; }
#ifdef __cplusplus
}
#endif
UNIT

# scan <asm file> <format: macho|elf> <fused-instruction regex>
# prints "<fused in user_fma> <fused elsewhere> <wrappers seen> <worst other function>"
scan() {
  awk -v fmt="$2" -v re="$3" '
    /^[^ \t.#;@][^ \t]*:/ {
      lab = $1; sub(/:.*/, "", lab)
      if (fmt == "macho" && lab !~ /^_/) next
      fn = lab; seen[fn] = 1; next
    }
    fn != "" && $0 ~ re { cnt[fn]++ }
    END {
      u = 0; o = 0; nf = 0; worst = "-"; wc = 0
      for (f in seen) {
        if (f ~ /^_?w_/) nf++
        c = (f in cnt) ? cnt[f] : 0
        if (f ~ /^_?user_fma$/) u += c
        else { o += c; if (c > wc) { wc = c; worst = f } }
      }
      printf "%d %d %d %s\n", u, o, nf, worst
    }' "$1"
}

WRAPPERS=$(grep -c '^void w_' "$T/unit.c")
fail=0
ARM64_RE='^[[:space:]]+(fn?madd|fn?msub|fmla|fmls)([.][0-9a-z]+)?[[:space:]]'
X86_RE='^[[:space:]]+vfn?m(add|sub)(sub|add)?[0-9][0-9][0-9][ps][sd][[:space:]]'
XT_RE='^[[:space:]]+(madd|msub|maddn|msubn)[.]s[[:space:]]'
CM_RE='^[[:space:]]+vfn?m[as][.]f(32|64)[[:space:]]'

# leg <label> <keep|nokeep> <format> <regex> <compiler> <flags...>
# keep: also check that the includer's own pragma before the #include survives
leg() {
  label=$1; keep=$2; fmt=$3; re=$4; cc=$5; shift 5
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf '  SKIP  %-44s not installed\n' "$label"; return
  fi
  if ! "$cc" "$@" -I"$ROOT" -S "$T/unit.c" -o "$T/real.s" 2> "$T/err"; then
    printf '  FAIL  %-44s did not compile: %s\n' "$label" "$(head -1 "$T/err")"; fail=1; return
  fi
  "$cc" "$@" -I"$T/stripped" -S "$T/unit.c" -o "$T/strip.s" 2> /dev/null || : > "$T/strip.s"
  kept=0
  if [ "$keep" = keep ]; then
    "$cc" "$@" -DUSER_OFF_FIRST -I"$ROOT" -S "$T/unit.c" -o "$T/keep.s" 2> /dev/null || : > "$T/keep.s"
    set -- $(scan "$T/keep.s" "$fmt" "$re")
    kept=$1
  fi
  set -- $(scan "$T/real.s" "$fmt" "$re")
  u=$1; o=$2; nf=$3; worst=$4
  set -- $(scan "$T/strip.s" "$fmt" "$re")
  ctl=$2
  if grep -q 'warning:' "$T/err"; then
    printf '  FAIL  %-44s warning: %s\n' "$label" "$(grep -m1 'warning:' "$T/err" | sed 's/.*warning: //')"; fail=1
  elif [ "$nf" -ne "$WRAPPERS" ]; then
    printf '  FAIL  %-44s %s of the %s wrappers found in the assembly\n' "$label" "$nf" "$WRAPPERS"; fail=1
  elif [ "$ctl" -eq 0 ]; then
    printf '  FAIL  %-44s positive control: no fused instruction even without the pragmas\n' "$label"; fail=1
  elif [ "$u" -lt 1 ]; then
    printf '  FAIL  %-44s user_fma not fused: the pragma leaks into the includer\n' "$label"; fail=1
  elif [ "$o" -ne 0 ]; then
    printf '  FAIL  %-44s %s fused in iris code (most in %s)\n' "$label" "$o" "$worst"; fail=1
  elif [ "$kept" -ne 0 ]; then
    printf '  FAIL  %-44s the includer'"'"'s own pragma before the #include was undone\n' "$label"; fail=1
  else
    [ "$keep" = keep ] && note=", own pragma kept" || note=""
    printf '  PASS  %-44s user_fma %s, iris 0 (%s without the pragmas)%s\n' \
      "$label" "$u" "$ctl" "$note"
  fi
}

case $(uname -s) in Darwin) HOSTFMT=macho ;; *) HOSTFMT=elf ;; esac
case $(uname -m) in
  arm64|aarch64) HRE=$ARM64_RE; HFLAGS="" ;;
  x86_64|amd64)  HRE=$X86_RE;   HFLAGS="-mfma" ;;
  *) echo "  SKIP  host processor $(uname -m): no fused-instruction pattern known"; HRE="" ;;
esac
LLVM=/opt/homebrew/opt/llvm/bin/clang
echo "fused multiply-add: in the includer's code, not in iris's"
if [ -n "$HRE" ]; then
  for pair in "cc:cc" "clang 22:$LLVM"; do
    name=${pair%%:*}; cc=${pair#*:}
    leg "$name, C   -std=c99 -O2"   keep $HOSTFMT "$HRE" "$cc" -x c -std=c99 -O2 $HFLAGS
    leg "$name, C++ -std=c++17 -O2" keep $HOSTFMT "$HRE" "$cc" -x c++ -std=c++17 -O2 $HFLAGS
  done
  leg "gcc-15, C   -std=gnu99 -O2"   keep $HOSTFMT "$HRE" gcc-15 -x c -std=gnu99 -O2 $HFLAGS
  leg "gcc-15, C++ -std=gnu++17 -O2" keep $HOSTFMT "$HRE" gcc-15 -x c++ -std=gnu++17 -O2 $HFLAGS
fi
XT=$HOME/Library/Arduino15/packages/esp32/tools/esp-x32/2507/bin/xtensa-esp32s3-elf-gcc
CM=/Applications/ARM/bin/arm-none-eabi-gcc
leg "ESP32-S3, C   -std=gnu17 -Os"   keep elf "$XT_RE" "$XT" -x c -std=gnu17 -Os -mlongcalls
leg "ESP32-S3, C++ -std=gnu++2a -Os" keep elf "$XT_RE" "$XT" -x c++ -std=gnu++2a -Os -mlongcalls
leg "Cortex-M4, C   -std=gnu17 -O2"  keep elf "$CM_RE" "$CM" -x c -std=gnu17 -O2 \
    -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
leg "Cortex-M4, C++ -std=gnu++17 -O2" keep elf "$CM_RE" "$CM" -x c++ -std=gnu++17 -O2 \
    -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
# clang ignores float_control here, so the includer's own earlier pragma is
# not kept (iris.h says so); the rest must hold, with no warning
for pair in "cc:cc" "clang 22:$LLVM"; do
  name=${pair%%:*}; cc=${pair#*:}
  leg "$name, Cortex-M7, C   -O2"   nokeep elf "$CM_RE" "$cc" -x c -std=c99 -O2 \
      --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -ffreestanding
done
leg "clang 22, Cortex-M7, C++ -O2" nokeep elf "$CM_RE" "$LLVM" -x c++ -std=c++17 -O2 \
    --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -ffreestanding -nostdinc++
[ "$fail" = 0 ] && echo "  pragma scope holds" || echo "  PRAGMA SCOPE BROKEN"
exit $fail
