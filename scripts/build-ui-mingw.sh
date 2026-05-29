#!/bin/bash
#
# build-ui-mingw.sh — Build the codebase-memory-mcp binary WITH the embedded
# graph UI, cross-compiled for Windows from WSL (Ubuntu) via MinGW.
#
# This is the cross-compile counterpart of the Makefile `cbm-with-ui` target.
# The stock target runs `embed: frontend` (npm) and uses `ld -r -b binary`
# (ELF-only) for embedding — neither works in the WSL→MinGW flow. This script
# handles the two cross-compile-specific differences:
#
#   1. Frontend assets must already be built (see step 1 in CLAUDE.md). WSL has
#      no Linux `node`; the frontend is built on Windows with `npm run build`.
#   2. embed-frontend.sh auto-selects `ld -r -b binary` on Linux, which emits
#      ELF objects the MinGW linker rejects. We force its portable path (C byte
#      arrays compiled by the MinGW CC) regardless of host OS.
#
# It also sidesteps a transient Windows/AV lock on the existing
# `codebase-memory-mcp.exe` by linking to a fresh `cbm-ui.exe`.
#
# Usage (in WSL Ubuntu-24.04, from the repo root):
#   bash scripts/build-ui-mingw.sh
#
# Output: build/c/cbm-ui.exe   (deploy this as codebase-memory-mcp.exe)
#
set -euo pipefail

CC="${CC:-x86_64-w64-mingw32-gcc}"
CXX="${CXX:-x86_64-w64-mingw32-g++}"

# Resolve repo root (this script lives in scripts/).
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DIST="graph-ui/dist"
OUT="build/c/cbm-ui.exe"

# ── 1. Frontend assets must exist ───────────────────────────────────────────
if [[ ! -f "$DIST/index.html" ]]; then
    echo "ERROR: $DIST/index.html not found." >&2
    echo "Build the frontend first (on Windows, which has node):" >&2
    echo "    cd graph-ui && npm ci && npm run build" >&2
    exit 1
fi

# ── 2. Make sure the vendored/prod object files exist ───────────────────────
# A fresh tree has no build/c/prod_*.o; `cbm` (no-UI) builds them all. This is
# the long pole on a clean tree (~10-15 min); a no-op once warm.
if [[ ! -f build/c/prod_mimalloc.o ]]; then
    echo "==> prod objects missing — running 'make cbm' to build them"
    touch internal/cbm/extract_type_refs.c   # Makefile default target quirk; force a real target
    make -f Makefile.cbm cbm CC="$CC" CXX="$CXX" -j4
fi

# ── 3. Embed the built frontend as PE/COFF objects ──────────────────────────
# Force embed-frontend.sh down its portable (xxd-free C byte-array) path so the
# MinGW CC produces COFF objects, not ELF. We run a normalized copy (CRLF
# stripped) so the script works regardless of how it was checked out.
echo "==> embedding $DIST"
EMBED=/tmp/embed-frontend.mingw.sh
tr -d '\r' < scripts/embed-frontend.sh \
    | sed 's/^    IS_LINUX=true/    IS_LINUX=false/' > "$EMBED"
CC="$CC" bash "$EMBED" "$DIST" build/c/embedded

# ── 4. Capture the cbm-with-ui link command and run it ──────────────────────
# `make -n` prints the recipe (embed objects now exist → the $(wildcard ...)
# resolves). We extract just the gcc link line, retarget the output to
# $OUT (fresh name avoids a lock on the deployed exe), and run it.
echo "==> linking $OUT"
make -f Makefile.cbm -n cbm-with-ui CC="$CC" CXX="$CXX" 2>/dev/null | tr -d '\r' > /tmp/cbm-ui-dryrun.txt
awk '/^x86_64-w64-mingw32-gcc/{f=1} /^echo "Built with UI/{f=0} f' /tmp/cbm-ui-dryrun.txt \
    | sed "s# -o build/c/codebase-memory-mcp # -o build/c/cbm-ui #" > /tmp/cbm-ui-link.sh

if [[ ! -s /tmp/cbm-ui-link.sh ]]; then
    echo "ERROR: failed to capture link command. Dry-run head:" >&2
    sed -n '1,8p' /tmp/cbm-ui-dryrun.txt >&2
    exit 1
fi

rm -f "$OUT"
bash /tmp/cbm-ui-link.sh

echo ""
echo "Built with UI: $OUT"
ls -la "$OUT"
echo ""
echo "Deploy + run: see the 'Build with embedded UI' section of CLAUDE.md"
