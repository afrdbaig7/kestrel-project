#!/usr/bin/env python3
"""Analyze a kestrel bench results.json into a markdown report, optionally
comparing against a baseline (regression REPORT ONLY — this sandbox is not a
controlled runner, so thresholds are advisory; see docs/benchmarks/report.md).

Usage:
  scripts/analyze_bench.py bench/results/<label>/results.json [--baseline OLD.json] [-o out.md]
"""

import argparse
import json
import sys


def fmt_ns(v):
    if v is None:
        return "-"
    if v >= 1_000_000:
        return f"{v/1e6:.2f} ms"
    if v >= 1_000:
        return f"{v/1e3:.2f} us"
    return f"{v} ns"


def stat_row(name, s, base=None, unit="ns"):
    cols = [name, str(s.get("count", 0))]
    for p in ("min", "p50", "p90", "p99", "p999", "p9999", "max"):
        key = f"{p}_ns" if unit == "ns" else p
        v = s.get(key, 0)
        cols.append(fmt_ns(v) if unit == "ns" else str(v))
    if base:
        delta = s.get("p50_ns", 0) - base.get("p50_ns", 0)
        pct = (100.0 * delta / base["p50_ns"]) if base.get("p50_ns") else 0.0
        cols.append(f"{delta:+.0f} ns ({pct:+.1f}%)")
    return "| " + " | ".join(cols) + " |"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--baseline")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    with open(args.results) as f:
        r = json.load(f)
    base = None
    if args.baseline:
        with open(args.baseline) as f:
            base = json.load(f)

    lines = []
    a = lines.append
    t2t = r["tick_to_trade"]
    tp = r["book_throughput"]
    tm = r["timer"]
    hm = r.get("host_metadata", {})

    a(f"# Kestrel engine benchmark — {r.get('label', '?')}")
    a("")
    a(f"- Run: `{args.results}`")
    if hm:
        a(f"- Commit: `{hm.get('git_commit','?')[:12]}{' (dirty)' if hm.get('git_dirty') else ''}`"
          f" | GCC {hm.get('compiler','?')} | {hm.get('cpu_model','?')}")
        a(f"- Kernel {hm.get('kernel','?')} | clocksource {hm.get('clocksource','?')}"
          f" | governor {hm.get('governor','(none)')} | {hm.get('hypervisor','')}")
    a(f"- Clock: {r.get('clock_source')} @ {r.get('ticks_per_ns', 0):.3f} ticks/ns"
      f" | pinned cpu {r.get('pinned_cpu')} of {r.get('allowed_cpus')}"
      f" | quick={r.get('quick')}")
    a("")
    a("**Timing regions (contract):**")
    a("")
    a(f"- tick-to-trade: {t2t['region']}")
    a(f"- book throughput: {tp['region']}")
    a("")
    a("## Tick-to-trade (software, order-producing MD events only)")
    a("")
    a("| config | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |"
      + (" p50 delta |" if base else ""))
    a("|---|---|---|---|---|---|---|---|---|" + ("---|" if base else ""))
    a(stat_row("minimal (no journal)", t2t["md_minimal_no_journal"],
               base and base["tick_to_trade"]["md_minimal_no_journal"]))
    a(stat_row("minimal (journal)", t2t["md_minimal_journal"],
               base and base["tick_to_trade"]["md_minimal_journal"]))
    a(stat_row("stage-timed (ext stamps)", t2t["md_stage_timed_external"],
               base and base["tick_to_trade"]["md_stage_timed_external"]))
    a(stat_row("stage-timed (internal)", t2t["md_stage_timed_internal_all_frames"],
               base and base["tick_to_trade"]["md_stage_timed_internal_all_frames"]))
    a(stat_row("exec-driven (minimal)", t2t["exec_minimal_no_journal"],
               base and base["tick_to_trade"]["exec_minimal_no_journal"]))
    a("")
    a(f"- observer overhead (stage-timed minus minimal, p50): "
      f"**{t2t.get('observer_overhead_ns_p50', 0)} ns** — the cost of the 3 "
      f"extra TSC reads + histogram records per event")
    a(f"- journal ring write cost (p50): **{t2t.get('journal_cost_ns_p50', 0)} ns**"
      f" (critical-path write into the preallocated journal ring, IO thread "
      f"drains excluded)")
    w = t2t["workload"]
    a(f"- workload: md={w['frames_md']} exec={w['frames_exec']} tx={w['tx_enqueued']}"
      f" intents={w['intents']} (rej={w['intents_rejected']}) | events={w['events']}"
      f" | rx_backlog_max={w['rx_backlog_max']} (open-loop verified)")
    a(f"- determinism cross-check: minimal~journal="
      f"{'OK' if t2t['workload_match_minimal_vs_journal'] else 'MISMATCH'}, "
      f"minimal~stage={'OK' if t2t['workload_match_minimal_vs_stage'] else 'MISMATCH'}")
    a("")
    a("## Feed + book throughput (single core, decode + apply only)")
    a("")
    a(f"- frames: {tp['frames']:,} ({tp['stream_bytes']:,} bytes) | stream_clean="
      f"**{tp['stream_clean']}** (applied={tp['stream_applied']}, gaps={tp['stream_gaps']},"
      f" dups={tp['stream_duplicates']}, anomalies={tp['stream_rejects_anomaly']})")
    a(f"- median **{tp['median_ns_per_frame']:.1f} ns/frame** | best "
      f"**{tp['best_mframes_per_sec']:.2f} Mframes/s**")
    a("- per-pass ns/frame: " + ", ".join(f"{v:.1f}" for v in tp["pass_ns_per_frame"]))
    a("")
    a("## Timer overhead / jitter baseline")
    a("")
    a("| measurement | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |")
    a("|---|---|---|---|---|---|---|---|---|")
    a(stat_row("TSC read pair (rdtscp)", tm["tsc_read_pair"]))
    a(stat_row("sleep overshoot 100us", tm["sleep_overshoot_100us"]))
    a(stat_row("sleep overshoot 100us (timerslack=1)",
               tm["sleep_overshoot_100us_timerslack_1"]))
    a(stat_row("sleep overshoot 1ms", tm["sleep_overshoot_1ms"]))
    a("")
    a(f"- steady_clock::now pair: {tm['steady_now_pair_ns']:.1f} ns | "
      f"rdtsc unordered pair: {tm['rdtsc_pair_ticks']:.1f} ticks | "
      f"core migrations: {tm['core_changes_100us']} (100us loop)")
    a("- interpretation: 'timer overhead' = absolute-deadline clock_nanosleep "
      "wakeup overshoot + clock read cost. The V1 engine's decision path uses "
      "event time only (no OS timers), so these bound the periodic-task "
      "costs (telemetry, IO pacing), not tick-to-trade.")
    a("")
    a("## Honest limits of this run")
    a("")
    a("- Shared 2-vCPU KVM guest, no isolcpus/no governor control, kernel 5.10 "
      "with default timer slack measured alongside a tuned variant. Numbers are "
      "indicative, not acceptance-grade; the wish-metric targets in the plan "
      "(t2t p50 < 500 ns, 10 Mmsg/s/core) are compared against as context only.")
    a("- TX submit = copy into the replay backend's TX arena + ring enqueue "
      "(software semantics), NOT a NIC doorbell; feed-to-order excludes the "
      "NIC and the wire.")
    a("- Raw distributions: `t2t_*.csv` next to this report.")

    if base:
        a("")
        a("## Regression view vs baseline (advisory — uncontrolled runner)")
        a("")
        a(f"- baseline: `{args.baseline}`")

    out = "\n".join(lines) + "\n"
    if args.output:
        with open(args.output, "w") as f:
            f.write(out)
        print(f"report -> {args.output}")
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
