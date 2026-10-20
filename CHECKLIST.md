# Kestrel — Implementation Checklist (persistent, updated every session)

Provisional internal project name: **Kestrel**. Renaming is deferred to shipping.

Status marks: `[ ]` todo · `[~]` in progress · `[x]` done and verified (a check is only
marked done when it was actually run successfully in this sandbox).

## Environment (verified 2026-11-01)

- OS: Debian-family Linux 5.10.134 x86_64 (container), 2 vCPU, 4.1 GiB RAM, 9.9 GiB disk
- Compiler: GCC 14.2.0 (C++23 OK). Clang 18: **not installed** → no libFuzzer/PGO via clang.
- Build: CMake (>=3.24 required by presets — verify version below), Ninja 1.13.2 (pip).
- Clock: `tsc` clocksource active, 3200 MHz, `nonstop_tsc` + `constant_tsc` + `rdtscp`
  flags present → ordered TSC clock supported.
- CPU governor/cpufreq: not exposed (VM). No isolcpus. Measurements must record this.
- Network: available (FetchContent works). Docker: **not installed**. Rust: **not installed**.
- Node 24 + npm available. Python 3.12 with numpy/pandas/matplotlib.
- CMake version on host: recorded by `cmake --version` (see build logs in session notes).

## Spec decisions (see docs/adr/ for full records)

- **ADR-0001**: The attached PDF did not arrive (`upload/` empty). The inline message
  spec is authoritative. Conflicts resolved toward correctness, reproducibility, and
  software-vs-hardware measurement separation.
- **ADR-0002**: GCC 14.2 only. libFuzzer targets are provided behind a Clang opt-in
  flag; default fuzz coverage is a seeded structured-mutation harness run in normal
  tests. PGO deferred (needs toolchain support).
- **ADR-0003**: Histogram = in-repo implementation of the published HdrHistogram
  algorithm (header-only, hot-path dependency-free). Bucket math validated in tests.
- **ADR-0004**: Synthetic protocols are named **KITCH** (market data, ITCH-inspired,
  big-endian) and **KOUCH** (order entry, OUCH-inspired, little-endian). They are NOT
  conformant ITCH/OUCH; clearly labeled synthetic.
- **ADR-0005**: Replay-first. Hardware backends (AF_XDP copy-mode, DPDK) documented as
  extensions; directories are created only when implementation begins. No stub code.
- **ADR-0006**: Determinism boundary: all nondeterministic inputs (md, exec reports,
  timers, control, seeds, transport outcomes) are journaled; live time never feeds
  decisions. Replay compares byte-identical outbound order messages + final state.
- **ADR-0007**: Rust gateway deferred until rustc is available in the environment;
  cold-path platform ships the C++ bridge + demo transport first (documented).

## Milestone A — Foundation

- [x] A1 Repository skeleton + CMake presets (debug/release/bench/asan/ubsan/tsan)
- [x] A1 FetchContent pins: googletest v1.15.2, benchmark v1.9.0 (verified configure)
- [x] A2 CHECKLIST.md + ADR-0001..0007 + system inspection implemented as
      tools/kestrel_inspect.cpp (binary `kestrel_inspect`: CPU/kernel/allowed-
      cpus/TSC flags/clock source — re-run green 2026-11-02). NOTE: earlier
      text said "scripts/inspect_system.sh" — wrong filename, corrected here;
      no such shell script ever existed.
- [x] A3 core/spsc_ring.hpp — fixed-capacity SPSC ring, acquire/release, padded cursors
- [x] A3 core/object_pool.hpp — fixed-capacity pool, generation-checked slot reuse
- [x] A3 core/flat_map.hpp — preallocated open-addressing map (linear probe, backward-shift delete)
- [x] A3 core/bitmap.hpp — word bitmap, forward/backward scan
- [x] A4 core/clock.hpp — ordered TSC (rdtscp) + steady fallback + off-path calibration
- [x] A4 core/jitter_meter.hpp — sleep overshoot + clock read overhead + core-change count
- [x] A4 core/cpu.hpp — affinity helpers, allowed-cpu list, model/flags inspection
- [x] A5 core/histogram.hpp — HdrHistogram algorithm, percentiles, merge, CSV export
- [x] A6 tests/unit + tests/property for all primitives (ctest suite `unit`)
- [x] A6 bench/: ring, pool, flat_map, bitmap, histogram, clock overhead (Google Benchmark)
- [ ] A6 CI workflow (ci/github-actions.yml, correctness + sanitizer + bench smoke)

## Milestone B — Vertical slice (feed → book → strategy → risk → encoder → sim ack)

- [x] B1 protocols/kitch.hpp — frame layout, msg types, BE decode/encode, span-safe
- [x] B1 protocols/kouch.hpp — inbound/outbound msgs, LE decode/encode, span-safe
- [x] B1 malformed/truncated frame test suite + seeded structured-mutation fuzz harness
- [x] B2 net/backend.hpp — compile-time RxSource/TxSink contract + semantics doc
- [x] B2 backends/replay — journal-backed RX source + capturing TX sink (engine logic unchanged)
- [x] B3 book/level_book.hpp — direct-indexed levels, bid/ask bitmaps, pooled intrusive
      FIFO nodes, ext-id → node map; defined behavior for dup ids, unknown cancels,
      over-execution, invalid prices, overflow, exhaustion
- [x] B3 book/ref_book.hpp — simple reference book + differential property tests
- [x] B3 book invariants checked after generated streams (5 invariants from spec)
- [x] B4 md/feed_handler.hpp — seq validation, duplicate suppression, gap detection,
      snapshot recovery boundary, A/B feed arbitration
- [x] B5 oms/oms.hpp — full transition table incl. pending-cancel/replace, duplicate
      execution handling, send-failure policy
- [x] B5 risk/risk.hpp — price bands, qty/notional caps, position/exposure limits,
      outstanding reservations, deterministic token bucket, stale-feed protection, kill switch
- [x] B6 strat/market_maker.hpp — passive MM producing bounded intents (event-driven)
- [x] B7 sim/exchange_sim.hpp — price-time matching, partial fills, exec reports,
      queue-position/latency model, 2 venues, seeded workloads, fault injection
      (gaps, duplicates, reorder, bursts, rejects, disconnect)
- [x] B8 engine/engine.hpp — run-to-completion loop, preallocated, stage pipeline;
      vertical-slice end-to-end test (sim feed → engine → sim ack → fill → position)

## Milestone C — Correctness

- [x] C1 journal/journal.hpp — versioned binary records, length validation, CRC32, writer ring
      (engine never blocks), critical fail-closed + telemetry drop counters
- [x] C1 golden replay tests — byte-identical outbound + equivalent final state
- [x] C2 OMS edge cases: duplicate exec reports, send failures, reservation accounting
- [x] C2 journal corruption detection + fail-closed-on-overflow tests
- [x] C2 snapshot-assisted replay seeking (journal snapshots + seek tool)

## Milestone D — Performance

- [x] D1 allocation-guard: engine loop runs with counting new → zero allocations
- [x] D1 ASan/UBSan/TSan ctest runs — ALL 119/119 green per preset (after fixing
      the alloc-guard operator-override set; see session notes)
- [x] D2 tick-to-trade (open-loop intended arrivals), book throughput, jitter baseline,
      timer overhead, observer-overhead comparison (minimal vs stage-timed)
      — bench/bench_engine.cpp, full run in bench/results/full-1919/
- [x] D2 scripts/analyze_bench.py — histogram stats, regression report (JSON in → md)
- [x] D2 scripts/run_bench.py — reproducibility metadata (commit, compiler, flags, CPU,
      kernel, affinity, clock source, config, seed) merged into results.json
- [x] D2 docs/benchmarks/report.md generated from the measured run
- [x] A6 CI workflow (ci/github-actions.yml) — authored here; UNVERIFIED on a real
      runner (no GitHub runner in sandbox), commands mirror verified local presets

## Milestone E — Cold-path platform (as resources allow; see ADR-0007)

- [x] E1 C++ bridge data plane: journal recording → JSON-lines projection
      (tools/kestrel_journal_export, schema kestrel.demo/1): replays Md/Exec/Tx
      through the SAME feed logic, emits CONFLATED snapshots (book top-10,
      position/PnL, orders, counters, kill-switch). Bounded count.
- [x] E1 recorder: tools/kestrel_record_demo — drives the REAL engine +
      journal to a binary file (same host wiring as the correctness tests).
- [x] E1 console demo mode: web/demo.html — self-contained dark-theme console,
      playback (play/step/scrub/speed), ladder+depth bars, mid trail with
      position shading, orders table, position/PnL + risk usage, kill-switch
      banner (verified with an injected Control record), honest bundled
      benchmark latency panel. VERIFIED in a headless browser against the
      generated recording (608 snapshots).
- [x] E1 one-command demo: scripts/demo.sh (build → record → project → serve;
      verified end-to-end).
- [x] E1 connected mode (WebSocket gateway) — DONE 2026-11-02 (ADR-0007
      closed): `services/gateway` (Rust, axum 0.8/tokio; rustc 1.99.1 via
      rustup). Streams the SAME kestrel.demo/1 JSONL with playback protocol
      (hello/snapshot/end; play/pause/step/seek/speed; recorded ts_ns
      pacing, clamped 1 ms–2 s) + `--follow` live-tail seam. Verified by
      scripts/ws_probe.mjs (full protocol) and the Playwright suite.
- [x] E1 React/TS/Vite console build + Playwright suite — DONE 2026-11-02:
      `web/console` (React 18 + TS strict + Vite → `web/console-dist`,
      palette ported from demo.html); `web/console/tests` 2/2 green on the
      real gateway (real-stream e2e incl. seek-to-end; kill-switch banner
      via routeWebSocket mock) — independently re-run by the reviewer
      session; negative control (dist removed → tests fail as intended).
      One-command demo: scripts/demo_connected.sh.
- [x] E2 auth/roles/two-person approval — documented deferral (single-user
      local demo; spec requires them only before multi-user exposure).
  [NOTE: kill-switch display verified via injected CONTROL record; the
   engine's engage/clear path itself is covered by VerticalSlice tests.]

## Milestone F — Version 1 release

- [x] F1 docs: docs/ARCHITECTURE.md (architecture, protocols, state machines,
      risk semantics, replay guarantees, benchmark methodology, limitations,
      deferred work + runbook pointer); docs/benchmarks/report.md; ADR-0001..7
- [x] F1 one-command demo: scripts/demo.sh (build → record → project → serve;
      verified). Docker Compose: NOT provided — docker absent in sandbox
      (was listed as a possibility; replaced by an honest deferral).
- [x] F1 verified benchmark report (docs/benchmarks/report.md generated from runs)
- [x] F1 final verification pass + session continuation notes (end of this file)
      — final pass 2026-11-02: 124/124 green on release/asan/ubsan/tsan,
      warning-free builds, demo pipeline verified in a headless browser.

## Milestones G/H — Hardware extensions (NOT started, by design)

- [ ] G: AF_XDP zero-copy, DPDK physical NIC, hardware timestamps — requires hardware
- [ ] H: FPGA golden-model track — not started (documented only)

### Session 2026-11-02 (later): connected mode + React console landed

- rustup installed (rustc 1.99.1). Gateway `services/gateway` built clean;
  protocol verified via `scripts/ws_probe.mjs` (hello/play/snapshot/seek/
  step/speed/end all pass, 218 ms full playback at min pacing) and a
  seek-to-last `end`-frame check (fixed: seek-to-last is now terminal).
- React console + Playwright delegated (Task g3) and independently
  re-verified: `npx playwright test` → 2/2 passed against the real gateway
  webServer; fresh C++ recording regenerated (134 snapshots).
- Docs updated: ARCHITECTURE.md §8/§9, ADR-0007 addendum (status: closed),
  new `scripts/demo_connected.sh`.
- STILL deferred, unchanged: Docker Compose (no docker), CI on a real
  GitHub runner (user-side repo), live bridge publisher beyond `--follow`
  (transport decision pending), G/H hardware tracks.

## Session notes / continuation

### Session 2026-11-02 (continued from 2026-11-01): Milestones A–C verified, D completed

**Verified from the previous session** (fresh builds + full ctest): release/asan/
ubsan/tsan all 119/119 — but ONLY after fixing two blockers:

1. **ASan was red (14 integration failures)**: the alloc-guard overrode sized
   `operator delete` but not the nothrow/aligned variants, so gtest's
   `__get_temporary_buffer` allocated via ASan's own `operator new(nothrow)` and
   freed through the override's `free()` → alloc-dealloc-mismatch. Fix: complete,
   self-consistent malloc/free override set in test_allocation_guard.cpp
   (unsized + sized + nothrow + aligned, all through one allocator family).
2. **Configure was broken**: bench/CMakeLists.txt referenced a bench_engine.cpp
   that had never been written (session died mid-D2). Everything below wrote it.

**Milestone D (this session) — what was built and measured:**

- `bench/bench_engine.cpp`: custom-main harness (no Google Benchmark) with three
  tick-to-trade configs (minimal / +journal / stage-timed), book throughput,
  timer+jitter baseline incl. a PR_SET_TIMERSLACK=1 variant. Raw CSV export +
  results.json. Methodology contract documented in the file header and
  docs/benchmarks/report.md.
- `scripts/run_bench.py` (metadata collector + runner) and
  `scripts/analyze_bench.py` (JSON → markdown, optional --baseline compare).
- `bench/results/full-1919/` + `docs/benchmarks/report.md`: measured run.
  Headline (2-vCPU shared KVM guest — INDICATIVE ONLY): software feed-to-order
  p50=387 ns / p99=528 ns / p99.9=627 ns (n=4525); journal ring write +637 ns
  p50; observer overhead +898 ns p50; exec-driven p50=365 ns; feed+book
  28.8 Mframes/s single core (stream-clean 400k frames); rdtscp pair 56 ns;
  sleep overshoot p50 54.8 µs default slack vs 3.46 µs with timerslack=1.
- `ci/github-actions.yml` (A6): authored, UNVERIFIED on a real runner — labeled
  as such inside the file.

**Bugs found and fixed while building the bench (all engine/sim/OMS/strategy
level, pinned by tests):**

1. **Allocation-guard overrides** (above).
2. **Sim absorbing empty-side state**: background adds anchored on the same-side
   best; a taker sweep could empty one side forever → strategy's one-sided
   guard blocked all quoting. Fix: empty-side re-seed (opposite touch, or the
   reference mid when both sides are empty).
3. **Sim silent kill of client orders**: `cancel_level_entry` erased CLIENT
   orders (churn) without sending a Canceled report → OMS kept a "live" order
   that no longer existed → infinite UnknownOrder replace loop. Fix: venue-
   initiated cancels now notify the client (as real venues must).
4. **Sim phantom orders**: handle_replace emitted the 'R' MD frame (announcing
   the new order as resting) BEFORE matching it; a new order that crossed was
   then consumed as a taker with NO further MD → feed consumers kept a ghost
   order → crossed book view → strategy's spread guard froze. Fix: match first,
   then announce the FINAL resting state ('R' with the remaining qty, or a
   plain cancel when fully matched).
5. **Sim exec-report noise**: background fills emitted Executed reports for
   client_id 0 (maker AND taker). Gated on non-zero client ids.
6. **OMS replace state machine**: track_replace never moved the orig out of
   ackable state → an in-flight 'A' for the orig re-bound a dead id into the
   strategy slot (ghost replaces). Fix: orig → PendingCancel at submit; 'U'
   retires it; declined replace restores it to Live. Reservation release on
   'U' now uses remaining qty (was full qty — over-release after partial fill).
7. **OMS venue-initiated cancels**: cancel confirms for Live/PartialFill orders
   are valid (real venues cancel client orders); were dropped as unknown.
8. **OMS terminal retirement**: terminal records were never erased → capacity
   256 saturated on any long session. Fix: deferred retire ring + sweep at
   loop start (no dangling handler pointers, no allocation, overflow counted).
9. **Fill racing 'U'** (response-order inversion): fills for ReplacePending
   orders were rejected as unknown; the late 'U' then bound a DEAD order into
   the strategy slot → permanent freeze. Fix: on_exec accepts ReplacePending;
   on_replaced accepts the late ack; the engine clears (not binds) the slot
   when the child is already Filled. Pinned by Oms.FillRacingReplaceAck.
10. **Strategy self-cross at spread==2**: both "improve by one tick" quotes
    landed on the same level → the venue matched our own bid against our own
    ask. Fix: paired desired prices from one snapshot (ask > bid always), plus
    |drift| ≥ requote_ticks in BOTH directions (the one-sided variant left
    stale quotes forever when the market walked away).
11. **bench: dangling stage-histogram pointers** (ASan heap-use-after-free —
    read after the engine scope closed) and a ~2 MB stack FeedHandler (ASan
    stack overflow). Snapshots + heap allocation.
12. Pre-existing warning cleanup: pedantic `__int128` guard, unused param,
    old-style casts, dangling-else braces, sign compares. All four presets
    now build warning-free; 124/124 tests green (5 new OMS protocol-race
    tests added).

**How to continue next session:**

- Remaining milestone: **E** (cold path: C++ bridge, positions/PnL + risk
  monitor services, recorder; console standalone demo + connected mode), then
  **F** (docs, one-command demo, final verification).
- Rust is NOT installed (ADR-0007 stands): bridge transport = C++ publisher +
  JSON-lines; a Rust gateway lands only when rustc exists in the sandbox.
- Build/verify loop: `cmake --preset release && cmake --build --preset release
  && ctest --preset release` (same for asan/ubsan/tsan). Bench:
  `python3 scripts/run_bench.py --label <name>` then
  `python3 scripts/analyze_bench.py bench/results/<name>/results.json -o report.md`.
- Sandbox caveats: 2 vCPU shared KVM guest, no isolcpus/governor; Docker and
  Rust absent; Clang absent (no libFuzzer/PGO — ADR-0002).

### Session 2026-11-02 (verification pass after sandbox recycle)

- Sandbox was recycled: pip cmake/ninja were gone; reinstalled via
  `pip install cmake ninja` (cmake 4.4.4, ninja 1.13.2). No repo changes needed.
- Fresh re-verification: `cmake --preset release && cmake --build --preset
  release && ctest --preset release` → 62/62 targets build, **124/124 tests
  pass** in 1.08 s. V1 status confirmed.
- Toolchain delta: **rustc 1.85.1 is now an apt candidate** (was absent —
  ADR-0007's blocker). The deferred Rust WS gateway is feasible next session.
- Remaining deferred (unchanged): Rust gateway/WS connected mode, React/Vite
  console port + Playwright, Docker Compose (no docker), CI on a real runner
  (no GitHub runner here), G/H hardware tracks (no NIC/FPGA).

### Final state (end of session 2026-11-02)

V1 is functionally complete within the sandbox's constraints: engine (A–D),
cold-path demo platform + console (E, now INCLUDING connected mode: Rust
WS gateway + React/TS/Vite console + Playwright), docs + one-command demos
(F). Remaining for future sessions: live bridge publisher beyond `--follow`
(transport decision), Docker Compose (needs docker), V2/V3 hardware tracks
(need hardware). Nothing in the repo claims otherwise.
