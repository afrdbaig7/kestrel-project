#pragma once
// ============================================================================
// kestrel/protocols/kitch.hpp
//
// KITCH — Kestrel market-data protocol, ITCH-*inspired*, SYNTHETIC (ADR-0004).
// Not ITCH 5.0 conformant; a documented, minimal lookalike.
//
// Wire format (all multi-byte fields BIG-endian, like ITCH):
//   frame  := u16_be payload_len | payload            (len <= kMaxPayload)
//   payload:= u8 msg_type | u32_be feed_seq | u64_be event_ts_ns | body
//
// feed_seq: per-feed sequence number, wraps at 2^32. Gap = seq jump forward.
// Duplicate = seq <= last applied. Snapshot 'B' resets the book (recovery).
//
// Body layouts (offsets relative to body start):
//  'S' system event       : u8 event_code                 len(body)=1
//  'A' add order          : u32 instrument, u64 order_ref, u8 side, u32 qty,
//                           u64 price                     len(body)=25
//  'X' order cancel       : u64 order_ref, u32 cancelled_qty   len=12
//  'E' order executed     : u64 order_ref, u32 executed_qty, u64 trade_ref
//                                                                len=20
//  'R' order replace      : u64 orig_ref, u64 new_ref, u32 new_qty, u64 new_price
//                                                                len=28
//  'T' trade (non-print)  : u32 instrument, u32 qty, u64 price, u64 trade_ref
//                                                                len=24
//  'B' book snapshot      : u32 instrument, u16 entry_count, then
//                           entry_count * (u8 side, u32 qty, u64 price,
//                                          u64 venue_ref)
//                           One entry PER RESTING ORDER (exact FIFO restore;
//                           aggregate levels would lose queue identity).
//                                                                len=6+n*21
// side: 'B' buy, 'S' sell.
//
// Prices are integer ticks; quantities integer lots. Scales are set by the
// instrument configuration (docs/architecture.md §scales).
//
// Safety: decode functions read from std::span<const std::uint8_t> and never
// read out of bounds; truncated frames return DecodeStatus::kTruncated.
// ============================================================================

#include <cstdint>
#include <span>

#include "kestrel/core/byte_codec.hpp"

namespace kestrel::protocols::kitch {

using core::ByteReader;
using core::ByteWriter;
using core::CodecStatus;

inline constexpr std::size_t kMaxPayload = 4095;   // u16 length prefix, framed
inline constexpr std::uint8_t kSideBuy = 'B';
inline constexpr std::uint8_t kSideSell = 'S';

enum class MsgType : std::uint8_t {
    SystemEvent = 'S',
    AddOrder = 'A',
    OrderCancel = 'X',
    OrderExecuted = 'E',
    OrderReplace = 'R',
    Trade = 'T',
    BookSnapshot = 'B',
};

/// Minimum payload lengths (header + body).
inline constexpr std::size_t kHeaderLen = 1 + 4 + 8;   // type + seq + ts
inline constexpr std::size_t kLenSystem = kHeaderLen + 1;
inline constexpr std::size_t kLenAdd = kHeaderLen + 25;
inline constexpr std::size_t kLenCancel = kHeaderLen + 12;
inline constexpr std::size_t kLenExec = kHeaderLen + 20;
inline constexpr std::size_t kLenReplace = kHeaderLen + 28;
inline constexpr std::size_t kLenTrade = kHeaderLen + 24;
inline constexpr std::size_t kLenSnapshotMin = kHeaderLen + 6;
inline constexpr std::size_t kSnapshotEntryLen = 21;

struct Header {
    MsgType type{};
    std::uint32_t feed_seq = 0;
    std::uint64_t event_ts_ns = 0;
};

struct AddOrder {
    std::uint32_t instrument = 0;
    std::uint64_t order_ref = 0;
    std::uint8_t side = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
};

struct OrderCancel {
    std::uint64_t order_ref = 0;
    std::uint32_t cancelled_qty = 0;
};

struct OrderExecuted {
    std::uint64_t order_ref = 0;
    std::uint32_t executed_qty = 0;
    std::uint64_t trade_ref = 0;
};

struct OrderReplace {
    std::uint64_t orig_ref = 0;
    std::uint64_t new_ref = 0;
    std::uint32_t new_qty = 0;
    std::uint64_t new_price = 0;
};

struct Trade {
    std::uint32_t instrument = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
    std::uint64_t trade_ref = 0;
};

struct SnapshotLevel {
    std::uint8_t side = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
    std::uint64_t venue_ref = 0;   // real venue ref (exact post-restore tracking)
};

enum class DecodeStatus : std::uint8_t { Ok, Truncated, BadType, BadLength };

/// Decode the common header. Safe on any input length.
inline DecodeStatus decode_header(std::span<const std::uint8_t> payload,
                                  Header& h) noexcept {
    if (payload.size() < kHeaderLen) return DecodeStatus::Truncated;
    switch (payload[0]) {
        case 'S': h.type = MsgType::SystemEvent; break;
        case 'A': h.type = MsgType::AddOrder; break;
        case 'X': h.type = MsgType::OrderCancel; break;
        case 'E': h.type = MsgType::OrderExecuted; break;
        case 'R': h.type = MsgType::OrderReplace; break;
        case 'T': h.type = MsgType::Trade; break;
        case 'B': h.type = MsgType::BookSnapshot; break;
        default: return DecodeStatus::BadType;
    }
    ByteReader r(payload.subspan(1));
    if (r.u32be(h.feed_seq) != CodecStatus::Ok ||
        r.u64be(h.event_ts_ns) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

inline DecodeStatus decode_add(std::span<const std::uint8_t> payload, AddOrder& m) noexcept {
    if (payload.size() != kLenAdd) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    if (r.u32be(m.instrument) != CodecStatus::Ok ||
        r.u64be(m.order_ref) != CodecStatus::Ok ||
        r.u8(m.side) != CodecStatus::Ok ||
        r.u32be(m.qty) != CodecStatus::Ok ||
        r.u64be(m.price) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

inline DecodeStatus decode_cancel(std::span<const std::uint8_t> payload,
                                  OrderCancel& m) noexcept {
    if (payload.size() != kLenCancel) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    if (r.u64be(m.order_ref) != CodecStatus::Ok ||
        r.u32be(m.cancelled_qty) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

inline DecodeStatus decode_exec(std::span<const std::uint8_t> payload,
                                OrderExecuted& m) noexcept {
    if (payload.size() != kLenExec) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    if (r.u64be(m.order_ref) != CodecStatus::Ok ||
        r.u32be(m.executed_qty) != CodecStatus::Ok ||
        r.u64be(m.trade_ref) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

inline DecodeStatus decode_replace(std::span<const std::uint8_t> payload,
                                   OrderReplace& m) noexcept {
    if (payload.size() != kLenReplace) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    if (r.u64be(m.orig_ref) != CodecStatus::Ok ||
        r.u64be(m.new_ref) != CodecStatus::Ok ||
        r.u32be(m.new_qty) != CodecStatus::Ok ||
        r.u64be(m.new_price) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

inline DecodeStatus decode_trade(std::span<const std::uint8_t> payload, Trade& m) noexcept {
    if (payload.size() != kLenTrade) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    if (r.u32be(m.instrument) != CodecStatus::Ok ||
        r.u32be(m.qty) != CodecStatus::Ok ||
        r.u64be(m.price) != CodecStatus::Ok ||
        r.u64be(m.trade_ref) != CodecStatus::Ok) {
        return DecodeStatus::Truncated;
    }
    return DecodeStatus::Ok;
}

/// Snapshot: iterate levels. `out_levels` receives up to its size; the total
/// count present in the frame is returned via `total_levels`.
inline DecodeStatus decode_snapshot(std::span<const std::uint8_t> payload,
                                    std::span<SnapshotLevel> out_levels,
                                    std::uint16_t& total_levels) noexcept {
    if (payload.size() < kLenSnapshotMin) return DecodeStatus::BadLength;
    ByteReader r(payload.subspan(kHeaderLen));
    std::uint32_t instrument{};
    std::uint16_t n{};
    if (r.u32be(instrument) != CodecStatus::Ok || r.u16be(n) != CodecStatus::Ok)
        return DecodeStatus::Truncated;
    total_levels = n;
    const std::size_t want =
        kHeaderLen + 6 + static_cast<std::size_t>(n) * kSnapshotEntryLen;
    if (payload.size() != want) return DecodeStatus::BadLength;
    if (out_levels.size() < n) {
        // Caller buffer too small: validate length first, then report.
        return DecodeStatus::Truncated;
    }
    for (std::uint16_t i = 0; i < n; ++i) {
        SnapshotLevel lv{};
        if (r.u8(lv.side) != CodecStatus::Ok ||
            r.u32be(lv.qty) != CodecStatus::Ok ||
            r.u64be(lv.price) != CodecStatus::Ok ||
            r.u64be(lv.venue_ref) != CodecStatus::Ok) {
            return DecodeStatus::Truncated;
        }
        out_levels[static_cast<std::size_t>(i)] = lv;
    }
    return DecodeStatus::Ok;
}

// ---- encoding --------------------------------------------------------------

/// Write frame prefix: u16_be payload_len. Returns false on overflow.
inline bool encode_frame_prefix(ByteWriter& w, std::size_t payload_len) noexcept {
    if (payload_len > kMaxPayload) return false;
    return w.u16be(static_cast<std::uint16_t>(payload_len)) == CodecStatus::Ok;
}

/// Encode a complete framed message: [u16 len][payload]. Returns bytes written,
/// or 0 on overflow (buffer too small / payload too long).
inline std::size_t encode_framed(std::span<std::uint8_t> out,
                                 MsgType type, std::uint32_t seq,
                                 std::uint64_t ts_ns,
                                 std::span<const std::uint8_t> body) noexcept {
    const std::size_t payload_len = kHeaderLen + body.size();
    if (payload_len > kMaxPayload) return 0;
    if (out.size() < 2 + payload_len) return 0;
    ByteWriter w(out);
    if (!encode_frame_prefix(w, payload_len)) return 0;
    if (w.u8(static_cast<std::uint8_t>(type)) != CodecStatus::Ok ||
        w.u32be(seq) != CodecStatus::Ok ||
        w.u64be(ts_ns) != CodecStatus::Ok) {
        return 0;
    }
    for (const std::uint8_t b : body) {
        if (w.u8(b) != CodecStatus::Ok) return 0;
    }
    return w.written();
}

/// Typed body encoders (used by the simulator and tools).
inline std::size_t body_add(std::span<std::uint8_t> out, const AddOrder& m) noexcept {
    ByteWriter w(out);
    if (w.u32be(m.instrument) != CodecStatus::Ok ||
        w.u64be(m.order_ref) != CodecStatus::Ok ||
        w.u8(m.side) != CodecStatus::Ok ||
        w.u32be(m.qty) != CodecStatus::Ok ||
        w.u64be(m.price) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_cancel(std::span<std::uint8_t> out, const OrderCancel& m) noexcept {
    ByteWriter w(out);
    if (w.u64be(m.order_ref) != CodecStatus::Ok ||
        w.u32be(m.cancelled_qty) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_exec(std::span<std::uint8_t> out, const OrderExecuted& m) noexcept {
    ByteWriter w(out);
    if (w.u64be(m.order_ref) != CodecStatus::Ok ||
        w.u32be(m.executed_qty) != CodecStatus::Ok ||
        w.u64be(m.trade_ref) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_replace(std::span<std::uint8_t> out, const OrderReplace& m) noexcept {
    ByteWriter w(out);
    if (w.u64be(m.orig_ref) != CodecStatus::Ok ||
        w.u64be(m.new_ref) != CodecStatus::Ok ||
        w.u32be(m.new_qty) != CodecStatus::Ok ||
        w.u64be(m.new_price) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_trade(std::span<std::uint8_t> out, const Trade& m) noexcept {
    ByteWriter w(out);
    if (w.u32be(m.instrument) != CodecStatus::Ok ||
        w.u32be(m.qty) != CodecStatus::Ok ||
        w.u64be(m.price) != CodecStatus::Ok ||
        w.u64be(m.trade_ref) != CodecStatus::Ok) return 0;
    return w.written();
}

}  // namespace kestrel::protocols::kitch
