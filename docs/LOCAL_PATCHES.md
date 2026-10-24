# Local patches (FleetHd fork, on top of upstream `main`)

Current build: `0.12.1-fleethd.1`, on upstream `main` at `10ab613d` (2026-10-22, after the v0.12.1 release). Build
with `scripts/build-win-ucrt.sh` (llvm-mingw clang + UCRT) and deploy with `scripts/deploy-win.ps1` after you close
every Claude Code session.

1. **C# type refs become USAGE edges** (`internal/cbm/extract_type_refs.c`). Class-level property and
   field types (with generic arguments, nullables, arrays) and method-body `new T()` / `typeof(T)` /
   `(T)x` produce `USAGE` edges, so `AlertRuleDto -USAGE-> FaultRuleDto`. `add_type_ref` pushes a
   usage (zero-initialized `CBMUsage`) instead of an unused type ref, for every language. It strips the `: ` of a
   TypeScript annotation and keeps only identifier names (letters, digits, `_`, `.`, `$`), so `() => void` and
   `std::vector` give no usage. Upstream test `repro_call_argument_ts_lsp_cannot_rescue` checks this.
2. **Scoped BM25 search ranks the full match set** (`src/mcp/mcp.c`, `bm25_search`). With
   `file_pattern`, `label`, `kind` or `ns`, the FTS subquery has no 2,000-candidate window, so a repo-scoped
   query on a multi-repo index keeps every in-scope hit and an exact total.
3. **A C# `USAGE` binds the class, not its constructor** (`src/pipeline/lsp_resolve.h`,
   `cbm_pipeline_cs_ctor_owner`, called from `pass_usages.c` and `pass_parallel.c`). Upstream's registry tie-break
   prefers a callable, so a type reference `Foo` went to the constructor `Foo.Foo`. Without this patch, at least
   8,081 `USAGE` edges went to constructors of 178 classes, and `(Class)<-[:USAGE]-` queries missed them.
4. **A C# name guess must be visible from the referencing file** (`src/pipeline/pipeline.c`,
   `cbm_pipeline_cs_visible_target` / `cbm_pipeline_cs_qn_visible`, called from `pass_usages.c`, `pass_calls.c` and
   `pass_parallel.c`). This applies only to a target that the registry guessed by name, not to an LSP-proven target.
   - A type is visible when its namespace encloses the file's namespace, a `using` imports it, or the reference's
     qualifier reaches it: `Entity.Foo` from inside `FleetHd.X` reaches `FleetHd.X.Entity`, and `A.Gps.Foo` with
     `using A = FleetHd.Contracts;` reaches `FleetHd.Contracts.Gps`. A bare usage takes its qualifier from the source
     bytes in front of it (`site_start_byte`), because the extractor keeps only the last name. A constructor guess
     counts as its class. When the guess is not visible, the edge goes to the one visible type of that name, or is
     dropped. A call guessed onto a type (`new T()`) is moved the same way (`cbm_pipeline_cs_call_type_qn`).
   - A member (method, field, call) is visible under the same rule or when it is in the caller's repo. An invisible
     member edge is dropped. At file scope, a member named like a segment of the file's namespace or `using` paths
     (the token of `namespace FleetHd.Models`) is dropped.
   - The check uses a map from file to declared namespace. An incremental run extracts only changed files, so
     `cbm_pipeline_cs_ns_add_unchanged` (called from `pipeline_incremental.c`) reads the first `namespace` line of
     every other `.cs` file. `CBM_FLEETHD_NO_CS_VISIBILITY=1` turns the check off.
   - Measured on the monolith (2026-10-24, A/B against the check turned off): C# `CALLS` + `USAGE` edges 506,229 →
     380,334. Of the 129,541 removed edges, 124,599 went to another repo. Of the 21,928 removed type edges, 9 named a
     type that the file reaches; the rest went to a type in a namespace the file does not reach.
5. **C# `using` directives inside `#if` or a namespace block are imports** (`internal/cbm/extract_imports.c`,
   `parse_csharp_imports_in`, `capture_namespace_decl`). The parser read only root-level `using` lines, so a file
   wrapped in `#if` had no imports and no namespace. The alias of `using A = N;` is read from the grammar's `name`
   field, so `local_name` is `A`, not the last segment of `N`.
6. **C# calls through a field are typed** (`internal/cbm/lsp/cs_lsp.c`, `cs_collect_file_fields`,
   `cs_member_field_type`). The cross-file C# pass registers types without fields, so `_service.Method()` fell back
   to a name guess. The pass now collects the file's own fields and properties against the cross-file registry. C#
   calls resolved as `cs_method_typed` went from 3,633 to 14,037.
7. **Server instructions state the watcher dependency** (`src/mcp/mcp.c`, `MCP_SERVER_INSTRUCTIONS`). The text
   says that indexes refresh automatically only when `auto_watch` is on. FleetHd runs with the watcher off.

## Tests

Each test below is in upstream's suite and fails when its patch is removed (checked 2026-10-24):

| Test | Patches |
|---|---|
| `parallel_csharp_fleethd_visibility_patches` (`tests/test_parallel.c`, sequential and parallel runs) | 3, 4, 5, 6 |
| `incr_csharp_visibility_matches_full_run` (`tests/test_incremental.c`) | 4 (incremental runs) |
| `tool_search_graph_bm25_scoped_query_ranks_full_match_set` (`tests/test_mcp.c`) | 2 |
| `repro_call_argument_ts_lsp_cannot_rescue` (upstream test) | 1 (TypeScript annotations) |

## Not carried

The v0.11.0 patch for `NOT` over a variable that is not yet bound (`src/cypher/cypher.c`) is not carried. Upstream
#2253 (v0.12.0) makes the early `WHERE` pass three-valued, and that fixes the same query.

A fallback from an unknown `project` name to the only indexed project was tried and removed: 14 upstream tests and
the session-start hooks require the not-found error for an unknown name.
