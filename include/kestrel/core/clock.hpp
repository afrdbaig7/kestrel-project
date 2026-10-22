#pragma once
// ============================================================================
// kestrel/core/clock.hpp
//
// Time sources.
//
//  * TscClock  — ordered invariant-TSC clock for x86-64 (rdtscp). Monotonic on
//    a pinned core. Ticks are the hot-path unit; conversion to nanoseconds
//    happens OFF the hot path via an off-path calibration factor.
//  * SteadyClockSource — portable fallback (CLOCK_MONOTONIC via std::chrono).
//
// Determinism rule (ADR-0006): live clocks never feed deterministic decisions;
// they are measurement-only. Decision paths use event time from journaled data.
//
// Ordered reads: rdtscp waits for all prior instructions to complete locally,
// so a read taken after stage N's work reflects that work. rdtscp does NOT
// serialize later instructions — pairs of reads are used for spans.
//
// Calibration is off-path: it sleeps and multiplies windows, executed at init
// or from a maintenance thread. The conversion factor is stored in a relaxed
// atomic; hot paths only ever read TSC ticks.
//
// Core-drift caveat: on x86 the invariant TSC is synchronized across cores at
// reset, but hypervisors may skew cores. The engine pins its thread (documented
// assumption); calibration detects core migration and discards affected
// windows. jitter_meter counts core changes for observability.
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
  #include <x86intrin.h>
  #define KESTREL_X86_TSC 1
#endif

namespace kestrel::core {

class TscClock {
public:
    using ticks = std::int64_t;

    /// True when this CPU reports invariant TSC (constant + nonstop).
    static bool invariant_tsc_available() noexcept {
#if defined(KESTREL_X86_TSC)
        static const bool ok = [] {
            std::uint32_t a{}, b{}, c{}, d{};
            if (!__get_cpuid(0x80000000u, &a, &b, &c, &d)) return false;
            if (a < 0x80000007u) return false;
            if (!__get_cpuid(0x80000007u, &a, &b, &c, &d)) return false;
            return (d & (1u << 8)) != 0;   // Invariant TSC
        }();
        return ok;
#else
        return false;
#endif
    }

    /// Ordered TSC read (rdtscp). Returns raw ticks.
    static ticks now() noexcept {
#if defined(KESTREL_X86_TSC)
        std::uint32_t aux{};
        return static_cast<ticks>(__builtin_ia32_rdtscp(&aux));
#else
        return fallback_now_ns();
#endif
    }

    /// Unordered, cheaper read (rdtsc) for non-critical stamps.
    static ticks now_unordered() noexcept {
#if defined(KESTREL_X86_TSC)
        return static_cast<ticks>(__builtin_ia32_rdtsc());
#else
        return fallback_now_ns();
#endif
    }

    /// Calibrate ticks<->ns. Blocking (sleeps ~windows*window_ms). Off-path.
    /// Call from the same core that will measure, or pin first.
    static void calibrate(unsigned windows = 8, unsigned window_ms = 25) noexcept {
        double best = 0.0;
        unsigned valid = 0;
        double sum = 0.0;
        for (unsigned w = 0; w < windows; ++w) {
#if defined(KESTREL_X86_TSC)
            const int cpu_before = current_cpu_hint();
            const ticks t0 = now();
            const auto s0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
            const auto s1 = std::chrono::steady_clock::now();
            const ticks t1 = now();
            const int cpu_after = current_cpu_hint();
            if (cpu_before != cpu_after) continue;   // migrated; discard window
            const double ns = std::chrono::duration<double, std::nano>(s1 - s0).count();
            const double d = static_cast<double>(t1 - t0);
            if (d <= 0 || ns <= 0) continue;
            const double tps = d / ns;               // ticks per ns
            sum += tps;
            ++valid;
            if (valid == 1 || tps > best * 1.0000001) { /* keep max: fastest window has least noise */ }
            best = valid == 1 ? tps : (tps > best ? tps : best);
#endif
        }
        if (valid > 0) {
            // Prefer the max observed ticks-per-ns (least-interrupted window).
            set_ticks_per_ns(best);
        }
    }

    /// ticks per nanosecond; 0 before first successful calibration.
    static double ticks_per_ns() noexcept {
        return ticks_per_ns_.load(std::memory_order_relaxed);
    }
    /// nanoseconds per tick; 0 before first calibration.
    static double ns_per_tick() noexcept {
        const double t = ticks_per_ns_.load(std::memory_order_relaxed);
        return t > 0 ? 1.0 / t : 0.0;
    }
    static void set_ticks_per_ns(double v) noexcept {
        if (v > 0) ticks_per_ns_.store(v, std::memory_order_relaxed);
    }

    /// Convert ticks to ns (off-path; returns -1 if uncalibrated).
    static std::int64_t ticks_to_ns(ticks t) noexcept {
        const double tps = ticks_per_ns_.load(std::memory_order_relaxed);
        if (tps <= 0) return -1;
        return static_cast<std::int64_t>(static_cast<double>(t) / tps);
    }

    /// Elapsed ns between two tick stamps (off-path).
    static std::int64_t span_ns(ticks t0, ticks t1) noexcept {
        return ticks_to_ns(t1 - t0);
    }

    static ticks fallback_now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static int current_cpu_hint() noexcept {
#if defined(__linux__)
        return static_cast<int>(::sched_getcpu());
#else
        return -1;
#endif
    }

    /// Source identifier for metadata reports.
    static const char* source_name() noexcept {
#if defined(KESTREL_X86_TSC)
        return invariant_tsc_available() ? "tsc_rdtscp" : "steady_clock";
#else
        return "steady_clock";
#endif
    }

private:
    static inline std::atomic<double> ticks_per_ns_{0.0};
};

/// Portable monotonic nanosecond source (fallback + measurement anchor).
class SteadyClockSource {
public:
    static std::int64_t now_ns() noexcept { return TscClock::fallback_now_ns(); }
};

#if defined(__linux__)
extern "C" int sched_getcpu(void) noexcept;
#endif

}  // namespace kestrel::core
