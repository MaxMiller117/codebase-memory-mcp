# codebase-memory-mcp — FleetHd fork

This fork is upstream DeusData `main` (after v0.12.1) plus the local patches in `docs/LOCAL_PATCHES.md`. Version
names use the form `<last upstream release>-fleethd.N`, for example `0.12.1-fleethd.1`. Usage rules for agents (query forms, paging, known engine limits) are in
`~/.claude/docs/codebase-memory-mcp.md`; this file covers build, deploy and release only.

## Build (Windows binary, cross-compiled in WSL)

Use the llvm-mingw toolchain (clang + UCRT) in WSL `Ubuntu-24.04`, at `~/toolchains/llvm-mingw-*-ucrt-ubuntu-*-x86_64`.

> **⚠ Do not build with Ubuntu's `x86_64-w64-mingw32-gcc`.** It links `msvcrt.dll`, and that binary fails every
> index run with `pipeline.stage action=lock_failed errno=22`. Upstream builds with MSYS2 CLANG64, which also links UCRT.

```bash
# In WSL, from the repo root. The first run also builds zlib into the toolchain (set ZLIB_TGZ to zlib-1.3.1.tar.gz).
bash scripts/build-win-ucrt.sh 0.12.1-fleethd.N          # warm build, ~3 min
bash scripts/build-win-ucrt.sh 0.12.1-fleethd.N clean    # cold build
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
$v = 'v0.12.1-fleethd.N'; $out = "$env:TEMP\cbm-release"; New-Item -ItemType Directory -Force $out | Out-Null
Compress-Archive build\c\codebase-memory-mcp.exe "$out\codebase-memory-mcp-windows-amd64.zip" -Force
$h = (Get-FileHash "$out\codebase-memory-mcp-windows-amd64.zip" -Algorithm SHA256).Hash.ToLower()
"$h  codebase-memory-mcp-windows-amd64.zip" | Set-Content "$out\checksums.txt"
gh release create $v "$out\codebase-memory-mcp-windows-amd64.zip" "$out\checksums.txt" `
  --repo MaxMiller117/codebase-memory-mcp --target main --title $v --notes-file <notes>
```

Then update the pinned tag in the plugin README with a PR on `fleet-hd-claude` (Azure DevOps).

## Update to a new upstream release

1. Make a branch from upstream `main` (or a tag) in a worktree named `.wt-<name>`, and apply the patches in
   `docs/LOCAL_PATCHES.md`.
2. Build, then index into a test cache beside the live one. Both folders must be under `C:\Users\Max\.cache\`,
   because a folder under `%TEMP%` fails the private-directory check. Create the runtime folder first:
   ```bash
   mkdir -p /c/Users/Max/.cache/cbm-test-rt
   CBM_CACHE_DIR='C:\Users\Max\.cache\cbm-test' CBM_RUNTIME_DIR='C:\Users\Max\.cache\cbm-test-rt' \
     build/c/codebase-memory-mcp.exe cli index_repository --repo-path C:/Users/Max/source/repos
   ```
   Run each check query with the same two variables set.
3. Before the deploy, run these checks. Stock v0.11.0 failed each of them:
   - `MATCH (b {name:'FaultRuleDto'})<-[:USAGE]-(a {name:'AlertRuleDto'}) RETURN a.file_path` returns the `fleet-hd-contracts` row.
   - `search_graph(query="get async vehicle", file_pattern="*fleet-hd-alert-notification/*")` returns `total` greater than 0 with `total_relation: eq`.
   - `MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) WHERE NOT (a.file_path CONTAINS 'Test') RETURN count(a)` returns a row.

   Stock upstream `main` (2026-10-22) failed these, and the local patches 3 to 6 fix them:
   - `MATCH (a)-[:USAGE]->(k {name:'IssueDto'}) RETURN k.label, count(a)` has no `Method` row (constructor).
   - `MATCH (t {name:'VehicleDto'})<-[:USAGE]-(a) WHERE a.file_path CONTAINS 'samsara' RETURN t.file_path, count(a)`
     returns only the `fleet-hd-samsara-contracts` row.
   - `MATCH (m {name:'GetByVinAsync'})-[:CALLS]->(t) WHERE m.file_path CONTAINS 'TruckContextQueryController' RETURN t.name, t.file_path`
     has no `Ok`, `NotFound` or `IsNullOrWhiteSpace` row.
   - `MATCH (m)-[r:CALLS]->(t {name:'GetByVinAsync'}) WHERE m.file_path CONTAINS 'TruckContextQueryControllerTests' RETURN t.file_path, r.strategy, count(*)`
     has a `cs_method_typed` row to `TruckContextQueryController.cs` (a call through the field `_sut`).
   - `MATCH (t {name:'AlertRuleDto'})<-[:USAGE]-(m)-[:HANDLES]->(r:Route) RETURN count(DISTINCT m)` is greater than 0.

   An incremental run must give the same edges as a full run. Index a fixture with two same-named classes in
   different namespaces and a third repo that `using`s one of them, change only the third file, and index again.

   Run upstream's suite on a copy inside WSL (`make -f Makefile.cbm test-par`). Copy with `tar
   --no-same-permissions` and `chmod -R go-w`: a world-writable folder fails the `cli` install tests. The
   fork's own tests are listed in `docs/LOCAL_PATCHES.md` under **Tests**; a merge conflict in one of them means
   upstream changed the behaviour that the patch depends on.

   To measure what a resolver change removes or adds, index a second copy with the change off (for patch 4,
   `CBM_FLEETHD_NO_CS_VISIBILITY=1`) and diff the `edges` tables of the two databases.
4. Merge the branch into `main` and publish a release.
