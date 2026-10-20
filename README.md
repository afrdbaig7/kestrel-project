# Kestrel — Low-Latency HFT Trading Engine (V1)

A complete, tested software trading engine built to the "Low-Latency HFT
Trading Engine" project plan: market-data feed → order book → strategy →
pre-trade risk → order entry, with a matching-exchange simulator, binary
journal + deterministic replay, benchmarks, and a cold-path web console.

**It simulates an exchange and a strategy; it does not trade real money and
makes no profitability claims. Benchmark numbers are indicative (measured on
a shared 2-vCPU cloud VM, no isolcpus).**

## Layout

| Path | Contents |
|---|---|
| `include/kestrel/core` | SPSC ring, object pool, flat_map, bitmap, TSC clock + calibration, HdrHistogram, jitter meter, CPU helpers |
| `include/kestrel/protocols` | KITCH (market data) / KOUCH (order entry) synthetic codecs |
| `include/kestrel/{md,book,strat,risk,oms}` | feed handler, order books, strategy, pre-trade risk, OMS state machine |
| `include/kestrel/{journal,sim,engine,net,backends}` | binary journal, exchange simulator, run-to-completion engine, backend contract + replay backend |
| `tests/` | unit, property, integration suites (incl. golden replay, allocation guard) |
| `bench/` | micro-benchmarks + tick-to-trade engine benchmarks; `bench/results/` holds a measured run |
| `tools/` | `kestrel_inspect`, `kestrel_record_demo`, `kestrel_journal_export` (cold-path bridge/projection) |
| `services/gateway/` | Rust (axum/tokio) WebSocket gateway — connected mode (ADR-0007) |
| `web/console/` + `web/console-dist/` | React/TS/Vite console (source + prebuilt dist) + Playwright suite |
| `web/demo.html`, `web/demo-data/` | standalone single-file console + recorded session |
| `docs/` | ARCHITECTURE.md, ADR-0001..0007, benchmark report |
| `CHECKLIST.md` | full milestone status, verification log, continuation notes |
| `ci/` | GitHub Actions workflow (authored; needs a real runner to execute) |

## Build & test (C++ engine)

Requires: GCC 14+ (C++23), CMake ≥ 3.24, Ninja. First configure downloads
GoogleTest/GoogleBenchmark via FetchContent.

```bash
cmake --preset release && cmake --build --preset release
ctest --preset release          # 124 tests
ctest --preset asan             # also: ubsan, tsan, debug
```

Toolchain note: developed with CMake 4.4 + Ninja 1.13 (pip) on Debian 13.

## Cold path: gateway + console

```bash
# Rust gateway (needs cargo; rustup minimal profile works)
(cd services/gateway && cargo build --release)

# React console (dist is PREBUILT in web/console-dist; rebuild optional)
(cd web/console && npm install && npm run build && npx playwright test)
```

## Demos (one command each, from repo root)

```bash
scripts/demo.sh            # standalone console (python http.server)
scripts/demo_connected.sh  # record → project → Rust gateway → React console
                           # prereqs: release build + gateway + console dist
```

Benchmarks: `python3 scripts/run_bench.py --label <name>` then
`python3 scripts/analyze_bench.py bench/results/<name>/results.json -o report.md`.

## Documentation

Start with `docs/ARCHITECTURE.md` (design, protocols, state machines,
replay guarantees, benchmark methodology, limitations) and `CHECKLIST.md`
(every milestone item with its verification evidence). Decision log:
`docs/adr/`.

## Not included (by design, documented in CHECKLIST/ADR)

V2 hardware tracks (DPDK, AF_XDP zero-copy, hardware timestamps — need a
real NIC), V3 FPGA track, Docker Compose (docker unavailable in the
development sandbox), live bridge publisher beyond the gateway's `--follow`
seam.
