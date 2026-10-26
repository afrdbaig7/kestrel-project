#pragma once
// ============================================================================
// kestrel/net/backend.hpp
//
// Compile-time backend contract (concepts) — ADR-0005.
//
// The engine is parameterized by an RxSource and a TxSink. Concrete
// configurations are selected at compile time; nothing in the event chain is
// virtual.
//
// Contract semantics (each backend MUST document these):
//
// RxSource:
//   poll_rx(out, max)     — fills `out` with up to `max` FrameViews.
//                           Returns the number filled. FrameView data is
//                           valid until the matching release_rx(n).
//   release_rx(n)         — marks the first n polled frames as consumed.
//                           After release, the data pointers are INVALID.
//
// TxSink:
//   tx_submit(data, len)  — copies `len` bytes from data (which is an
//                           engine-owned scratch buffer; lifetime ends when
//                           submit returns). Returns TxResult:
//                             kEnqueued — accepted into the backend's local
//                                         queue; NOT yet on the wire (this is
//                                         the only semantics the replay
//                                         backend provides).
//                             kBusy     — queue full; engine re-submits the
//                                         same bytes later (idempotent:
//                                         retry must produce identical bytes
//                                         or abandon with a journaled reason).
//                             kError    — terminal failure; engine records a
//                                         send failure and the OMS handles
//                                         the pending order.
//   tx_poll_completed(max)— reclaims up to max completed sends. For the replay
//                           backend, completion == enqueue time. For future
//                           hardware backends, completion is the documented
//                           boundary (e.g., NIC TX doorbell RECLAIM, not the
//                           wire timestamp).
//
// Buffer validity / ownership table (per backend):
//   replay : rx frames owned by backend arena until release_rx;
//            tx_submit copies into a capture ring (kEnqueued).
//   afxdp  : (planned) rx points into UMEM frames until release_rx;
//            tx_submit copies into a UMEM frame + explicit kick — the kick is
//            a syscall on some paths and MUST be documented per build.
//   dpdk   : (planned) rx from mempool burst; tx_submit copies into mbuf,
//            flush on burst boundary.
//
// Guarantee separation (spec §4): the ENGINE core is post-init allocation-free,
// lock-free, blocking-free, sync-IO-free; backend internals may perform
// documented syscalls (e.g., AF_XDP wakeup). No backend may silently violate
// the RX/TX semantics above.
// ============================================================================

#include <concepts>
#include <cstdint>
#include <span>

namespace kestrel::net {

/// Distinguishes the two logical input streams at the engine boundary.
enum class FrameKind : std::uint8_t { MarketData = 0, ExecReport = 1 };

/// A received frame: view into backend-owned memory.
struct FrameView {
    std::uint64_t ts_event_ns;     // backend-reported capture/event time
                                   // (replay: journaled event time; hardware:
                                   // NIC/software timestamp — documented)
    const std::uint8_t* data;
    std::uint32_t len;
    FrameKind kind;
};

enum class TxResult : std::uint8_t { kEnqueued, kBusy, kError };

template <typename B>
concept RxSource = requires(B b, FrameView* out, std::size_t max, std::size_t n) {
    { b.poll_rx(out, max) } -> std::convertible_to<std::size_t>;
    { b.release_rx(n) } -> std::same_as<void>;
};

template <typename B>
concept TxSink = requires(B b, const std::uint8_t* data, std::size_t len,
                          std::size_t max) {
    { b.tx_submit(data, len) } -> std::convertible_to<TxResult>;
    { b.tx_poll_completed(max) } -> std::convertible_to<std::size_t>;
};

}  // namespace kestrel::net
