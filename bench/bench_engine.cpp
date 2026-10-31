// ============================================================================
// kestrel/bench/bench_engine.cpp
//
// Engine-level latency/throughput harnesses (Milestone D2). Custom main —
// NOT Google Benchmark — because these measurements need multi-second runs,
// percentile reporting over raw distributions, and raw-sample export, which
// the micro framework does not model well. bench_micro.cpp keeps the
// primitive microbenchmarks.
//
// Methodology contract (docs/benchmarks/report.md carries the same text):
//
//   * Timing regions, exactly:
//     - tick-to-trade (software): TSC read immediately before
//       eng->run_once(1) — the frame is ALREADY resident in the backend RX
//       arena (push cost models the NIC/DMA and is excluded) — through the
//       TSC read immediately after run_once returns. Included: poll_rx,
//       journal write (when attached), decode, book update, strategy,
//       pre-trade risk, order encode, tx_submit (= copy into TX arena +
//       enqueue; replay backend semantics — NOT a NIC doorbell), release_rx.
//       Excluded: frame push, venue/sim work, host TX drain, journal drain.
//     - Only events that produced >= 1 submitted order are tick-to-trade
//       samples; MD-driven and EXEC-driven samples are histogrammed
//       separately. Idle loop cost is NOT part of tick-to-trade.
//     - book throughput: decode_header + book apply only (no strategy,
//       risk, encode, journal, engine loop), fresh book per pass.
//     - timer overhead / jitter: TSC read-pair cost, steady_clock read-pair
//       cost, absolute-deadline clock_nanosleep wakeup overshoot (100us and
//       1ms intervals). Interpretation: "timer overhead" = wakeup overshoot
//       + clock read cost; documented here because the V1 engine has no OS
//       timers on its decision path (event-time only).
//
//   * Open-loop arrivals: the venue simulator generates frames on its own
//     event-time schedule; the engine is measured per event regardless of
//     arrival pacing. RX backlog is asserted ~0 so no queueing pollutes the
//     region (queueing would make this a closed-loop/soaked number).
//
//   * Workload comparability: the three tick-to-trade configurations share
//     the same seed and the same deterministic engine decisions (journal
//     attach and stage stamps do not alter decisions — verified by tests).
//     The harness cross-checks workload counters across runs and records
//     "workload_match" in the JSON output.
//
//   * Raw distributions win: every sample set is exported as CSV; the JSON
//     carries HdrHistogram percentiles. No warmup samples are discarded
//     silently — warmup happens BEFORE the measured loop and is counted in
//     the workload but not in the histograms.
//
//   * This sandbox is a shared 2-vCPU KVM guest without isolcpus or CPU
//     governor control: numbers are indicative only, not acceptance-grade.
//     scripts/run_bench.py records full host metadata alongside.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/prctl.h>

#include "host.hpp"   // integration host: same configs as correctness tests

#include "kestrel/core/cpu.hpp"
#include "kestrel/core/jitter_meter.hpp"

using namespace kestrel;
using kestrel::test::Backend;
using kestrel::test::HostParams;
using kestrel::test::TestHost;
using Clock = core::TscClock;

namespace {

// ---------------------------------------------------------------------------
// CLI + config
// ---------------------------------------------------------------------------
struct BenchConfig {
    bool quick = false;
    std::string out_dir = "bench/results/current";
    std::string label = "unlabeled";
    std::uint64_t steps = 60'000;       // event-time steps of 250us each
    std::uint64_t warmup_steps = 2'000;
    std::size_t throughput_frames = 400'000;
    unsigned throughput_passes = 5;
};

BenchConfig parse_args(int argc, char** argv) {
    BenchConfig c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--quick") {
            c.quick = true;
            c.steps = 8'000;
            c.warmup_steps = 500;
            c.throughput_frames = 80'000;
            c.throughput_passes = 3;
        } else if (a == "--out") {
            c.out_dir = next();
        } else if (a == "--label") {
            c.label = next();
        } else if (a == "--steps") {
            c.steps = std::strtoull(next().c_str(), nullptr, 10);
        } else if (a == "--warmup") {
            c.warmup_steps = std::strtoull(next().c_str(), nullptr, 10);
        } else if (a == "--frames") {
            c.throughput_frames = std::strtoull(next().c_str(), nullptr, 10);
        } else if (a == "--passes") {
            c.throughput_passes = std::strtoul(next().c_str(), nullptr, 10);
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// Minimal JSON writer (values are numeric/bool/string; no escaping needed for
// the fixed key set used here).
// ---------------------------------------------------------------------------
struct Json {
    // Object mode: key() handles element separation. Array mode (in_arr):
    // val() handles it. Only flat arrays are used (no nesting inside arrays).
    std::string s = "{";
    bool first = true;
    bool in_arr = false;
    void sep_arr() {
        if (in_arr && !first) s += ",";
        first = false;
    }
    void key(const char* k) {
        if (!first) s += ",";
        first = false;
        s += "\"";
        s += k;
        s += "\":";
    }
    void val(std::int64_t v) { sep_arr(); s += std::to_string(v); }
    void val(std::uint64_t v) { sep_arr(); s += std::to_string(v); }
    void val(double v) {
        sep_arr();
        char b[32];
        std::snprintf(b, sizeof b, "%.6f", v);
        s += b;
    }
    void val(bool v) { sep_arr(); s += v ? "true" : "false"; }
    void val(const char* v) { sep_arr(); s += "\""; s += v; s += "\""; }
    void val(const std::string& v) { sep_arr(); s += "\""; s += v; s += "\""; }
    void open_obj(const char* k) { key(k); s += "{"; first = true; }
    void open_arr(const char* k) { key(k); s += "["; first = true; in_arr = true; }
    void close_obj() { s += "}"; first = false; }
    void close_arr() { s += "]"; first = false; in_arr = false; }
    void kv(const char* k, auto v) { key(k); val(v); }
    std::string str() const { return s + "}"; }
};

struct StatsOut {
    std::int64_t count = 0;
    std::int64_t min = 0;
    std::int64_t mean = 0;       // rounded
    std::int64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    std::int64_t overflow = 0;

    static StatsOut from(const core::Histogram& h) {
        StatsOut o;
        o.count = h.total_count();
        if (!o.count) return o;
        o.min = h.min_recorded();
        o.mean = static_cast<std::int64_t>(h.mean() + 0.5);
        o.p50 = h.value_at_percentile(50.0);
        o.p90 = h.value_at_percentile(90.0);
        o.p99 = h.value_at_percentile(99.0);
        o.p999 = h.value_at_percentile(99.9);
        o.p9999 = h.value_at_percentile(99.99);
        o.max = h.max_recorded();
        o.overflow = h.overflow_count();
        return o;
    }
    void json(Json& j, const char* k) const {
        j.open_obj(k);
        j.kv("count", count);
        j.kv("min_ns", min);
        j.kv("mean_ns", mean);
        j.kv("p50_ns", p50);
        j.kv("p90_ns", p90);
        j.kv("p99_ns", p99);
        j.kv("p999_ns", p999);
        j.kv("p9999_ns", p9999);
        j.kv("max_ns", max);
        j.kv("overflow", overflow);
        j.close_obj();
    }
};

void write_text(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "bench: cannot write %s\n", p.c_str());
        return;
    }
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void export_csv(const std::filesystem::path& p, const std::string& col,
                const std::vector<std::int64_t>& samples) {
    std::string s = col + "\n";
    s.reserve(samples.size() * 8 + 16);
    for (const std::int64_t v : samples) s += std::to_string(v), s += "\n";
    write_text(p, s);
}

void print_stats(const char* name, const StatsOut& st) {
    std::printf("  %-28s n=%-8lld p50=%-7lld p90=%-7lld p99=%-7lld p99.9=%-7lld "
                "p99.99=%-7lld max=%-7lld\n",
                name, static_cast<long long>(st.count), static_cast<long long>(st.p50),
                static_cast<long long>(st.p90), static_cast<long long>(st.p99),
                static_cast<long long>(st.p999), static_cast<long long>(st.p9999),
                static_cast<long long>(st.max));
}

// ---------------------------------------------------------------------------
// Bench workload configuration.
//
// The DEFAULT test venue (test::HostParams) accumulates resting orders
// (add_prob 0.60 vs cancel_prob 0.25): the live-order count grows without
// bound, and after ~25k background steps the engine's bounded book pool
// (kMaxBookOrders=8192) exhausts. The engine's response — feed anomaly ->
// Seeking until snapshot — is correct spec behavior, but it freezes quoting,
// so it is the WRONG workload for latency measurement. The bench venue keeps
// the book in steady state (balanced add/cancel churn, light two-sided
// taker flow) and widens the price range so the unbiased mid walk stays far
// from the boundary within the measurement window.
// ---------------------------------------------------------------------------
sim::VenueConfig make_bench_venue() {
    sim::VenueConfig v{};
    v.egress_latency_ns = 5'000;
    v.md_publish_latency_ns = 2'000;
    v.instrument_count = 1;
    sim::VenueInstrumentCfg& ic = v.instruments[0];
    ic.instrument = 0;
    ic.min_price = 85'000;
    ic.max_price = 115'000;          // 30k ticks < kMaxBookLevels (32768)
    ic.initial_mid = 100'000;
    ic.initial_spread_ticks = 4;
    ic.initial_levels = 8;
    ic.initial_level_qty = 50;
    ic.background_step_ns = 250'000;
    // Cancel-heavy: the background cancel branch is the residual probability
    // (add 0.50, taker 0.50*0.10, cancel the rest) which gives a slightly
    // NEGATIVE live-order drift. With the empty-side re-seed the book then
    // oscillates in a bounded band instead of growing into the engine's
    // bounded book pool (kMaxBookOrders=8192) on long streams.
    ic.background_add_prob = 0.50;
    ic.background_cancel_prob = 0.75;
    ic.background_qty = 25;
    ic.background_taker_prob = 0.10;
    return v;
}

/// Order-dense strategy: requote on any 1-tick move (the test config's
/// requote_ticks=3 barely triggers on this venue — 7 orders in 8k events).
engine::EngineConfig<test::kInstr> make_bench_engine_config() {
    engine::EngineConfig<test::kInstr> c = test::TestHost::make_engine_config();
    c.book_min_price[0] = 85'000;
    c.risk[0].price_min = 85'000;
    c.risk[0].price_max = 115'000;
    c.strat[0].requote_ticks = 1;
    c.strat[0].min_spread_ticks = 1;
    c.strat[0].price_min = 86'000;
    c.strat[0].price_max = 114'000;
    return c;
}

// ---------------------------------------------------------------------------
// Workload counters (cross-run comparability check)
// ---------------------------------------------------------------------------
struct WorkloadCounters {
    std::uint64_t frames_md = 0, frames_exec = 0, tx = 0, intents = 0;
    bool operator==(const WorkloadCounters& o) const {
        return frames_md == o.frames_md && frames_exec == o.frames_exec &&
               tx == o.tx && intents == o.intents;
    }
};

// ---------------------------------------------------------------------------
// Tick-to-trade driver. One engine instance per configuration; identical
// seeding per configuration (determinism gives identical frame streams).
//
// Per event (open loop, one frame per run_once):
//   [untimed] sim generates on its event-time schedule; frame pushed to RX
//   [timed]   t0 -> run_once(1) -> t1
//   [untimed] if the event submitted orders -> sample (t1-t0) split by kind
//   [untimed] TX drain -> venue; journal drain -> counter
// ---------------------------------------------------------------------------
struct T2TResult {
    core::Histogram md{1'000'000'000, 3};
    core::Histogram exec{1'000'000'000, 3};
    std::vector<std::int64_t> md_samples_ns;
    std::vector<std::int64_t> exec_samples_ns;
    WorkloadCounters wc;
    std::uint64_t md_invalid = 0;
    std::uint64_t intents_rejected = 0;
    std::uint64_t rx_backlog_max = 0;
    std::uint64_t events = 0;
    // SNAPSHOTS of the engine's internal stage histograms (taken while the
    // engine is alive — raw pointers would dangle after the engine scope).
    bool has_stage = false;
    StatsOut stage_t2t, stage_journal, stage_processing;
};

template <typename EngineT>
void run_tick_to_trade(TestHost& h, EngineT& eng, const BenchConfig& cfg,
                       bool attach_journal, T2TResult& out) {
    out.md_samples_ns.reserve(static_cast<std::size_t>(cfg.steps) / 2 + 16);
    out.exec_samples_ns.reserve(static_cast<std::size_t>(cfg.steps) / 8 + 16);

    if (attach_journal) eng.attach_journal(h.jw.get());
    const auto drain_journal = [&] {
        journal::Record r;
        while (h.jw->pop_critical(r)) {}
        while (h.jw->pop_telemetry(r)) {}
    };

    // Bench venue: replace the host's default venue (see make_bench_venue).
    // Same event-time pacing as the integration tests (250 us steps).
    h.vcfg = make_bench_venue();
    h.sim = std::make_unique<sim::ExchangeSim>(h.vcfg);

    // Warmup (untimed, not histogrammed; establishes steady state).
    {
        std::vector<sim::OutFrame> frames;
        std::uint64_t now = 0;
        for (std::uint64_t i = 0; i < cfg.warmup_steps; ++i) {
            now += 250'000;
            h.sim->run_background(now);
            h.sim->pump(now, frames);
            for (const sim::OutFrame& f : frames) {
                h.backend->push_rx(f.kind == sim::OutputKind::MarketData
                                       ? net::FrameKind::MarketData
                                       : net::FrameKind::ExecReport,
                                   f.ts_ns, f.payload);
                eng.run_once(1);
                std::uint8_t txbuf[4608];
                std::size_t len = 0;
                while (h.backend->pop_tx(txbuf, len))
                    h.sim->on_request({txbuf, len}, now);
            }
            frames.clear();
            drain_journal();
        }
    }

    // Measured loop.
    std::vector<sim::OutFrame> frames;
    std::uint64_t now = 0;
    for (std::uint64_t i = 0; i < cfg.steps; ++i) {
        now += 250'000;
        h.sim->run_background(now);
        h.sim->pump(now, frames);
        for (const sim::OutFrame& f : frames) {
            const net::FrameKind kind = f.kind == sim::OutputKind::MarketData
                                            ? net::FrameKind::MarketData
                                            : net::FrameKind::ExecReport;
            if (!h.backend->push_rx(kind, f.ts_ns, f.payload)) {
                continue;   // arena full: counted via backlog below
            }
            const std::uint64_t tx_before = eng.stats().tx_enqueued;
            const Clock::ticks t0 = Clock::now();
            eng.run_once(1);
            const Clock::ticks t1 = Clock::now();
            const std::uint64_t tx_after = eng.stats().tx_enqueued;
            ++out.events;

            if (tx_after > tx_before) {
                const std::int64_t ns = Clock::ticks_to_ns(t1 - t0);
                if (kind == net::FrameKind::MarketData) {
                    out.md.record(ns);
                    out.md_samples_ns.push_back(ns);
                } else {
                    out.exec.record(ns);
                    out.exec_samples_ns.push_back(ns);
                }
            }
            // Untimed: drain engine TX into the venue (completion loop).
            std::uint8_t txbuf[4608];
            std::size_t len = 0;
            while (h.backend->pop_tx(txbuf, len))
                h.sim->on_request({txbuf, len}, now);
            // Untimed: IO-thread emulation for the journal rings.
            drain_journal();
        }
        frames.clear();
        const std::size_t bl = h.backend->rx_backlog();
        if (bl > out.rx_backlog_max) out.rx_backlog_max = bl;
    }

    out.wc = {eng.stats().frames_md, eng.stats().frames_exec,
              eng.stats().tx_enqueued, eng.stats().intents_generated};
    out.md_invalid = eng.stats().frames_md_invalid;
    out.intents_rejected = eng.stats().intents_rejected;
    if (const core::Histogram* t = eng.stage_tick_to_trade()) {
        out.has_stage = true;
        out.stage_t2t = StatsOut::from(*t);
        out.stage_journal = StatsOut::from(*eng.stage_journal());
        out.stage_processing = StatsOut::from(*eng.stage_processing());
    }
}

// ---------------------------------------------------------------------------
// Book throughput: feed+book ceiling. Fresh FeedHandler per pass; identical
// stream; region = decode + apply only.
// ---------------------------------------------------------------------------
struct ThroughputResult {
    std::vector<double> ns_per_frame;
    std::vector<double> mfps;             // million frames per second
    std::size_t frames = 0;
    std::uint64_t stream_bytes = 0;
    bool stream_clean = false;            // full accounting closes, no anomalies
    std::uint64_t applied = 0, duplicates = 0, gaps = 0, dropped_while_seeking = 0;
    std::uint64_t rejects_decode = 0, rejects_anomaly = 0, pre_start_skipped = 0;
    double median_ns_per_frame = 0.0;
    double best_mfps = 0.0;
};

ThroughputResult run_book_throughput(const BenchConfig& cfg) {
    // Generate a valid MD stream with the BENCH venue (balanced churn — the
    // default test venue would exhaust the bounded book pool mid-stream).
    sim::VenueConfig v = make_bench_venue();
    sim::ExchangeSim gen(v);
    std::vector<std::uint8_t> pool;
    struct Off { std::uint32_t off; std::uint16_t len; };
    std::vector<Off> offs;
    std::vector<sim::OutFrame> frames;
    std::uint64_t now = 0;
    std::uint64_t stall = 0;
    while (offs.size() < cfg.throughput_frames && stall < 4'000'000) {
        now += 250'000;
        gen.run_background(now);
        gen.pump(now, frames);
        bool any = false;
        for (const sim::OutFrame& f : frames) {
            if (f.kind != sim::OutputKind::MarketData) continue;
            Off o;
            o.off = static_cast<std::uint32_t>(pool.size());
            o.len = static_cast<std::uint16_t>(f.payload.size());
            pool.insert(pool.end(), f.payload.begin(), f.payload.end());
            offs.push_back(o);
            any = true;
        }
        frames.clear();
        stall = any ? 0 : stall + 1;
    }

    ThroughputResult r;
    r.frames = offs.size();
    r.stream_bytes = pool.size();
    const std::int64_t min_price = v.instruments[0].min_price;

    for (unsigned pass = 0; pass < cfg.throughput_passes; ++pass) {
        // Heap-allocated: the book is ~2 MB of preallocated levels — too big
        // for sanitizer-inflated stack frames. Allocation happens BEFORE the
        // timed region (init-time alloc, same policy as the test hosts).
        auto feed = std::make_unique<md::FeedHandler<engine::kMaxBookLevels,
                                                     engine::kMaxBookOrders>>(min_price);
        const Clock::ticks t0 = Clock::now();
        for (const Off o : offs)
            feed->on_frame({pool.data() + o.off, o.len}, true);
        const Clock::ticks t1 = Clock::now();
        const double ns = static_cast<double>(Clock::ticks_to_ns(t1 - t0));
        if (pass == 0) {
            const auto& s = feed->stats();
            r.applied = s.applied;
            r.duplicates = s.duplicates;
            r.gaps = s.gaps;
            r.dropped_while_seeking = s.dropped_while_seeking;
            r.rejects_decode = s.rejects_decode;
            r.rejects_anomaly = s.rejects_anomaly;
            r.pre_start_skipped = s.pre_start_skipped;
            r.stream_clean = (s.applied + s.duplicates + s.gaps +
                              s.dropped_while_seeking + s.rejects_decode +
                              s.rejects_anomaly + s.pre_start_skipped) ==
                                 offs.size() &&
                             s.gaps == 0 && s.duplicates == 0 &&
                             s.rejects_decode == 0 && s.rejects_anomaly == 0 &&
                             s.dropped_while_seeking == 0 && s.pre_start_skipped == 0;
        }
        r.ns_per_frame.push_back(ns / static_cast<double>(offs.size()));
        // frames/ns = Gframes/s; x1000 = Mframes/s.
        r.mfps.push_back(1000.0 * static_cast<double>(offs.size()) / ns);
    }
    r.ns_per_frame.erase(r.ns_per_frame.begin());   // first pass = cache warm
    r.mfps.erase(r.mfps.begin());
    if (!r.ns_per_frame.empty()) {
        std::vector<double> sorted = r.ns_per_frame;
        std::sort(sorted.begin(), sorted.end());
        r.median_ns_per_frame = sorted[sorted.size() / 2];
        r.best_mfps = *std::max_element(r.mfps.begin(), r.mfps.end());
    }
    return r;
}

// ---------------------------------------------------------------------------
// Timer overhead + jitter baseline
// ---------------------------------------------------------------------------
struct JitterOut {
    core::JitterReport jitter_100us;
    core::JitterReport jitter_1ms;
    core::JitterReport jitter_100us_tight;   // PR_SET_TIMERSLACK=1
    double steady_now_pair_ns = 0.0;
    double rdtsc_pair_ticks = 0.0;
};

JitterOut run_timer_overhead() {
    JitterOut o;
    o.jitter_100us = core::JitterMeter::measure(2000, 100'000);
    o.jitter_1ms = core::JitterMeter::measure(500, 1'000'000);

    // Tight timer slack: Linux adds up to 50us default slack to nanosleep
    // (p50 overshoot ~= 50us confirms it on this host). Real HFT processes
    // set PR_SET_TIMERSLACK to 1; measure the same loop under that regime
    // and restore the previous value afterwards.
    const unsigned long prev_slack =
        static_cast<unsigned long>(prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL));
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    o.jitter_100us_tight = core::JitterMeter::measure(2000, 100'000);
    prctl(PR_SET_TIMERSLACK, prev_slack, 0UL, 0UL, 0UL);

    // steady_clock::now() pair cost (mean over 20k pairs).
    {
        std::uint64_t acc = 0;
        constexpr int kIters = 20'000;
        for (int i = 0; i < kIters; ++i) {
            const auto a = std::chrono::steady_clock::now();
            const auto b = std::chrono::steady_clock::now();
            acc += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        o.steady_now_pair_ns = static_cast<double>(acc) / kIters;
    }
    // Unordered rdtsc pair (ticks).
    {
        std::uint64_t acc = 0;
        constexpr int kIters = 100'000;
        for (int i = 0; i < kIters; ++i) {
            const auto a = Clock::now_unordered();
            const auto b = Clock::now_unordered();
            acc += static_cast<std::uint64_t>(b - a);
        }
        o.rdtsc_pair_ticks = static_cast<double>(acc) / kIters;
    }
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    const BenchConfig cfg = parse_args(argc, argv);

    // Pin to one allowed CPU (VM: no isolation; still removes most migration
    // noise). Record the outcome.
    const std::vector<int> cpus = core::allowed_cpus();
    const bool pinned = !cpus.empty() && core::pin_to_cpu(cpus[0]);
    Clock::calibrate();

    std::printf("kestrel_bench_engine | label=%s %s | pinned=%s (cpu %d of %zu) | "
                "clock=%s | %.3f ticks/ns\n",
                cfg.label.c_str(), cfg.quick ? "(QUICK)" : "", pinned ? "yes" : "no",
                Clock::current_cpu_hint(), cpus.size(), Clock::source_name(),
                Clock::ticks_per_ns());

    std::filesystem::create_directories(cfg.out_dir);

    Json j;
    j.kv("schema", "kestrel.bench.engine/1");
    j.kv("label", cfg.label);
    j.kv("quick", cfg.quick);
    j.kv("steps", cfg.steps);
    j.kv("warmup_steps", cfg.warmup_steps);
    j.kv("pinned", pinned);
    j.kv("pinned_cpu", static_cast<std::int64_t>(Clock::current_cpu_hint()));
    j.kv("allowed_cpus", static_cast<std::int64_t>(cpus.size()));
    j.kv("clock_source", Clock::source_name());
    j.kv("ticks_per_ns", Clock::ticks_per_ns());
    // Determinism input: the venue simulator RNG is seeded from VenueFaults.seed
    // (HostParams.seed is not consumed by the host's make_venue).
    j.kv("seed", static_cast<std::int64_t>(sim::VenueFaults{}.seed));
    j.kv("engine_config", "TestHost::make_engine_config() (same as correctness tests)");

    // ---- 1. timer overhead + jitter ----------------------------------------
    std::printf("[1/3] timer overhead + jitter baseline...\n");
    const JitterOut jo = run_timer_overhead();
    const StatsOut sleep_100 = StatsOut::from(jo.jitter_100us.sleep_overshoot_ns);
    const StatsOut sleep_1ms = StatsOut::from(jo.jitter_1ms.sleep_overshoot_ns);
    const StatsOut sleep_tight = StatsOut::from(jo.jitter_100us_tight.sleep_overshoot_ns);
    const StatsOut read_ns = StatsOut::from(jo.jitter_100us.clock_read_overhead_ns);
    print_stats("tsc read pair (rdtscp)", read_ns);
    print_stats("sleep overshoot 100us", sleep_100);
    print_stats("sleep overshoot 100us (slack=1)", sleep_tight);
    print_stats("sleep overshoot 1ms", sleep_1ms);
    std::printf("  %-28s %.1f ns pair | rdtsc pair %.1f ticks\n", "steady_clock::now pair",
                jo.steady_now_pair_ns, jo.rdtsc_pair_ticks);
    std::printf("  %-28s start_cpu=%d end_cpu=%d changes(100us)=%llu\n", "core migrations",
                jo.jitter_100us.start_cpu, jo.jitter_100us.end_cpu,
                static_cast<unsigned long long>(jo.jitter_100us.core_changes));

    j.open_obj("timer");
    read_ns.json(j, "tsc_read_pair");
    sleep_100.json(j, "sleep_overshoot_100us");
    sleep_tight.json(j, "sleep_overshoot_100us_timerslack_1");
    sleep_1ms.json(j, "sleep_overshoot_1ms");
    j.kv("steady_now_pair_ns", jo.steady_now_pair_ns);
    j.kv("rdtsc_pair_ticks", jo.rdtsc_pair_ticks);
    j.kv("core_changes_100us", jo.jitter_100us.core_changes);
    j.kv("core_changes_1ms", jo.jitter_1ms.core_changes);
    j.close_obj();

    // ---- 2. tick-to-trade (three configurations) ----------------------------
    std::printf("[2/3] tick-to-trade (%llu event steps + %llu warmup)...\n",
                static_cast<unsigned long long>(cfg.steps),
                static_cast<unsigned long long>(cfg.warmup_steps));
    const engine::EngineConfig<test::kInstr> bench_ecfg = make_bench_engine_config();

    // (a) minimal, no journal — headline software feed-to-order number.
    T2TResult r_min;
    {
        TestHost h;
        engine::Engine<test::kInstr, Backend, test::MM> eng(*h.backend, bench_ecfg);
        run_tick_to_trade(h, eng, cfg, /*attach_journal=*/false, r_min);
    }
    // (b) minimal, journal attached — quantifies journal ring write cost.
    T2TResult r_jrn;
    {
        TestHost h;
        engine::Engine<test::kInstr, Backend, test::MM> eng(*h.backend, bench_ecfg);
        run_tick_to_trade(h, eng, cfg, /*attach_journal=*/true, r_jrn);
    }
    // (c) stage-timed, journal attached — observer overhead reference.
    using TimingEngine = engine::Engine<test::kInstr, Backend, test::MM, /*kStageTiming=*/true>;
    T2TResult r_stage;
    {
        TestHost h;
        TimingEngine teng(*h.backend, bench_ecfg);
        run_tick_to_trade(h, teng, cfg, /*attach_journal=*/true, r_stage);
    }

    const StatsOut s_min = StatsOut::from(r_min.md);
    const StatsOut s_min_exec = StatsOut::from(r_min.exec);
    const StatsOut s_jrn = StatsOut::from(r_jrn.md);
    const StatsOut s_stage_ext = StatsOut::from(r_stage.md);
    const StatsOut& s_stage_int = r_stage.has_stage ? r_stage.stage_t2t : s_stage_ext;
    const StatsOut& s_journal_stage = r_stage.has_stage ? r_stage.stage_journal
                                                        : s_stage_ext;
    const StatsOut& s_proc_stage = r_stage.has_stage ? r_stage.stage_processing
                                                     : s_stage_ext;

    std::printf("  MD-driven (order-producing events only):\n");
    print_stats("t2t minimal, no journal", s_min);
    print_stats("t2t minimal, journal", s_jrn);
    print_stats("t2t stage-timed (ext t0/t1)", s_stage_ext);
    print_stats("t2t stage-timed (internal)", s_stage_int);
    print_stats("  journal stage", s_journal_stage);
    print_stats("  processing stage", s_proc_stage);
    print_stats("t2t exec-driven, minimal", s_min_exec);
    std::printf("  workload: md=%llu exec=%llu tx=%llu intents=%llu (rej=%llu, md_invalid=%llu)"
                " | rx_backlog_max=%llu\n",
                static_cast<unsigned long long>(r_min.wc.frames_md),
                static_cast<unsigned long long>(r_min.wc.frames_exec),
                static_cast<unsigned long long>(r_min.wc.tx),
                static_cast<unsigned long long>(r_min.wc.intents),
                static_cast<unsigned long long>(r_min.intents_rejected),
                static_cast<unsigned long long>(r_min.md_invalid),
                static_cast<unsigned long long>(r_min.rx_backlog_max));
    const bool match_ab = r_min.wc == r_jrn.wc;
    const bool match_ac = r_min.wc == r_stage.wc;
    std::printf("  workload match: minimal~journal=%s minimal~stage=%s\n",
                match_ab ? "OK" : "MISMATCH", match_ac ? "OK" : "MISMATCH");

    j.open_obj("tick_to_trade");
    j.kv("region", "t0 before run_once(1) (frame pre-pushed) -> t1 after return; "
                   "includes poll+journal+decode+book+strategy+risk+encode+tx_submit; "
                   "order-producing events only");
    j.kv("note_internal_histograms", "stage_* histograms cover ALL frames "
         "(MD+EXEC, incl. non-order-producing); external histograms cover "
         "order-producing MD events only");
    s_min.json(j, "md_minimal_no_journal");
    s_jrn.json(j, "md_minimal_journal");
    s_stage_ext.json(j, "md_stage_timed_external");
    s_stage_int.json(j, "md_stage_timed_internal_all_frames");
    s_journal_stage.json(j, "md_stage_journal_slice");
    s_proc_stage.json(j, "md_stage_processing_slice");
    s_min_exec.json(j, "exec_minimal_no_journal");
    j.kv("workload_match_minimal_vs_journal", match_ab);
    j.kv("workload_match_minimal_vs_stage", match_ac);
    j.kv("observer_overhead_ns_p50", s_stage_ext.p50 - s_min.p50);
    j.kv("journal_cost_ns_p50", s_jrn.p50 - s_min.p50);
    j.open_obj("workload");
    j.kv("frames_md", r_min.wc.frames_md);
    j.kv("frames_exec", r_min.wc.frames_exec);
    j.kv("tx_enqueued", r_min.wc.tx);
    j.kv("intents", r_min.wc.intents);
    j.kv("intents_rejected", r_min.intents_rejected);
    j.kv("frames_md_invalid", r_min.md_invalid);
    j.kv("rx_backlog_max", r_min.rx_backlog_max);
    j.kv("events", r_min.events);
    j.kv("venue", "balanced-churn bench venue (make_bench_venue): add=0.60 "
                  "cancel=0.60 taker=0.10 range 85k-115k; steady-state book");
    j.kv("strategy", "requote_ticks=1 min_spread_ticks=1 (order-dense variant of "
                     "the test config; delta documented)");
    j.close_obj();
    j.close_obj();

    export_csv(std::filesystem::path(cfg.out_dir) / "t2t_md_minimal.csv", "ns",
               r_min.md_samples_ns);
    export_csv(std::filesystem::path(cfg.out_dir) / "t2t_md_minimal_journal.csv", "ns",
               r_jrn.md_samples_ns);
    export_csv(std::filesystem::path(cfg.out_dir) / "t2t_md_stage_timed.csv", "ns",
               r_stage.md_samples_ns);
    export_csv(std::filesystem::path(cfg.out_dir) / "t2t_exec_minimal.csv", "ns",
               r_min.exec_samples_ns);

    // ---- 3. book throughput --------------------------------------------------
    std::printf("[3/3] book throughput (%zu frames/pass)...\n", cfg.throughput_frames);
    const ThroughputResult tp = run_book_throughput(cfg);
    for (std::size_t i = 0; i < tp.ns_per_frame.size(); ++i)
        std::printf("  pass %zu: %.1f ns/frame | %.2f Mframes/s\n", i + 2,
                    tp.ns_per_frame[i], tp.mfps[i]);  // pass numbering: pass 1 discarded (cache warm)
    std::printf("  median %.1f ns/frame | best %.2f Mframes/s | stream_clean=%s\n",
                tp.median_ns_per_frame, tp.best_mfps, tp.stream_clean ? "yes" : "NO");

    j.open_obj("book_throughput");
    j.kv("region", "decode_header + book apply; fresh FeedHandler per pass; "
                   "no strategy/risk/encode/journal/engine loop");
    j.kv("frames", tp.frames);
    j.kv("stream_bytes", tp.stream_bytes);
    j.kv("stream_clean", tp.stream_clean);
    j.kv("stream_applied", tp.applied);
    j.kv("stream_duplicates", tp.duplicates);
    j.kv("stream_gaps", tp.gaps);
    j.kv("stream_dropped_while_seeking", tp.dropped_while_seeking);
    j.kv("stream_rejects_decode", tp.rejects_decode);
    j.kv("stream_rejects_anomaly", tp.rejects_anomaly);
    j.kv("stream_pre_start_skipped", tp.pre_start_skipped);
    j.kv("median_ns_per_frame", tp.median_ns_per_frame);
    j.kv("best_mframes_per_sec", tp.best_mfps);
    j.open_arr("pass_ns_per_frame");
    for (const double v : tp.ns_per_frame) j.val(v);
    j.close_arr();
    j.close_obj();

    const std::string out = j.str();
    write_text(std::filesystem::path(cfg.out_dir) / "results.json", out + "\n");
    std::printf("results written to %s/results.json (+ raw CSV samples)\n",
                cfg.out_dir.c_str());
    return 0;
}
