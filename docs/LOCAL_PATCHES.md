# Local patches (FleetHd fork, on top of upstream v0.11.0)

Deployed as `0.11.0-fleethd.1`. Build with `scripts/build-win-ucrt.sh` (llvm-mingw clang + UCRT).

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
