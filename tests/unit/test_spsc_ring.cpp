// Unit tests: kestrel::core::SpscRing
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

#include "kestrel/core/spsc_ring.hpp"

using kestrel::core::SpscRing;

TEST(SpscRing, BasicPushPopPreservesOrder) {
    SpscRing<int, 8> r;
    EXPECT_TRUE(r.empty_approx());
    EXPECT_TRUE(r.try_push(1));
    EXPECT_TRUE(r.try_push(2));
    EXPECT_TRUE(r.try_push(3));
    EXPECT_EQ(r.size_approx(), 3u);
    int v{};
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, FullBehaviorIsBounded) {
    SpscRing<int, 4> r;
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));   // full -> documented false
    EXPECT_EQ(r.size_approx(), 4u);
    int v{};
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 0);
    EXPECT_TRUE(r.try_push(99));
    EXPECT_EQ(r.size_approx(), 4u);
    for (int want : {1, 2, 3, 99}) {
        EXPECT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, want);
    }
}

TEST(SpscRing, WrapsCorrectlyOverManyCycles) {
    SpscRing<std::uint64_t, 8> r;
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        ASSERT_TRUE(r.try_push(i));
        ASSERT_TRUE(r.try_pop(v));
        ASSERT_EQ(v, i);
    }
}

TEST(SpscRing, MoveOnlyTypesSupported) {
    struct MoveOnly {
        explicit MoveOnly(int x) noexcept : v(x) {}   // nothrow ctor keeps the
                                                      // ring's nothrow guarantee
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        int v;
    };
    SpscRing<MoveOnly, 4> r;
    EXPECT_TRUE(r.try_emplace(7));
    MoveOnly out{0};
    ASSERT_TRUE(r.try_pop(out));
    EXPECT_EQ(out.v, 7);
}

TEST(SpscRing, ConcurrencyStressChecksum) {
    // Producer/consumer checksum over 2M items; also runs under TSan preset.
    using Ring = SpscRing<std::uint64_t, 1024>;
    Ring ring;
    constexpr std::uint64_t N = 2'000'000;
    std::atomic<std::uint64_t> consumed_sum{0};
    std::atomic<std::uint64_t> consumed_n{0};

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= N; ++i) {
            while (!ring.try_push(i)) {
                // bounded spin: exercised policy is retry (backpressure)
            }
        }
    });
    std::thread consumer([&] {
        std::uint64_t v;
        while (consumed_n.load(std::memory_order_relaxed) < N) {
            if (ring.try_pop(v)) {
                consumed_sum.fetch_add(v, std::memory_order_relaxed);
                consumed_n.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    producer.join();
    consumer.join();

    const std::uint64_t want = N * (N + 1) / 2;
    EXPECT_EQ(consumed_sum.load(), want);
    EXPECT_EQ(consumed_n.load(), N);
    EXPECT_TRUE(ring.empty_approx());
}

TEST(SpscRing, HighWaterMark) {
    SpscRing<int, 16> r;
    for (int i = 0; i < 10; ++i) r.try_push(i);
    r.high_water_mark();
    EXPECT_EQ(r.high_water_mark(), 10u);
}
