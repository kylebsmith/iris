#!/bin/sh
# tests/freestanding.sh -- iris.h calls no library function. The claim, the
# flags it needs, and the one target where the list is not empty.
#
# Run from anywhere; it writes only to a temporary directory:
#     sh tests/freestanding.sh            or   sh build.sh freestanding
# The exit status is non-zero if any check fails.
# The host compilers. With CC set, the host rows test that compiler alone
# (CC=gcc-15 sh tests/freestanding.sh). Without it they test each distinct
# compiler among cc, Homebrew's clang (/opt/homebrew/opt/llvm/bin/clang),
# clang, gcc-15 and gcc that is installed; two names for one compiler (on
# macOS, gcc and clang are both Apple clang) are tested once.
# The ESP32-S3 rows need the esp32 Arduino core, found under
# ~/Library/Arduino15 (macOS) or ~/.arduino15 (Linux). Without it they print
# SKIP; a SKIP is not a pass. With IRIS_REQUIRE containing the word xtensa
# (as continuous integration sets it where the core is installed), a missing
# toolchain fails instead.
# The Linux rows of section 4 need Docker and an image with gcc and clang in
# it, named in IRIS_LINUX_IMAGE (any Debian image with both installed will
# do); they are for checking Linux from a Mac, and are skipped otherwise:
#     IRIS_LINUX_IMAGE=my-debian-with-compilers sh tests/freestanding.sh
#
# The claim. A translation unit that calls every public function in iris.h,
# compiled with
#     -std=c99 -ffreestanding -fno-stack-protector       (clang)
#     -std=c99 -ffreestanding -fno-stack-protector \
#              -fno-tree-loop-distribute-patterns        (GCC, the GNU
#                                                         Compiler Collection)
# at -O0, -O1, -O2, -O3 and -Os, as C and as C++, has ZERO undefined symbols
# and links with -nostdlib -static and no C library at all. That holds for
# Apple clang, Homebrew clang 22 and gcc-15 on a 64-bit ARM Mac, and for
# Debian's gcc 14 and clang 19 on 64-bit ARM Linux.
#
# Why each flag is needed. None of them changes an output bit; each
# stops the COMPILER, not this code, from reaching for the C library.
#   -ffreestanding  says there is no C library. Without it clang recognises
#                   the loops that zero or copy an array and replaces them
#                   with calls to bzero, memset, memset_pattern16 and memcpy
#                   (Apple clang and clang 22 at -O2 and above).
#   -fno-stack-protector  where the toolchain turns stack protection on by
#                   default (Apple clang and Homebrew clang on macOS), every
#                   function with an array calls __stack_chk_fail from the C
#                   library. gcc-15 and Debian's gcc and clang need nothing.
#   -fno-tree-loop-distribute-patterns  GCC only: GCC replaces the same
#                   loops with memset, memcpy and memmove even under
#                   -ffreestanding (-O2, -O3 and -Os).
# -fno-math-errno is NOT needed. iris_internal_sqrt computes square roots in
# integer arithmetic, so there is no square root instruction for the compiler
# to back up with a call to sqrtf when it must set errno.
# Section 3 below rebuilds without each flag and prints what comes back, so
# a flag that stops being needed shows up (as a NOTE, not a failure).
#
# On the ESP32-S3 the list is not empty. Every entry comes from libgcc, the
# compiler's own support library, and none from the C library
# (xtensa-esp32s3-elf-gcc, the flags above, -O0, -O2 and -Os):
#   the playing path (init, record, train, predict, novelty, neighbours,
#   delete, clear):    __divsf3 and nothing else. The processor has divide
#                      step instructions but no single divide instruction,
#                      so every float division is this libgcc routine.
#   every function:    adds __adddf3 __divdf3 __extendsfdf2 __floatsidf
#                      __ledf2 __muldf3 __subdf3 __truncdfsf2, the
#                      double-precision routines behind the leave-one-out
#                      sweep iris_loo_error and iris_suggest_smoothing share
#                      (its double accumulator, and the comparison that skips
#                      an output whose demonstrations never moved).
#                      iris_suggest_smoothing's five-entry table of settings
#                      is static const, so GCC for this processor does not
#                      copy it onto the stack with memcpy.
# Section 5 requires exactly these lists, the ones the masthead of iris.h
# states, so a symbol that appears or disappears fails here until the
# masthead and EVERY below are brought up to date together.
# sqrtf is not on the list: the square root is in the header.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

# ---- the probe: every public function, called once ------------------------
cat > "$T/every.c" <<'PROBE'
#include "iris.h"
#define NI 2
#define NH 12
#define NO 3
#define CAP 16
static unsigned char mem[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char mem2[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char file[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char scratch[IRIS_ELM_SCRATCH(NH, NO) + IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char word[16];
static float in[NI], out[NO], xn[NI];
volatile float SINKF;
volatile int SINKI;
volatile unsigned long SINKU;
static int progress(void *user, int done, int ceiling, float err) {
  (void)user; (void)done; (void)ceiling; SINKF += err; return 1;
}
#ifdef __cplusplus
extern "C"
#endif
int iris_probe(void);
int iris_probe(void) {
  iris_internal_rng r;
  float margin = 0.0f, lo = 0.0f, hi = 0.0f, inv[NI];
  int i;
  size_t n;
  iris *k, *k2;
  r.s = 12345u;
  SINKI += iris_internal_isbad(SINKF);
  SINKF += iris_internal_tanh(SINKF) + iris_internal_sigmoid(SINKF) + iris_internal_sqrt(SINKF)
         + iris_internal_absf(SINKF) + iris_internal_clampf(SINKF, -1.0f, 1.0f)
         + iris_internal_artanh(SINKF) + iris_internal_logit(SINKF);
  SINKU += iris_internal_rand_u32(&r);
  SINKF += iris_internal_rand_sym(&r);
  SINKU += (unsigned long)iris_size(NI, NH, NO, CAP);
  k = iris_init(mem, sizeof mem, NI, NH, NO, CAP, 1234u);
  if (!k) return 1;
  SINKI += (int)iris_get_status(k);
  iris_reseed(k, 99u);
  iris_internal_set_learning(k, 0.1f, 0.85f);
  iris_internal_default_learning(k);
  iris_internal_set_l2(k, 0.0f);
  SINKF += iris_internal_get_l2(k);
  iris_set_smoothing(k, 0.0f);
  SINKF += iris_get_smoothing(k);
  for (i = 0; i < CAP; ++i) {
    in[0] = (float)(i % 4) * SINKF;
    in[1] = (float)(i / 4) * SINKF;
    out[0] = in[0]; out[1] = in[1]; out[2] = in[0] - in[1];
    SINKI += iris_record(k, in, out);
  }
  SINKI += iris_count(k) + iris_capacity(k) + iris_shape(k, 0, 0, 0, 0);
  SINKI += iris_index_of(k, 3) + iris_id_at(k, 2) + iris_get(k, 1, in, out);
  iris_internal_fit_ranges(k);
  SINKF += iris_internal_norm_in(k, 0, SINKF) + iris_internal_norm_out(k, 0, SINKF)
         + iris_internal_denorm_out(k, 0, SINKF);
  xn[0] = SINKF; xn[1] = SINKF;
  iris_internal_forward_norm(k, xn);
  SINKI += iris_internal_shape_fits(k);
  SINKF += iris_continue(k, 50);
  SINKF += iris_continue_to_plateau(k, 4000, progress, 0);
  SINKI += iris_train(k);
  SINKI += iris_internal_check_weights(k);
  SINKF += iris_internal_recall_error(k, xn) + iris_internal_trap_nan(k);
  SINKF += iris_internal_train_run(k, 10, 0, 0, 0, 0);
  SINKI += iris_train_begin(k, 2000);
  while (iris_train_slice(k, 500)) SINKF += iris_train_progress(k);
  SINKI += iris_train_busy(k) + iris_train_epochs_done(k);
  iris_predict(k, in, out);
  SINKF += out[0] + iris_novelty(k, in) + iris_example_stress(k, 0);
  SINKI += iris_worst_example(k, &margin) + iris_worst_example_id(k, &margin);
  iris_internal_zero_velocity(k);
  SINKI += iris_is_trained(k);
  SINKF += iris_last_error(k);
  SINKU += iris_seed(k);
  SINKF += iris_loo_error(k, 50);
  SINKF += iris_internal_loo(k, 50, 1) + (float)iris_internal_out_span(k, 3, 0);
  SINKI += iris_internal_trainable(k) + iris_internal_pinned(k)
         + iris_internal_cold_start(k);
  iris_internal_begin_session(k, 2000);
  SINKF += iris_suggest_smoothing(k, scratch, sizeof scratch);
  SINKI += iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
  SINKI += iris_internal_train_elm_ex(k, 1e-4f, 2.0f, 2.0f, scratch, sizeof scratch);
  iris_knn_predict(k, in, out, 3);
  SINKF += out[1];
  SINKI += iris_classify_1nn(k, in, out);
  iris_internal_span(k, 0, &lo, &hi);
  iris_internal_neighbour_scale(k, inv);
  SINKF += lo + hi + iris_internal_centre(k, 0)
         + iris_internal_distance2(k, inv, xn, in)
         + iris_internal_distance2_far(k, inv, xn, in);
  { float bd[2]; int bi[2];
    bd[0] = 1.0f; bd[1] = 2.0f; bi[0] = 0; bi[1] = 1;
    iris_internal_knn_insert(bd, bi, 2, 2, SINKF);
    SINKI += bi[0]; }
  SINKI += iris_internal_nearest(k, in);
  SINKU += (unsigned long)iris_save_size(k);
  n = iris_save(k, file, sizeof file);
  SINKU += iris_internal_crc32(file, n);
  SINKU += (unsigned long)iris_internal_file_bytes(k, 3u)
         + iris_internal_get_u32(file + 4);
  SINKI += iris_internal_file_ok(k, file, n) + iris_internal_range_ok(xn[0], xn[1], 1);
  SINKF += iris_internal_get_f32(file + 44);
  SINKU += (unsigned long)(iris_internal_get_f32s(file + 48, xn, NI) - file);
  iris_internal_put_u32(word, 7u);
  iris_internal_put_f32(word + 4, SINKF);
  SINKU += (unsigned long)(iris_internal_put_f32s(word + 8, xn, NI) - word);
  k2 = iris_init(mem2, sizeof mem2, NI, NH, NO, CAP, 1u);
  SINKI += iris_load(k2, file, n);
  SINKI += iris_delete_nearest(k, in);
  SINKI += iris_delete_index(k, 0) + iris_delete_id(k, 5) + iris_delete_last(k);
  iris_clear(k);
  return 0;
}
PROBE

# ---- the playing path a sketch runs, for the ESP32-S3 row -----------------
cat > "$T/play.c" <<'PROBE'
#include "iris.h"
static unsigned char m[IRIS_ARENA(2, 12, 3, 32)];
volatile float SINK;
static iris *K;
static float IN[2], OUT[3];
void play(void);
void play(void) {
  K = iris_init(m, sizeof m, 2, 12, 3, 32, 1234u);
  iris_record(K, IN, OUT); iris_train(K); iris_predict(K, IN, OUT); SINK = OUT[0];
  SINK += iris_novelty(K, IN);
  iris_knn_predict(K, IN, OUT, 3); SINK += OUT[0];
  SINK += (float)iris_classify_1nn(K, IN, OUT);
  iris_delete_nearest(K, IN); iris_delete_last(K); iris_clear(K);
  SINK += (float)iris_get_status(K);
}
PROBE

undefined() { nm -u "$1" | awk '{print $NF}' | sort -u | tr '\n' ' ' | sed 's/ *$//'; }
case $(uname -s) in Darwin) ENTRY=_iris_probe ;; *) ENTRY=iris_probe ;; esac
LLVM=/opt/homebrew/opt/llvm/bin/clang
# the host compilers: CC alone, or each distinct installed compiler
if [ -n "${CC:-}" ]; then HOSTCC=$CC; else
  HOSTCC=""; seen=""
  for c in cc "$LLVM" clang gcc-15 gcc; do
    command -v "$c" >/dev/null 2>&1 || continue
    v=$("$c" --version 2>/dev/null | head -1)
    case "$seen" in *"|$v|"*) continue ;; esac
    seen="$seen|$v|"; HOSTCC="$HOSTCC $c"
  done
fi
requires() { case " ${IRIS_REQUIRE:-} " in *" $1 "*) return 0 ;; esac; return 1; }

# ---- 1. the probe really calls every public function ---------------------
missing=""
for f in $(sed -n 's/^IRIS_API[^(]*[ *]\(iris_[a-z0-9_]*\) *(.*/\1/p' "$ROOT/iris.h" | sort -u); do
  grep -qw "$f" "$T/every.c" || missing="$missing $f"
done
total=$(sed -n 's/^IRIS_API[^(]*[ *]\(iris_[a-z0-9_]*\) *(.*/\1/p' "$ROOT/iris.h" | sort -u | wc -l | tr -d ' ')
echo "1. the probe calls every public function"
if [ -z "$missing" ] && [ "$total" -gt 0 ]; then
  echo "  PASS  all $total functions iris.h defines are called"
else
  echo "  FAIL  the probe does not call:$missing"; fail=1
fi

# ---- 2. zero undefined symbols, no warnings, a real link ------------------
echo "2. zero undefined symbols on the host, no warnings, links with no C library"
label_of() { case "$1" in /opt/homebrew/*) echo "Homebrew $(basename "$1")" ;; *) basename "$1" ;; esac; }
for cc in $HOSTCC; do
  name=$(label_of "$cc")
  if ! command -v "$cc" >/dev/null 2>&1; then
    [ -n "${CC:-}" ] && { echo "  FAIL  $cc (from CC) is not installed"; fail=1; }
    continue
  fi
  if "$cc" --version 2>&1 | grep -qi clang; then
    FLAGS="-ffreestanding -fno-stack-protector"
  else
    FLAGS="-ffreestanding -fno-stack-protector -fno-tree-loop-distribute-patterns"
  fi
  for lang in "c -std=c99" "c++"; do
    bad=""
    for O in -O0 -O1 -O2 -O3 -Os; do
      for api in default every; do
        D=""; [ $api = every ] && D="-DIRIS_API="
        if ! "$cc" -x $lang $O $FLAGS $D -Wall -Wextra -I"$ROOT" -c "$T/every.c" \
             -o "$T/p.o" > "$T/err" 2>&1; then
          bad="$bad [$O $api: did not compile: $(grep -m1 -i 'error' "$T/err")]"; continue
        fi
        # warnings count only in the ordinary build; -DIRIS_API= is a probe device
        [ $api = default ] && grep -q "warning:" "$T/err" && \
          bad="$bad [$O: $(grep -c 'warning:' "$T/err") warnings, first: $(grep -m1 'warning:' "$T/err" | sed 's/.*warning: //')]"
        U=$(undefined "$T/p.o")
        [ -n "$U" ] && bad="$bad [$O $api: $U]"
        if [ $api = default ] && ! "$cc" -x $lang $O $FLAGS -nostdlib -static \
             -Wl,-e,$ENTRY -I"$ROOT" "$T/every.c" -o "$T/p.exe" > /dev/null 2>&1; then
          bad="$bad [$O: static link failed]"
        fi
      done
    done
    label="$name, $(echo $lang | sed 's/c -std=c99/C/; s/c++/C++/')"
    if [ -z "$bad" ]; then
      printf '  PASS  %-20s -O0 -O1 -O2 -O3 -Os, called and every definition: none\n' "$label"
    else
      printf '  FAIL  %-20s%s\n' "$label" "$bad"; fail=1
    fi
  done
done

# ---- 3. each flag is still needed ----------------------------------------
echo "3. what each flag keeps out (C, -O2; a NOTE means a flag is no longer needed there)"
flagcheck() { # <name> <cc> <flag to drop> <flags...>
  name=$1; cc=$2; drop=$3; shift 3
  command -v "$cc" >/dev/null 2>&1 || return 0
  "$cc" -x c -std=c99 -O2 "$@" -I"$ROOT" -c "$T/every.c" -o "$T/f.o" 2>/dev/null || return 0
  U=$(undefined "$T/f.o")
  if [ -n "$U" ]; then printf '  NEEDED  %-38s %-12s without it: %s\n' "$drop" "$name" "$U"
  else printf '  NOTE    %-38s %-12s not needed here\n' "$drop" "$name"; fi
}
for cc in $HOSTCC; do
  command -v "$cc" >/dev/null 2>&1 || continue
  name=$(label_of "$cc")
  if "$cc" --version 2>&1 | grep -qi clang; then
    flagcheck "$name" "$cc" -ffreestanding -fno-stack-protector
    flagcheck "$name" "$cc" -fno-stack-protector -ffreestanding
  else
    flagcheck "$name" "$cc" -fno-tree-loop-distribute-patterns -ffreestanding -fno-stack-protector
  fi
done

# ---- 4. Linux, in a container --------------------------------------------
echo "4. Linux gcc and clang"
if [ -z "${IRIS_LINUX_IMAGE:-}" ]; then
  echo "  SKIP  set IRIS_LINUX_IMAGE to an image with gcc and clang to run these"
elif ! command -v docker >/dev/null 2>&1; then
  echo "  SKIP  no docker"
else
  mkdir -p "$T/linux"
  cp "$ROOT/iris.h" "$T/every.c" "$T/linux/"
  cat > "$T/linux/run.sh" <<'INNER'
bad=0
for cc in gcc clang; do
  case $cc in gcc) F="-ffreestanding -fno-stack-protector -fno-tree-loop-distribute-patterns" ;;
              *)   F="-ffreestanding -fno-stack-protector" ;; esac
  line=""
  for O in -O0 -O1 -O2 -O3 -Os; do
    $cc -std=c99 $O $F -I. -c every.c -o p.o 2>/dev/null || { line="$line [$O: did not compile]"; continue; }
    U=$(nm -u p.o | awk '{print $NF}' | sort -u | tr '\n' ' ' | sed 's/ *$//')
    [ -n "$U" ] && line="$line [$O: $U]"
    $cc -std=c99 $O $F -nostdlib -static -Wl,-e,iris_probe -I. every.c -o p.exe 2>/dev/null \
      || line="$line [$O: static link failed]"
  done
  v=$($cc --version | head -1)
  if [ -z "$line" ]; then echo "  PASS  $v: none, -O0 to -Os, links"
  else echo "  FAIL  $v:$line"; bad=1; fi
done
exit $bad
INNER
  if ( cd "$T/linux" && tar -cf - iris.h every.c run.sh ) 2>/dev/null | \
       docker run -i --rm "$IRIS_LINUX_IMAGE" \
         sh -c 'mkdir -p /w && cd /w && tar -xf - 2>/dev/null && sh run.sh'; then :
  else fail=1; fi
fi

# ---- 5. the ESP32-S3 ------------------------------------------------------
echo "5. the ESP32-S3's own compiler"
XT=$(find "$HOME/Library/Arduino15/packages/esp32" "$HOME/.arduino15/packages/esp32" \
       -name 'xtensa-esp32s3-elf-gcc' -type f 2>/dev/null | head -1)
XNM=$(find "$HOME/Library/Arduino15/packages/esp32" "$HOME/.arduino15/packages/esp32" \
       -name 'xtensa-esp32s3-elf-nm' -type f 2>/dev/null | head -1)
if [ -z "$XT" ] || [ -z "$XNM" ]; then
  if requires xtensa; then
    echo "  FAIL  no ESP32-S3 toolchain installed, and IRIS_REQUIRE asks for it"; fail=1
  else
    echo "  SKIP  no ESP32-S3 toolchain installed (install the esp32 Arduino core)"
  fi
else
  XF="-std=c99 -ffreestanding -fno-stack-protector -fno-tree-loop-distribute-patterns -mlongcalls"
  # every function: the list the masthead of iris.h states, in nm's order
  EVERY=$(printf '%s\n' __divsf3 __adddf3 __divdf3 __extendsfdf2 __floatsidf \
    __ledf2 __muldf3 __subdf3 __truncdfsf2 | sort -u | tr '\n' ' ' | sed 's/ *$//')
  for O in -O0 -O2 -Os; do
    if ! "$XT" $XF $O -I"$ROOT" -c "$T/play.c" -o "$T/x.o" 2> "$T/err"; then
      echo "  FAIL  playing path $O did not compile: $(head -1 "$T/err")"; fail=1; continue
    fi
    U=$("$XNM" -u "$T/x.o" | awk '{print $NF}' | sort -u | tr '\n' ' ' | sed 's/ *$//')
    if [ "$U" = "__divsf3" ]; then printf '  PASS  playing path    %-4s %s\n' "$O" "$U"
    else printf '  FAIL  playing path    %-4s want __divsf3 alone, got: %s\n' "$O" "$U"; fail=1; fi
    for api in default every; do
      D=""; [ $api = every ] && D="-DIRIS_API="
      if ! "$XT" $XF $O $D -I"$ROOT" -c "$T/every.c" -o "$T/x.o" 2> "$T/err"; then
        echo "  FAIL  every function $O did not compile: $(head -1 "$T/err")"; fail=1; continue
      fi
      U=$("$XNM" -u "$T/x.o" | awk '{print $NF}' | sort -u | tr '\n' ' ' | sed 's/ *$//')
      [ $api = every ] && label="every definition" || label="every function  "
      if [ "$U" = "$EVERY" ]; then printf '  PASS  %s %-4s %s\n' "$label" "$O" "$U"
      else printf '  FAIL  %s %-4s want the masthead'"'"'s list, got: %s\n' "$label" "$O" "$U"; fail=1; fi
    done
  done
  "$XT" $(echo "$XF" | sed 's/-fno-tree-loop-distribute-patterns//') -Os -I"$ROOT" \
    -c "$T/play.c" -o "$T/x.o" 2>/dev/null && \
    printf '  NEEDED  -fno-tree-loop-distribute-patterns   without it, playing path -Os: %s\n' \
      "$("$XNM" -u "$T/x.o" | awk '{print $NF}' | sort -u | tr '\n' ' ')"
fi

[ "$fail" = 0 ] && echo "  the zero-dependency claim holds" || echo "  ZERO-DEPENDENCY CLAIM BROKEN"
exit $fail
