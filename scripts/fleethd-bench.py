"""Benchmark one codebase-memory-mcp build on the FleetHd monolith, or compare two runs.

usage:
  python -I scripts/fleethd-bench.py run --label old --exe <exe> --cache <dir> [--fresh] [--rounds 5]
  python -I scripts/fleethd-bench.py compare <a.json> <b.json> [--md <out.md>]

`run` uses its own cache folder (under ~/.cache/cbm-bench) and a runtime folder <cache>-rt, so it runs beside
the live daemon. `--fresh` deletes that cache's monolith DB first and measures a full index.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

import psutil

PROJECT = "C-Users-Max-source-repos"
REPOS = "C:/Users/Max/source/repos"
BENCH_ROOT = os.path.join(os.path.expanduser("~"), ".cache", "cbm-bench")


# Known-answer probes: (name, kind, args, evaluator(result) -> (value, pass or None for information only)).
def rows_of(res):
    return res.get("rows") or []


def count_of(res):
    rows = rows_of(res)
    return int(str(rows[0][0]).strip('"')) if rows else 0


def zero(res):
    return count_of(res), count_of(res) == 0


PROBES = [
    ("faultrule_usage_from_alertrule", "query_graph",
     "MATCH (b {name:'FaultRuleDto'})<-[:USAGE]-(a {name:'AlertRuleDto'}) RETURN a.file_path",
     lambda r: (len(rows_of(r)), any("fleet-hd-contracts" in str(x[0]) for x in rows_of(r)))),
    ("troublecode_with_where", "query_graph",
     "MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) WITH a WHERE NOT (a.file_path CONTAINS 'Test') "
     "RETURN count(a)", lambda r: (count_of(r), None)),
    ("troublecode_match_where", "query_graph",
     "MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) WHERE NOT (a.file_path CONTAINS 'Test') RETURN count(a)",
     lambda r: (count_of(r), None)),
    ("vehicledto_to_samsara_from_other_repos", "query_graph",
     "MATCH (b {name:'VehicleDto'})<-[:USAGE]-(a) WHERE b.file_path CONTAINS 'fleet-hd-samsara-contracts' "
     "AND NOT (a.file_path CONTAINS 'samsara') RETURN count(a)", lambda r: (count_of(r), count_of(r) == 0)),
    ("usage_to_vehicledto_constructor", "query_graph",
     "MATCH (c:Method {name:'VehicleDto'})<-[:USAGE]-(a) RETURN count(a)",
     lambda r: (count_of(r), count_of(r) == 0)),
    # Framework calls (LINQ, string, ControllerBase) that bind a same-named method in another repo.
    ("framework_calls_GetEmailRecipientsAsync", "query_graph",
     "MATCH (m {name:'GetEmailRecipientsAsync'})-[:CALLS]->(t) WHERE t.name IN ['Select','IsNullOrWhiteSpace'] "
     "RETURN count(t)", zero),
    ("framework_calls_TruckContext_GetByVinAsync", "query_graph",
     "MATCH (m {name:'GetByVinAsync'})-[:CALLS]->(t) WHERE m.file_path CONTAINS 'TruckContextQueryController' "
     "AND t.name IN ['Ok','NotFound','IsNullOrWhiteSpace','IsNullOrEmpty'] RETURN count(t)", zero),
    ("inherits_base_with_constructor", "query_graph",
     "MATCH (b {name:'OrderedQueryRepository'})<-[:INHERITS]-(a) RETURN count(a)",
     lambda r: (count_of(r), count_of(r) > 0)),
    ("troublecode_3hop_consumers", "query_graph",
     "MATCH (tc {name:'TroubleCodeDto'})<-[:USAGE]-(fr {name:'FaultRuleDto'})<-[:USAGE]-"
     "(ar {name:'AlertRuleDto'})<-[:USAGE]-(m) RETURN count(DISTINCT m)", lambda r: (count_of(r), None)),
    ("cs_method_typed_calls", "query_graph",
     "MATCH (a)-[r:CALLS]->(b) WHERE r.strategy = 'cs_method_typed' RETURN count(r)",
     lambda r: (count_of(r), None)),
    ("scoped_bm25_total", "search_graph",
     {"query": "get async vehicle", "file_pattern": "*fleet-hd-alert-notification/*"},
     lambda r: (r.get("total"), (r.get("total") or 0) > 0 and r.get("total_relation") == "eq")),
]

LATENCY = [
    ("search_name_exact", "search_graph", {"name_pattern": "^VehicleDto$"}),
    ("search_bm25_scoped", "search_graph",
     {"query": "get async vehicle", "file_pattern": "*fleet-hd-alert-notification/*"}),
    ("search_bm25_global", "search_graph", {"query": "get async vehicle", "limit": 20}),
    ("query_1hop_count", "query_graph",
     {"query": "MATCH (b {name:'TroubleCodeDto'})<-[:USAGE]-(a) RETURN count(a)"}),
    ("query_3hop_rows", "query_graph",
     {"query": "MATCH (tc {name:'TroubleCodeDto'})<-[:USAGE]-(fr {name:'FaultRuleDto'})<-[:USAGE]-"
               "(ar {name:'AlertRuleDto'})<-[:USAGE]-(m) RETURN DISTINCT m.name, m.file_path LIMIT 200"}),
    ("trace_outbound_d2", "trace_path", {"function_name": "GetByVinAsync", "direction": "outbound", "depth": 2}),
    ("search_code_scoped", "search_code", {"pattern": "TroubleCodeDto", "path_filter": "fleet-hd-contracts"}),
    ("code_snippet", "get_code_snippet", None),  # qualified_name filled in at run time
]

BURST = ["query_1hop_count", "search_name_exact", "search_bm25_scoped", "trace_outbound_d2"]


class Server:
    """One MCP stdio server; calls may run from several threads."""

    def __init__(self, exe, env):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  env=env, text=True, encoding="utf-8", bufsize=1)
        self.lock = threading.Lock()
        self.waiters = {}
        self.next_id = 1
        threading.Thread(target=self._reader, daemon=True).start()
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "fleethd-bench", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, msg):
        with self.lock:
            self.p.stdin.write(json.dumps(msg) + "\n")
            self.p.stdin.flush()

    def _reader(self):
        for line in self.p.stdout:
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            w = self.waiters.pop(msg.get("id"), None)
            if w:
                w["msg"] = msg
                w["end"] = time.perf_counter()
                w["ev"].set()

    def request(self, method, params, timeout=300):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
        w = {"ev": threading.Event()}
        self.waiters[rid] = w
        t0 = time.perf_counter()
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        if not w["ev"].wait(timeout):
            return None, timeout * 1000.0
        return w["msg"], (w["end"] - t0) * 1000.0

    def tool(self, name, args):
        msg, ms = self.request("tools/call", {"name": name, "arguments": dict(args, project=PROJECT)})
        if msg is None:
            return {"error": "timeout"}, ms
        res = msg.get("result") or {}
        text = "".join(c.get("text", "") for c in res.get("content", []) if isinstance(c, dict))
        out = {"error": text[:300]} if res.get("isError") or "error" in msg else {}
        try:
            parsed = json.loads(text)
            if isinstance(parsed, dict):
                out.update(parsed)
        except ValueError:
            out["text"] = text
            for line in text.splitlines():  # compact text output: `total: 96`
                key, sep, value = line.partition(": ")
                if sep and key.isidentifier() and " " not in value:
                    out.setdefault(key, int(value) if value.isdigit() else value)
        return out, ms

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(15)
        except Exception:
            self.p.kill()


def exe_rss(exe, peak=False):
    """Summed RSS (MB) of every process running this exe file (CLI, frontend, daemon); with `peak`,
    the summed peak working set of each process so far (Windows)."""
    target = os.path.normcase(os.path.abspath(exe))
    total = 0
    for p in psutil.process_iter(["exe", "memory_info"]):
        try:
            if p.info["exe"] and os.path.normcase(p.info["exe"]) == target:
                mi = p.info["memory_info"]
                total += getattr(mi, "peak_wset", mi.rss) if peak else mi.rss
        except (psutil.Error, TypeError):
            continue
    return total / 1e6


def run_cli(exe, env, tool, args, sample=False):
    proc = subprocess.Popen([exe, "cli", tool, json.dumps(args)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env, text=True, encoding="utf-8", errors="replace")
    peak, t0 = 0.0, time.perf_counter()
    while sample and proc.poll() is None:
        peak = max(peak, exe_rss(exe))
        time.sleep(0.5)
    out = proc.communicate()[0]
    return out, time.perf_counter() - t0, peak


def parse_index(out):
    for line in reversed(out.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                pass
    return {}


def parse_schema(text):
    section, out = None, {"node_labels": {}, "edge_types": {}}
    for line in text.splitlines():
        head = line.split(":", 1)[0]
        if head in out:
            section = head
        elif line.startswith("  ") and section:
            parts = line.split()
            if len(parts) == 2 and parts[1].isdigit():
                out[section][parts[0]] = int(parts[1])
        else:
            section = None
    return out


def cmd_run(a):
    exe = os.path.abspath(a.exe)
    cache = os.path.abspath(a.cache)
    if not os.path.normcase(cache).startswith(os.path.normcase(BENCH_ROOT)):
        sys.exit(f"FAIL: --cache must be under {BENCH_ROOT}")
    rt = cache + "-rt"
    os.makedirs(cache, exist_ok=True)
    os.makedirs(rt, exist_ok=True)
    env = dict(os.environ, CBM_CACHE_DIR=cache, CBM_RUNTIME_DIR=rt)
    version = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    result = {"label": a.label, "exe": exe, "version": version, "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    print(f"run {a.label}: {version}", flush=True)

    db = os.path.join(cache, PROJECT + ".db")
    if a.fresh:
        for suffix in ("", "-wal", "-shm"):
            if os.path.exists(db + suffix):
                os.remove(db + suffix)
    if a.fresh or not os.path.exists(db):
        attempts = []
        for _ in range(3):
            out, secs, peak = run_cli(exe, env, "index_repository", {"repo_path": REPOS}, sample=True)
            info = parse_index(out)
            attempts.append({"secs": round(secs, 1), "peak_rss_mb": round(peak), "status": info.get("status")})
            print(f"  index: {attempts[-1]}", flush=True)
            if info.get("status") != "aborted_previous_preserved":
                break
        result["index"] = dict(attempts[-1], attempts=len(attempts), nodes=info.get("nodes"),
                               edges=info.get("edges"), db_mb=round(os.path.getsize(db) / 1e6)
                               if os.path.exists(db) else None)
    else:
        result["index"] = {"skipped": "existing DB, no --fresh"}

    schema_out, _, _ = run_cli(exe, env, "get_graph_schema", {"project": PROJECT, "limit": 200})
    result["schema"] = parse_schema(schema_out)
    if not result["schema"]["edge_types"]:
        print(f"WARN: no edge types parsed from get_graph_schema: {schema_out[:200]!r}", flush=True)

    t0 = time.perf_counter()
    srv = Server(exe, env)  # no daemon runs for this cache yet: a cold start
    result["first_call_ms"] = round((time.perf_counter() - t0) * 1000)
    _, list_ms = srv.tool("list_projects", {})
    result["list_projects_ms"] = round(list_ms)

    probes = {}
    for name, kind, q, judge in PROBES:
        args = {"query": q, "format": "json"} if kind == "query_graph" else dict(q)
        res, ms = srv.tool(kind, args)
        if res.get("error"):
            probes[name] = {"value": None, "pass": False, "error": res["error"], "ms": round(ms)}
            continue
        try:
            value, ok = judge(res)
        except Exception as e:  # an unexpected result shape is a probe failure, not a crash
            value, ok = None, False
            res = {"error": f"unparsed result: {e}"}
        probes[name] = {"value": value, "pass": ok, "ms": round(ms)}
        if res.get("error"):
            probes[name]["error"] = res["error"]
    w, m = probes["troublecode_with_where"], probes["troublecode_match_where"]
    w["pass"] = w["value"] is not None and w["value"] == m["value"]
    result["probes"] = probes

    res, _ = srv.tool("query_graph", {"format": "json", "query": "MATCH (n:Class {name:'AlertRuleDto'}) WHERE "
                                      "n.file_path CONTAINS 'fleet-hd-contracts' RETURN n.qualified_name"})
    snippet_qn = rows_of(res)[0][0] if rows_of(res) else None
    calls = [(n, k, {"qualified_name": snippet_qn} if k == "get_code_snippet" else args) for n, k, args in LATENCY]

    latency = {}
    for name, kind, args in calls:
        srv.tool(kind, args)  # warm-up
        times, err = [], None
        for _ in range(a.rounds):
            res, ms = srv.tool(kind, args)
            times.append(ms)
            err = err or res.get("error")
        latency[name] = {"median_ms": round(statistics.median(times)), "max_ms": round(max(times)),
                         "min_ms": round(min(times)), "error": err}
        print(f"  {name}: {latency[name]}", flush=True)
    result["latency"] = latency

    by_name = {n: (k, args) for n, k, args in calls}
    bursts = []
    for _ in range(2):
        jobs = [by_name[BURST[i % len(BURST)]] for i in range(16)]
        lat, threads = [], []
        t0 = time.perf_counter()
        for kind, args in jobs:
            th = threading.Thread(target=lambda k=kind, g=args: lat.append(srv.tool(k, g)[1]))
            th.start()
            threads.append(th)
        for th in threads:
            th.join()
        bursts.append({"wall_ms": round((time.perf_counter() - t0) * 1000),
                       "median_ms": round(statistics.median(lat))})
    result["burst16"] = bursts
    result["server_rss_mb"] = round(exe_rss(exe, peak=True))
    srv.close()

    out_dir = os.path.join(BENCH_ROOT, "results")
    os.makedirs(out_dir, exist_ok=True)
    path = a.out or os.path.join(out_dir, f"{a.label}-{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    failed = [n for n, p in probes.items() if p["pass"] is False]
    print(f"OK: wrote {path}; probes failed: {', '.join(failed) or '(none)'}")


def fmt(v):
    return "-" if v is None else (f"{v:,}" if isinstance(v, int) else str(v))


def delta(x, y):
    if isinstance(x, (int, float)) and isinstance(y, (int, float)) and x:
        return f"{(y - x) / x * 100:+.0f}%"
    return ""


def cmd_compare(a):
    A, B = (json.load(open(p, encoding="utf-8")) for p in (a.a, a.b))
    la, lb = A["label"], B["label"]
    lines = [f"# Benchmark: {la} ({A['version']}) vs {lb} ({B['version']})", "",
             f"Runs started {A['started']} and {B['started']}.", ""]

    def table(title, rows, cols=("Metric", la, lb, "Change")):
        lines.extend([f"## {title}", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)])
        lines.extend("| " + " | ".join(r) + " |" for r in rows)
        lines.append("")

    ia, ib = A.get("index", {}), B.get("index", {})
    table("Full index", [[k, fmt(ia.get(k)), fmt(ib.get(k)), delta(ia.get(k), ib.get(k))]
                         for k in ("secs", "peak_rss_mb", "nodes", "edges", "db_mb", "attempts", "status")])
    ea, eb = A["schema"]["edge_types"], B["schema"]["edge_types"]
    keys = sorted(set(ea) | set(eb), key=lambda k: -max(ea.get(k, 0), eb.get(k, 0)))
    table("Edges by type", [[k, fmt(ea.get(k)), fmt(eb.get(k)), delta(ea.get(k), eb.get(k))] for k in keys])
    pa, pb = A["probes"], B["probes"]

    def pv(p):
        if not p:
            return "-"
        mark = {True: "pass", False: "FAIL", None: "info"}[p["pass"]]
        return f"{fmt(p['value'])} ({mark})" + (f" error: {p['error'][:60]}" if p.get("error") else "")

    table("Correctness probes", [[k, pv(pa.get(k)), pv(pb.get(k)), ""] for k in pa])
    xa, xb = A["latency"], B["latency"]
    table("Latency, one server, median of rounds (ms)",
          [[k, fmt(xa[k]["median_ms"]), fmt(xb[k]["median_ms"]), delta(xa[k]["median_ms"], xb[k]["median_ms"])]
           for k in xa])
    ba, bb = A["burst16"], B["burst16"]
    table("16 concurrent requests on one server (ms)",
          [[f"burst {i + 1} {m}", fmt(ba[i][m]), fmt(bb[i][m]), delta(ba[i][m], bb[i][m])]
           for i in range(2) for m in ("wall_ms", "median_ms")] +
          [["initialize, cold daemon", fmt(A["first_call_ms"]), fmt(B["first_call_ms"]), ""],
           ["list_projects", fmt(A.get("list_projects_ms")), fmt(B.get("list_projects_ms")), ""],
           ["server peak working set (MB)", fmt(A["server_rss_mb"]), fmt(B["server_rss_mb"]),
            delta(A["server_rss_mb"], B["server_rss_mb"])]])
    text = "\n".join(lines)
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    print(text)
    print(f"OK: compared {la} and {lb}" + (f"; wrote {a.md}" if a.md else ""))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--label", required=True)
    r.add_argument("--exe", required=True)
    r.add_argument("--cache", required=True)
    r.add_argument("--fresh", action="store_true")
    r.add_argument("--rounds", type=int, default=5)
    r.add_argument("--out")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--md")
    a = ap.parse_args()
    cmd_run(a) if a.cmd == "run" else cmd_compare(a)


if __name__ == "__main__":
    main()
