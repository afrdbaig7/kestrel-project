# ADR-0006: Determinism boundary and journal contents

Date: 2026-11-01 · Status: accepted

## Context

The engine must replay byte-identically: live timing must never influence
deterministic decisions, and all nondeterministic inputs must be captured.

## Decision

- **Event time** (from KITCH/KOUCH messages, journaled TIMER records, and config
  records) drives: rate limiting (token bucket refills), staleness detection, feed
  stall arbitration, simulator latency model, timer wheel.
- **Live time** (TSC/steady clock) is used only for measurement (histograms, spans)
  and never reaches a decision path.
- Journal record types: `MD` (KITCH frame), `EXEC` (KOUCH response frame),
  `TIMER`, `CONFIG`, `CONTROL` (kill switch, params), `SEED`, `TX` (engine outbound,
  captured for replay comparison), `SNAPSHOT` (state checkpoint), `MARKER`.
- Records: `[u32 len][u16 type][u16 ver][u32 crc32(payload)][payload]`. Truncation or
  CRC failure ends the replay stream at the last good record (with a gap marker).
- Replay acceptance = byte-identical outbound TX records + equivalent final state
  (positions, OMS order states, risk counters).
- Two write policies: **critical** (md/exec/timer/control; on full ring → fail-closed:
  raise kill switch, mark gap, stop new exposure) and **best-effort** (telemetry;
  drops counted).

## Consequences

Replay is a reproducibility guarantee tested by golden tests, not an aspiration.
Snapshot records also give the replay debugger O(1) seek points (Milestone C).
