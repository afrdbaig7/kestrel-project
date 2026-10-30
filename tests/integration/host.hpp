// Integration test host: wires venue simulator -> replay backend -> engine
// and drives a deterministic event-time loop. Shared by vertical-slice,
// golden-replay, and fault-scenario tests.
//
// NOTE: the heavy members (journal rings ~17 MB, backend arena ~5 MB, engine
// books ~1 MB) are heap-allocated via unique_ptr — init-time allocation is
// allowed for host/test infrastructure and keeps test stacks small. The
// ENGINE's own no-alloc guarantee applies to its event path, not to hosts.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "kestrel/backends/replay/replay_backend.hpp"
#include "kestrel/engine/engine.hpp"
#include "kestrel/journal/journal.hpp"
#include "kestrel/sim/exchange_sim.hpp"
#include "kestrel/strat/market_maker.hpp"

namespace kestrel::test {

inline constexpr std::size_t kInstr = 1;
using Backend = backends::ReplayBackend<512, 4608, 512>;
using MM = strat::MarketMakerStrategy<kInstr>;
using TestEngine = engine::Engine<kInstr, Backend, MM>;

struct HostParams {
    std::uint64_t seed = 42;
    std::int64_t mid = 100'000;
    double taker_prob = 0.20;
    sim::VenueFaults faults{};
};

/// A journaled input frame (MD/EXEC) with its event timestamp.
struct ReplayInput {
    net::FrameKind kind;
    std::uint64_t ts;
    std::vector<std::uint8_t> bytes;
};
using CapturedInputs = std::vector<ReplayInput>;

struct TestHost {
    std::unique_ptr<Backend> backend;
    sim::VenueConfig vcfg{};
    std::unique_ptr<sim::ExchangeSim> sim;
    engine::EngineConfig<kInstr> ecfg{};
    std::unique_ptr<TestEngine> eng;
    std::unique_ptr<journal::JournalWriter<>> jw;

    std::uint64_t now = 0;
    std::uint64_t steps = 0;
    std::vector<sim::OutFrame> out;
    std::uint64_t input_drops = 0;
    std::uint64_t tx_to_venue = 0;

    explicit TestHost(const HostParams& p = HostParams{})
        : backend(std::make_unique<Backend>()),
          vcfg(make_venue(p)),
          sim(std::make_unique<sim::ExchangeSim>(vcfg)),
          ecfg(make_engine_config()),
          eng(std::make_unique<TestEngine>(*backend, ecfg)),
          jw(std::make_unique<journal::JournalWriter<>>()) {}

    static sim::VenueConfig make_venue(const HostParams& p) {
        sim::VenueConfig v{};
        v.egress_latency_ns = 5'000;
        v.md_publish_latency_ns = 2'000;
        v.faults = p.faults;
        v.instrument_count = 1;
        sim::VenueInstrumentCfg& ic = v.instruments[0];
        ic.instrument = 0;
        ic.min_price = 90'000;
        ic.max_price = 110'000;
        ic.initial_mid = p.mid;
        ic.initial_spread_ticks = 4;
        ic.initial_levels = 8;
        ic.initial_level_qty = 50;
        ic.background_step_ns = 250'000;
        ic.background_add_prob = 0.60;
        ic.background_cancel_prob = 0.25;
        ic.background_qty = 25;
        ic.background_taker_prob = p.taker_prob;
        return v;
    }

    static engine::EngineConfig<kInstr> make_engine_config() {
        engine::EngineConfig<kInstr> c{};
        c.book_min_price[0] = 90'000;
        risk::RiskConfig& rc = c.risk[0];
        rc.price_min = 90'000;
        rc.price_max = 110'000;
        rc.max_order_qty = 100;
        rc.max_order_notional = 100 * 110'000;
        rc.max_position = 200;
        rc.max_pending_notional = 200 * 110'000;
        rc.max_pending_lots = 150;
        rc.rate.capacity = 10'000;
        rc.rate.refill_per_sec = 10'000;
        rc.max_orders_per_bucket = 1;
        strat::MarketMakerParams& sp = c.strat[0];
        sp.quote_qty = 10;
        sp.min_spread_ticks = 2;
        sp.requote_ticks = 3;
        sp.position_cap = 200;
        sp.price_min = 91'000;
        sp.price_max = 109'000;
        c.oms_capacity = 256;
        return c;
    }

    void enable_journal() { eng->attach_journal(jw.get()); }

    /// One deterministic event-time step: venue flow -> engine -> venue.
    void step(std::uint64_t dt_ns = 250'000) {
        now += dt_ns;
        ++steps;
        sim->run_background(now);
        sim->pump(now, out);
        for (const sim::OutFrame& f : out) {
            const net::FrameKind k = f.kind == sim::OutputKind::MarketData
                                         ? net::FrameKind::MarketData
                                         : net::FrameKind::ExecReport;
            if (!backend->push_rx(k, f.ts_ns, f.payload)) ++input_drops;
        }
        out.clear();
        eng->run_once();
        std::uint8_t txbuf[4608];
        std::size_t len = 0;
        while (backend->pop_tx(txbuf, len)) {
            sim->on_request(std::span<const std::uint8_t>(txbuf, len), now);
            ++tx_to_venue;
        }
    }

    /// Run N steps.
    void run(std::uint64_t n, std::uint64_t dt_ns = 250'000) {
        for (std::uint64_t i = 0; i < n; ++i) step(dt_ns);
    }
};

/// Drain a journal writer into a file (IO-thread emulation).
inline std::size_t drain_journal(journal::JournalWriter<>& w,
                                 journal::JournalFileWriter& f) {
    journal::Record r;
    std::size_t n = 0;
    while (w.pop_critical(r)) {
        f.append_record(r);
        ++n;
    }
    while (w.pop_telemetry(r)) {
        f.append_record(r);
        ++n;
    }
    return n;
}

}  // namespace kestrel::test
