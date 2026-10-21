# ADR-0004: Synthetic protocols KITCH (market data) and KOUCH (order entry)

Date: 2026-11-01 · Status: accepted

## Context

The spec asks for ITCH-inspired market data and OUCH-inspired order entry, and
requires that synthetic protocols be explicitly labeled as such unless full public
conformance is implemented and tested. Real ITCH 5.0 / OUCH 5.0 conformance requires
MITlicensed spec texts and wire-level test vectors that are not available offline.

## Decision

Two synthetic, fully documented wire protocols:

- **KITCH** (KestrelITCH): market data. Framing `u16 BE payload_len | payload`.
  Payload: `u8 msg_type | u32 BE feed_seq | u64 BE event_ts_ns | body`.
  Body fields big-endian (network order), like ITCH. Messages: `S` system event,
  `A` add order, `X` order cancel, `E` order executed, `R` order replace, `T` trade
  (non-printable), `B` book snapshot (recovery boundary).
- **KOUCH** (KestrelOUCH): order entry, little-endian like OUCH. Requests: `E` enter,
  `C` cancel, `R` replace. Responses: `A` accepted, `D` declined, `U` replaced,
  `x` canceled, `G` cancel reject, `E` executed, `S` sequence/session.

Both are implemented as span-safe decode / bounded-encode with no unaligned loads,
no packed-struct aliasing, and strict length checks. Every message type and field is
documented in `docs/protocols.md` with byte offsets and scales.

## Consequences

- Nothing in the repo claims ITCH/OUCH conformance.
- The protocols are intentionally minimal but exercise the same engineering problems:
  fixed offsets, endianness, sequence numbers, timestamps, add/cancel/execute/replace,
  acks/rejects/partial fills/fills/cancel confirms.
- A future conformance layer can sit behind the same backend/decoder interfaces.
