#!/bin/sh
# tests/targets.sh -- iris.h builds for every processor it is meant for, and
# refuses the one whose float arithmetic it cannot pin.
#
# RUN (from anywhere; compile only, nothing is executed, nothing is written
# outside a temporary directory):
#     sh tests/targets.sh
# The exit status is non-zero if any check fails. A compiler that is not
# installed prints SKIP; a SKIP is not a pass for that target.
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
XT=$HOME/Library/Arduino15/packages/esp32/tools/esp-x32/2507/bin/xtensa-esp32s3-elf-gcc
CM=/Applications/ARM/bin/arm-none-eabi-gcc
AVR=$HOME/Library/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin/avr-gcc
echo "wider float evaluation: refused where it happens, silent everywhere else"
target BUILDS  "Apple clang, this machine, C"               cc -x c -std=c99 -O2
target BUILDS  "Apple clang, this machine, C++"             cc -x c++ -O2
target BUILDS  "clang 22, this machine, C"                  "$LLVM" -x c -std=c99 -O2
target BUILDS  "clang 22, this machine, C++"                "$LLVM" -x c++ -O2
target BUILDS  "gcc-15, this machine, C"                    gcc-15 -x c -std=gnu17 -O2
target BUILDS  "gcc-15, this machine, C++"                  gcc-15 -x c++ -std=gnu++17 -O2
target BUILDS  "gcc-15, Cortex-A76 in GNU C (reports 16)"   gcc-15 -x c -std=gnu17 -mcpu=cortex-a76 -O2
target BUILDS  "Apple clang, x86-64 macOS"                  cc -arch x86_64 -x c -std=c99 -O2
target BUILDS  "clang 22, x86-64 Linux"                     "$LLVM" --target=x86_64-linux-gnu -x c -std=c99 -O2
target BUILDS  "clang 22, 32-bit x86 with SSE arithmetic"   "$LLVM" --target=i686-linux-gnu -msse2 -mfpmath=sse -x c -std=c99 -O2
target BUILDS  "clang 22, WebAssembly (wasm32)"             "$LLVM" --target=wasm32 -x c -std=c99 -O2
target BUILDS  "arm-none-eabi-gcc, Cortex-M4, C"            "$CM" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -x c -std=gnu17 -O2
target BUILDS  "arm-none-eabi-gcc, Cortex-M4, C++"          "$CM" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -x c++ -std=gnu++17 -O2
target BUILDS  "xtensa-esp32s3-elf-gcc, C"                  "$XT" -mlongcalls -x c -std=gnu17 -Os
target BUILDS  "xtensa-esp32s3-elf-gcc, C++ (Arduino mode)" "$XT" -mlongcalls -x c++ -std=gnu++2a -Os
target BUILDS  "avr-gcc 7.3.0, ATmega328P, C"               "$AVR" -mmcu=atmega328p -x c -std=gnu11 -Os
target BUILDS  "avr-gcc 7.3.0, ATmega328P, C++"             "$AVR" -mmcu=atmega328p -x c++ -std=gnu++11 -Os
target BUILDS  "clang 22, Cortex-M7, C"                     "$LLVM" --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c -std=c99 -O2
target BUILDS  "clang 22, Cortex-M7, C++"                   "$LLVM" --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c++ -nostdinc++ -O2
target BUILDS  "Apple clang, Cortex-M7, C"                  cc --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfloat-abi=hard -x c -std=c99 -O2
target BUILDS  "clang 22, 32-bit RISC-V (rv32imafc)"        "$LLVM" --target=riscv32-unknown-elf -march=rv32imafc -mabi=ilp32f -x c -std=c99 -O2
target BUILDS  "clang 22, ATmega328P"                       "$LLVM" --target=avr -mmcu=atmega328p -x c -std=c99 -Os
target REFUSES "clang 22, 32-bit x86 with x87 arithmetic"   "$LLVM" --target=i686-linux-gnu -mno-sse -mfpmath=387 -x c -std=c99 -O2
target REFUSES "Apple clang, 32-bit x86 with x87 arithmetic" cc --target=i686-linux-gnu -mno-sse -mfpmath=387 -x c -std=c99 -O2
target REFUSES "clang 22, -ffp-eval-method=double"          "$LLVM" -ffp-eval-method=double -x c -std=c99 -O2
target REFUSES "clang 22, -ffp-eval-method=extended"        "$LLVM" -ffp-eval-method=extended -x c -std=c99 -O2
[ "$fail" = 0 ] && echo "  every target as expected" || echo "  TARGET CHECK FAILED"
exit $fail
