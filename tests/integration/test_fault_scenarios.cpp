// Milestone C: fault scenarios — gaps + snapshot recovery, disconnects,
// drops, venue rejects, malformed frames. The engine must stay consistent,
// valid, and fail-closed where required (spec §9).
#include <gtest/gtest.h>
#include <optional>
#include <random>

#include "host.hpp"

using namespace kestrel::test;
using namespace kestrel;

TEST(FaultScenarios, MdGapTriggersSeekingAndSnapshotRecovers) {
    sim::VenueFaults f{};
    f.inject_gap_next_md = true;
    TestHost h{HostParams{.seed = 7, .faults = f}};
    h.run(300);
    // A gap happened: feed entered Seeking and was restored by a venue
    // snapshot (host emits one every seek cadence? here: rely on a later
    // explicit snapshot for recovery).
    const auto& stats = h.eng->feed(0).stats();
    EXPECT_EQ(stats.gaps, 1u);
    EXPECT_FALSE(h.eng->feed(0).is_synced());
    EXPECT_GT(stats.dropped_while_seeking, 0u);

    // Recovery boundary: venue publishes a book snapshot.
    h.sim->emit_snapshot_md(0);
    h.run(10);
    EXPECT_TRUE(h.eng->feed(0).is_synced());
    EXPECT_EQ(h.eng->feed(0).stats().snapshot_restores, 1u);
    // Book tracks the venue exactly again.
    EXPECT_EQ(h.eng->feed(0).book().best_bid(), h.sim->best_bid(0));
    EXPECT_EQ(h.eng->feed(0).book().best_ask(), h.sim->best_ask(0));
    EXPECT_TRUE(h.eng->feed(0).book().validate());
}

TEST(FaultScenarios, DisconnectWindowDropsEverythingEngineStaysSane) {
    sim::VenueFaults f{};
    f.disconnect_start_ns = 5'000'000;      // 20 steps in
    f.disconnect_end_ns = 30'000'000;       // through step 120
    TestHost h{HostParams{.seed = 11, .faults = f}};
    h.run(600);
    // Engine survived; state remains internally consistent.
    EXPECT_TRUE(h.eng->feed(0).book().validate());
    // Risk reservations never exceed caps despite the outage.
    const auto& rs = h.eng->risk_gate(0).state();
    EXPECT_LE(rs.pending_buy_lots + rs.pending_sell_lots, 200);
}

TEST(FaultScenarios, DropsAndDuplicatesKeepBooksConsistent) {
    sim::VenueFaults f{};
    f.drop_prob = 0.10;
    f.duplicate_md_prob = 0.10;
    f.reject_prob = 0.10;
    f.reorder_prob = 0.05;
    f.seed = 99;
    TestHost h{HostParams{.seed = 5, .faults = f}};
    h.run(3000);
    EXPECT_TRUE(h.eng->feed(0).book().validate());
    // With duplicates suppressed and gaps only from real drops, the feed
    // either stays synced or recovers via venue snapshots; both are fine.
    // What must NOT happen: corruption or deadlock.
    EXPECT_EQ(h.eng->stats().journal_gaps, 0u);
}

TEST(FaultScenarios, JournalOverflowFailsClosed) {
    TestHost h;
    h.enable_journal();
    // Shrink the ring by hammering: we cannot resize, so instead simulate
    // overflow by NOT draining while running many steps with big frames.
    // The critical ring (1024 records) fills; engine must fail closed.
    h.run(4000);   // no drain: far more than 1024 md records
    EXPECT_TRUE(h.eng->killed());
    EXPECT_GT(h.eng->stats().journal_gaps, 0u);
    EXPECT_TRUE(h.jw->gap_flagged());
    // After fail-closed, no new exposure: tx_enqueued frozen.
    const auto tx = h.eng->stats().tx_enqueued;
    h.run(50);
    EXPECT_EQ(h.eng->stats().tx_enqueued, tx);
}

TEST(FaultScenarios, MalformedMdFramesNeverCorrupt) {
    TestHost h;
    h.run(100);
    // Inject garbage frames directly (truncated/mutated), draining the
    // backend between chunks (bounded arena — like a real NIC ring).
    std::mt19937_64 rng(31337);
    for (int i = 0; i < 2000; ++i) {
        std::vector<std::uint8_t> f(1 + rng() % 60);
        for (auto& b : f) b = static_cast<std::uint8_t>(rng());
        const net::FrameKind k = (rng() & 1) ? net::FrameKind::MarketData
                                             : net::FrameKind::ExecReport;
        ASSERT_TRUE(h.backend->push_rx(k, h.now + i, f));
        if (i % 64 == 63) {
            h.eng->run_once();
            std::uint8_t txbuf[4608];
            std::size_t len = 0;
            while (h.backend->pop_tx(txbuf, len)) {}
        }
    }
    h.eng->run_once();
    h.run(100);
    EXPECT_TRUE(h.eng->feed(0).book().validate());
    // Garbage frames are counted at the engine boundary (header decode or
    // routing) — never silently dropped, never corrupting state.
    EXPECT_GT(h.eng->stats().frames_md_invalid + h.eng->stats().md_route_misses +
                  h.eng->feed(0).stats().rejects_decode,
              0u);
}

TEST(FaultScenarios, SnapshotSeekResumesByteIdentical) {
    // Full run with snapshots every 256 steps.
    const std::string path = "/tmp/kestrel_seek.jrnl";
    {
        TestHost h;
        journal::JournalFileWriter fw;
        ASSERT_TRUE(fw.open(path));
        h.enable_journal();
        for (int i = 0; i < 2000; ++i) {
            if (i % 256 == 0 && i > 0) {
                h.sim->emit_snapshot_md(0);          // venue book snapshot (md)
                h.eng->write_snapshot_record();      // engine state (snapshot)
            }
            h.step();
            if (i % 8 == 7) drain_journal(*h.jw, fw);   // IO-thread emulation
        }
        drain_journal(*h.jw, fw);
        fw.close();
    }

    // Walk the journal; at the FIRST SNAPSHOT record past step 256, restore
    // state and continue with byte-identical TX.
    std::vector<std::uint8_t> data;
    ASSERT_TRUE(journal::read_file(path, data));
    journal::JournalFileReader r(std::move(data));
    journal::RecordHeader rh{};
    std::span<const std::uint8_t> pl;

    CapturedInputs inputs;
    std::vector<std::vector<std::uint8_t>> expected_tx;
    std::optional<std::vector<std::uint8_t>> snapshot_state;
    std::size_t snapshot_input_index = 0;

    while (r.next(rh, pl) == journal::ParseStatus::Ok) {
        const auto t = static_cast<journal::RecordType>(rh.type);
        if (t == journal::RecordType::Md || t == journal::RecordType::Exec) {
            core::ByteReader br(pl);
            std::uint64_t ts = 0;
            br.u64be(ts);
            inputs.push_back({t == journal::RecordType::Md ? net::FrameKind::MarketData
                                                           : net::FrameKind::ExecReport,
                              ts, std::vector<std::uint8_t>(pl.begin() + 8, pl.end())});
        } else if (t == journal::RecordType::Tx) {
            expected_tx.emplace_back(pl.begin(), pl.end());
        } else if (t == journal::RecordType::Snapshot && !snapshot_state) {
            snapshot_state.emplace(pl.begin(), pl.end());
            snapshot_input_index = inputs.size();
        }
    }
    ASSERT_TRUE(snapshot_state.has_value());
    ASSERT_GT(snapshot_input_index, 10u);

    // Replay FULL (baseline) then from seek point; compare TX suffixes.
    TestHost h;
    ASSERT_TRUE(h.eng->load_state(*snapshot_state));
    std::vector<std::vector<std::uint8_t>> got_tx;
    std::uint8_t txbuf[4608];
    std::size_t len = 0;
    for (std::size_t i = snapshot_input_index; i < inputs.size(); ++i) {
        ASSERT_TRUE(h.backend->push_rx(inputs[i].kind, inputs[i].ts, inputs[i].bytes));
        h.eng->run_once();
        while (h.backend->pop_tx(txbuf, len)) {
            got_tx.emplace_back(txbuf, txbuf + len);
        }
    }
    // The expected suffix starts at the TX emitted after the snapshot point.
    // Count TXs emitted before the snapshot: walk the journal again counting
    // TX records that appear BEFORE the snapshot record.
    std::vector<std::uint8_t> data2;
    ASSERT_TRUE(journal::read_file(path, data2));
    journal::JournalFileReader r2(std::move(data2));
    journal::RecordHeader rh2{};
    std::span<const std::uint8_t> pl2;
    std::size_t tx_before_snapshot = 0;
    bool seen_snapshot = false;
    while (r2.next(rh2, pl2) == journal::ParseStatus::Ok) {
        const auto t = static_cast<journal::RecordType>(rh2.type);
        if (t == journal::RecordType::Snapshot) { seen_snapshot = true; break; }
        if (t == journal::RecordType::Tx) ++tx_before_snapshot;
    }
    ASSERT_TRUE(seen_snapshot);
    ASSERT_GE(expected_tx.size(), tx_before_snapshot);

    const std::size_t suffix = expected_tx.size() - tx_before_snapshot;
    ASSERT_EQ(got_tx.size(), suffix)
        << "seeked replay must produce exactly the suffix of outbound messages";
    for (std::size_t i = 0; i < suffix; ++i) {
        const auto& want = expected_tx[tx_before_snapshot + i];
        ASSERT_EQ(got_tx[i].size(), want.size()) << "suffix tx #" << i;
        ASSERT_EQ(std::memcmp(got_tx[i].data(), want.data(), want.size()), 0)
            << "suffix tx #" << i << " bytes differ after snapshot seek";
    }
}
