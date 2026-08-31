#!/bin/sh
# A FAILED COMPILE MUST FAIL THE TARGET.
#
# Every arm here was written as `cc ... && ./build/thing`. When cc failed the &&
# short-circuited, the script carried on to the next line, and the arm exited 0
# with none of its checks having run. `sh build.sh audit` returned success with
# all 41 checks silently absent -- measured. Nine arms shared the shape.
#
# set -e makes any failing command end the run, which is the behaviour every
# caller already assumed: CI, tools/mutate.sh, and every "exit 0" this project
# has ever quoted as evidence.
set -e

# iris — build everything three ways from one core.
#   ./build.sh audit        run the correctness checks (41)
#   ./build.sh claims       verify the docs still match the code
#   ./build.sh mpe          run the MPE sink's byte-level checks
#   ./build.sh sinks        run the CC and OSC sinks' byte-level checks
#                           (CC is the DEFAULT sink; see extras/ports/cc/iris_cc.h)
#   ./build.sh experiment   map out when rerolling changes the instrument
#   ./build.sh bench        rebuild the browser prototype (bench.html)
#   ./build.sh golden       REGENERATE the golden v1 baseline (deliberate
#                           act only — the audit compares against these bytes;
#                           regenerating them re-defines the baseline)
#
# The core (iris.h) is never compiled differently. Only the shim changes.
set -e
mkdir -p build
# -Wno-unused-function is GONE (2026-08-27). It was suppressing the compiler's
# own dead-code detector, which is how 36 lines of dead code sat unnoticed until
# a 23-agent audit found them. IRIS_API is `static inline` now, so unused
# definitions in a header no longer warn and the flag is not needed.
CFLAGS="-O2 -Wall -Wextra"

case "${1:-audit}" in
  mutate)     # Does every check actually protect what it claims to? See tools/.
              sh tools/mutate.sh "${2:-}" ;;
  bloat)      # Is the file getting harder to read? A ratchet, not a report.
              sh tools/bloat.sh ;;
  sketches)   # Do the students' copies of iris.h still match this one?
              # Each sketch folder carries its own copy so a student needs no
              # install step, and those copies drift. All ten were once a
              # version behind while every suite here was green -- the students'
              # code was missing a fix this repository had already made. A
              # commit message promised to wire this in and did not; that is
              # what this arm is.
              ST=${IRIS_STARTER:-../iris-esp32-starter}
              if [ -f "$ST/sync-iris.sh" ]; then
                ( cd "$ST" && sh sync-iris.sh )
              else
                echo "  SKIP  no starter repo at $ST (set IRIS_STARTER)"
              fi ;;
  target)     # The zero-dependency claim, on the CHIP's compiler, not the host.
              sh tools/freestanding-esp32.sh ;;
  tu)         # Two translation units disagreeing about IRIS_MAX_*, which is a
              # thing iris.h documents doing. Under a sanitizer, because the
              # failure it guards was a stack overwrite, not a wrong answer.
              cc -std=c99 -O1 -g -fsanitize=address,undefined \
                 -fno-sanitize-recover=all -I. -c tests/tu/big.c -o build/tu_big.o
              cc -std=c99 -O1 -g -fsanitize=address,undefined \
                 -fno-sanitize-recover=all -I. -c tests/tu/small.c -o build/tu_small.o
              cc -fsanitize=address,undefined build/tu_small.o build/tu_big.o \
                 -o build/tu -lm
              ./build/tu ;;
  coverage)   # The refusal paths. Every case asks a function to say no.
              cc $CFLAGS -o build/coverage tests/coverage.c -lm
              ./build/coverage ;;
  claims)
    # Do the documents still tell the truth about the code? See tools/.
    sh tools/check-claims.sh
    ;;
  audit)
    cc $CFLAGS -o build/audit tests/audit.c -lm
    ./build/audit
    # Guards are inert on healthy runs — provable only across two builds:
    # core with guards vs core with -DIRIS_NO_GUARDS, same recipe, same bits.
    cc $CFLAGS -o build/guards_ab tests/guards_ab.c -lm
    cc $CFLAGS -DIRIS_NO_GUARDS -o build/guards_ab_ng tests/guards_ab.c -lm
    G=$(./build/guards_ab | head -1);      N=$(./build/guards_ab_ng | head -1)
    GP=$(./build/guards_ab | tail -1);     NP=$(./build/guards_ab_ng | tail -1)
    if [ "$G" = "$N" ]; then
      echo "PASS  guards are inert on healthy runs           $G == no-guards build"
    else
      echo "FAIL  guards are inert on healthy runs           guarded: $G  no-guards: $N"
      exit 1
    fi
    # POSITIVE CONTROL. The equality above is also what you get when the flag
    # does nothing, so it cannot detect its own defeat -- renaming the macro in
    # the header left it passing. This asserts the flag actually reaches the
    # code: on a poisoned demonstration the two builds MUST disagree.
    if [ "$GP" != "$NP" ]; then
      echo "PASS  the guards flag actually reaches the code  guarded: $GP / no-guards: $NP"
    else
      echo "FAIL  the guards flag is inert -- IRIS_NO_GUARDS reached nothing.  both: $GP"
      exit 1
    fi ;;
  mpe)
    # The MPE sink is platform-free on purpose: no USB, no board, no serial
    # port. It emits complete MIDI messages into a caller-supplied iris_bytes,
    # so every byte it will ever put on the wire can be asserted here.
    cc $CFLAGS -I. -o build/mpe_test extras/tests/mpe_test.c \
       extras/ports/mpe/iris_mpe.c extras/ports/mpe/iris_mpe_wire.c -lm
    ./build/mpe_test
    # The 32-bit struct sizes the port claims, asserted on a real 32-bit
    # target rather than halved by hand from this 64-bit host.
    clang --target=wasm32 -I. -fsyntax-only extras/ports/mpe/iris_mpe_wire.c \
      && echo "PASS  32-bit sizes: bytes 8, desc 12, sink 36, voice 18, pool 132, mpe 276" ;;
  sinks)
    # Same idiom as `mpe`, same reason: these ports have no USB, no board and
    # no serial port in them, so every byte they will ever emit is asserted
    # here, on a laptop, with a transport that can be told to refuse.
    cc $CFLAGS -I. -o build/cc_test  extras/tests/cc_test.c  extras/ports/cc/iris_cc.c   -lm
    ./build/cc_test
    cc $CFLAGS -I. -o build/osc_test extras/tests/osc_test.c extras/ports/osc/iris_osc.c -lm
    ./build/osc_test
    # The 32-bit struct sizes the ports claim, asserted on a real 32-bit
    # target rather than halved by hand from this 64-bit host. The template
    # sink is compiled too: the file students copy must never be broken.
    clang --target=wasm32 -std=c99 -I. -fsyntax-only extras/ports/cc/iris_cc.c \
      && clang --target=wasm32 -std=c99 -I. -fsyntax-only extras/ports/osc/iris_osc.c \
      && clang --target=wasm32 -std=c99 -I. -fsyntax-only extras/ports/template/iris_yoursink.c \
      && echo "PASS  32-bit sizes: cc_cfg 28, cc 224, osc_cfg 20, osc 480; template builds" ;;
  experiment) cc $CFLAGS -o build/experiment tests/experiment.c -lm
              ./build/experiment ;;
  bench)
    clang --target=wasm32 -O2 -nostdlib -ffreestanding \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
      -Wl,-z,stack-size=32768 -Wl,--initial-memory=1114112 \
      -o build/iris.wasm extras/ports/wasm/wasm_shim.c
    python3 -c "import base64;w=base64.b64encode(open('build/iris.wasm','rb').read()).decode();\
open('build/bench.html','w').write(open('extras/bench/page.html').read().replace('__WASM_B64__',w))"
    echo "built build/bench.html — open it in a browser" ;;
  golden)
    # DESTRUCTIVE. This rewrites the frozen baselines the audit compares against,
    # so a regression it should have caught becomes the new "correct" answer.
    # It happened: on 2026-08-27 an automated run regenerated v3-instrument.bin
    # as a v4 file, and the backward-compatibility check went from PASS to a
    # silent FAIL that looked like a code defect. Recovered from git.
    # Refuse unless the caller says so out loud.
    if [ "$IRIS_REGENERATE_GOLDEN" != "yes-i-mean-it" ]; then
      echo "REFUSED: 'golden' overwrites the frozen test baselines in tests/golden/."
      echo "The audit compares against those files; regenerating them re-defines"
      echo "what 'correct' means and silently erases whatever they would have caught."
      echo
      echo "If that is genuinely what you want:"
      echo "  IRIS_REGENERATE_GOLDEN=yes-i-mean-it sh build.sh golden"
      echo
      echo "Commit first, so 'git checkout -- tests/golden' can undo it."
      exit 1
    fi
    cc $CFLAGS -o build/make_golden tests/golden/make_golden.c -lm
      ./build/make_golden ;;
  clean)      rm -rf build ;;
  tiny)       cc $CFLAGS -o build/tiny docs/tiny.c -lm
              ./build/tiny ;;
  regressions)
              # One test per reviewed defect, each written before its fix and
              # watched to fail. Non-zero exit if any regresses.
              cc $CFLAGS -I. -o build/regressions tests/regressions.c -lm
                ./build/regressions ;;
  fuzz)       # Oracle-free: the sanitizers decide, not our assertions.
              cc -std=c99 -O1 -g -fsanitize=address,undefined \
                 -fno-sanitize-recover=all -I. -o build/fuzz tests/fuzz.c -lm
                ./build/fuzz "${2:-400}" ;;
  *)          echo "usage: ./build.sh [audit|mpe|sinks|claims|fuzz|regressions|mutate|experiment|tiny|bench|golden|clean]"; exit 1 ;;
esac
