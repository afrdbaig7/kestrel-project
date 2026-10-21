# ADR-0007: Cold-path platform without Rust in this environment

Date: 2026-11-01 · Status: closed 2026-11-02 (gateway implemented; see addendum)

## Context

The spec prescribes Rust (axum/tokio) for the cold-path gateway. This sandbox has no
Rust toolchain and the engine work takes priority; the environment also lacks Docker.

## Decision

- Milestone E ships the cold-path platform in two pieces:
  1. **C++ bridge** (service process, separate binary): drains bounded engine
     snapshot rings via shared-memory-like bounded handoff, conflates to ~30 Hz,
     publishes versioned JSON (v1) over UDP/WebSocket-friendly line transport.
     JSON is an explicitly versioned *development* transport; no SBE schema
     files were ever created (a binary transport remains future work).
  2. **Console** (React + TypeScript + Vite): standalone demo mode reading a bundled
     recording produced by the real engine, and connected mode speaking the same
     JSON protocol over WebSocket when a gateway is present.
- The Rust axum/tokio gateway was deferred until a Rust toolchain became
  available; it has since been IMPLEMENTED (see addendum below). The console's
  connected mode speaks the stable JSON protocol, so the gateway swap was
  transparent. NOTE (accuracy fix 2026-11-02): an earlier version of this ADR
  referenced a `docs/gateway-rust-plan.md` and `schemas/` SBE files that were
  never created — no such artifacts exist; binary-protocol schema files remain
  future work if a binary transport is ever added.
- No database/Prometheus/Grafana integration is required for the demo; the console
  and bridge are self-contained.

## Consequences

The deliverable keeps the engine↔cold-path boundary (bounded rings, no synchronous
queries into the engine) exactly as specified; only the gateway process language is
deferred. Multi-user auth/roles/two-person-approval remain documented plans (E2) and
are not half-implemented.

## Addendum (2026-11-02): deferral closed

Rust became available (rustup, rustc 1.99.1 stable), so the gateway landed:

- `services/gateway` — axum 0.8 / tokio 1 WS gateway, release-builds clean.
  Streams the SAME `kestrel.demo/1` JSONL projection with playback control
  (hello/snapshot/end; play/pause/step/seek/speed; inter-snapshot pacing from
  recorded `ts_ns`, clamped 1 ms–2 s, speed-scaled) and `--follow` live-tail
  mode as the documented seam for a future live publisher. Serves the built
  React console from `web/console-dist`.
- `web/console` — React 18 + TypeScript + Vite port of `demo.html` (same
  palette/UX), outDir `web/console-dist`; Playwright chromium suite, 2/2
  green against the real gateway (real-stream e2e + mocked kill-switch
  snapshot via routeWebSocket).
- `scripts/ws_probe.mjs` — protocol conformance probe (hello, play, seek,
  step, speed, end) used for gateway verification.
- `scripts/demo_connected.sh` — record → project → gateway one-command demo.

Boundary unchanged: the console still never queries the engine; the gateway
still streams recorded snapshots by default and fabricates no live data.
