#!/usr/bin/env bash
# Cross-compile the Windows binary in WSL with llvm-mingw (clang + UCRT), as upstream's MSYS2 CLANG64 build does.
# Ubuntu's x86_64-w64-mingw32-gcc links msvcrt.dll; that build indexes nothing (stage lock_failed errno=22).
# usage: bash scripts/build-win-ucrt.sh <version> [clean]
#   needs ~/toolchains/llvm-mingw-*-ucrt-ubuntu-*-x86_64 and zlib-1.3.1.tar.gz in $ZLIB_TGZ (first run only)
set -euo pipefail
VER="${1:?version, e.g. 0.11.0-fleethd.1}"
TC="$(ls -d ~/toolchains/llvm-mingw-*-ucrt-ubuntu-*-x86_64 | tail -1)"
export PATH="$TC/bin:$PATH"
if [ ! -f "$TC/x86_64-w64-mingw32/lib/libz.a" ]; then
  : "${ZLIB_TGZ:?set ZLIB_TGZ to zlib-1.3.1.tar.gz}"
  rm -rf ~/zbuild && mkdir -p ~/zbuild && tar -xzf "$ZLIB_TGZ" -C ~/zbuild
  make -C ~/zbuild/zlib-1.3.1 -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-clang AR=llvm-ar RC=x86_64-w64-mingw32-windres libz.a >/dev/null
  cp ~/zbuild/zlib-1.3.1/libz.a "$TC/x86_64-w64-mingw32/lib/"
  cp ~/zbuild/zlib-1.3.1/zlib.h ~/zbuild/zlib-1.3.1/zconf.h "$TC/x86_64-w64-mingw32/include/"
fi
cd "$(dirname "$0")/.."
[ "${2:-}" = clean ] && rm -rf build/c
make -f Makefile.cbm cbm CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++ CXX_STDLIB=-lc++ \
  CFLAGS_EXTRA="-DCBM_VERSION=\\\"$VER\\\"" -j"$(nproc)"
imports="$(x86_64-w64-mingw32-objdump -p build/c/codebase-memory-mcp.exe)"
[[ "$imports" == *api-ms-win-crt-runtime* ]] || { echo "FAIL: binary does not link UCRT"; exit 1; }
echo "OK: build/c/codebase-memory-mcp.exe ($VER, UCRT)"
