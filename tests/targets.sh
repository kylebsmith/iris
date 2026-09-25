#!/bin/sh
# tests/targets.sh -- iris.h builds for every processor it is meant for, and
# refuses the one whose float arithmetic it cannot pin.
#
# RUN (from anywhere; compile only, nothing is executed, nothing is written
# outside a temporary directory):
#     sh tests/targets.sh            or   sh build.sh targets
# The exit status is non-zero if any check fails. A compiler that is not
# installed prints SKIP; a SKIP is not a pass for that target.
#
# THE COMPILERS. The host rows use CC alone when it is set, and otherwise
# each distinct compiler among cc, Homebrew's clang, clang, gcc-15 and gcc.
# The cross rows find their compilers: a clang with every code generator
# (Homebrew's, or a clang on the PATH that is not Apple's, which lacks
# several), arm-none-eabi-gcc on the PATH or in /Applications/ARM/bin,
# xtensa-esp32s3-elf-gcc and avr-gcc from an installed Arduino core
# (~/Library/Arduino15 or ~/.arduino15) or the PATH. IRIS_REQUIRE lists
# families that must be present, so that continuous integration fails
# rather than skips when an install step breaks: any of "cross-clang",
# "arm-none-eabi", "xtensa" and "avr".
#
# WHAT IT CHECKS. iris.h refuses to compile when __FLT_EVAL_METHOD__ says
# float arithmetic is carried in a wider format than float (see "2. Wider
# intermediate results" in its determinism contract). That #error is only
# worth having if it fires where it should and nowhere else, so this script
# compiles one small translation unit (the playing and training calls a sketch
# makes) for each target below.
#   BUILDS   must compile with no error and no warning under -Wall -Wextra.
#            The warning half matters: clang ignores some pragmas on some
#            processors (float_control on 32-bit ARM, WebAssembly and AVR)
#            and says so in every build, so the clang cross targets are here
#            too.
#   REFUSES  must fail, and the failure must be iris's own #error -- a build
#            that fails for any other reason (a missing target, a missing
#            header) is reported as a failure of this script, not as a pass.
# Every target is compiled -ffreestanding, because iris.h needs only
# <stddef.h> and <stdint.h> and the cross targets here have no C library.
# gcc -m32, 32-bit x86 in GCC's default x87 mode, belongs under REFUSES; no
# 32-bit x86 GCC is installed here, so it is not in the list.
set -u
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cat > "$T/unit.c" <<'UNIT'
#include "iris.h"
static unsigned char mem[IRIS_ARENA(2, 12, 3, 8)];
float probe(float a, float b);
float probe(float a, float b) {
  float in[2], out[3];
  iris *k = iris_init(mem, sizeof mem, 2, 12, 3, 8, 1234u);
  in[0] = a; in[1] = b; out[0] = a; out[1] = b; out[2] = a;
  iris_record(k, in, out);
  iris_train(k);
  iris_predict(k, in, out);
  return out[0] + iris_novelty(k, in);
}
UNIT
MSG="carries float arithmetic in a wider format"
fail=0
# target <BUILDS|REFUSES> <label> <compiler> <flags...>
target() {
  want=$1; label=$2; cc=$3; shift 3
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf '  SKIP  %-8s %-58s not installed\n' "$want" "$label"; return
  fi
  if "$cc" -ffreestanding -Wall -Wextra "$@" -I"$ROOT" -c "$T/unit.c" -o "$T/unit.o" > "$T/out" 2>&1; then
    got=BUILDS
    if grep -q 'warning:' "$T/out"; then
      printf '  FAIL  %-8s %-58s warning: %s\n' "$want" "$label" \
        "$(grep -m1 'warning:' "$T/out" | sed 's/.*warning: //')"; fail=1; return
    fi
  elif grep -q "$MSG" "$T/out"; then
    got=REFUSES
  else
    printf '  FAIL  %-8s %-58s failed for another reason: %s\n' "$want" "$label" \
      "$(grep -m1 'error' "$T/out")"; fail=1; return
  fi
  if [ "$got" = "$want" ]; then
    printf '  PASS  %-8s %s\n' "$want" "$label"
  else
    printf '  FAIL  %-8s %-58s it %s\n' "$want" "$label" "$(echo $got | tr 'A-Z' 'a-z')"; fail=1
  fi
}
LLVM=/opt/homebrew/opt/llvm/bin/clang
requires() { case " ${IRIS_REQUIRE:-} " in *" $1 "*) return 0 ;; esac; return 1; }
# absent <family> <what>: a SKIP, or a FAIL when IRIS_REQUIRE names the family
absent() {
  if requires "$1"; then printf '  FAIL  %-67s not installed, and IRIS_REQUIRE asks for it\n' "$2"; fail=1
  else printf '  SKIP  %-67s not installed\n' "$2"; fi
}
is_clang() { "$1" --version 2>/dev/null | grep -qi clang; }
is_apple() { "$1" --version 2>/dev/null | head -1 | grep -q '^Apple clang'; }
arduino_tool() {   # arduino_tool <package>/tools/<tool> <program>: the newest installed copy
  find "$HOME/Library/Arduino15/packages/$1" "$HOME/.arduino15/packages/$1" \
       -name "$2" -type f 2>/dev/null | sort | tail -1
}
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
# a clang with every code generator: Homebrew's, or one that is not Apple's
XC=""
for c in "$LLVM" ${CC:-} clang; do
  if command -v "$c" >/dev/null 2>&1 && is_clang "$c" && ! is_apple "$c"; then XC=$c; break; fi
done
CM=$(command -v arm-none-eabi-gcc 2>/dev/null || true)
[ -z "$CM" ] && [ -x /Applications/ARM/bin/arm-none-eabi-gcc ] && CM=/Applications/ARM/bin/arm-none-eabi-gcc
XT=$(arduino_tool esp32/tools xtensa-esp32s3-elf-gcc)
AVR=$(arduino_tool arduino/tools/avr-gcc avr-gcc)
[ -z "$AVR" ] && AVR=$(command -v avr-gcc 2>/dev/null || true)

echo "wider float evaluation: refused where it happens, silent everywhere else"
for cc in $HOSTCC; do
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf '  FAIL  %-67s not installed\n' "$cc (from CC)"; fail=1; continue
  fi
  n=$(basename "$cc"); case "$cc" in /opt/homebrew/*) n="Homebrew $n" ;; esac
  if is_clang "$cc"; then
    target BUILDS "$n, this machine, C"   "$cc" -x c -std=c99 -O2
    target BUILDS "$n, this machine, C++" "$cc" -x c++ -O2
  else
    target BUILDS "$n, this machine, C"   "$cc" -x c -std=gnu17 -O2
    target BUILDS "$n, this machine, C++" "$cc" -x c++ -std=gnu++17 -O2
    case $(uname -m) in arm64|aarch64)
      target BUILDS "$n, Cortex-A76 in GNU C (reports 16)" "$cc" -x c -std=gnu17 -mcpu=cortex-a76 -O2 ;;
    esac
  fi
  if is_apple "$cc"; then
    target BUILDS  "$n, x86-64 macOS"                    "$cc" -arch x86_64 -x c -std=c99 -O2
    target BUILDS  "$n, Cortex-M7, C"                    "$cc" --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c -std=c99 -O2
    target REFUSES "$n, 32-bit x86 with x87 arithmetic"  "$cc" --target=i686-linux-gnu -mno-sse -mfpmath=387 -x c -std=c99 -O2
  fi
done
if [ -n "$XC" ]; then
  n=$(basename "$XC"); case "$XC" in /opt/homebrew/*) n="Homebrew $n" ;; esac
  target BUILDS  "$n, x86-64 Linux"                       "$XC" --target=x86_64-linux-gnu -x c -std=c99 -O2
  target BUILDS  "$n, 32-bit x86 with SSE arithmetic"     "$XC" --target=i686-linux-gnu -msse2 -mfpmath=sse -x c -std=c99 -O2
  target BUILDS  "$n, WebAssembly (wasm32)"               "$XC" --target=wasm32 -x c -std=c99 -O2
  target BUILDS  "$n, Cortex-M7, C"                       "$XC" --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c -std=c99 -O2
  target BUILDS  "$n, Cortex-M7, C++"                     "$XC" --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c++ -nostdinc++ -O2
  target BUILDS  "$n, 32-bit RISC-V (rv32imafc)"          "$XC" --target=riscv32-unknown-elf -march=rv32imafc -mabi=ilp32f -x c -std=c99 -O2
  target BUILDS  "$n, ATmega328P"                         "$XC" --target=avr -mmcu=atmega328p -x c -std=c99 -Os
  target REFUSES "$n, 32-bit x86 with x87 arithmetic"     "$XC" --target=i686-linux-gnu -mno-sse -mfpmath=387 -x c -std=c99 -O2
  target REFUSES "$n, -ffp-eval-method=double"            "$XC" -ffp-eval-method=double -x c -std=c99 -O2
  target REFUSES "$n, -ffp-eval-method=extended"          "$XC" -ffp-eval-method=extended -x c -std=c99 -O2
else
  absent cross-clang "a clang with every code generator (x86, WebAssembly, ARM, RISC-V, AVR)"
fi
if [ -n "$CM" ]; then
  target BUILDS  "arm-none-eabi-gcc, Cortex-M4, C"        "$CM" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -x c -std=gnu17 -O2
  target BUILDS  "arm-none-eabi-gcc, Cortex-M4, C++"      "$CM" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -x c++ -std=gnu++17 -O2
else
  absent arm-none-eabi "arm-none-eabi-gcc (Cortex-M4)"
fi
if [ -n "$XT" ]; then
  target BUILDS  "xtensa-esp32s3-elf-gcc, C"              "$XT" -mlongcalls -x c -std=gnu17 -Os
  target BUILDS  "xtensa-esp32s3-elf-gcc, C++ (Arduino mode)" "$XT" -mlongcalls -x c++ -std=gnu++2a -Os
else
  absent xtensa "xtensa-esp32s3-elf-gcc (ESP32-S3)"
fi
if [ -n "$AVR" ]; then
  target BUILDS  "avr-gcc, ATmega328P, C"                 "$AVR" -mmcu=atmega328p -x c -std=gnu11 -Os
  target BUILDS  "avr-gcc, ATmega328P, C++"               "$AVR" -mmcu=atmega328p -x c++ -std=gnu++11 -Os
else
  absent avr "avr-gcc (ATmega328P)"
fi
[ "$fail" = 0 ] && echo "  every target as expected" || echo "  TARGET CHECK FAILED"
exit $fail
