# codebase-memory-mcp — FleetHd fork

This fork is upstream DeusData v0.11.0 plus the local patches in `docs/LOCAL_PATCHES.md`. Version names use
the form `0.11.0-fleethd.N`. Usage rules for agents (query forms, paging, known engine limits) are in
`~/.claude/docs/codebase-memory-mcp.md`; this file covers build, deploy and release only.

## Build (Windows binary, cross-compiled in WSL)

Use the llvm-mingw toolchain (clang + UCRT) in WSL `Ubuntu-24.04`, at `~/toolchains/llvm-mingw-*-ucrt-ubuntu-*-x86_64`.

> **⚠ Do not build with Ubuntu's `x86_64-w64-mingw32-gcc`.** It links `msvcrt.dll`, and that binary fails every
> index run with `pipeline.stage action=lock_failed errno=22`. Upstream builds with MSYS2 CLANG64, which also links UCRT.

```bash
# In WSL, from the repo root. The first run also builds zlib into the toolchain (set ZLIB_TGZ to zlib-1.3.1.tar.gz).
bash scripts/build-win-ucrt.sh 0.11.0-fleethd.N          # warm build, ~3 min
bash scripts/build-win-ucrt.sh 0.11.0-fleethd.N clean    # cold build
```

The output is `build/c/codebase-memory-mcp.exe`. The script fails if the binary does not link UCRT.
The UI is not built (the binary links `embedded_stub.c`). FleetHd runs with `ui_enabled=false`.

## Deploy

> **⚠ Close every Claude Code session first.** v0.11 runs one account-wide daemon per version and per cache
> folder. A new build refuses to start while any process of another build runs, so a rename-in-place
> deploy breaks every session that starts later.

```powershell
& scripts\deploy-win.ps1     # refuses while any CBM process runs; keeps the old exe as <exe>.<version>
```

Start the sessions again after the script prints `OK:`.

**Re-index after a change to extraction** (any change under `internal/cbm/` or `src/pipeline/`):
run `/refresh-monolith`, or `codebase-memory-mcp cli index_repository --repo-path C:/Users/Max/source/repos`.
A text-only or query-side change needs no re-index. The run returns one of these statuses:
- `aborted_previous_preserved`: files changed during the run. Run it again.
- `persist_failed`: a process still holds the old DB open. Close it, then run it again.

## Release (for teammates)

The shared plugin `fleet-hd-claude/plugins/codebase-memory-mcp` installs the Windows binary from a release on
this fork. To publish one:

```powershell
$v = 'v0.11.0-fleethd.N'; $out = "$env:TEMP\cbm-release"; New-Item -ItemType Directory -Force $out | Out-Null
Compress-Archive build\c\codebase-memory-mcp.exe "$out\codebase-memory-mcp-windows-amd64.zip" -Force
$h = (Get-FileHash "$out\codebase-memory-mcp-windows-amd64.zip" -Algorithm SHA256).Hash.ToLower()
"$h  codebase-memory-mcp-windows-amd64.zip" | Set-Content "$out\checksums.txt"
gh release create $v "$out\codebase-memory-mcp-windows-amd64.zip" "$out\checksums.txt" `
  --repo MaxMiller117/codebase-memory-mcp --target main --title $v --notes-file <notes>
```

Then update the pinned tag in the plugin README with a PR on `fleet-hd-claude` (Azure DevOps).

## Update to a new upstream release

1. Make a branch from the upstream tag and apply the patches in `docs/LOCAL_PATCHES.md`.
2. Build, then index into the live cache only after all sessions close (see Deploy).
3. Before the deploy, run these checks. Stock v0.11.0 failed each of them:
   - `MATCH (b {name:'FaultRuleDto'})<-[:USAGE]-(a {name:'AlertRuleDto'}) RETURN a.file_path` returns the `fleet-hd-contracts` row.
   - `search_graph(query="get async vehicle", file_pattern="*fleet-hd-alert-notification/*")` returns `total` greater than 0 with `total_relation: eq`.
   - `MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) WHERE NOT (a.file_path CONTAINS 'Test') RETURN count(a)` returns a row.
4. Merge the branch into `main` and publish a release.
