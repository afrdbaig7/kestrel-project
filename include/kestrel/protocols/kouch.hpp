#pragma once
// ============================================================================
// kestrel/protocols/kouch.hpp
//
// KOUCH — Kestrel order-entry protocol, OUCH-*inspired*, SYNTHETIC (ADR-0004).
// Not OUCH 5.0 conformant. Little-endian fields, like OUCH.
//
// Two separate byte streams (the engine's TX to the venue, the venue's RX back):
//
// Requests  (client -> exchange):
//   'E' enter order   : u8 type, u64 client_order_id, u32 instrument, u8 side,
//                       u32 qty, u64 price, u8 tif ('D' day, 'I' IOC)
//                       len=27
//   'C' cancel order  : u8 type, u64 client_order_id         len=9
//   'R' replace order : u8 type, u64 orig_client_id, u64 new_client_id,
//                       u32 new_qty, u64 new_price            len=29
// Responses (exchange -> client): u8 type | u32_le msg_seq | u64_le exch_ts_ns
//                       | body                                header=13
//   'A' accepted      : u64 client_order_id, u32 instrument, u8 side,
//                       u32 qty, u64 price                    body=25
//   'D' declined      : u64 client_order_id, u16 reason      body=10
//   'U' replaced      : u64 orig, u64 new, u32 qty, u64 price body=28
//   'x' canceled      : u64 client_order_id, u32 cancelled_qty body=12
//   'G' cancel reject : u64 client_order_id, u16 reason      body=10
//   'E' executed      : u64 client_order_id, u32 instrument, u8 side,
//                       u32 fill_qty, u64 fill_price, u64 trade_ref,
//                       u8 liquidity ('A' added / 'R' removed) body=34
//
// msg_seq: exchange-to-client response sequence (wraps at 2^32), used for
// session liveness and replay ordering. exch_ts_ns: exchange event time.
//
// Note 'E' is both "enter" (request stream) and "executed" (response stream) —
// direction disambiguates, mirroring OUCH's own convention.
//
// Frames on a stream carry no outer length prefix in this design: the engine
// and simulator exchange fixed-size datagram-like buffers (replay backend
// preserves message boundaries). Tools that need stream framing use
// kitch::encode_frame_prefix-style wrappers themselves.
// ============================================================================

#include <cstdint>
#include <span>

#include "kestrel/core/byte_codec.hpp"

namespace kestrel::protocols::kouch {

using core::ByteReader;
using core::ByteWriter;
using core::CodecStatus;

inline constexpr std::uint8_t kSideBuy = 1;
inline constexpr std::uint8_t kSideSell = 2;
inline constexpr std::uint8_t kTifDay = 'D';
inline constexpr std::uint8_t kTifIoc = 'I';

enum class Reason : std::uint16_t {
    None = 0,
    UnknownInstrument = 1,
    InvalidPrice = 2,
    InvalidQty = 3,
    DuplicateClientOrderId = 4,
    UnknownOrder = 5,
    RiskRejected = 6,
    ExceedsLimits = 7,
    SessionClosed = 8,
    Unknown = 99,
};

// ---- requests --------------------------------------------------------------
inline constexpr std::size_t kLenEnter = 27;    // 1+8+4+1+4+8+1
inline constexpr std::size_t kLenCancel = 9;    // 1+8
inline constexpr std::size_t kLenReplace = 29;  // 1+8+8+4+8

struct Enter {
    std::uint64_t client_order_id = 0;
    std::uint32_t instrument = 0;
    std::uint8_t side = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
    std::uint8_t tif = kTifDay;
};

struct Cancel {
    std::uint64_t client_order_id = 0;
};

struct Replace {
    std::uint64_t orig_client_id = 0;
    std::uint64_t new_client_id = 0;
    std::uint32_t new_qty = 0;
    std::uint64_t new_price = 0;
};

// ---- responses -------------------------------------------------------------
inline constexpr std::size_t kRespHeaderLen = 1 + 4 + 8;   // type + seq + ts
inline constexpr std::size_t kLenAccepted = kRespHeaderLen + 25;   // 8+4+1+4+8
inline constexpr std::size_t kLenDeclined = kRespHeaderLen + 10;   // 8+2
inline constexpr std::size_t kLenReplaced = kRespHeaderLen + 28;   // 8+8+4+8
inline constexpr std::size_t kLenCanceled = kRespHeaderLen + 12;   // 8+4
inline constexpr std::size_t kLenCancelReject = kRespHeaderLen + 10;
inline constexpr std::size_t kLenExecuted = kRespHeaderLen + 34;   // 8+4+1+4+8+8+1

enum class RespType : std::uint8_t {
    Accepted = 'A',
    Declined = 'D',
    Replaced = 'U',
    Canceled = 'x',
    CancelReject = 'G',
    Executed = 'E',
};

struct RespHeader {
    RespType type{};
    std::uint32_t msg_seq = 0;
    std::uint64_t exch_ts_ns = 0;
};

struct Accepted {
    std::uint64_t client_order_id = 0;
    std::uint32_t instrument = 0;
    std::uint8_t side = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
};

struct Declined {
    std::uint64_t client_order_id = 0;
    std::uint16_t reason = 0;
};

struct Replaced {
    std::uint64_t orig_client_id = 0;
    std::uint64_t new_client_id = 0;
    std::uint32_t qty = 0;
    std::uint64_t price = 0;
};

struct Canceled {
    std::uint64_t client_order_id = 0;
    std::uint32_t cancelled_qty = 0;
};

struct CancelReject {
    std::uint64_t client_order_id = 0;
    std::uint16_t reason = 0;
};

struct Executed {
    std::uint64_t client_order_id = 0;
    std::uint32_t instrument = 0;
    std::uint8_t side = 0;
    std::uint32_t fill_qty = 0;
    std::uint64_t fill_price = 0;
    std::uint64_t trade_ref = 0;
    std::uint8_t liquidity = 'R';
};

// ---- request decoding ------------------------------------------------------
enum class ReqDecode : std::uint8_t { Ok, Truncated, BadType, BadLength };

inline ReqDecode decode_request(std::span<const std::uint8_t> buf, Enter& e,
                                Cancel& c, Replace& r) noexcept {
    if (buf.empty()) return ReqDecode::Truncated;
    switch (buf[0]) {
        case 'E':
            if (buf.size() != kLenEnter) return ReqDecode::BadLength;
            {
                ByteReader rd(buf.subspan(1));
                if (rd.u64le(e.client_order_id) != CodecStatus::Ok ||
                    rd.u32le(e.instrument) != CodecStatus::Ok ||
                    rd.u8(e.side) != CodecStatus::Ok ||
                    rd.u32le(e.qty) != CodecStatus::Ok ||
                    rd.u64le(e.price) != CodecStatus::Ok ||
                    rd.u8(e.tif) != CodecStatus::Ok) return ReqDecode::Truncated;
            }
            return ReqDecode::Ok;
        case 'C':
            if (buf.size() != kLenCancel) return ReqDecode::BadLength;
            {
                ByteReader rd(buf.subspan(1));
                if (rd.u64le(c.client_order_id) != CodecStatus::Ok)
                    return ReqDecode::Truncated;
            }
            return ReqDecode::Ok;
        case 'R':
            if (buf.size() != kLenReplace) return ReqDecode::BadLength;
            {
                ByteReader rd(buf.subspan(1));
                if (rd.u64le(r.orig_client_id) != CodecStatus::Ok ||
                    rd.u64le(r.new_client_id) != CodecStatus::Ok ||
                    rd.u32le(r.new_qty) != CodecStatus::Ok ||
                    rd.u64le(r.new_price) != CodecStatus::Ok) return ReqDecode::Truncated;
            }
            return ReqDecode::Ok;
        default:
            return ReqDecode::BadType;
    }
}

// ---- response decoding -----------------------------------------------------
inline ReqDecode decode_response_header(std::span<const std::uint8_t> buf,
                                        RespHeader& h) noexcept {
    if (buf.size() < kRespHeaderLen) return ReqDecode::Truncated;
    switch (buf[0]) {
        case 'A': h.type = RespType::Accepted; break;
        case 'D': h.type = RespType::Declined; break;
        case 'U': h.type = RespType::Replaced; break;
        case 'x': h.type = RespType::Canceled; break;
        case 'G': h.type = RespType::CancelReject; break;
        case 'E': h.type = RespType::Executed; break;
        default: return ReqDecode::BadType;
    }
    ByteReader r(buf.subspan(1));
    if (r.u32le(h.msg_seq) != CodecStatus::Ok ||
        r.u64le(h.exch_ts_ns) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}

inline ReqDecode decode_accepted(std::span<const std::uint8_t> buf, Accepted& m) noexcept {
    if (buf.size() != kLenAccepted) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.client_order_id) != CodecStatus::Ok ||
        r.u32le(m.instrument) != CodecStatus::Ok ||
        r.u8(m.side) != CodecStatus::Ok ||
        r.u32le(m.qty) != CodecStatus::Ok ||
        r.u64le(m.price) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}
inline ReqDecode decode_declined(std::span<const std::uint8_t> buf, Declined& m) noexcept {
    if (buf.size() != kLenDeclined) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.client_order_id) != CodecStatus::Ok ||
        r.u16le(m.reason) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}
inline ReqDecode decode_replaced(std::span<const std::uint8_t> buf, Replaced& m) noexcept {
    if (buf.size() != kLenReplaced) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.orig_client_id) != CodecStatus::Ok ||
        r.u64le(m.new_client_id) != CodecStatus::Ok ||
        r.u32le(m.qty) != CodecStatus::Ok ||
        r.u64le(m.price) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}
inline ReqDecode decode_canceled(std::span<const std::uint8_t> buf, Canceled& m) noexcept {
    if (buf.size() != kLenCanceled) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.client_order_id) != CodecStatus::Ok ||
        r.u32le(m.cancelled_qty) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}
inline ReqDecode decode_cancel_reject(std::span<const std::uint8_t> buf,
                                      CancelReject& m) noexcept {
    if (buf.size() != kLenCancelReject) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.client_order_id) != CodecStatus::Ok ||
        r.u16le(m.reason) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}
inline ReqDecode decode_executed(std::span<const std::uint8_t> buf, Executed& m) noexcept {
    if (buf.size() != kLenExecuted) return ReqDecode::BadLength;
    ByteReader r(buf.subspan(kRespHeaderLen));
    if (r.u64le(m.client_order_id) != CodecStatus::Ok ||
        r.u32le(m.instrument) != CodecStatus::Ok ||
        r.u8(m.side) != CodecStatus::Ok ||
        r.u32le(m.fill_qty) != CodecStatus::Ok ||
        r.u64le(m.fill_price) != CodecStatus::Ok ||
        r.u64le(m.trade_ref) != CodecStatus::Ok ||
        r.u8(m.liquidity) != CodecStatus::Ok) return ReqDecode::Truncated;
    return ReqDecode::Ok;
}

// ---- encoding --------------------------------------------------------------
inline std::size_t encode_enter(std::span<std::uint8_t> out, const Enter& m) noexcept {
    if (out.size() < kLenEnter) return 0;
    ByteWriter w(out);
    if (w.u8('E') != CodecStatus::Ok ||
        w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u32le(m.instrument) != CodecStatus::Ok ||
        w.u8(m.side) != CodecStatus::Ok ||
        w.u32le(m.qty) != CodecStatus::Ok ||
        w.u64le(m.price) != CodecStatus::Ok ||
        w.u8(m.tif) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t encode_cancel(std::span<std::uint8_t> out, const Cancel& m) noexcept {
    if (out.size() < kLenCancel) return 0;
    ByteWriter w(out);
    if (w.u8('C') != CodecStatus::Ok ||
        w.u64le(m.client_order_id) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t encode_replace(std::span<std::uint8_t> out, const Replace& m) noexcept {
    if (out.size() < kLenReplace) return 0;
    ByteWriter w(out);
    if (w.u8('R') != CodecStatus::Ok ||
        w.u64le(m.orig_client_id) != CodecStatus::Ok ||
        w.u64le(m.new_client_id) != CodecStatus::Ok ||
        w.u32le(m.new_qty) != CodecStatus::Ok ||
        w.u64le(m.new_price) != CodecStatus::Ok) return 0;
    return w.written();
}

inline std::size_t encode_response_header(std::span<std::uint8_t> out, RespType t,
                                          std::uint32_t seq,
                                          std::uint64_t ts_ns) noexcept {
    if (out.size() < kRespHeaderLen) return 0;
    ByteWriter w(out);
    if (w.u8(static_cast<std::uint8_t>(t)) != CodecStatus::Ok ||
        w.u32le(seq) != CodecStatus::Ok ||
        w.u64le(ts_ns) != CodecStatus::Ok) return 0;
    return w.written();
}
/// Append body starting at offset kRespHeaderLen of a response buffer.
inline std::size_t response_append_body(std::span<std::uint8_t> resp_buf,
                                        std::span<const std::uint8_t> body) noexcept {
    if (resp_buf.size() < kRespHeaderLen + body.size()) return 0;
    for (std::size_t i = 0; i < body.size(); ++i)
        resp_buf[kRespHeaderLen + i] = body[i];
    return kRespHeaderLen + body.size();
}
inline std::size_t body_accepted(std::span<std::uint8_t> out, const Accepted& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u32le(m.instrument) != CodecStatus::Ok ||
        w.u8(m.side) != CodecStatus::Ok ||
        w.u32le(m.qty) != CodecStatus::Ok ||
        w.u64le(m.price) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_declined(std::span<std::uint8_t> out, const Declined& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u16le(m.reason) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_replaced(std::span<std::uint8_t> out, const Replaced& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.orig_client_id) != CodecStatus::Ok ||
        w.u64le(m.new_client_id) != CodecStatus::Ok ||
        w.u32le(m.qty) != CodecStatus::Ok ||
        w.u64le(m.price) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_canceled(std::span<std::uint8_t> out, const Canceled& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u32le(m.cancelled_qty) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_cancel_reject(std::span<std::uint8_t> out,
                                      const CancelReject& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u16le(m.reason) != CodecStatus::Ok) return 0;
    return w.written();
}
inline std::size_t body_executed(std::span<std::uint8_t> out, const Executed& m) noexcept {
    ByteWriter w(out);
    if (w.u64le(m.client_order_id) != CodecStatus::Ok ||
        w.u32le(m.instrument) != CodecStatus::Ok ||
        w.u8(m.side) != CodecStatus::Ok ||
        w.u32le(m.fill_qty) != CodecStatus::Ok ||
        w.u64le(m.fill_price) != CodecStatus::Ok ||
        w.u64le(m.trade_ref) != CodecStatus::Ok ||
        w.u8(m.liquidity) != CodecStatus::Ok) return 0;
    return w.written();
}

}  // namespace kestrel::protocols::kouch
