# codebase-memory-mcp — Local Build Notes

> **Operational issues to fix** (observed in real multi-agent use, 2026-06-18) → see
> [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md): (1) `project` arg should default to the
> sole/monolith project instead of 404-ing on unknown/short names (`resolve_store()` in
> `src/mcp/mcp.c`); (2) `search_graph` is slow under concurrent multi-agent load — likely
> serial request dispatch in the stdio loop.

## Build (Windows via WSL cross-compile)

WSL distro: `Ubuntu-24.04`. MinGW toolchain: `x86_64-w64-mingw32-gcc` / `-g++`.

```bash
# In WSL Ubuntu-24.04:
cd /mnt/c/Users/Max/source/repos/codebase-memory-mcp
touch internal/cbm/extract_type_refs.c   # force recompile; Makefile default target is 'build/c' dir
make -f Makefile.cbm cbm CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ -j4
```

Output: `build/c/codebase-memory-mcp.exe`

## Build with embedded UI (graph visualization)

The default `cbm` build links a UI **stub** (`embedded_stub.c`) — the HTTP/graph
server is compiled in but serves nothing. To get the real graph UI you must
embed the built frontend. The stock `cbm-with-ui` Makefile target assumes a
native Linux/mac build (runs `npm` itself, embeds via `ld -r -b binary` → ELF),
neither of which works in the WSL→MinGW cross-compile. Use the two-step flow:

```powershell
# Step 1 — build the frontend ON WINDOWS (WSL has no Linux node; fnm node is on
# the Windows PATH). Produces graph-ui/dist.
cd graph-ui ; npm ci ; npm run build ; cd ..
```

```bash
# Step 2 — embed + cross-compile, IN WSL Ubuntu-24.04 from the repo root:
bash scripts/build-ui-mingw.sh
```

Output: `build/c/cbm-ui.exe` (the no-UI `cbm` binary + ~1.4 MB of embedded
assets). The script builds the prod object files first if the tree is cold
(~10-15 min), then embeds and links; warm rebuilds are ~2-3 min.

### Deploy the UI build

```powershell
# Rename-in-place (running MCP procs lock the file; renaming a running image is
# allowed, so this needs no process kills — new binary takes effect on next start):
$dst = "$env:LOCALAPPDATA\codebase-memory-mcp\codebase-memory-mcp.exe"
Move-Item $dst "$dst.old" -Force
Copy-Item build\c\cbm-ui.exe $dst -Force
```

The UI is gated on a persisted config flag. Enable it once (writes
`%USERPROFILE%\.cache\codebase-memory-mcp\config.json`):

```json
{ "ui_enabled": true, "ui_port": 9749 }
```

Or pass `--ui=true --port=9749` on any invocation (the flags persist to that
config). Restart Claude Code; the MCP server starts the UI on a background
thread and serves it at **http://localhost:9749**. To try the binary standalone
without the MCP host, keep stdin open so the MCP stdio loop doesn't hit EOF:

```bash
sleep 3600 | ./build/c/cbm-ui.exe --ui=true --port=9749   # then open the URL
```

### UI build gotchas

- **Frontend must be built first** — `build-ui-mingw.sh` errors out if
  `graph-ui/dist` is missing. It does not run `npm` (no Linux node in WSL).
- **ELF vs PE embedding** — the script forces `embed-frontend.sh` down its
  portable C-byte-array path (`IS_LINUX=false`) so the MinGW CC emits COFF
  objects. Letting it auto-detect Linux yields ELF objects the linker rejects.
- **Output-file lock** — Windows/AV may briefly lock the freshly-written exe;
  the script links to `cbm-ui.exe` (a fresh name) to avoid `ld: cannot open
  output file ...: Permission denied`. Deploy by copying that file.
- **`*.sh` line endings** — `.gitattributes` pins shell scripts to LF; a CRLF
  checkout breaks the shebang in WSL. Run scripts as `bash scripts/<x>.sh`.

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

See `~/.claude/docs/codebase-memory-mcp.md` — the "Cypher engine quirks" section is verified
against the currently-deployed binary (which constructs work vs. throw parse errors), alongside the
node-label / edge-type legends.
