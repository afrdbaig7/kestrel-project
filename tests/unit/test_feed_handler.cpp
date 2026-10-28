// Unit tests: feed handler — sequence validation, gaps, duplicates, recovery
#include <gtest/gtest.h>

#include "kestrel/md/feed_handler.hpp"
#include "kestrel/protocols/kitch.hpp"

using namespace kestrel::protocols;
using kestrel::md::FeedHandler;
using kestrel::md::FeedState;
namespace core = kestrel::core;

namespace {
std::vector<std::uint8_t> make_add(std::uint32_t seq, std::uint64_t ts,
                                   std::uint64_t ref, kitch::AddOrder a = {1, 0, kitch::kSideBuy, 10, 100'000}) {
    a.order_ref = ref;
    std::uint8_t body[32];
    const std::size_t blen = kitch::body_add(body, a);
    std::vector<std::uint8_t> out(2 + kitch::kLenAdd);
    const std::size_t n = kitch::encode_framed(out, kitch::MsgType::AddOrder, seq, ts, {body, blen});
    out.resize(n);
    // strip outer length prefix for on_frame (expects payload only)
    return std::vector<std::uint8_t>(out.begin() + 2, out.end());
}
}  // namespace

TEST(FeedHandler, AppliesInOrderAndUpdatesBook) {
    FeedHandler fh(100'000);
    fh.on_frame(make_add(1, 1000, 10), true);
    fh.on_frame(make_add(2, 1001, 11, {1, 0, kitch::kSideSell, 20, 100'002}), true);
    EXPECT_TRUE(fh.is_synced());
    EXPECT_EQ(fh.applied_seq(), 2u);
    EXPECT_EQ(fh.book().best_bid(), std::optional<std::int64_t>(100'000));
    EXPECT_EQ(fh.book().best_ask(), std::optional<std::int64_t>(100'002));
    EXPECT_EQ(fh.stats().applied, 2u);
}

TEST(FeedHandler, DuplicateSuppressed) {
    FeedHandler fh(100'000);
    fh.on_frame(make_add(1, 1000, 10), true);
    fh.on_frame(make_add(1, 1000, 10), true);    // exact duplicate (A/B copy)
    fh.on_frame(make_add(1, 1000, 10), false);   // duplicate from channel B
    EXPECT_EQ(fh.applied_seq(), 1u);
    EXPECT_EQ(fh.stats().duplicates, 2u);
    EXPECT_EQ(fh.book().level_qty(100'000, kestrel::book::Side::Buy), 10);
}

TEST(FeedHandler, GapBlocksExposureUntilSnapshot) {
    FeedHandler fh(100'000);
    fh.on_frame(make_add(1, 1000, 10), true);
    fh.on_frame(make_add(5, 1004, 50), true);    // gap: seq jumped to 5
    EXPECT_FALSE(fh.is_synced());
    EXPECT_EQ(fh.stats().gaps, 1u);
    // While seeking, in-order-looking messages are DROPPED (book would lie).
    fh.on_frame(make_add(6, 1005, 60), true);
    EXPECT_EQ(fh.stats().dropped_while_seeking, 1u);
    EXPECT_EQ(fh.book().best_bid(), std::optional<std::int64_t>(100'000));  // unchanged

    // Recovery boundary: snapshot with seq 7 (per-order entry incl. ref).
    // Body = u32 instrument | u16 count | count * 21-byte entries (NO header).
    std::uint8_t body[6 + kitch::kSnapshotEntryLen];
    core::ByteWriter w(body);
    w.u32be(1);
    w.u16be(1);
    w.u8(kitch::kSideBuy);
    w.u32be(40);
    w.u64be(100'005);
    w.u64be(777);
    // payload = kHeaderLen + body; framed = 2 + payload
    std::vector<std::uint8_t> snap(2 + kitch::kHeaderLen + sizeof(body));
    const std::size_t n =
        kitch::encode_framed(snap, kitch::MsgType::BookSnapshot, 7, 1006, {body, sizeof(body)});
    snap.resize(n);
    fh.on_frame(std::vector<std::uint8_t>(snap.begin() + 2, snap.end()), true);
    EXPECT_TRUE(fh.is_synced());
    EXPECT_EQ(fh.applied_seq(), 7u);
    EXPECT_EQ(fh.stats().snapshot_restores, 1u);
    EXPECT_EQ(fh.book().best_bid(), std::optional<std::int64_t>(100'005));
    EXPECT_EQ(fh.book().level_qty(100'000, kestrel::book::Side::Buy), 0);  // old gone
}

TEST(FeedHandler, AnomalyMarksSuspect) {
    FeedHandler fh(100'000);
    fh.on_frame(make_add(1, 1000, 10), true);
    // Over-execution on the only order -> anomaly -> seeking.
    std::uint8_t body[32];
    const kitch::OrderExecuted e{10, 99, 1};   // exec 99 of 10
    const std::size_t blen = kitch::body_exec(body, e);
    std::vector<std::uint8_t> out(2 + kitch::kLenExec);
    const std::size_t n = kitch::encode_framed(out, kitch::MsgType::OrderExecuted, 2, 1001, {body, blen});
    out.resize(n);
    fh.on_frame(std::vector<std::uint8_t>(out.begin() + 2, out.end()), true);
    EXPECT_FALSE(fh.is_synced());
    EXPECT_EQ(fh.stats().rejects_anomaly, 1u);
}

TEST(FeedHandler, UnknownCancelTolerated) {
    FeedHandler fh(100'000);
    fh.on_frame(make_add(1, 1000, 10), true);
    std::uint8_t body[32];
    const kitch::OrderCancel c{12345, 5};   // unknown ref
    const std::size_t blen = kitch::body_cancel(body, c);
    std::vector<std::uint8_t> out(2 + kitch::kLenCancel);
    const std::size_t n = kitch::encode_framed(out, kitch::MsgType::OrderCancel, 2, 1001, {body, blen});
    out.resize(n);
    fh.on_frame(std::vector<std::uint8_t>(out.begin() + 2, out.end()), true);
    EXPECT_TRUE(fh.is_synced());   // tolerated: idempotent drop
    EXPECT_EQ(fh.applied_seq(), 2u);
}

TEST(FeedHandler, MalformedFrameCounted) {
    FeedHandler fh(100'000);
    std::vector<std::uint8_t> bad{'Q', 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    fh.on_frame(bad, true);
    EXPECT_EQ(fh.stats().rejects_decode, 1u);
}
