// Unit tests: kestrel::core::Bitmap
#include <gtest/gtest.h>

#include "kestrel/core/bitmap.hpp"

using kestrel::core::Bitmap;

TEST(Bitmap, SetClearTest) {
    Bitmap<256> b;
    EXPECT_FALSE(b.test(0));
    b.set(0);
    b.set(63);
    b.set(64);
    b.set(255);
    EXPECT_TRUE(b.test(0));
    EXPECT_TRUE(b.test(63));
    EXPECT_TRUE(b.test(64));
    EXPECT_TRUE(b.test(255));
    EXPECT_EQ(b.count(), 4u);
    b.clear(63);
    EXPECT_FALSE(b.test(63));
    EXPECT_EQ(b.count(), 3u);
    b.clear_all();
    EXPECT_EQ(b.count(), 0u);
}

TEST(Bitmap, FindForwardScan) {
    Bitmap<1024> b;
    EXPECT_EQ(b.find_from(0), std::nullopt);
    b.set(100);
    b.set(200);
    b.set(900);
    EXPECT_EQ(b.find_from(0), 100u);
    EXPECT_EQ(b.find_from(100), 100u);
    EXPECT_EQ(b.find_from(101), 200u);
    EXPECT_EQ(b.find_from(201), 900u);
    EXPECT_EQ(b.find_from(901), std::nullopt);
    EXPECT_EQ(b.find_from(1023), std::nullopt);
    EXPECT_EQ(b.find_from(1024), std::nullopt);  // out of range safe
}

TEST(Bitmap, FindBackwardScan) {
    Bitmap<1024> b;
    EXPECT_EQ(b.find_to(1023), std::nullopt);
    b.set(5);
    b.set(300);
    b.set(999);
    EXPECT_EQ(b.find_to(1023), 999u);
    EXPECT_EQ(b.find_to(999), 999u);
    EXPECT_EQ(b.find_to(998), 300u);
    EXPECT_EQ(b.find_to(299), 5u);
    EXPECT_EQ(b.find_to(4), std::nullopt);
    EXPECT_EQ(b.find_to(0), std::nullopt);
}

TEST(Bitmap, WordBoundaryEdges) {
    Bitmap<128> b;
    b.set(62);
    b.set(63);
    b.set(64);
    b.set(127);
    EXPECT_EQ(b.find_from(63), 63u);
    EXPECT_EQ(b.find_from(64), 64u);
    EXPECT_EQ(b.find_to(63), 63u);
    EXPECT_EQ(b.find_to(64), 64u);
    EXPECT_EQ(b.find_from(65), 127u);
    EXPECT_EQ(b.find_to(126), 64u);
}

TEST(Bitmap, ChurnStaysConsistent) {
    Bitmap<64> b;
    b.set(10);
    b.set(11);
    EXPECT_EQ(b.find_from(10), 10u);
    b.clear(10);
    EXPECT_EQ(b.find_from(10), 11u);
    b.clear(11);
    EXPECT_EQ(b.find_from(0), std::nullopt);
    EXPECT_EQ(b.find_to(63), std::nullopt);
}
