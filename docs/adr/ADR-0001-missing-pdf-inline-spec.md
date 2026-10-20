# ADR-0001: Missing specification PDF — inline message spec is authoritative

Date: 2026-11-01 · Status: accepted

## Context

The task references an attached "Low-Latency HFT Trading Engine — Project Plan" PDF.
`upload/` was empty when the session started (verified: `ls /home/z/my-project/upload/`).

## Decision

The inline message specification is treated as the complete, authoritative product
specification. Where the message offered alternatives (e.g., "optionally AF_XDP …
once the core works"), the option preserving correctness, reproducibility and the
separation between software and hardware measurements was chosen and recorded here.

## Consequences

- No document-specific figures (PDF latency tables, diagrams) are available; the
  inline aspiration figures (tick-to-trade p50 < 500 ns, p99 < 1 µs software;
  10M msgs/s/core book throughput; zero steady-state allocations) are used as the
  software targets and are explicitly labeled *aspirations*, never acceptance claims.
- If the PDF surfaces in a later session, reconcile it against this repo and update
  the ADR set.
