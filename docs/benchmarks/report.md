# Kestrel engine benchmark — full-1919

- Run: `bench/results/full-1919/results.json`
- Commit: `15ee6c7c81ae (dirty)` | GCC 14 | Intel(R) Xeon(R) Processor
- Kernel 5.10.134-013.15.kangaroo.al8.x86_64 | clocksource tsc | governor None | KVM (shared 2-vCPU guest; numbers indicative only)
- Clock: tsc_rdtscp @ 3.202 ticks/ns | pinned cpu 0 of 2 | quick=False

**Timing regions (contract):**

- tick-to-trade: t0 before run_once(1) (frame pre-pushed) -> t1 after return; includes poll+journal+decode+book+strategy+risk+encode+tx_submit; order-producing events only
- book throughput: decode_header + book apply; fresh FeedHandler per pass; no strategy/risk/encode/journal/engine loop

## Tick-to-trade (software, order-producing MD events only)

| config | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| minimal (no journal) | 4525 | 301 ns | 387 ns | 456 ns | 528 ns | 627 ns | 6.29 us | 6.29 us |
| minimal (journal) | 4525 | 637 ns | 1.02 us | 1.36 us | 1.74 us | 14.26 us | 18.24 us | 18.23 us |
| stage-timed (ext stamps) | 4525 | 841 ns | 1.28 us | 1.64 us | 2.06 us | 14.14 us | 21.01 us | 21.00 us |
| stage-timed (internal) | 68199 | 530 ns | 1.79 us | 2.58 us | 4.67 us | 38.91 us | 57.38 us | 68.24 us |
| exec-driven (minimal) | 785 | 289 ns | 365 ns | 435 ns | 504 ns | 557 ns | 587 ns | 587 ns |

- observer overhead (stage-timed minus minimal, p50): **898 ns** — the cost of the 3 extra TSC reads + histogram records per event
- journal ring write cost (p50): **637 ns** (critical-path write into the preallocated journal ring, IO thread drains excluded)
- workload: md=52732 exec=15467 tx=7929 intents=36780 (rej=16303) | events=64995 | rx_backlog_max=0 (open-loop verified)
- determinism cross-check: minimal~journal=OK, minimal~stage=OK

## Feed + book throughput (single core, decode + apply only)

- frames: 400,000 (12,666,235 bytes) | stream_clean=**True** (applied=400000, gaps=0, dups=0, anomalies=0)
- median **35.7 ns/frame** | best **28.79 Mframes/s**
- per-pass ns/frame: 35.7, 35.8, 34.7, 35.0

## Timer overhead / jitter baseline

| measurement | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| TSC read pair (rdtscp) | 100000 | 50 ns | 56 ns | 58 ns | 70 ns | 82 ns | 278 ns | 31.22 us |
| sleep overshoot 100us | 2000 | 4.29 us | 54.81 us | 55.81 us | 59.33 us | 68.86 us | 254.21 us | 254.14 us |
| sleep overshoot 100us (timerslack=1) | 2000 | 3.28 us | 3.46 us | 6.53 us | 28.61 us | 87.23 us | 121.15 us | 121.09 us |
| sleep overshoot 1ms | 500 | 29.14 us | 56.67 us | 64.93 us | 104.38 us | 177.02 us | 177.02 us | 176.97 us |

- steady_clock::now pair: 23.4 ns | rdtsc unordered pair: 41.2 ticks | core migrations: 0 (100us loop)
- interpretation: 'timer overhead' = absolute-deadline clock_nanosleep wakeup overshoot + clock read cost. The V1 engine's decision path uses event time only (no OS timers), so these bound the periodic-task costs (telemetry, IO pacing), not tick-to-trade.

## Honest limits of this run

- Shared 2-vCPU KVM guest, no isolcpus/no governor control, kernel 5.10 with default timer slack measured alongside a tuned variant. Numbers are indicative, not acceptance-grade; the wish-metric targets in the plan (t2t p50 < 500 ns, 10 Mmsg/s/core) are compared against as context only.
- TX submit = copy into the replay backend's TX arena + ring enqueue (software semantics), NOT a NIC doorbell; feed-to-order excludes the NIC and the wire.
- Raw distributions: `t2t_*.csv` next to this report.
