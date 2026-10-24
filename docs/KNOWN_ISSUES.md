# Known issues (FleetHd fork, `0.12.1-fleethd.1`)

These issues come from multi-agent use on the FleetHd monolith (one project, about 280 repos). Each status was
measured on 2026-10-24 on a full test index of `0.12.1-fleethd.1` (upstream `main` `10ab613d` plus the local
patches in `LOCAL_PATCHES.md`), beside the live `0.11.0-fleethd.2` index.

## 1. A wrong `project` name does not fall back to the monolith

Every query tool requires the exact project key `C-Users-Max-source-repos`. Agents often guess a short repo name
such as `fleet-hd-contracts`. On 2026-06-18 a 146-agent workflow did this, retried in a loop, and ran for about 3 h
with almost no completions.

**Status:** open, but the error now gives the fix. `project="fleet-hd-contracts"` returns
`project not found or not indexed` with `available_projects: ["C-Users-Max-source-repos"]` and the hint "never a
repo or folder name". A directory path resolves to the project that owns it (upstream #1690). Do not create
per-repo indexes beside the monolith: a short name matches an indexed project name, and a stale per-repo index
answers with no warning.

**Fix not taken:** a fallback to the only indexed project. Upstream tests (`tool_sg_invalid_project`,
`tool_qg_invalid_project`, the session-start hook tests) and the hooks require the not-found error.

## 2. Concurrent requests on one MCP server run one at a time

All subagents of one Claude Code session share that session's MCP server. Each server is one connection to the
account daemon, and the daemon runs the requests of one connection one at a time (`src/daemon/runtime.c`).

**Status:** open. Measured on `0.12.1-fleethd.1` (2026-10-24, 4 mixed queries):

| Case | Wall time | Median latency | Max latency |
|---|---:|---:|---:|
| 16 requests one after another, one server (sum of single latencies) | 6.1 s | 0.26 s | 0.94 s |
| 16 concurrent requests, one server | 6.7 s | 3.8 s | 6.7 s |
| 16 servers, one request each, concurrent | 1.1 s | 0.46 s | 1.1 s |

**Possible fix (upstream):** let one daemon connection run read-only requests in parallel. This changes the
daemon protocol (requests in flight, response order, cancellation), so it is a request for upstream, not a fork
patch. Separate sessions already run in parallel.

## 3. One build version per account daemon

The default account daemon serves one build and one cache folder. A server of a different build refuses to start
while it runs (`CBM could not start because a conflicting CBM process is active`). A binary swap therefore needs
all sessions closed (`scripts/deploy-win.ps1` checks this).

**Status:** a test build can run beside the live daemon. Set `CBM_RUNTIME_DIR` (a separate rendezvous folder) and
`CBM_CACHE_DIR`. Both folders must pass the private-directory check: a folder under `%TEMP%` fails with
`could not load saved runtime settings`. Folders under `C:\Users\Max\.cache\` pass. Create the runtime folder first,
or the start fails with `secure CLI coordination could not be created (endpoint)`. See `CLAUDE.md`.

## 4. Calls that the C# pass cannot type still resolve by name inside the repo

Local patch 6 types calls through fields, parameters and locals. A receiver whose type the pass cannot infer (for
example a `var` from a method whose return type is not registered) still falls back to a name guess. Local patch 4
removes such a guess only when it goes to another repo, so a guess inside the caller's repo stays, and it can name
the wrong class. On the monolith, 20,675 C# `CALLS` edges are `suffix_match` guesses, and 2 of the 9
`_sut.GetByVinAsync` calls in `TruckContextQueryControllerTests` still go to `SensorQueryController`.

**Possible fix:** trace the receiver's type further (method return types, `await` of `Task<T>`).

## 5. Some qualified type references still lose their edge

Local patch 4 reads a reference's qualifier from the source, but 9 of the 21,928 type edges that it removes name
a type that the file does reach. These are rare forms that the qualifier reader does not parse.

## Fixed in `0.12.1-fleethd.1`

- **`WITH … WHERE` ignored the `WHERE`.** The `TroubleCodeDto` count with `WITH a WHERE NOT …` returned 170, the
  unfiltered count. It now returns 31, the same as the filter in `MATCH … WHERE` (upstream `951b2fdd`).
- **Cross-repo type references went to the wrong same-named class.** `VehicleDto` edges from outside the Samsara
  repos went to `fleet-hd-samsara-contracts` (524 edges on `0.11.0-fleethd.2`). They now go to `fleet-hd-contracts`,
  and the 9 edges from `fleet-hd-subscriber-samsara-vehicle-function` go to the Samsara class (local patch 4).
- **Framework names resolved to symbols in unrelated repos.** `Ok`, `NotFound`, `IsNullOrWhiteSpace`, `DateTime`,
  `UtcNow`, `MaxValue` and `Location` in `TruckContextQueryController.GetByVinAsync` have no cross-repo edge now
  (local patch 4).
- **Calls through a field were not typed.** `_service.Method()` fell back to a name guess. C# calls resolved as
  `cs_method_typed` went from 3,633 to 14,037 (local patch 6).
- **An incremental index checked visibility only for changed files.** It now reads the namespace of every unchanged
  `.cs` file. A fixture shows the same edges after a full run and after two incremental runs (local patch 4).
- **A qualified or aliased type reference lost its edge.** `Entity.Foo`, `Vehicle.VehicleDto` and
  `MobileContracts.Gps.MobilePositionDto` (`using MobileContracts = FleetHd.Mobile.Contracts;`) keep their edges.
  Lost correct type edges went from 504 to 9 (local patches 4 and 5).
- **The renamed clone `fleet-hd-anonymizer-function (deleted)` was indexed** (549 nodes). The repos-root
  `.cbmignore` now has `/* (deleted)/`, and the folder has 0 nodes.
- **The documented transitive recipe missed subclasses and private helpers.** `~/.claude/docs/codebase-memory-mcp.md`
  now adds an `INHERITS` hop and a `CALLS` hop.
- **`EXAMPLE.md` was out of date.** It now uses the 17-type closure and `Route` / `HANDLES` edges. The route query
  finds 33 of the 34 endpoints that a source scan finds, with no false result.
- **`new List<AlertDto>()` had no `USAGE` edge.** Not an extractor fault: the index reads each repo's working tree,
  and the fleet-hd-mobile-api checkout was on a branch without that code. A fixture with the same shape gives
  `GetByVin -USAGE-> AlertDto`.
