// Unit tests: kestrel::core::FlatMap (open addressing, backward-shift deletion)
#include <gtest/gtest.h>
#include <unordered_map>

#include "kestrel/core/flat_map.hpp"

using kestrel::core::FlatMap;

TEST(FlatMap, InsertFindErase) {
    FlatMap<int> m(16);
    EXPECT_TRUE(m.insert(10, 100));
    EXPECT_TRUE(m.insert(20, 200));
    EXPECT_FALSE(m.insert(10, 111));        // duplicate rejected, value kept
    EXPECT_EQ(*m.find(10), 100);
    EXPECT_EQ(*m.find(20), 200);
    EXPECT_EQ(m.find(30), nullptr);
    EXPECT_TRUE(m.erase(10));
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_FALSE(m.erase(10));              // double erase
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatMap, KeyZeroIsRemapped) {
    FlatMap<int> m(8);
    EXPECT_TRUE(m.insert(0, 5));
    EXPECT_EQ(*m.find(0), 5);
    EXPECT_FALSE(m.insert(0, 6));
    EXPECT_TRUE(m.erase(0));
    EXPECT_EQ(m.find(0), nullptr);
}

TEST(FlatMap, ExhaustionAtKeyCapacity) {
    FlatMap<int> m(4);
    for (std::uint64_t k = 1; k <= 4; ++k) EXPECT_TRUE(m.insert(k, int(k) * 10));
    EXPECT_FALSE(m.insert(5, 50));    // documented exhaustion policy
    EXPECT_TRUE(m.erase(3));          // erase frees budget (no tombstone garbage)
    EXPECT_TRUE(m.insert(5, 50));
    EXPECT_EQ(*m.find(5), 50);
}

TEST(FlatMap, InsertOrAssign) {
    FlatMap<int> m(4);
    EXPECT_TRUE(m.insert_or_assign(1, 10));
    EXPECT_TRUE(m.insert_or_assign(1, 11));   // overwrite path
    EXPECT_EQ(*m.find(1), 11);
}

TEST(FlatMap, BackwardShiftKeepsLookupsCorrect) {
    // Craft collisions: sequential keys hash via mix64 -> check all lookups
    // after heavy delete patterns still succeed.
    FlatMap<int> m(64);
    for (std::uint64_t k = 0; k < 50; ++k) ASSERT_TRUE(m.insert(k, int(k) + 1));
    // Delete every third key.
    for (std::uint64_t k = 0; k < 50; k += 3) ASSERT_TRUE(m.erase(k));
    for (std::uint64_t k = 0; k < 50; ++k) {
        const int* v = m.find(k);
        if (k % 3 == 0) {
            EXPECT_EQ(v, nullptr) << "key " << k;
        } else {
            ASSERT_NE(v, nullptr) << "key " << k;
            EXPECT_EQ(*v, int(k) + 1);
        }
    }
    EXPECT_EQ(m.size(), 50u - (50 + 2) / 3);
}

TEST(FlatMap, Clear) {
    FlatMap<int> m(8);
    m.insert(1, 1);
    m.insert(2, 2);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_TRUE(m.insert(1, 9));
}
