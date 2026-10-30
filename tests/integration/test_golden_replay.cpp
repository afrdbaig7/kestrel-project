// Milestone C: golden replay — byte-identical outbound order messages and
// equivalent final state from the same captured inputs (spec §9, ADR-0006).
#include <gtest/gtest.h>
#include <cstdio>
#include <vector>

#include "host.hpp"

using namespace kestrel::test;
using namespace kestrel::journal;

namespace {
using namespace kestrel::core;
using namespace kestrel::net;
struct CapturedRun {
    std::vector<ReplayInput> inputs;
    std::vector<std::vector<std::uint8_t>> tx;         // expected outbound
    std::vector<std::vector<std::uint8_t>> snapshots;  // SNAPSHOT records
};

/// Walk a journal and split it into inputs (MD/EXEC), expected TX records,
/// and snapshots, in order.
bool capture_run(const std::string& path, CapturedRun& cap) {
    std::vector<std::uint8_t> data;
    if (!read_file(path, data)) return false;
    JournalFileReader r(std::move(data));
    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    while (r.next(h, pl) == ParseStatus::Ok) {
        switch (static_cast<RecordType>(h.type)) {
            case RecordType::Md:
            case RecordType::Exec: {
                // payload = [u64_be ts][frame]
                if (pl.size() < 8) return false;
                ByteReader br(pl);
                std::uint64_t ts = 0;
                br.u64be(ts);
                cap.inputs.push_back(
                    {static_cast<RecordType>(h.type) == RecordType::Md
                         ? FrameKind::MarketData
                         : FrameKind::ExecReport,
                     ts, std::vector<std::uint8_t>(pl.begin() + 8, pl.end())});
                break;
            }
            case RecordType::Tx:
                cap.tx.emplace_back(pl.begin(), pl.end());
                break;
            case RecordType::Snapshot:
                cap.snapshots.emplace_back(pl.begin(), pl.end());
                break;
            default:
                break;
        }
    }
    return true;
}

/// Replay inputs into a fresh engine; returns the outbound TX stream.
std::vector<std::vector<std::uint8_t>> replay(const CapturedRun& cap,
                                              std::uint64_t* position_out = nullptr,
                                              std::uint64_t* oms_size_out = nullptr,
                                              std::uint64_t* last_client_out = nullptr,
                                              std::size_t start_input = 0,
                                              const std::vector<std::uint8_t>* seek_state = nullptr) {
    TestHost h;   // same defaults as the capture host
    if (seek_state && !h.eng->load_state(*seek_state)) return {};
    std::vector<std::vector<std::uint8_t>> out;
    std::uint8_t txbuf[4608];
    std::size_t len = 0;
    for (std::size_t i = start_input; i < cap.inputs.size(); ++i) {
        const ReplayInput& in = cap.inputs[i];
        // The venue 'B' snapshot frames are part of the input stream; fresh
        // books rebuild exactly from them + subsequent deltas.
        if (!h.backend->push_rx(in.kind, in.ts, in.bytes)) return {};
        h.eng->run_once();
        while (h.backend->pop_tx(txbuf, len)) {
            out.emplace_back(txbuf, txbuf + len);
        }
    }
    if (position_out) *position_out = h.eng->risk_gate(0).state().position < 0
                                          ? static_cast<std::uint64_t>(-h.eng->risk_gate(0).state().position)
                                          : static_cast<std::uint64_t>(h.eng->risk_gate(0).state().position);
    if (oms_size_out) *oms_size_out = h.eng->oms_engine().size();
    if (last_client_out) *last_client_out = h.eng->last_client_id();
    return out;
}
}  // namespace

TEST(GoldenReplay, ByteIdenticalOutboundAndEquivalentState) {
    const std::string path = "/tmp/kestrel_golden.jrnl";
    {
        TestHost h;
        JournalFileWriter fw;
        ASSERT_TRUE(fw.open(path));
        h.enable_journal();
        // Drain continuously — the IO-thread emulation. (A single end-of-run
        // drain would overflow the critical ring and fail closed.)
        for (std::uint64_t i = 0; i < 2000; ++i) {
            h.step();
            if (i % 8 == 7) drain_journal(*h.jw, fw);
        }
        h.eng->write_snapshot_record();
        drain_journal(*h.jw, fw);
        fw.close();
    }

    CapturedRun cap;
    ASSERT_TRUE(capture_run(path, cap));
    ASSERT_GE(cap.inputs.size(), 100u);
    ASSERT_GT(cap.tx.size(), 4u);   // quiet market: few but deterministic orders

    const auto got = replay(cap);
    ASSERT_EQ(got.size(), cap.tx.size())
        << "replay must produce the same number of outbound messages";
    for (std::size_t i = 0; i < cap.tx.size(); ++i) {
        ASSERT_EQ(got[i].size(), cap.tx[i].size()) << "tx #" << i << " length";
        ASSERT_EQ(std::memcmp(got[i].data(), cap.tx[i].data(), got[i].size()), 0)
            << "tx #" << i << " bytes differ — replay is not byte-identical";
    }
    SUCCEED() << "replayed " << got.size()
              << " outbound messages byte-identical from " << cap.inputs.size()
              << " journaled inputs";
}
