// kestrel/tools/record_demo.cpp
//
// Drives the REAL engine (same host wiring as the correctness tests) against
// the venue simulator with the journal attached, writing a binary journal
// file for the demo console pipeline:
//   record_demo -> <journal file> -> kestrel_journal_export -> demo.jsonl
//
// This is a RECORDING of real engine behavior — not scripted demo data.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "host.hpp"

using namespace kestrel;

int main(int argc, char** argv) {
    const char* out_path = argc > 1 ? argv[1] : "demo.journal";
    std::uint64_t steps = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 12'000;

    test::TestHost h;
    h.enable_journal();
    journal::JournalFileWriter jw_file;
    if (!jw_file.open(out_path)) {
        std::fprintf(stderr, "cannot open %s\n", out_path);
        return 1;
    }

    std::vector<sim::OutFrame> frames;
    std::uint64_t now = 0;
    for (std::uint64_t i = 0; i < steps; ++i) {
        now += 250'000;
        h.sim->run_background(now);
        h.sim->pump(now, frames);
        for (const sim::OutFrame& f : frames) {
            h.backend->push_rx(f.kind == sim::OutputKind::MarketData
                                   ? net::FrameKind::MarketData
                                   : net::FrameKind::ExecReport,
                               f.ts_ns, f.payload);
            h.eng->run_once();
            std::uint8_t txbuf[4608];
            std::size_t len = 0;
            while (h.backend->pop_tx(txbuf, len))
                h.sim->on_request({txbuf, len}, now);
        }
        frames.clear();
        // IO-thread emulation: drain journal rings into the file.
        journal::Record r;
        while (h.jw->pop_critical(r)) jw_file.append_record(r);
        while (h.jw->pop_telemetry(r)) jw_file.append_record(r);
    }
    jw_file.flush_ok();
    jw_file.close();
    const auto& es = h.eng->stats();
    const auto& os = h.eng->oms_engine().stats();
    std::printf("recorded %llu steps -> %s | md=%llu exec=%llu tx=%llu "
                "fills=%llu\n",
                static_cast<unsigned long long>(steps), out_path,
                static_cast<unsigned long long>(es.frames_md),
                static_cast<unsigned long long>(es.frames_exec),
                static_cast<unsigned long long>(es.tx_enqueued),
                static_cast<unsigned long long>(os.fills));
    return 0;
}
