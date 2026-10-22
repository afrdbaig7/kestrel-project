#pragma once
// ============================================================================
// kestrel/core/jitter_meter.hpp
//
// Jitter and timer-overhead measurement baseline.
//
// What it measures (all off the trading hot path — this IS a measurement tool):
//   1. clock_read_overhead_ns — cost of one ordered TSC read pair.
//   2. sleep_overshoot_ns     — wake-up latency beyond the requested absolute
//      deadline, using clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME).
//   3. spin_overshoot_ns      — busy-wait until a TSC deadline (no sleep);
//      shows scheduling/execution granularity without kernel sleep.
//   4. core_changes           — count of detected CPU migrations during the run
//      (rdtscp aux / sched_getcpu), i.e. pinning quality of the host.
//
// The report is a Histogram (HdrHistogram algorithm) so distributions — not
// just means — are visible.
// ============================================================================

#include <cstdint>
#include <ctime>

#include "kestrel/core/clock.hpp"
#include "kestrel/core/histogram.hpp"

namespace kestrel::core {

struct JitterReport {
    Histogram clock_read_overhead_ns{1'000'000, 3};   // 0..1 ms range
    Histogram sleep_overshoot_ns{1'000'000'000, 3};   // 0..1 s range
    Histogram spin_overshoot_ns{1'000'000'000, 3};
    std::uint64_t sleep_iterations = 0;
    std::uint64_t core_changes = 0;
    int start_cpu = -1;
    int end_cpu = -1;
};

class JitterMeter {
public:
    /// Cost of reading the ordered clock: pairs of now() minus a calibrated
    /// empty-loop baseline is not needed — we measure the minimal span of two
    /// back-to-back reads, whose lower bound IS the read cost.
    static JitterReport measure(std::uint64_t sleep_iterations = 2000,
                                std::int64_t interval_ns = 100'000) {
        JitterReport rep;
        rep.start_cpu = TscClock::current_cpu_hint();

        // 1. Clock read overhead (quick pass first to warm caches).
        for (std::uint64_t i = 0; i < 100'000; ++i) {
            const auto t0 = TscClock::now();
            const auto t1 = TscClock::now();
            rep.clock_read_overhead_ns.record(t1 - t0);
        }

        // 2. Sleep overshoot with absolute-deadline clock_nanosleep.
        std::int64_t prev_cpu_seq = cpu_seq();
        for (std::uint64_t i = 0; i < sleep_iterations; ++i) {
            const auto deadline = make_deadline(interval_ns);
            clock_nanosleep_abs(deadline);
            const auto now_ns = SteadyClockSource::now_ns();
            const std::int64_t overshoot = now_ns - deadline;
            rep.sleep_overshoot_ns.record(overshoot > 0 ? overshoot : 0);
            const std::int64_t seq = cpu_seq();
            if (seq != prev_cpu_seq) { ++rep.core_changes; prev_cpu_seq = seq; }
        }
        rep.sleep_iterations = sleep_iterations;

        // 3. Spin until deadline (no sleep) — execution granularity only.
        for (std::uint64_t i = 0; i < 100'000; ++i) {
            const auto dl = SteadyClockSource::now_ns() + interval_ns / 10;
            std::int64_t spins = 0;
            while (SteadyClockSource::now_ns() < dl) ++spins;
            rep.spin_overshoot_ns.record(SteadyClockSource::now_ns() - dl);
        }

        rep.end_cpu = TscClock::current_cpu_hint();
        return rep;
    }

private:
    static std::int64_t make_deadline(std::int64_t horizon_ns) noexcept {
        struct timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec + horizon_ns;
    }
    static void clock_nanosleep_abs(std::int64_t deadline_ns) noexcept {
        struct timespec ts{};
        ts.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000LL);
        ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000LL);
        while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }
    /// sched_getcpu as a stable sequence (changes => migration happened).
    static std::int64_t cpu_seq() noexcept {
        return TscClock::current_cpu_hint();
    }
};

}  // namespace kestrel::core
