// Unit tests: KITCH codec (framing, endianness, malformed inputs)
#include <gtest/gtest.h>
#include <random>

#include "kestrel/protocols/kitch.hpp"

using namespace kestrel::protocols::kitch;
namespace core = kestrel::core;
using kestrel::core::ByteWriter;

namespace {
std::size_t build_add(std::span<std::uint8_t> out, std::uint32_t seq,
                      std::uint64_t ts, const AddOrder& a) {
    std::uint8_t body[32];
    const std::size_t blen = body_add(body, a);
    return encode_framed(out, MsgType::AddOrder, seq, ts,
                         std::span<const std::uint8_t>(body, blen));
}
std::size_t frame_payload(std::span<const std::uint8_t> framed,
                          std::span<const std::uint8_t>& payload) {
    if (framed.size() < 2) return 0;
    const std::size_t len = (static_cast<std::size_t>(framed[0]) << 8) | framed[1];
    if (framed.size() < 2 + len) return 0;
    payload = framed.subspan(2, len);
    return 2 + len;
}
}  // namespace

TEST(Kitch, AddRoundTripBigEndian) {
    std::uint8_t buf[64];
    const AddOrder a{7, 0x1122334455667788ull, kSideBuy, 500, 100'000};
    const std::size_t n = build_add(buf, 42, 1'000'000, a);
    ASSERT_EQ(n, 2 + kLenAdd);

    std::span<const std::uint8_t> payload;
    ASSERT_EQ(frame_payload({buf, n}, payload), n);
    Header h{};
    ASSERT_EQ(decode_header(payload, h), DecodeStatus::Ok);
    EXPECT_EQ(h.type, MsgType::AddOrder);
    EXPECT_EQ(h.feed_seq, 42u);
    EXPECT_EQ(h.event_ts_ns, 1'000'000u);

    AddOrder d{};
    ASSERT_EQ(decode_add(payload, d), DecodeStatus::Ok);
    EXPECT_EQ(d.instrument, 7u);
    EXPECT_EQ(d.order_ref, 0x1122334455667788ull);
    EXPECT_EQ(d.side, kSideBuy);
    EXPECT_EQ(d.qty, 500u);
    EXPECT_EQ(d.price, 100'000u);
}

TEST(Kitch, BigEndianByteLayout) {
    // Verify explicit wire bytes: type | seq(BE) | ts(BE)
    std::uint8_t body[32];
    const AddOrder a{1, 0x0102030405060708ull, kSideSell, 0x0A0B0C0D, 0x1112131415161718ull};
    const std::size_t blen = body_add(body, a);
    std::uint8_t buf[64];
    const std::size_t n = encode_framed(buf, MsgType::AddOrder, 0x0102, 0x030405060708090aull,
                                        {body, blen});
    ASSERT_EQ(n, 2 + kLenAdd);
    // frame length prefix BE
    EXPECT_EQ(buf[0], 0x00);
    EXPECT_EQ(buf[1], kLenAdd);
    const std::uint8_t* p = buf + 2;
    // payload: type[0] seq[1..4] ts[5..12] body[13..]
    EXPECT_EQ(p[0], 'A');
    EXPECT_EQ(p[1], 0x00); EXPECT_EQ(p[3], 0x01); EXPECT_EQ(p[4], 0x02);  // seq u32 BE
    EXPECT_EQ(p[5], 0x03); EXPECT_EQ(p[12], 0x0a);              // ts BE
    // body: instrument BE (u32 at body offset 0)
    EXPECT_EQ(p[kHeaderLen], 0x00); EXPECT_EQ(p[kHeaderLen + 3], 0x01);
}

TEST(Kitch, TruncatedFramesRejected) {
    std::uint8_t buf[64];
    const AddOrder a{1, 1, kSideBuy, 10, 100};
    const std::size_t n = build_add(buf, 1, 1, a);
    // Every truncation of the payload must fail decode (not crash, not OOB).
    std::span<const std::uint8_t> payload;
    ASSERT_EQ(frame_payload({buf, n}, payload), n);
    for (std::size_t cut = 0; cut < payload.size(); ++cut) {
        Header h{};
        AddOrder d{};
        const auto hs = decode_header(payload.first(cut), h);
        if (cut < kHeaderLen) {
            EXPECT_EQ(hs, DecodeStatus::Truncated) << cut;
        }
        // decode_add expects EXACT length: anything != kLenAdd -> BadLength
        const auto as = decode_add(payload.first(cut), d);
        EXPECT_NE(as, DecodeStatus::Ok) << cut;
    }
}

TEST(Kitch, BadTypeRejected) {
    std::uint8_t payload[] = {'Z', 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    Header h{};
    EXPECT_EQ(decode_header(payload, h), DecodeStatus::BadType);
}

TEST(Kitch, CancelExecReplaceTradeRoundTrip) {
    std::uint8_t body[64], buf[96];
    const OrderCancel c{77, 25};
    std::size_t n = encode_framed(buf, MsgType::OrderCancel, 1, 2,
                                  {body, body_cancel(body, c)});
    ASSERT_EQ(n, 2 + kLenCancel);
    OrderCancel cd{};
    std::span<const std::uint8_t> payload;
    frame_payload({buf, n}, payload);
    EXPECT_EQ(decode_cancel(payload, cd), DecodeStatus::Ok);
    EXPECT_EQ(cd.order_ref, 77u);
    EXPECT_EQ(cd.cancelled_qty, 25u);

    const OrderExecuted e{77, 12, 900};
    n = encode_framed(buf, MsgType::OrderExecuted, 1, 2, {body, body_exec(body, e)});
    OrderExecuted ed{};
    frame_payload({buf, n}, payload);
    EXPECT_EQ(decode_exec(payload, ed), DecodeStatus::Ok);
    EXPECT_EQ(ed.executed_qty, 12u);
    EXPECT_EQ(ed.trade_ref, 900u);

    const OrderReplace r{77, 78, 30, 555};
    n = encode_framed(buf, MsgType::OrderReplace, 1, 2, {body, body_replace(body, r)});
    OrderReplace rd{};
    frame_payload({buf, n}, payload);
    EXPECT_EQ(decode_replace(payload, rd), DecodeStatus::Ok);
    EXPECT_EQ(rd.new_ref, 78u);

    const Trade t{3, 15, 4242, 43};
    n = encode_framed(buf, MsgType::Trade, 1, 2, {body, body_trade(body, t)});
    Trade td{};
    frame_payload({buf, n}, payload);
    EXPECT_EQ(decode_trade(payload, td), DecodeStatus::Ok);
    EXPECT_EQ(td.price, 4242u);
}

TEST(Kitch, SnapshotRoundTrip) {
    std::uint8_t buf[512];
    // Build snapshot: 3 per-order entries (side, qty, price, ref).
    const std::uint16_t nlevels = 3;
    std::uint8_t body[6 + 3 * kSnapshotEntryLen];
    core::ByteWriter w(body);
    w.u32be(5);
    w.u16be(nlevels);
    struct LV { std::uint8_t side; std::uint32_t qty; std::uint64_t price; std::uint64_t ref; };
    const LV levels[nlevels] = {{kSideBuy, 100, 99'998, 5001}, {kSideBuy, 40, 99'997, 5002},
                                {kSideSell, 70, 100'002, 5003}};
    for (const auto& lv : levels) {
        w.u8(lv.side);
        w.u32be(lv.qty);
        w.u64be(lv.price);
        w.u64be(lv.ref);
    }
    const std::size_t n = encode_framed(buf, MsgType::BookSnapshot, 9, 10,
                                        {body, sizeof(body)});
    ASSERT_EQ(n, 2 + kHeaderLen + 6 + 3 * kSnapshotEntryLen);

    std::span<const std::uint8_t> payload;
    frame_payload({buf, n}, payload);
    SnapshotLevel out[8];
    std::uint16_t got = 0;
    ASSERT_EQ(decode_snapshot(payload, out, got), DecodeStatus::Ok);
    EXPECT_EQ(got, 3u);
    EXPECT_EQ(out[0].price, 99'998u);
    EXPECT_EQ(out[0].side, kSideBuy);
    EXPECT_EQ(out[0].venue_ref, 5001u);
    EXPECT_EQ(out[2].price, 100'002u);
    EXPECT_EQ(out[2].qty, 70u);
    EXPECT_EQ(out[2].venue_ref, 5003u);
}

TEST(Kitch, SnapshotSmallBufferReportsTruncated) {
    std::uint8_t buf[512];
    std::uint8_t body[6 + 2 * kSnapshotEntryLen];
    core::ByteWriter w(body);
    w.u32be(1);
    w.u16be(2);
    for (int i = 0; i < 2; ++i) {
        w.u8(kSideBuy);
        w.u32be(10);
        w.u64be(1000);
        w.u64be(100 + i);
    }
    const std::size_t n = encode_framed(buf, MsgType::BookSnapshot, 1, 1,
                                        {body, sizeof(body)});
    std::span<const std::uint8_t> payload;
    frame_payload({buf, n}, payload);
    SnapshotLevel out[1];   // too small for 2 levels
    std::uint16_t got = 0;
    EXPECT_EQ(decode_snapshot(payload, out, got), DecodeStatus::Truncated);
}

TEST(Kitch, EncodeOverflowReturnsZero) {
    std::uint8_t small[8];
    std::uint8_t body[32];
    const AddOrder a{1, 1, kSideBuy, 1, 1};
    EXPECT_EQ(encode_framed(small, MsgType::AddOrder, 1, 1, {body, body_add(body, a)}), 0u);
}
