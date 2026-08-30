#!/bin/sh
# The zero-dependency claim, checked on the chip's OWN compiler.
#
# tools/check-claims.sh and CI both test this with host compilers, where the
# answer is "zero undefined symbols" and the ESP32's answer is different: its
# floating-point unit has no divide instruction, so every float division is a
# libgcc call. That was marked UNVERIFIED in the header for three days; this is
# what un-marks it, and it re-checks on every run instead of trusting a note.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
XT=$(find "$HOME/Library/Arduino15/packages/esp32" -name 'xtensa-esp32s3-elf-gcc' -type f 2>/dev/null | head -1)
NM=$(find "$HOME/Library/Arduino15/packages/esp32" -name 'xtensa-esp32s3-elf-nm'  -type f 2>/dev/null | head -1)
[ -n "$XT" ] || { echo "  SKIP  no ESP32 toolchain installed (install the esp32 core)"; exit 0; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cat > "$W/p.c" <<'EOF'
#define IRIS_IMPLEMENTATION
#include "iris.h"
static unsigned char m[IRIS_ARENA(2,12,3,32)];
volatile float SINK; iris *K; float IN[2], OUT[3];
void use(void){
  K = iris_init(m,sizeof m,2,12,3,32,1234u);
  iris_record(K,IN,OUT); iris_train(K); iris_predict(K,IN,OUT); SINK=OUT[0];
  iris_delete_last(K); iris_clear(K); SINK += (float)iris_get_status(K);
}
EOF
fail=0
for O in -O0 -O2 -Os; do
  "$XT" -std=c99 $O -ffreestanding -fno-stack-protector -I"$ROOT" -c "$W/p.c" -o "$W/p.o" 2>/dev/null
  GOT=$("$NM" -u "$W/p.o" | sed 's/^ *U *//' | sort -u | tr '\n' ' ' | sed 's/ *$//')
  # A SUBSET test, not an exact match. At -O0 the compiler fills arrays inline
  # and never calls memset; at -O2 it does. Requiring the exact same set made
  # this check fail on the library for a difference that is purely the
  # optimiser's, which is a check crying wolf about its own strictness.
  # ALLOWED is what the playing path may legitimately need on this target:
  # single-precision divide (the FPU has no divide instruction), the compiler's
  # own block-memory helpers, and sqrtf.
  ALLOWED=" __divsf3 memcpy memmove memset sqrtf "
  bad=""
  for sym in $GOT; do
    case "$ALLOWED" in *" $sym "*) ;; *) bad="$bad $sym" ;; esac
  done
  if [ -z "$bad" ]; then
    printf "  PASS  playing path, xtensa %-4s  %s\n" "$O" "$GOT"
  else
    printf "  FAIL  playing path, xtensa %-4s  unexpected:%s\n" "$O" "$bad"
    printf "        A __*df3 / __*df2 / __floatsidf here means a DOUBLE reached\n"
    printf "        the playing path, which rule 3 in the masthead forbids.\n"
    fail=1
  fi
done
[ "$fail" = "0" ] || exit 1
echo "  the playing path pulls in no doubles on the target."
