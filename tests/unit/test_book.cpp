// Unit tests: LevelBook behaviors defined by spec §7
#include <gtest/gtest.h>

#include "kestrel/book/level_book.hpp"
#include "kestrel/book/ref_book.hpp"

using namespace kestrel::book;

namespace {
using LB = LevelBook<256, 64>;
}  // namespace

TEST(LevelBook, AddAndBestPrices) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'198), BookResult::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 20, 99'199), BookResult::Ok);
    EXPECT_EQ(b.add(3, Side::Sell, 15, 99'201), BookResult::Ok);
    EXPECT_EQ(b.add(4, Side::Sell, 25, 99'202), BookResult::Ok);
    EXPECT_EQ(b.best_bid(), std::optional<std::int64_t>(99'199));
    EXPECT_EQ(b.best_ask(), std::optional<std::int64_t>(99'201));
    EXPECT_EQ(b.level_qty(99'198, Side::Buy), 10);
    EXPECT_EQ(b.level_qty(99'199, Side::Buy), 20);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, DuplicateIdRejected) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'199), BookResult::Ok);
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'199), BookResult::DuplicateId);
    EXPECT_EQ(b.add(1, Side::Sell, 10, 99'201), BookResult::DuplicateId);
    EXPECT_EQ(b.level_qty(99'199, Side::Buy), 10);   // unchanged
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, UnknownCancelAndExecute) {
    LB b(99'000);
    EXPECT_EQ(b.cancel(999, 10), BookResult::UnknownOrder);
    EXPECT_EQ(b.execute(999, 10), BookResult::UnknownOrder);
    EXPECT_EQ(b.stats().unknown_orders, 2u);
}

TEST(LevelBook, PartialCancelThenFull) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Buy, 30, 99'199), BookResult::Ok);
    EXPECT_EQ(b.cancel(1, 10), BookResult::Ok);       // partial
    EXPECT_EQ(b.level_qty(99'199, Side::Buy), 20);
    EXPECT_EQ(b.cancel(1, 5), BookResult::Ok);        // partial again
    EXPECT_EQ(b.level_qty(99'199, Side::Buy), 15);
    EXPECT_EQ(b.cancel(1, 0), BookResult::Ok);        // qty<=0: remove entirely
    EXPECT_EQ(b.best_bid(), std::nullopt);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, ExecutionRemovesAtZero) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Sell, 30, 99'201), BookResult::Ok);
    EXPECT_EQ(b.execute(1, 10), BookResult::Ok);
    EXPECT_EQ(b.level_qty(99'201, Side::Sell), 20);
    EXPECT_EQ(b.execute(1, 20), BookResult::Ok);
    EXPECT_EQ(b.best_ask(), std::nullopt);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, OverExecutionAnomaly) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Sell, 10, 99'201), BookResult::Ok);
    EXPECT_EQ(b.execute(1, 15), BookResult::OverExecuted);   // 10 applied, node gone
    EXPECT_EQ(b.best_ask(), std::nullopt);                   // applied fully
    EXPECT_EQ(b.stats().over_executions, 1u);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, InvalidPriceRejected) {
    LB b(99'000);   // range [99'000, 99'000+256)
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'049), BookResult::Ok);        // in range
    EXPECT_EQ(b.add(2, Side::Buy, 10, 90'000), BookResult::InvalidPrice);
    EXPECT_EQ(b.add(3, Side::Sell, 10, 200'000), BookResult::InvalidPrice);
    EXPECT_EQ(b.stats().invalid_prices, 2u);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, ExhaustionBehavior) {
    LevelBook<64, 4> b(99'000);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        EXPECT_EQ(b.add(i, Side::Buy, 10, 99'049), BookResult::Ok);
    }
    EXPECT_EQ(b.add(5, Side::Buy, 10, 99'049), BookResult::CapacityExhausted);
    EXPECT_EQ(b.stats().capacity_blocks, 1u);
    EXPECT_TRUE(b.cancel(1, 0) == BookResult::Ok);
    EXPECT_EQ(b.add(5, Side::Buy, 10, 99'049), BookResult::Ok);   // after free
}

TEST(LevelBook, ReplaceResetsQueue) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'049), BookResult::Ok);   // first in queue
    EXPECT_EQ(b.add(2, Side::Buy, 20, 99'049), BookResult::Ok);
    EXPECT_EQ(b.replace(2, 3, 25, 99'049), BookResult::Ok);       // queue reset
    EXPECT_EQ(b.level_qty(99'049, Side::Buy), 35);
    EXPECT_EQ(b.stats().replacements, 1u);
    // Executing 10 must hit order 1 (FIFO head), leaving 15.
    EXPECT_EQ(b.execute(1, 10), BookResult::Ok);
    EXPECT_EQ(b.level_qty(99'049, Side::Buy), 25);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, ReplaceUnknownOrig) {
    LB b(99'000);
    EXPECT_EQ(b.replace(99, 100, 10, 99'199), BookResult::UnknownOrder);
}

TEST(LevelBook, SnapshotRestore) {
    LB b(99'000);
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'049), BookResult::Ok);
    EXPECT_EQ(b.add(2, Side::Sell, 12, 99'052), BookResult::Ok);
    std::array<LB::SnapshotEntry, 3> snap = {
        LB::SnapshotEntry{Side::Buy, 99'047, 40},
        LB::SnapshotEntry{Side::Buy, 99'046, 50},
        LB::SnapshotEntry{Side::Sell, 99'054, 70},
    };
    ASSERT_TRUE(b.restore_from_snapshot(snap));
    EXPECT_EQ(b.best_bid(), std::optional<std::int64_t>(99'047));
    EXPECT_EQ(b.best_ask(), std::optional<std::int64_t>(99'054));
    EXPECT_EQ(b.live_orders(), 3u);
    EXPECT_TRUE(b.validate());
}

TEST(LevelBook, SnapshotRestoreTooBigFails) {
    LevelBook<64, 2> b(100'000);
    std::array<LevelBook<64, 2>::SnapshotEntry, 3> snap = {
        LevelBook<64, 2>::SnapshotEntry{Side::Buy, 99'047, 40},
        LevelBook<64, 2>::SnapshotEntry{Side::Buy, 99'046, 50},
        LevelBook<64, 2>::SnapshotEntry{Side::Sell, 99'054, 70},
    };
    EXPECT_FALSE(b.restore_from_snapshot(snap));   // exceeds kMaxOrders=2
}

TEST(LevelBook, FifoAcrossPriceLevelsAndBitmapScans) {
    LB b(99'000);   // range [99'000, 99'000+256)
    EXPECT_EQ(b.add(1, Side::Buy, 10, 99'240), BookResult::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 20, 99'245), BookResult::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 30, 99'250), BookResult::Ok);
    EXPECT_EQ(b.best_bid(), std::optional<std::int64_t>(99'250));
    EXPECT_EQ(b.cancel(2, 0), BookResult::Ok);
    EXPECT_EQ(b.best_bid(), std::optional<std::int64_t>(99'250));
    EXPECT_EQ(b.cancel(3, 0), BookResult::Ok);
    EXPECT_EQ(b.best_bid(), std::optional<std::int64_t>(99'240));
    EXPECT_TRUE(b.validate());
}

TEST(RefBook, MatchesLevelBookOnSimpleFlows) {
    // LevelBook range: [99'000, 99'000+256). RefBook mirrors the range.
    LB lb(99'000);
    RefBook rb(99'000, 99'255);
    const auto add = [&](std::uint64_t id, Side s, std::int64_t q, std::int64_t p) {
        EXPECT_EQ(lb.add(id, s, q, p), rb.add(id, s, q, p));
    };
    add(1, Side::Buy, 10, 99'248);
    add(2, Side::Buy, 20, 99'249);
    add(3, Side::Sell, 15, 99'251);
    EXPECT_EQ(lb.best_bid(), rb.best_bid());
    EXPECT_EQ(lb.best_ask(), rb.best_ask());
    EXPECT_EQ(lb.execute(2, 5), rb.execute(2, 5));
    EXPECT_EQ(lb.level_qty(99'249, Side::Buy), rb.level_qty(99'249, Side::Buy));
    EXPECT_EQ(lb.cancel(3, 0), rb.cancel(3, 0));
    EXPECT_EQ(lb.best_ask(), rb.best_ask());
    EXPECT_EQ(lb.live_orders(), rb.live_orders());
}
