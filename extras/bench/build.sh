#!/bin/sh
# bench/build.sh — build the browser product into ../build/bench.html
set -e
cd "$(dirname "$0")/.."
mkdir -p build

# Apple's system clang has no wasm32 backend, and clang needs wasm-ld to
# link. Put Homebrew's lld on PATH if present, then probe for a clang that
# can actually compile AND link a wasm32 object. Override with CLANG=<path>.
[ -d /opt/homebrew/opt/lld/bin ]  && PATH="/opt/homebrew/opt/lld/bin:$PATH"
[ -d /usr/local/opt/lld/bin ]     && PATH="/usr/local/opt/lld/bin:$PATH"
export PATH

CC="${CLANG:-}"
if [ -z "$CC" ]; then
  for c in clang /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang; do
    if echo 'int x;' | "$c" --target=wasm32 -nostdlib -ffreestanding \
         -Wl,--no-entry -x c -o /dev/null - 2>/dev/null; then
      CC="$c"; break
    fi
  done
fi
if [ -z "$CC" ]; then
  echo "error: no clang that can build wasm32 found (brew install llvm lld, or set CLANG=)" >&2
  exit 1
fi

"$CC" --target=wasm32 -O2 -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
  -Wl,-z,stack-size=32768 -Wl,--initial-memory=1114112 \
  -o build/iris.wasm ports/wasm/wasm_shim.c
python3 -c "import base64;w=base64.b64encode(open('build/iris.wasm','rb').read()).decode();\
open('build/bench.html','w').write(open('bench/page.html').read().replace('__WASM_B64__',w))"
echo "built build/bench.html"
