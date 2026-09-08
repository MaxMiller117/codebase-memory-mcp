# Known issues / improvement backlog

Operational issues observed in real multi-agent use (logged 2026-06-18), written up for a
future fix session: symptom, evidence, root-cause hypothesis, code pointers, suggested
direction, acceptance.

---

## 1. `project` arg is required and hard-errors on unknown/short names — should default to the sole/monolith project

**Symptom.** Every query tool (`search_graph`, `search_code`, `trace_path`, `query_graph`,
`get_code_snippet`, …) requires an exact `project` key. Passing anything that isn't an exact
indexed key returns:

```json
{"error":"project not found or not indexed","hint":"Use list_projects ...","available_projects":[...]}
```

LLM agents naturally guess a short repo name (e.g. `fleet-hd-contracts`) instead of the actual
key (`C-Users-Max-source-repos`, the monolith index of all repos) and get a hard error, then
retry-loop. In a 146-agent workflow fan-out this silently degraded the whole run: every agent
404'd `search_graph` repeatedly and fell back to slow Read/Grep → ~3h, near-zero completions.

**Desired behavior.** When there is effectively one project (or a configured default), treat
every command as scoped to it — i.e. default `project` to the sole indexed project (or a
`default_project` from config) when the arg is omitted or doesn't resolve, instead of
hard-erroring. ("We're using a monolith; just treat every command as scoped to it, or at least
default to it.")

**Code pointers.** `src/mcp/mcp.c`: the error is emitted via `build_project_list_error(...)` at
the sites guarded by `resolve_store(srv, project)` returning NULL — the `STORE_OR_ERR`-style
macro (~L833) plus direct checks (~L1495, ~L1906, ~L2542, ~L3237). Fix in `resolve_store()`
(and/or the macro): when `project` is empty/unknown, fall back to (a) the only indexed project
when exactly one exists, else (b) a configured `default_project`, else (c) keep the error but
have the hint name the default. Make `project` optional in the tool input schemas to match.

**Acceptance.** `search_graph(name_pattern=…)` with **no** `project` succeeds when exactly one
project is indexed; an unknown/short name resolves to the default (or returns an error that
states the default and how to set it). No retry-loops for the common single-monolith setup.

---

## 2. `search_graph` is slow under concurrent multi-agent load (likely serial request dispatch)

**Symptom.** Under ~16 concurrent agents each calling `search_graph` against the monolith
(≈246k nodes / 493k edges / ~487 MB store), aggregate throughput was **~6 tool-calls/min
(~2.5–3 min effective latency per call)**. The same agents at low concurrency (~18 in a single
batch) returned in seconds. Results are correct (`{"total":N,"results":[...]}`) — purely a
throughput/latency-under-load problem. At this rate a 146-agent fan-out projects to ~10–14h.

**Root-cause hypothesis.** A single shared MCP server process serves all of a session's
subagents over one stdio channel. If the request loop processes requests **serially**
(read → handle → write, one at a time), then N concurrent agents' calls queue behind each
other and effective latency ≈ N × per-query time. With BM25 search over 246k nodes per call,
that serializes into minutes. Secondary factor: broad queries return very large result sets
(observed `"total":2947`), which are expensive to rank/serialize — consider default result caps.

**Code pointers.** `src/mcp/mcp.c` — the MCP stdio request/dispatch loop: confirm whether it
handles one request at a time. `src/store/store.c` / `store.h` — graph store + query backend;
check for a global lock around reads and whether read-only queries can run concurrently.
`src/foundation/compat_thread.{c,h}` and `src/pipeline/worker_pool.{c,h}` already provide
threading primitives a request worker-pool can build on. The BM25/full-text path behind
`search_graph` is the per-call cost to profile.

**Suggested direction.** Confirm serial-vs-concurrent dispatch first (cheap: log request
enqueue/finish timestamps under load). If serial, move query handling onto a worker pool with
concurrent read access to the store (read-only queries must not block each other). Add a
concurrency benchmark (N parallel `search_graph`) to `docs/BENCHMARK.md` asserting per-call
latency stays ~flat as N rises. Note: `search_code` is separately known-slow on Windows
(per-file `Select-String`); **this** issue is specifically `search_graph` degrading under
concurrency, which it should not.

**Acceptance.** N concurrent `search_graph` calls do not degrade per-call latency
superlinearly; documented expected concurrent throughput.
