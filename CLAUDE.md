# codebase-memory-mcp — Local Build Notes

## Build (Windows via WSL cross-compile)

WSL distro: `Ubuntu-24.04`. MinGW toolchain: `x86_64-w64-mingw32-gcc` / `-g++`.

```bash
# In WSL Ubuntu-24.04:
cd /mnt/c/Users/Max/source/repos/codebase-memory-mcp
touch internal/cbm/extract_type_refs.c   # force recompile; Makefile default target is 'build/c' dir
make -f Makefile.cbm cbm CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ -j4
```

Output: `build/c/codebase-memory-mcp.exe`

## Deploy

```powershell
# Kill running MCP proc first (file is locked while running):
#   wmic process where "ExecutablePath like '%codebase-memory%'" get ProcessId
#   Stop-Process -Id <pid> -Force
Copy-Item build\c\codebase-memory-mcp.exe `
    "$env:LOCALAPPDATA\codebase-memory-mcp\codebase-memory-mcp.exe" -Force
```

Restart Claude Code to reconnect the MCP server.

## Re-index after binary swap

Extraction changes require a fresh index (delta detection sees unchanged source files):

```bash
./build/c/codebase-memory-mcp.exe cli delete_project '{"project":"C-Users-Max-source-repos"}'
./build/c/codebase-memory-mcp.exe cli index_repository '{"repo_path":"C:/Users/Max/source/repos","project":"C-Users-Max-source-repos"}'
```

CLI JSON field names: `project` (not `project_name`), `repo_path` (not `path`).

## Local patches

See `docs/LOCAL_PATCHES.md`. Current divergence from upstream (DeusData v0.6.0):

- C# class-level property/field type refs (`extract_type_refs.c`)
- C# method-body type refs for `object_creation_expression`, `typeof_expression`, `cast_expression`

## Makefile gotchas

- Default target is the `$(BUILD_DIR)` directory (`build/c`), which exists → `make -f Makefile.cbm` with no args says "up to date". Always pass `cbm` target explicitly.
- Production binary is a unity build (all sources linked in one gcc call), not per-file object compilation, so changing one source rebuilds the whole binary (~2–3 min).
- On MinGW the output is `codebase-memory-mcp.exe` but the Makefile target name is `codebase-memory-mcp` (no .exe) — make never sees the target as "existing", so it always rebuilds when invoked.

## Cypher engine quirks (affect MCP usage, not the build)

See `C:\Users\Max\.claude\projects\C--Users-Max-source-repos\memory\project_cbm_cypher.md`.
