# Known issues (FleetHd fork, `0.11.0-fleethd.2`)

These issues come from multi-agent use on the FleetHd monolith (one project, about 280 repos). Each entry
gives the status that was measured on 2026-10-02.

## 1. A wrong `project` name does not fall back to the monolith

Every query tool requires the exact project key `C-Users-Max-source-repos`. Agents often guess a short repo
name such as `fleet-hd-contracts`. On 2026-06-18 a 146-agent workflow did this, retried in a loop, and ran for
about 3 h with almost no completions.

**Status on v0.11.0:** still open, but the impact is lower.
- An unknown name returns `project not found or not indexed` with `available_projects: ["C-Users-Max-source-repos"]`.
- v0.11 matches a short name against indexed project names. While the old standalone index
  `C-Users-Max-source-repos-fleet-hd-contracts` existed, `project="fleet-hd-contracts"` returned its stale
  2026-06-14 rows with no warning. That index was deleted on 2026-10-02. Do not create per-repo indexes beside
  the monolith.

**Possible fix:** in `resolve_store()` (`src/mcp/mcp.c`), use the only indexed project when exactly one exists.

## 2. Concurrent requests on one MCP server run one at a time

All subagents of one Claude Code session share that session's MCP server over one stdio channel.

**Status on v0.11.0 (load test on 2026-10-02, live index, 4 mixed queries of 0.15–0.72 s each):**

| Case | Wall time | Median latency | Max latency |
|---|---:|---:|---:|
| 16 requests one after another, one server (sum of single latencies) | 7.3 s | — | — |
| 16 concurrent requests, one server | 6.2 s | 3.9 s | 6.2 s |
| 16 servers, one request each, concurrent | 1.2 s | 1.1 s | 1.2 s |

Requests on one server still queue: 16 concurrent requests take about as long as 16 sequential ones.
Separate servers (separate sessions) run in parallel through the shared account daemon. The absolute
cost is now small, because single queries take well under 1 s.

**Possible fix:** dispatch read-only tool calls from the stdio loop to a worker pool.

## 3. One build version and one cache folder per account

v0.11 runs one account-wide daemon. A server of a different build, or with a different `CBM_CACHE_DIR`,
refuses to start while it runs:
- `CBM could not start because a conflicting CBM process is active (version; …)`
- `CBM could not start because the active account daemon uses a different cache directory`

**Consequence:** a binary swap needs all sessions closed (`scripts/deploy-win.ps1` checks this), and a test
index in a second cache folder cannot run beside the live one.

## 4. `WITH … WHERE` ignores the `WHERE`

`MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) WITH a WHERE NOT (a.file_path CONTAINS 'Test') RETURN count(a)`
returned 603, the unfiltered count, on the 2026-10-02 test index. The filter allows 34. Put the filter in the
`MATCH … WHERE`. The cause is not known; the post-`WITH` bindings may not resolve node properties.
