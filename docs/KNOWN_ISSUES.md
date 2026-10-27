# Known issues (FleetHd fork, `0.12.1-fleethd.1`)

These issues come from multi-agent use on the FleetHd monolith (one project, about 280 repos). Each status was
measured on 2026-10-24 on a full test index of `0.12.1-fleethd.1` (upstream `main` `10ab613d` plus the local
patches in `LOCAL_PATCHES.md`), beside the live `0.11.0-fleethd.2` index.

`0.12.1-fleethd.1` was deployed on 2026-10-26. On the live index of that day (`mode=moderate`, 390,677 nodes), the 3
checks in `CLAUDE.md` pass, 570 `VehicleDto` edges go to `fleet-hd-contracts`, and 111 classes inherit
`OrderedQueryRepository`.

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

## 2. The first parallel burst of a session is not faster

Local patch 8 opens extra daemon connections only when read-only calls queue. Each new connection replays the
handshake and opens its own session store, so the burst that opens it runs at about 1-connection speed. Later
bursts run in parallel: 4 concurrent `query_graph` calls took 2.5 s in the first burst and 0.7 s in the second
(4 connections, 2026-10-24).

**Not changed on purpose:** connections that open at session start would hold daemon connections (64 in total)
for sessions that never call tools in parallel.

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
removes such a guess when it goes to another repo, or when a static receiver (`Assert.True`) does not match the
target's type, so a guess inside the caller's repo can stay and name the wrong class. On the monolith
(2026-10-24), 26,847 C# `CALLS` edges are name guesses:

| Receiver shape | Edges | Note |
|---|---:|---|
| bare `M()` | 19,844 | 11,808 go to a class (constructor calls, mostly correct) |
| local or parameter `x.M()` | 2,846 | LINQ and collection methods (`Select`, `Add`) bind a project method |
| chain `a.B().C()` | 1,334 | |
| member chain `a.b.C()` | 1,318 | |
| field `_f.M()` | 1,050 | framework types (`ILogger.LogError`) bind a project method |
| static `X.M()` | 442 | X is a property in the repo |

**Partly fixed (2026-10-26):** when the C# pass typed the receiver to a type with no type node in the graph (an
`ILogger`, a `List<T>`), the name guess is dropped (local patch 4). This removed 916 edges. A guess onto a field
that is not a delegate is dropped too (798 edges). A receiver that the pass cannot type at all (a `var` from an
unregistered method) still falls back to a name guess.

**Precision of the guesses that remain:** a sample of 50 name-guess `CALLS` edges (2026-10-26, before the field
rule) had 44 correct, 6 wrong and 1 uncertain. The 44 correct were mostly constructor calls and calls through
variables of project types. The wrong ones were LINQ or collection methods on a receiver that the pass could not
type (`entities.Max`, `MapView.Pins.Remove`), a mock setup lambda (`x.GetByVinAsync`), and two `new DateTime()`
calls bound to a field, which the field rule now drops. Check the target's class before you follow a guessed call
(`strategy` is not `cs_*` or `lsp_*`).

## 5. Some qualified type references still lose their edge

Local patch 4 reads a reference's qualifier from the source, but 9 of the 21,928 type edges that it removes name
a type that the file does reach. These are rare forms that the qualifier reader does not parse.

## 6. Visibility filter limits (Codex review, 2026-10-26)

Local patch 4 does not model these C# forms. Each count below is from the main checkouts on 2026-10-26.

| Form | Effect | Count |
|---|---|---:|
| `global using N;` in one file | Other files of the project do not see N, so a correct guessed edge can drop | 1 file with a FleetHd namespace (a test project) |
| More than one namespace in one file | Every type of the file gets the first namespace | 0 of 14,781 `.cs` files |
| `namespace` alone on a line, or after `*/` on the same line | An incremental run does not read the namespace of an unchanged file | 0 files |
| `using A;` plus an explicit `B.T` where A and B both declare T | The guess `A.T` stays, because the import makes it visible before the qualifier is read | not measured |

## 7. `list_projects` takes 1.5 s per call

On `0.12.1-fleethd.1`, every `list_projects` call takes about 1,500 ms, against 60–75 ms on `0.11.0-fleethd.2`. The
first call and later calls take the same time, and stock upstream `10ab613d` takes the same time, so the cost is
upstream's (measured 2026-10-26). Other tools answer in 15–30 ms (`search_graph` by name). A cold start is faster:
`initialize` took 750 ms, against 3,400 ms on the old build.

The cause is not found. `metadata_only=true`, which skips the git branch lookup, takes the same time, and SQLite
opens the index and reads its `projects` row in less than 1 ms. The FleetHd hooks do not call the binary, so only an
agent that calls `list_projects` waits.

## Fixed in `0.12.1-fleethd.1`

Most of these entries come from the setup benchmark (`fleet-hd-claude/skills/setup-bench`), measured on the live
`0.11.0-fleethd.2` index on 2026-10-23.

- **Concurrent requests on one MCP server ran one at a time.** 16 concurrent requests to one server took 6.7 s
  (median latency 3.8 s), against 1.1 s on 16 separate servers. With local patch 8 and 4 connections they take
  2.4 s (median 1.2 s); with 8, 1.3 s.

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
