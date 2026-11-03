# Kestrel — Architecture (Version 1: software engine & platform)

Provisional internal codename; renaming is deferred to shipping. This
document describes what IS IMPLEMENTED AND TESTED in this repository. Every
deferred item is listed in §9 with the reason. Companion records: ADR-0001
… ADR-0007 in `docs/adr/`, benchmark methodology in `docs/benchmarks/report.md`,
session/continuation state in `CHECKLIST.md`.

## 1. Scope and versions

- **V1 (this repo)**: software engine on plain Linux — in-memory replay
  backend, deterministic venue simulator, full feed→order engine, binary
  journal with deterministic replay, cold-path projections, demo console.
- **V2 (requires hardware)**: DPDK physical NIC, AF_XDP zero-copy, hardware
  timestamps. Directories are created only when implementation starts
  (no stub code). Build options exist (`KESTREL_ENABLE_AF_XDP/DPDK`, default
  OFF) so the replay build never needs them.
- **V3 (optional)**: FPGA golden-model track. Not started.
- Simulated exchange only; no real-money trading; no profitability claims.

## 2. Hot path (engine.hpp)

One pinned thread, run-to-completion per event:

```
poll_rx → journal_input (fail-closed) → decode (KITCH/KOUCH) → route
→ book update (FeedHandler→LevelBook) → strategy → pre-trade risk
→ order encode (KOUCH) → tx_submit → (intents from the same event run
through risk before the next frame is polled)
```

- Post-init guarantees, enforced by tests (`test_allocation_guard.cpp`):
  zero heap allocation, no locks, no blocking I/O, no virtual dispatch on
  the event chain (Backend/Strategy are compile-time template parameters).
- Integer prices/quantities/notionals; `checked_notional` guards the
  qty×price multiply (documented `__int128` intermediate on x86-64).
- Bounded storage everywhere with defined exhaustion behavior: intent
  buffer (strategy re-emits next update), OMS map (drop + counter +
  reservation release), TX retries (bounded in-place, then retry queue,
  then abandon → SendFailed + reservation release), journal ring
  (critical overflow ⇒ fail-closed kill switch + Gap record).
- Live time never reaches a decision path; the engine runs on **event time**
  (journal timestamps). Kill switch blocks NEW exposure at the event
  boundary; cancels remain a separate workflow.
- Stage timing is a compile-time option (`Engine<..., kStageTiming=true>`)
  used by the bench to quantify observer overhead (measured: +898 ns p50 —
  `docs/benchmarks/report.md`).

## 3. Primitives (core/)

`spsc_ring` (acquire/release, padded cursors), `object_pool` (generation-
checked slots), `flat_map` (open addressing, backward-shift delete),
`bitmap` (price-level scans), `clock` (ordered rdtscp TSC + monotonic
fallback + off-path calibration), `histogram` (HdrHistogram algorithm),
`cpu` (affinity helpers), `jitter_meter` (sleep overshoot / clock read cost
/ core migrations). MPSC and seqlock are deliberately NOT implemented (no
concrete consumer yet).

## 4. Protocols and backends (protocols/, net/, backends/)

- **KITCH** (market data, ITCH-inspired, big-endian) and **KOUCH** (order
  entry, OUCH-inspired, little-endian) are SYNTHETIC protocols — clearly
  labeled, not conformant to any public spec (ADR-0004). Spans decode
  safely; malformed/truncated frames are fuzzed by a seeded structured-
  mutation harness (libFuzzer needs Clang, absent — ADR-0002).
- Backend contract (net/backend.hpp): RX poll/Release semantics (slot
  lifetime), TX submit (copy + enqueue ≠ flushed doorbell), kBusy retries,
  completion polling. `ReplayBackend` implements it over preallocated
  arenas — the universal backend for simulation, backtest, journal replay.
- **Determinism boundary (ADR-0006)**: every nondeterministic input (MD,
  exec reports, timers, control, seeds, transport outcomes) is journaled
  BEFORE processing; MD/EXEC records are `[u64_be ts][frame]`, TX records
  the exact submitted bytes. Golden replay test: byte-identical outbound
  orders + equivalent final state, with snapshot-assisted seeking.

## 5. Market data and book (md/, book/)

FeedHandler: sequence validation, duplicate suppression, gap detection →
Seeking (only a venue snapshot recovers), snapshot restore, A/B
arbitration bookkeeping, full stats (applied/dups/gaps/drops/anomalies/
pre-start skipped). LevelBook: direct-indexed price range + bid/ask
bitmaps for best-price scans, pooled intrusive FIFO order nodes,
ext-ref → node map with generation-checked handles. Five documented
invariants are checked by property tests and after generated streams.
RefBook is the differential reference.

## 6. OMS / risk / strategy (oms/, risk/, strat/)

- OMS state machine (oms.hpp header comment is authoritative):
  PendingNew→Live→(PartialFill)→Filled / PendingCancel→Cancelled /
  ReplacePending; declines, send failures (bounded retries → abandon),
  duplicate executions (trade-ref + over-fill detection), venue-initiated
  cancel confirms (Live/PartialFill), replace-orig → PendingCancel at
  submit (in-flight 'A' cannot re-ack), 'U' retires the orig, declined
  replace restores it, and a fill racing the 'U' is accepted with the
  late 'U' reconciled (protocol-order-inversion safe; pinned by tests).
  Terminal records retire through a bounded deferred ring (no unbounded
  growth, no allocation, no dangling handler pointers).
- RiskGate (pre-trade, per instrument): price band, max order qty /
  notional (checked multiply), position + pending exposure caps, outstanding
  reservations, deterministic token bucket (event time), stale-feed guard,
  kill switch. Reservations convert to position on fill; releases use
  remaining qty.
- MarketMakerStrategy: emits BOUNDED intents only; cannot bypass risk or
  touch the network. Quote logic uses paired desired prices from one
  snapshot (never self-crosses) and requotes on |drift| ≥ requote_ticks.

## 7. Simulation and measurement (sim/, bench/)

ExchangeSim: price-time matching, two venues, seeded background flow with
balanced churn + empty-side re-seed (no absorbing states), fault injection
(drop/duplicate/reorder/reject/disconnect/gap), venue-initiated cancels
that NOTIFY the client, replace-matches-then-announces (no phantom adds).
Benchmarks: `bench/bench_engine.cpp` (tick-to-trade in three
instrumentation configs with a workload cross-check, book throughput,
timer/jitter baseline incl. PR_SET_TIMERSLACK=1) + `bench_micro` for
primitives. Methodology contract (timing regions, open-loop verification,
raw distributions, metadata) lives in the bench header and
`docs/benchmarks/report.md`. Measured on a shared 2-vCPU KVM guest —
INDICATIVE ONLY: feed-to-order p50=387 ns / p99=528 ns; 28.8 Mframes/s
feed+book; journal +637 ns p50.

## 8. Cold path and console (tools/, web/)

`kestrel_record_demo` drives the real engine with the journal attached and
drains the rings to a binary journal file. `kestrel_journal_export` replays
the journal through the same feed logic and emits **conflated JSONL
snapshots** (schema `kestrel.demo/1`: book top-10, position/cash/PnL,
orders, counters, kill switch) — the cold path never queries the engine.
`web/demo.html` plays the recording (play/step/scrub/speed), renders the
ladder, mid-trail with position shading, orders, position/PnL with risk
usage, kill-switch banner, and the MEASURED bench latency panel (per-event
latency is deliberately not journaled). `scripts/demo.sh` runs the whole
pipeline.

**Connected mode (ADR-0007 closed, 2026-11-02).** The Rust gateway
(`services/gateway`, axum/tokio) serves the React/TS/Vite console
(`web/console` → `web/console-dist`) and a WebSocket endpoint `/ws` that
streams the SAME JSONL projection with a playback protocol
(hello/snapshot/end + play/pause/step/seek/speed), plus `--follow` mode
tailing a growing file — the seam where a live bridge publisher attaches.
Two demo entry points: `scripts/demo.sh` (standalone single-file console)
and `scripts/demo_connected.sh` (record → project → gateway + React
console over WS). Playwright suite in `web/console/tests` covers the real
gateway stream (2/2 green, incl. a mocked kill-switch snapshot).

## 9. Verification, limitations, deferred work

- Tests: 124 (unit + property + integration incl. golden replay, fault
  scenarios, allocation guard) — green on release/ASan/UBSan/TSan, plus a
  warning-free build on GCC 14.2 / C++23. `ctest --preset <debug|release|
  asan|ubsan|tsan>`.
- Benchmarks execute and export raw distributions; regression thresholds
  are report-only (no controlled runner in this sandbox).
- Deferred (with reasons): AF_XDP/DPDK/hardware timestamps (need
  hardware, V2); FPGA track (V3); live bridge publisher beyond `--follow`
  (needs a live engine↔gateway transport decision; the seam exists);
  libFuzzer/PGO (no Clang — ADR-0002); Docker Compose (unverified here,
  docker absent); auth/roles/two-person approvals (single-user demo;
  required before any multi-user exposure).
- Runbook (this sandbox): see `CHECKLIST.md` "How to continue next
  session" — build/test/bench/demo commands with verified invocations.
