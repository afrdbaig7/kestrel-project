# ADR-0005: Replay-first backends; hardware backends are separate extensions

Date: 2026-11-01 · Status: accepted

## Context

The sandbox has no NIC beyond loopback, no root networking privileges, and no DPDK.
The spec requires replay first, hardware integrations as optional extensions, and
forbids stubbed/simulated hardware support presented as working.

## Decision

- `net/backend.hpp` defines a compile-time contract (concepts) for `RxSource` /
  `TxSink` with documented semantics: RX buffer validity windows, TX ownership,
  whether a submit means copied/enqueued/flushed/doorbell, completion processing.
- Version 1 ships the **replay backend** (in-memory ring of captured frames; TX is an
  in-memory capture ring — "enqueued", never claimed as a wire timestamp) and the
  **simulator** as counterparty.
- AF_XDP (copy/zero-copy) and DPDK directories are created only when their
  implementation begins. No empty placeholder folders, no stubs.
- Any backend-specific syscall (e.g., an AF_XDP wakeup) must be documented per
  backend; the core's guarantees (post-init: no heap alloc, no locks, no blocking,
  no sync I/O, no virtual dispatch on the event chain) apply to the engine, not to
  backend internals.

## Consequences

Version 1 is fully functional and verifiable in this sandbox. Milestone G work
(physical NICs, hardware timestamping) requires actual hardware and remains
documented as an extension.
