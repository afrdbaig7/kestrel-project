# ADR-0003: In-repo HdrHistogram implementation for hot-path percentile recording

Date: 2026-11-01 · Status: accepted

## Context

The spec asks for Google Benchmark and HdrHistogram. Google Benchmark is fetched and
pinned (v1.9.0). For latency recording *inside* the engine process (jitter meter,
latency harnesses) we need a bounded-memory, dependency-free, allocation-free-after-
init recorder. HdrHistogram_c could be FetchContent'ed, but the hot-path rule says the
core stays free of external dependencies, and the bucket algorithm is a published,
well-specified design.

## Decision

`include/kestrel/core/histogram.hpp` implements the HdrHistogram algorithm
(log-linear buckets, configurable significant figures — default 3 — value range and
unit magnitude identical to the reference implementation):

- `counts_index = (bucket+1) << sub_bucket_half_count_magnitude + (sub - sub_bucket_half_count)`
- `bucket_index = pow2ceil(v | sub_bucket_mask) - unit_magnitude - (sub_bucket_half_count_magnitude + 1)`
- values above `highest_trackable` are clamped into the top bucket and counted in a
  dedicated overflow counter.

Unit tests validate percentile math against brute-force order statistics on known
samples, monotonic percentile curves, merge correctness, and CSV export.

## Consequences

- Hot path has zero external dependencies; memory per histogram ≈
  `(bucket_count+1) * sub_bucket_half_count * 8 B` (≈ 184 KiB for 3 sig-figs, 0–1 s
  range) — acceptable off-path, and harnesses allocate once at init.
- Google Benchmark is still used for all micro-benchmarks; the in-repo histogram is
  used for latency distributions (tick-to-trade, jitter) so that the same code path
  is exercised in tests and in the field.
