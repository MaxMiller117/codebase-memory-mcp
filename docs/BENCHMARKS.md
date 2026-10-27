# Benchmark: 0.11.0-fleethd.2 against 0.12.1-fleethd.1

Made with `scripts/fleethd-bench.py` (see `CLAUDE.md`, step 4). Each build made a fresh full index of the monolith in its own cache, then served one MCP stdio server. Both runs used a cold daemon. Other Claude Code sessions ran on the machine at the same time, so compare the columns of one run, not absolute times across runs.

Runs started 2026-10-26 15:32:38 and 2026-10-26 15:35:55.

## Full index

| Metric | old | new | Change |
|---|---|---|---|
| secs | 108.0 | 96.8 | -10% |
| peak_rss_mb | 8,444 | 6,602 | -22% |
| nodes | 487,412 | 477,654 | -2% |
| edges | 1,553,286 | 1,477,783 | -5% |
| db_mb | 2,265 | 2,309 | +2% |
| attempts | 1 | 1 | +0% |
| status | indexed | indexed |  |

## Edges by type

| Metric | old | new | Change |
|---|---|---|---|
| USAGE | 495,308 | 463,869 | -6% |
| DEFINES | 469,235 | 458,826 | -2% |
| CALLS | 256,105 | 205,126 | -20% |
| WRITES | 84,782 | 89,397 | +5% |
| IMPORTS | 87,045 | 85,054 | -2% |
| DEFINES_METHOD | 51,970 | 52,079 | +0% |
| SIMILAR_TO | 29,584 | 29,742 | +1% |
| CONTAINS_FILE | 25,774 | 26,689 | +4% |
| DECORATES | 25,861 | 25,953 | +0% |
| MENTIONS | - | 6,919 |  |
| CONTAINS_FOLDER | 5,849 | 5,917 | +1% |
| OVERRIDE | 5,540 | 5,863 | +6% |
| IMPLEMENTS | 3,299 | 3,288 | -0% |
| INHERITS | 3,197 | 3,297 | +3% |
| CALL_REFERENCE | 2,398 | 3,264 | +36% |
| REFERENCES_FILE | - | 3,112 |  |
| CONFIGURES | 1,770 | 1,737 | -2% |
| TESTS | 1,667 | 1,425 | -15% |
| DIAGRAM_ARROW | - | 962 |  |
| SEMANTICALLY_RELATED | 942 | 519 | -45% |
| HANDLES | 177 | 880 | +397% |
| RAISES | 865 | 853 | -1% |
| HTTP_CALLS | 516 | 846 | +64% |
| THROWS | 714 | 712 | -0% |
| SPAWNS | - | 713 |  |
| TESTS_FILE | 518 | 518 | +0% |
| DEPENDS_ON | 134 | 187 | +40% |
| GRPC_CALLS | 20 | 20 | +0% |
| LISTENS_ON | 14 | 14 | +0% |
| HAS_BRANCH | 1 | 1 | +0% |
| EMITS | 1 | 1 | +0% |

## Correctness probes

| Metric | old | new | Change |
|---|---|---|---|
| faultrule_usage_from_alertrule | 1 (pass) | 1 (pass) |  |
| troublecode_with_where | 171 (FAIL) | 31 (pass) |  |
| troublecode_match_where | 31 (info) | 31 (info) |  |
| vehicledto_to_samsara_from_other_repos | 532 (FAIL) | 0 (pass) |  |
| usage_to_vehicledto_constructor | 0 (pass) | 0 (pass) |  |
| framework_calls_GetEmailRecipientsAsync | 2 (FAIL) | 0 (pass) |  |
| framework_calls_TruckContext_GetByVinAsync | 4 (FAIL) | 0 (pass) |  |
| inherits_base_with_constructor | 111 (pass) | 111 (pass) |  |
| troublecode_3hop_consumers | 304 (info) | 306 (info) |  |
| cs_method_typed_calls | - (FAIL) error: query exceeded the execution time limit — narrow the pattern | 14,217 (info) |  |
| scoped_bm25_total | 95 (pass) | 96 (pass) |  |

## Latency, one server, median of rounds (ms)

| Metric | old | new | Change |
|---|---|---|---|
| search_name_exact | 139 | 29 | -79% |
| search_bm25_scoped | 400 | 392 | -2% |
| search_bm25_global | 956 | 890 | -7% |
| query_1hop_count | 1,100 | 699 | -36% |
| query_3hop_rows | 1,165 | 728 | -38% |
| trace_outbound_d2 | 15 | 16 | +7% |
| search_code_scoped | 973 | 501 | -49% |
| code_snippet | 15 | 15 | +0% |

## 16 concurrent requests on one server (ms)

| Metric | old | new | Change |
|---|---|---|---|
| burst 1 wall_ms | 6,271 | 1,484 | -76% |
| burst 1 median_ms | 3,670 | 737 | -80% |
| burst 2 wall_ms | 6,180 | 1,438 | -77% |
| burst 2 median_ms | 3,612 | 723 | -80% |
| first call (`list_projects`; see KNOWN_ISSUES 7) | 74 | 1,553 |  |
| server peak working set (MB) | 2,951 | 232 | -92% |

