// Unit tests: KOUCH codec (requests + responses, LE layout, malformed inputs)
#include <gtest/gtest.h>

#include "kestrel/protocols/kouch.hpp"

using namespace kestrel::protocols::kouch;

TEST(Kouch, EnterRoundTrip) {
    std::uint8_t buf[32];
    const Enter e{0xAABBCCDDEEFF0011ull, 3, kSideBuy, 42, 123'456, kTifDay};
    const std::size_t n = encode_enter(buf, e);
    ASSERT_EQ(n, kLenEnter);

    Enter e2{};
    Cancel c2{};
    Replace r2{};
    ASSERT_EQ(decode_request({buf, kLenEnter}, e2, c2, r2), ReqDecode::Ok);
    EXPECT_EQ(e2.client_order_id, 0xAABBCCDDEEFF0011ull);
    EXPECT_EQ(e2.instrument, 3u);
    EXPECT_EQ(e2.side, kSideBuy);
    EXPECT_EQ(e2.qty, 42u);
    EXPECT_EQ(e2.price, 123'456u);
    EXPECT_EQ(e2.tif, kTifDay);
}

TEST(Kouch, EnterLittleEndianLayout) {
    std::uint8_t buf[32];
    const Enter e{0x0102030405060708ull, 0x0A0B0C0D, kSideSell, 0x11121314, 5, kTifIoc};
    ASSERT_EQ(encode_enter(buf, e), kLenEnter);
    // Layout: type[0] client_id[1..8] instrument[9..12] side[13] qty[14..17]
    //         price[18..25] tif[26]  (all multi-byte fields LE)
    EXPECT_EQ(buf[0], 'E');
    EXPECT_EQ(buf[1], 0x08);          // client_id LSB first
    EXPECT_EQ(buf[8], 0x01);          // client_id MSB last
    EXPECT_EQ(buf[9], 0x0D);          // instrument LSB
    EXPECT_EQ(buf[12], 0x0A);         // instrument MSB
    EXPECT_EQ(buf[13], kSideSell);
    EXPECT_EQ(buf[14], 0x14);         // qty LSB
    EXPECT_EQ(buf[17], 0x11);         // qty MSB
    EXPECT_EQ(buf[18], 0x05);         // price LSB
    EXPECT_EQ(buf[25], 0x00);         // price MSB
    EXPECT_EQ(buf[26], kTifIoc);
}

TEST(Kouch, CancelReplaceRoundTrip) {
    std::uint8_t buf[32];
    const Cancel c{0x1234567890ABCDEFull};
    ASSERT_EQ(encode_cancel(buf, c), kLenCancel);
    Enter e2{}; Cancel c2{}; Replace r2{};
    ASSERT_EQ(decode_request({buf, kLenCancel}, e2, c2, r2), ReqDecode::Ok);
    EXPECT_EQ(c2.client_order_id, 0x1234567890ABCDEFull);

    const Replace r{1, 2, 30, 999};
    ASSERT_EQ(encode_replace(buf, r), kLenReplace);
    ASSERT_EQ(decode_request({buf, kLenReplace}, e2, c2, r2), ReqDecode::Ok);
    EXPECT_EQ(r2.orig_client_id, 1u);
    EXPECT_EQ(r2.new_client_id, 2u);
    EXPECT_EQ(r2.new_qty, 30u);
    EXPECT_EQ(r2.new_price, 999u);
}

TEST(Kouch, ResponseHeaderAndAccepted) {
    std::uint8_t buf[64];
    const Accepted a{77, 2, kSideBuy, 10, 50'000};
    const std::size_t blen = body_accepted(buf, a);
    ASSERT_EQ(blen, 25u);
    std::uint8_t resp[64];
    ASSERT_EQ(encode_response_header(resp, RespType::Accepted, 5, 1'000), kRespHeaderLen);
    ASSERT_EQ(response_append_body(resp, {buf, blen}), kLenAccepted);

    RespHeader h{};
    const std::size_t total = response_append_body(resp, {buf, blen});
    ASSERT_EQ(total, kLenAccepted);
    ASSERT_EQ(decode_response_header({resp, total}, h), ReqDecode::Ok);
    EXPECT_EQ(h.type, RespType::Accepted);
    EXPECT_EQ(h.msg_seq, 5u);
    EXPECT_EQ(h.exch_ts_ns, 1'000u);

    Accepted ad{};
    ASSERT_EQ(decode_accepted({resp, total}, ad), ReqDecode::Ok);
    EXPECT_EQ(ad.client_order_id, 77u);
    EXPECT_EQ(ad.price, 50'000u);
}

namespace {
/// Assemble header+body and return the EXACT sized span (decoders require it).
std::span<std::uint8_t> make_response(std::span<std::uint8_t> resp, RespType t,
                                      std::span<const std::uint8_t> body) {
    encode_response_header(resp, t, 1, 2);
    const std::size_t total = response_append_body(resp, body);
    return resp.first(total);
}
}  // namespace

TEST(Kouch, AllResponseTypesRoundTrip) {
    std::uint8_t body[64], resp[96];

    const Declined d{9, static_cast<std::uint16_t>(Reason::InvalidPrice)};
    auto n = body_declined(body, d);
    Declined dd{};
    ASSERT_EQ(decode_declined(make_response(resp, RespType::Declined, {body, n}), dd),
              ReqDecode::Ok);
    EXPECT_EQ(dd.reason, static_cast<std::uint16_t>(Reason::InvalidPrice));

    const Replaced rp{1, 2, 33, 444};
    n = body_replaced(body, rp);
    Replaced rpd{};
    ASSERT_EQ(decode_replaced(make_response(resp, RespType::Replaced, {body, n}), rpd),
              ReqDecode::Ok);
    EXPECT_EQ(rpd.new_client_id, 2u);

    const Canceled c{7, 12};
    n = body_canceled(body, c);
    Canceled cd{};
    ASSERT_EQ(decode_canceled(make_response(resp, RespType::Canceled, {body, n}), cd),
              ReqDecode::Ok);
    EXPECT_EQ(cd.cancelled_qty, 12u);

    const CancelReject g{7, static_cast<std::uint16_t>(Reason::UnknownOrder)};
    n = body_cancel_reject(body, g);
    CancelReject gd{};
    ASSERT_EQ(decode_cancel_reject(
                  make_response(resp, RespType::CancelReject, {body, n}), gd),
              ReqDecode::Ok);
    EXPECT_EQ(gd.reason, static_cast<std::uint16_t>(Reason::UnknownOrder));

    const Executed e{7, 3, kSideSell, 8, 12'345, 99, 'A'};
    n = body_executed(body, e);
    Executed ed{};
    ASSERT_EQ(decode_executed(make_response(resp, RespType::Executed, {body, n}), ed),
              ReqDecode::Ok);
    EXPECT_EQ(ed.fill_price, 12'345u);
    EXPECT_EQ(ed.liquidity, 'A');
}

TEST(Kouch, TruncatedRequestsRejected) {
    std::uint8_t buf[32];
    const Enter e{1, 1, kSideBuy, 1, 1, kTifDay};
    ASSERT_EQ(encode_enter(buf, e), kLenEnter);
    Enter e2{}; Cancel c2{}; Replace r2{};
    for (std::size_t cut = 0; cut < kLenEnter; ++cut) {
        const auto st = decode_request({buf, cut}, e2, c2, r2);
        if (cut == 0) EXPECT_EQ(st, ReqDecode::Truncated);
        else EXPECT_NE(st, ReqDecode::Ok) << cut;
    }
    // Short-but-wrong lengths
    EXPECT_EQ(decode_request({buf, kLenEnter - 1}, e2, c2, r2), ReqDecode::BadLength);
    EXPECT_EQ(decode_request({buf, kLenEnter + 1}, e2, c2, r2), ReqDecode::BadLength);
}

TEST(Kouch, BadTypeRejected) {
    std::uint8_t bad[8] = {'Q', 0, 0, 0, 0, 0, 0, 0};
    Enter e2{}; Cancel c2{}; Replace r2{};
    EXPECT_EQ(decode_request(bad, e2, c2, r2), ReqDecode::BadType);
    RespHeader h{};
    std::uint8_t badr[13] = {'Q', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(decode_response_header(badr, h), ReqDecode::BadType);
}
