# Local patches (FleetHd fork, on top of upstream v0.11.0)

Current build: `0.11.0-fleethd.2`. Build with `scripts/build-win-ucrt.sh` (llvm-mingw clang + UCRT) and deploy with
`scripts/deploy-win.ps1` after you close every Claude Code session.

1. **C# type refs become USAGE edges** (`internal/cbm/extract_type_refs.c`). Class-level property and
   field types (with generic arguments, nullables, arrays) and method-body `new T()` / `typeof(T)` /
   `(T)x` produce `USAGE` edges, so `AlertRuleDto -USAGE-> FaultRuleDto`. `add_type_ref` pushes a
   usage (zero-initialized `CBMUsage`) instead of an unused type ref, for every language.
2. **Scoped BM25 search ranks the full match set** (`src/mcp/mcp.c`, `bm25_search`). With
   `file_pattern` or `label`, the FTS subquery has no 2,000-candidate window, so a repo-scoped query
   on a multi-repo index keeps every in-scope hit and an exact total. `label` is now applied.
3. **`NOT` over a not-yet-bound variable** (`src/cypher/cypher.c`, `eval_expr`). The early WHERE
   pass runs before expansion binds every variable; `NOT` now defers instead of inverting the
   lenient pass, so `WHERE NOT (a.file_path CONTAINS 'Test')` works after `<-[:USAGE]-(a)`.
4. **Server instructions state the watcher dependency** (`src/mcp/mcp.c`, `MCP_SERVER_INSTRUCTIONS`). The text
   says that indexes refresh automatically only when `auto_watch` is on. FleetHd runs with the watcher off.
