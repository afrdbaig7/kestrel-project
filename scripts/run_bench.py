#!/usr/bin/env python3
"""Run the kestrel engine benchmark and record full reproducibility metadata.

Usage: scripts/run_bench.py [--quick] [--label LABEL] [--out DIR]

Produces <out>/results.json (benchmark output merged with host metadata),
plus the raw-sample CSVs the binary emits. The metadata follows the
methodology contract in docs/benchmarks/report.md: commit, compiler, flags,
CPU model/features, kernel, affinity, clock source, governor state, config
echo, and seed. Results from THIS sandbox are indicative only (shared 2-vCPU
KVM guest, no isolcpus) — not acceptance-grade numbers.
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=30).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"<error: {e}>"


def read_first(path, pattern=None):
    try:
        with open(path) as f:
            data = f.read().strip()
        if pattern:
            m = re.search(pattern, data)
            return m.group(1) if m else data
        return data
    except OSError:
        return None


def collect_metadata():
    meta = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "host": platform.node(),
        "kernel": platform.release(),
        "cpu_model": read_first("/proc/cpuinfo", r"model name\s*:\s*(.*)\n"),
        "cpu_mhz": read_first("/proc/cpuinfo", r"cpu MHz\s*:\s*(.*)\n"),
        "cpu_flags_present": [
            f for f in ("constant_tsc", "nonstop_tsc", "rdtscp", "invariant_tsc",
                        "avx2", "avx512f", "bmi2")
            if f in read_first("/proc/cpuinfo", r"flags\s*:\s*(.*)\n") or ""
        ],
        "cpu_count": os.cpu_count(),
        "clocksource": read_first(
            "/sys/devices/system/clocksource/clocksource0/current_clocksource"),
        "governor": read_first(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "isolcpus": "not configured (shared VM)" ,
        "task_isolation": "none",
        "compiler": sh("g++ -dumpversion").replace("<error", "gcc-<error"),
        "compiler_full": sh("g++ --version | head -1"),
        "cmake": sh("cmake --version | head -1"),
        "ninja": sh("ninja --version"),
        "build_type": "Release (-O3; bench preset adds -march=native)",
        "git_commit": sh(f"git -C {REPO} rev-parse HEAD"),
        "git_dirty": bool(sh(f"git -C {REPO} status --porcelain")),
        "hypervisor": "KVM (shared 2-vCPU guest; numbers indicative only)",
    }
    return meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", default=None)
    ap.add_argument("--binary", default=None)
    args = ap.parse_args()

    out = args.out or os.path.join(REPO, "bench", "results", args.label)
    binary = args.binary or os.path.join(REPO, "build", "release", "bench",
                                         "kestrel_bench_engine")
    if not os.path.exists(binary):
        sys.exit(f"bench binary not found: {binary} (build the release preset)")

    cmd = [binary, "--label", args.label, "--out", out]
    if args.quick:
        cmd.append("--quick")
    print("+", " ".join(cmd), flush=True)
    rc = subprocess.run(cmd).returncode
    if rc != 0:
        sys.exit(f"bench binary failed: rc={rc}")

    results_path = os.path.join(out, "results.json")
    with open(results_path) as f:
        results = json.load(f)
    results["host_metadata"] = collect_metadata()
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
        f.write("\n")
    print(f"metadata merged -> {results_path}")


if __name__ == "__main__":
    main()
