// Unit tests: clock, calibration, CPU helpers, jitter meter smoke
#include <gtest/gtest.h>
#include <thread>

#include "kestrel/core/clock.hpp"
#include "kestrel/core/cpu.hpp"
#include "kestrel/core/jitter_meter.hpp"

using namespace kestrel::core;

TEST(Clock, InvariantTscDetectedOnThisHost) {
    // This sandbox reports nonstop_tsc/constant_tsc/rdtscp (verified at setup);
    // the assertion documents that the ordered path is the one under test.
    EXPECT_TRUE(TscClock::invariant_tsc_available());
    EXPECT_STREQ(TscClock::source_name(), "tsc_rdtscp");
}

TEST(Clock, MonotonicOnSingleCore) {
    if (allowed_cpus().size() > 1) pin_to_cpu(0);
    std::int64_t prev = TscClock::now();
    for (int i = 0; i < 100'000; ++i) {
        const std::int64_t t = TscClock::now();
        ASSERT_GE(t, prev) << "TSC went backwards at iter " << i;
        prev = t;
    }
    if (allowed_cpus().size() > 1) pin_to_cpu(1);  // restore-ish
}

TEST(Clock, CalibrationSaneTicksPerNs) {
    TscClock::calibrate(6, 20);
    const double tps = TscClock::ticks_per_ns();
    ASSERT_GT(tps, 0.0);
    // TSC is 3200 MHz on this host; sanity band 1 MHz .. 10 GHz.
    EXPECT_GT(tps, 1.0);
    EXPECT_LT(tps, 10'000.0);
    const auto t0 = TscClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto t1 = TscClock::now();
    const std::int64_t ns = TscClock::span_ns(t0, t1);
    ASSERT_GT(ns, 0);
    // 10 ms ± generous VM jitter bounds
    EXPECT_GT(ns, 1'000'000);     // > 1 ms
    EXPECT_LT(ns, 1'000'000'000); // < 1 s
}

TEST(Cpu, AffinityHelpers) {
    const auto cpus = allowed_cpus();
    ASSERT_FALSE(cpus.empty());
    EXPECT_TRUE(pin_to_cpu(cpus[0]));
    EXPECT_EQ(current_cpu(), cpus[0]);
}

TEST(Cpu, InspectReportsModelAndFlags) {
    const CpuInfo info = inspect_cpu();
    EXPECT_FALSE(info.model.empty());
    // Flags observed in environment inspection: nonstop_tsc + rdtscp present.
    EXPECT_TRUE(info.invariant_tsc);
    EXPECT_TRUE(info.rdtscp);
    EXPECT_FALSE(kernel_release().empty());
}

TEST(JitterMeter, SmokeRunProducesSaneCounts) {
    // Short smoke run; tail claims must NOT be made from this (see methodology
    // docs — full runs live in bench harnesses).
    const JitterReport rep = JitterMeter::measure(200, 50'000);
    EXPECT_EQ(rep.clock_read_overhead_ns.total_count(), 100'000u);
    EXPECT_EQ(rep.sleep_overshoot_ns.total_count(), 200u);
    // Clock read on this host is tens of ns; nothing above 10 µs (sanity band).
    EXPECT_LT(rep.clock_read_overhead_ns.value_at_percentile(99.0), 10'000);
    EXPECT_GE(rep.sleep_overshoot_ns.min_recorded(), 0);
}
