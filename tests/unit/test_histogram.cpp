// Unit tests: kestrel::core::Histogram — validates the HdrHistogram algorithm
// against brute-force order statistics (see ADR-0003).
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "kestrel/core/histogram.hpp"

using kestrel::core::Histogram;

namespace {
std::int64_t brute_percentile(std::vector<std::int64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double target = std::ceil(p / 100.0 * static_cast<double>(v.size()));
    const std::size_t idx =
        std::min(v.size() - 1, static_cast<std::size_t>(std::max(1.0, target)) - 1);
    // HdrHistogram reports the bucket ceiling; compare with the smallest
    // bucket-value >= sample (bucket resolution: ~1/1000 relative at 3 sig figs).
    return v[idx];
}
}  // namespace

TEST(Histogram, CountsAndBasicStats) {
    Histogram h(1'000'000, 3);
    for (int i = 1; i <= 100; ++i) h.record(i * 100);
    EXPECT_EQ(h.total_count(), 100);
    EXPECT_EQ(h.min_recorded(), 100);
    EXPECT_EQ(h.max_recorded(), 10'000);
    EXPECT_EQ(h.overflow_count(), 0);
}

TEST(Histogram, PercentilesMatchBruteForceWithinBucketResolution) {
    Histogram h(1'000'000'000, 3);
    std::mt19937_64 rng(42);
    std::vector<std::int64_t> samples;
    for (int i = 0; i < 10'000; ++i) {
        // Log-uniform-ish latency mix: 100 ns .. 1 ms.
        const std::int64_t v = 100 + static_cast<std::int64_t>(rng() % 999'900);
        samples.push_back(v);
        h.record(v);
    }
    // Bucket resolution at 3 sig figs is ~0.1%: allow 1% slack.
    const auto check = [&](double p) {
        const std::int64_t got = h.value_at_percentile(p);
        const std::int64_t want = brute_percentile(samples, p);
        EXPECT_LE(got, want * 1.01 + 1) << "p=" << p << " got=" << got
                                        << " want=" << want;
        EXPECT_GE(got, want * 0.99) << "p=" << p << " got=" << got
                                    << " want=" << want;
    };
    check(50.0);
    check(90.0);
    check(99.0);
    check(99.9);
    // p100 reports the bucket TOP of the max sample (HdrHistogram convention);
    // at 3 sig-figs the top can exceed the max by up to ~0.1%.
    EXPECT_GE(h.value_at_percentile(100.0), h.max_recorded());
    EXPECT_LE(h.value_at_percentile(100.0),
              static_cast<std::int64_t>(h.max_recorded() * 1.001) + 2);
    // Monotonic curve
    EXPECT_LE(h.value_at_percentile(50.0), h.value_at_percentile(99.0));
    EXPECT_LE(h.value_at_percentile(99.0), h.value_at_percentile(99.9));
    EXPECT_LE(h.value_at_percentile(99.9), h.value_at_percentile(99.99));
}

TEST(Histogram, PercentileRoundingMatchesHdrHistogram) {
    // Exact-match case: 100 records of 1000 -> p50 target = ceil(50) = 50th
    // cumulative bucket -> value 1000 (bucket resolution aside, expect 1000).
    Histogram h(1'000'000, 3);
    for (int i = 0; i < 100; ++i) h.record(1000);
    EXPECT_EQ(h.value_at_percentile(50.0), 1000);
    EXPECT_EQ(h.value_at_percentile(99.0), 1000);
    EXPECT_EQ(h.value_at_percentile(99.99), 1000);
    EXPECT_EQ(h.value_at_percentile(100.0), 1000);
}

TEST(Histogram, OverflowClampedAndCounted) {
    Histogram h(10'000, 3);
    h.record(5000);
    h.record(50'000);      // above highest_trackable
    h.record(50'000);
    EXPECT_EQ(h.overflow_count(), 2);
    EXPECT_EQ(h.total_count(), 3);
    EXPECT_EQ(h.max_recorded(), 50'000);
    // Clamped into top bucket; percentile reports the bucket TOP
    // (HdrHistogram highest_equivalent_value convention).
    const std::int64_t p100 = h.value_at_percentile(100.0);
    EXPECT_GE(p100, 10'000);
    EXPECT_LE(p100, 10'007);   // top of the top bucket at 3 sig-figs
}

TEST(Histogram, ZeroAndNegativeValues) {
    Histogram h(1000, 3);
    h.record(0);
    h.record(-5);          // clamps to 0 (capped-record semantics)
    EXPECT_EQ(h.total_count(), 2);
    EXPECT_EQ(h.min_recorded(), 0);   // min tracks non-zero values only
    EXPECT_EQ(h.max_recorded(), 0);
    EXPECT_EQ(h.value_at_percentile(50.0), 0);
}

TEST(Histogram, MergeAccumulates) {
    Histogram a(1'000'000, 3), b(1'000'000, 3);
    for (int i = 0; i < 50; ++i) a.record(1000);
    for (int i = 0; i < 50; ++i) b.record(3000);
    a.merge(b);
    EXPECT_EQ(a.total_count(), 100);
    EXPECT_EQ(a.value_at_percentile(50.0), 1000);
    EXPECT_EQ(a.value_at_percentile(99.0), 3001);  // bucket top of 3000
    EXPECT_EQ(a.max_recorded(), 3000);
}

TEST(Histogram, Reset) {
    Histogram h(1'000'000, 3);
    h.record(123);
    h.reset();
    EXPECT_EQ(h.total_count(), 0);
    EXPECT_EQ(h.max_recorded(), 0);
    h.record(456);
    EXPECT_EQ(h.value_at_percentile(100.0), 456);
}

TEST(Histogram, ExportsCsvAndSummary) {
    Histogram h(1'000'000, 3);
    for (int i = 0; i < 10; ++i) h.record(1000 + i);
    const std::string csv = h.to_csv();
    EXPECT_NE(csv.find("percentile,value_ns"), std::string::npos);
    const std::string s = h.summary();
    EXPECT_NE(s.find("count=10"), std::string::npos);
    EXPECT_NE(s.find("p50="), std::string::npos);
}
