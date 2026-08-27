#!/bin/sh
# iris — build everything three ways from one core.
#   ./build.sh audit        run the twenty-five correctness checks
#   ./build.sh mpe          run the MPE sink's byte-level checks
#   ./build.sh sinks        run the CC and OSC sinks' byte-level checks
#                           (CC is the DEFAULT sink; see ports/cc/iris_cc.h)
#   ./build.sh experiment   map out when rerolling changes the instrument
#   ./build.sh bench        rebuild the browser prototype (bench.html)
#   ./build.sh golden       REGENERATE the golden v1 baseline (deliberate
#                           act only — the audit compares against these bytes;
#                           regenerating them re-defines the baseline)
#
# The core (iris.h) is never compiled differently. Only the shim changes.
set -e
mkdir -p build
CFLAGS="-O2 -Wall -Wextra -Wno-unused-function"

case "${1:-audit}" in
  audit)
    cc $CFLAGS -o build/audit tests/audit.c -lm && ./build/audit
    # Guards are inert on healthy runs — provable only across two builds:
    # core with guards vs core with -DEW_NO_GUARDS, same recipe, same bits.
    cc $CFLAGS -o build/guards_ab tests/guards_ab.c -lm
    cc $CFLAGS -DEW_NO_GUARDS -o build/guards_ab_ng tests/guards_ab.c -lm
    G=$(./build/guards_ab); N=$(./build/guards_ab_ng)
    if [ "$G" = "$N" ]; then
      echo "PASS  guards are inert on healthy runs           $G == no-guards build"
    else
      echo "FAIL  guards are inert on healthy runs           guarded: $G  no-guards: $N"
      exit 1
    fi ;;
  mpe)
    # The MPE sink is platform-free on purpose: no USB, no board, no serial
    # port. It emits complete MIDI messages into a caller-supplied iris_bytes,
    # so every byte it will ever put on the wire can be asserted here.
    cc $CFLAGS -o build/mpe_test tests/mpe_test.c \
       ports/mpe/iris_mpe.c ports/mpe/iris_mpe_wire.c -lm && ./build/mpe_test
    # The 32-bit struct sizes the port claims, asserted on a real 32-bit
    # target rather than halved by hand from this 64-bit host.
    clang --target=wasm32 -fsyntax-only ports/mpe/iris_mpe_wire.c \
      && echo "PASS  32-bit sizes: bytes 8, desc 12, sink 36, voice 18, pool 132, mpe 276" ;;
  sinks)
    # Same idiom as `mpe`, same reason: these ports have no USB, no board and
    # no serial port in them, so every byte they will ever emit is asserted
    # here, on a laptop, with a transport that can be told to refuse.
    cc $CFLAGS -o build/cc_test  tests/cc_test.c  ports/cc/iris_cc.c   -lm && ./build/cc_test
    cc $CFLAGS -o build/osc_test tests/osc_test.c ports/osc/iris_osc.c -lm && ./build/osc_test
    # The 32-bit struct sizes the ports claim, asserted on a real 32-bit
    # target rather than halved by hand from this 64-bit host. The template
    # sink is compiled too: the file students copy must never be broken.
    clang --target=wasm32 -std=c99 -fsyntax-only ports/cc/iris_cc.c \
      && clang --target=wasm32 -std=c99 -fsyntax-only ports/osc/iris_osc.c \
      && clang --target=wasm32 -std=c99 -fsyntax-only ports/template/iris_yoursink.c \
      && echo "PASS  32-bit sizes: cc_cfg 28, cc 224, osc_cfg 20, osc 480; template builds" ;;
  experiment) cc $CFLAGS -o build/experiment tests/experiment.c -lm && ./build/experiment ;;
  bench)
    clang --target=wasm32 -O2 -nostdlib -ffreestanding \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
      -Wl,-z,stack-size=32768 -Wl,--initial-memory=1114112 \
      -o build/iris.wasm ports/wasm/wasm_shim.c
    python3 -c "import base64;w=base64.b64encode(open('build/iris.wasm','rb').read()).decode();\
open('build/bench.html','w').write(open('bench/page.html').read().replace('__WASM_B64__',w))"
    echo "built build/bench.html — open it in a browser" ;;
  golden)     cc $CFLAGS -o build/make_golden tests/golden/make_golden.c -lm \
                && ./build/make_golden ;;
  clean)      rm -rf build ;;
  *)          echo "usage: ./build.sh [audit|mpe|sinks|experiment|bench|golden|clean]"; exit 1 ;;
esac
