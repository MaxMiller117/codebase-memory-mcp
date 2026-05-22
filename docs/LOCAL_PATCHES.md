# Local Patches

This file documents changes made to this repository that diverge from upstream (DeusData v0.6.0).
These patches are applied to support cross-repo DTO impact analysis in a C# / .NET codebase.

## Patch 1: C# class-level property and field type refs

**File:** `internal/cbm/extract_type_refs.c`  
**Function:** `handle_type_refs`

### Problem

Upstream only extracted type references from *function signatures* (parameter types and return
types). In C#, DTOs reference other types through class-level property and field declarations —
not inside any function. For example:

```csharp
public class AlertRuleDto : BaseDto
{
    public ICollection<FaultRuleDto> FaultRules { get; set; }
    public SeverityDto Severity { get; set; }
}
```

Without this patch, `AlertRuleDto` had no outgoing USAGE edges to `FaultRuleDto` or `SeverityDto`,
making cross-repo DTO impact tracing impossible via graph traversal.

### Fix

Added `property_declaration` and `field_declaration` handlers in `handle_type_refs` for
`CBM_LANG_CSHARP`. When either node is encountered during the unified AST walk, the enclosing
class QN (from `state->enclosing_class_qn`) is used as the source, and
`extract_csharp_type_refs` recursively extracts all referenced types — including generic type
arguments (e.g. both `ICollection` and `FaultRuleDto` from `ICollection<FaultRuleDto>`).

**Result:** `AlertRuleDto CLASS -[USAGE]-> FaultRuleDto CLASS` edge is now produced.

---

## Patch 2: C# method body type refs (object creation, typeof, cast)

**File:** `internal/cbm/extract_type_refs.c`  
**Function:** `process_body_type_ref`

### Problem

Upstream had no C# case in `process_body_type_ref`, so type references inside method bodies were
never extracted for C#. This meant that controllers and services that referenced DTOs only in
method bodies (e.g. `new List<AlertRuleDto>()`, `typeof(AlertRuleDto)`) had no USAGE edges to
those DTOs — even though the dependency clearly exists.

For example, `AlertRuleQueryController.BuildSummary` contains:

```csharp
[ProducesResponseType(typeof(PagedResponseHeader<AlertRuleDto>), 200)]
public async Task<IActionResult> BuildSummary(...)
{
    var result = new List<AlertRuleDto>();
    ...
}
```

Without this patch, `BuildSummary` had no USAGE edge to `AlertRuleDto`.

### Fix

Added a `CBM_LANG_CSHARP` case to `process_body_type_ref` that handles:

- `object_creation_expression` — `new T(...)`, `new List<T>()`
- `typeof_expression` — `typeof(T)`, `typeof(IEnumerable<T>)`
- `cast_expression` — `(T)x`

All three node types expose a `type` named field in the tree-sitter C# grammar.
`extract_csharp_type_refs` is called recursively, so generic type arguments are extracted too.

Because `push_boundary_scopes` pushes `SCOPE_FUNC` for the enclosing `method_declaration` before
descending into its children (including attribute lists), the `enclosing_func_qn` is correctly set
to the method QN when these nodes are visited — both in method bodies and in method attributes.

**Result:** `BuildSummary METHOD -[USAGE]-> AlertRuleDto CLASS` edge is now produced.

---

## Combined effect

With both patches applied, indirect cross-repo DTO relationships are detectable via explicit
multi-hop Cypher:

```cypher
MATCH (ctrl {name:'AlertRuleQueryController'})-[r1]->(m)-[r2]->(dto {name:'AlertRuleDto'})-[r3]->(target {name:'FaultRuleDto'})
RETURN ctrl.name, r1.type, m.name, r2.type, dto.name, r3.type, target.name LIMIT 10
```

Returns:
```
AlertRuleQueryController | DEFINES_METHOD | BuildSummary | USAGE | AlertRuleDto | USAGE | FaultRuleDto
```

---

## Rebuilding

Requires MinGW cross-compiler in WSL (Ubuntu-24.04):

```bash
sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
```

Build:

```bash
cd /mnt/c/Users/Max/source/repos/codebase-memory-mcp
touch internal/cbm/extract_type_refs.c   # force recompile if make thinks it's current
make -f Makefile.cbm cbm CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ -j4
```

Output binary: `build/c/codebase-memory-mcp.exe`

Deploy (kill the running MCP process first, then copy):

```powershell
Copy-Item build\c\codebase-memory-mcp.exe `
    "$env:LOCALAPPDATA\codebase-memory-mcp\codebase-memory-mcp.exe" -Force
```
