// Property tests: differential LevelBook vs RefBook over random event streams,
// plus the five spec §7 invariants after every generated stream.
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <vector>

#include "kestrel/book/level_book.hpp"
#include "kestrel/book/ref_book.hpp"

using namespace kestrel::book;

namespace {
using LB = LevelBook<256, 4096>;   // generous pool: differential
                                  // semantics, not capacity, is under test
enum class Op { Add, Cancel, Execute, Replace };

struct Event {
    Op op;
    std::uint64_t ref, ref2;
    Side side;
    std::int64_t qty, price;
};
}  // namespace

TEST(PropertyBook, DifferentialAgainstRefBook) {
    std::mt19937_64 rng(20261101);
    for (int trial = 0; trial < 20; ++trial) {
        LB lb(100'000);
        RefBook rb(100'000, 100'255);
        std::vector<std::uint64_t> live_refs;

        for (int step = 0; step < 2'000; ++step) {
            const Op op = static_cast<Op>(rng() % 4);
            // Adds ~40% of the time or when nothing is live; ops on dead refs
            // are part of the contract (both books must agree on the result).
            if (op == Op::Add || live_refs.empty() || (rng() % 5) == 0) {
                const std::uint64_t ref = rng() + 1;
                const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
                const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 100);
                const std::int64_t p = 100'000 + static_cast<std::int64_t>(rng() % 256);
                const auto a = lb.add(ref, s, q, p);
                const auto b = rb.add(ref, s, q, p);
                ASSERT_EQ(a, b) << "trial " << trial << " step " << step;
                if (a == BookResult::Ok) live_refs.push_back(ref);
                ASSERT_EQ(lb.best_bid(), rb.best_bid());
                ASSERT_EQ(lb.best_ask(), rb.best_ask());
                continue;
            }
            const std::uint64_t ref = live_refs[rng() % live_refs.size()];
            switch (op) {
                case Op::Cancel: {
                    const std::int64_t q = static_cast<std::int64_t>(rng() % 120);
                    ASSERT_EQ(lb.cancel(ref, q), rb.cancel(ref, q));
                    break;
                }
                case Op::Execute: {
                    const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 120);
                    const auto a = lb.execute(ref, q);
                    const auto b = rb.execute(ref, q);
                    ASSERT_EQ(a, b) << "trial " << trial << " step " << step
                                    << " ref " << ref;
                    break;
                }
                case Op::Replace: {
                    const std::uint64_t nref = rng() + 1;
                    const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 100);
                    const std::int64_t p = 100'000 + static_cast<std::int64_t>(rng() % 256);
                    ASSERT_EQ(lb.replace(ref, nref, q, p),
                              rb.replace(ref, nref, q, p));
                    break;
                }
                default:
                    break;
            }
            // Best-price agreement after every event.
            ASSERT_EQ(lb.best_bid(), rb.best_bid()) << "trial " << trial << " step " << step;
            ASSERT_EQ(lb.best_ask(), rb.best_ask()) << "trial " << trial << " step " << step;
        }
        // Invariants 1-4 after the stream (5 = identical inputs -> identical
        // state is covered by the golden replay test).
        ASSERT_TRUE(lb.validate());
    }
}

TEST(PropertyBook, InvariantLevelQtyEqualsOrderSum) {
    // Directly exercise the invariant checker with a dense churn pattern.
    std::mt19937_64 rng(7);
    LB lb(100'000);
    for (int i = 0; i < 5'000; ++i) {
        const std::uint64_t ref = 1 + rng() % 500;
        const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
        const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 50);
        const std::int64_t p = 100'000 + static_cast<std::int64_t>(rng() % 30) - 15;
        const auto r = lb.add(ref, s, q, p);
        if (r == BookResult::Ok && (rng() % 3) == 0) {
            lb.cancel(ref, static_cast<std::int64_t>(rng() % 60));
        }
        if (r == BookResult::Ok && (rng() % 5) == 0) {
            lb.execute(ref, 1 + static_cast<std::int64_t>(rng() % 60));
        }
        if (i % 50 == 0) {
            ASSERT_TRUE(lb.validate()) << "i=" << i;
        }
    }
    ASSERT_TRUE(lb.validate());
}

TEST(PropertyBook, BestPricesMatchOccupiedLevels) {
    std::mt19937_64 rng(11);
    LB lb(100'000);
    for (int i = 0; i < 2'000; ++i) {
        const std::uint64_t ref = 1 + rng() % 300;
        const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
        const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 20);
        const std::int64_t p = 100'000 + static_cast<std::int64_t>(rng() % 50) - 25;
        lb.add(ref, s, q, p);
        if (rng() % 2) lb.cancel(ref, static_cast<std::int64_t>(rng() % 25));
        const auto bb = lb.best_bid();
        const auto ba = lb.best_ask();
        if (bb) {
            ASSERT_GT(lb.level_qty(*bb, Side::Buy), 0);
        }
        if (ba) {
            ASSERT_GT(lb.level_qty(*ba, Side::Sell), 0);
        }
        // No better price levels may exist (bitmap scans are exhaustive).
        if (bb) {
            for (std::int64_t p2 = *bb + 1; p2 < 100'025; ++p2)
                ASSERT_EQ(lb.level_qty(p2, Side::Buy), 0) << p2;
        }
        if (ba) {
            for (std::int64_t p2 = 99'975; p2 < *ba; ++p2)
                ASSERT_EQ(lb.level_qty(p2, Side::Sell), 0) << p2;
        }
    }
}
