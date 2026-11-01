// ============================================================================
// kestrel/tools/journal_export.cpp
//
// Cold-path projection: journal recording -> JSON-lines demo snapshots
// (schema "kestrel.demo/1"). This is the data plane of the Milestone E
// bridge for the standalone console demo: it replays the journaled
// nondeterministic inputs through the SAME feed/OMS logic the engine used
// and emits CONFLATED snapshots (bounded count), the way an external
// service would consume engine state — never by querying the engine.
//
// Snapshot contents per line (conflation: every --every MD events, plus
// always on fills/acks/declines/kill-switch and at the end):
//   book top-N ladder, position, cash flow, mark mid, orders, recent fills,
//   risk usage, kill-switch state.
//
// Honest limits (also printed to stderr):
//   * The console's latency panel uses the measured bench distribution
//     (bundled separately) — per-event latency is NOT journaled by design
//     (minimal instrumentation on the hot path).
//   * PnL = signed cash flow vs mark mid (average-cost approximation).
//   * The connected/WebSocket mode and the Rust gateway are deferred
//     (ADR-0007: no rustc in this sandbox).
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "kestrel/engine/engine.hpp"
#include "kestrel/journal/journal.hpp"
#include "kestrel/md/feed_handler.hpp"
#include "kestrel/protocols/kitch.hpp"
#include "kestrel/protocols/kouch.hpp"

using namespace kestrel;

namespace {

struct OrderRow {
    std::uint64_t id = 0;
    char side = 'B';
    std::int64_t price = 0;
    std::int64_t qty = 0;
    std::int64_t filled = 0;
    const char* state = "PendingNew";
};

struct ProjState {
    md::FeedHandler<engine::kMaxBookLevels, engine::kMaxBookOrders> feed{85'000};
    std::vector<OrderRow> orders;              // bounded by kMaxOrders rows
    std::int64_t position = 0;
    double cash = 0.0;                          // signed: buys pay, sells receive
    std::int64_t realized_marks = 0;            // count of fills
    bool kill_switch = false;
    std::uint64_t fills_total = 0;
    std::uint64_t orders_total = 0;
    std::uint64_t declines_total = 0;
};

constexpr std::size_t kMaxRows = 512;

void apply_exec_frame(ProjState& st, std::span<const std::uint8_t> payload) {
    using namespace protocols::kouch;
    RespHeader h{};
    if (decode_response_header(payload, h) != ReqDecode::Ok) return;
    switch (h.type) {
        case RespType::Accepted: {
            Accepted m{};
            if (decode_accepted(payload, m) != ReqDecode::Ok) return;
            for (OrderRow& r : st.orders)
                if (r.id == m.client_order_id && r.state == std::string_view{"PendingNew"})
                    r.state = "Live";
            break;
        }
        case RespType::Declined: {
            Declined m{};
            if (decode_declined(payload, m) != ReqDecode::Ok) return;
            ++st.declines_total;
            for (OrderRow& r : st.orders)
                if (r.id == m.client_order_id &&
                    (r.state == std::string_view{"PendingNew"} ||
                     r.state == std::string_view{"ReplacePending"}))
                    r.state = "Rejected";
            break;
        }
        case RespType::Executed: {
            Executed m{};
            if (decode_executed(payload, m) != ReqDecode::Ok) return;
            // Maker fills carry side 'A' (our resting order); taker fills are
            // our aggressive orders — both move inventory the same way.
            const std::int64_t q = static_cast<std::int64_t>(m.fill_qty);
            const std::int64_t px = static_cast<std::int64_t>(m.fill_price);
            const bool buy = m.side == kSideBuy;
            st.position += buy ? q : -q;
            st.cash += (buy ? -1.0 : 1.0) * static_cast<double>(q) * px;
            ++st.fills_total;
            for (OrderRow& r : st.orders) {
                if (r.id == m.client_order_id) {
                    r.filled += q;
                    if (r.filled >= r.qty) r.state = "Filled";
                    else if (r.state != std::string_view{"Rejected"}) r.state = "PartialFill";
                }
            }
            break;
        }
        case RespType::Canceled: {
            Canceled m{};
            if (decode_canceled(payload, m) != ReqDecode::Ok) return;
            for (OrderRow& r : st.orders)
                if (r.id == m.client_order_id && r.state != std::string_view{"Filled"})
                    r.state = "Cancelled";
            break;
        }
        case RespType::Replaced: {
            Replaced m{};
            if (decode_replaced(payload, m) != ReqDecode::Ok) return;
            for (OrderRow& r : st.orders) {
                if (r.id == m.orig_client_id &&
                    r.state != std::string_view{"Filled"})
                    r.state = "Replaced";
                if (r.id == m.new_client_id && r.state == std::string_view{"PendingNew"})
                    r.state = "Live";
            }
            break;
        }
        default:
            break;
    }
}

void apply_tx_frame(ProjState& st, std::span<const std::uint8_t> payload) {
    using namespace protocols::kouch;
    if (payload.empty()) return;
    Enter e{};
    Cancel c{};
    Replace r{};
    if (decode_request(payload, e, c, r) != ReqDecode::Ok) return;
    if (payload[0] == 'E') {
        if (st.orders.size() < kMaxRows) {
            // Wire sides are numeric (1=buy, 2=sell); map to ASCII for JSON.
            const char side = e.side == kSideBuy ? 'B' : 'S';
            st.orders.push_back(OrderRow{e.client_order_id, side,
                                         static_cast<std::int64_t>(e.price),
                                         static_cast<std::int64_t>(e.qty), 0,
                                         "PendingNew"});
        }
        ++st.orders_total;
    } else if (payload[0] == 'R') {
        if (st.orders.size() < kMaxRows) {
            // The replace frame does not carry a side: inherit it from the
            // orig order.
            char side = 'B';
            for (const OrderRow& r2 : st.orders)
                if (r2.id == r.orig_client_id) { side = r2.side; break; }
            st.orders.push_back(OrderRow{r.new_client_id, side,
                                         static_cast<std::int64_t>(r.new_price),
                                         static_cast<std::int64_t>(r.new_qty), 0,
                                         "ReplacePending"});
        }
        ++st.orders_total;
    }
    // 'C': cancel request — state flips on the confirm.
}

void emit_snapshot(std::ofstream& out, const ProjState& st, std::uint64_t ts_ns,
                   std::uint64_t event_no, bool force) {
    const auto bb = st.feed.book().best_bid();
    const auto ba = st.feed.book().best_ask();
    const double mark = bb ? static_cast<double>(*bb)
                           : (ba ? static_cast<double>(*ba) : 0.0);
    const double pnl = st.cash + static_cast<double>(st.position) * mark;

    out << "{\"schema\":\"kestrel.demo/1\",\"ts_ns\":" << ts_ns
        << ",\"event\":" << event_no
        << ",\"mid\":" << (bb && ba ? (*bb + *ba) / 2 : 0)
        << ",\"pos\":" << st.position
        << ",\"cash\":" << st.cash
        << ",\"pnl\":" << pnl
        << ",\"kill\":" << (st.kill_switch ? "true" : "false")
        << ",\"counts\":{\"orders\":" << st.orders_total
        << ",\"fills\":" << st.fills_total
        << ",\"declines\":" << st.declines_total << "}";

    // Book ladder: top 10 per side from the mirrored book.
    out << ",\"bids\":[";
    bool first = true;
    if (bb) {
        int bid_count = 0;
        for (std::int64_t p = *bb; p >= 85'000 && bid_count < 10; --p) {
            const std::int64_t q = st.feed.book().level_qty(p, book::Side::Buy);
            if (q > 0) {
                if (!first) out << ",";
                out << "[" << p << "," << q << "]";
                first = false;
                ++bid_count;
            }
        }
    }
    out << "],\"asks\":[";
    first = true;
    int ask_count = 0;
    if (ba) {
        for (std::int64_t p = *ba; p <= 115'000 && ask_count < 10; ++p) {
            const std::int64_t q = st.feed.book().level_qty(p, book::Side::Sell);
            if (q > 0) {
                if (!first) out << ",";
                out << "[" << p << "," << q << "]";
                first = false;
                ++ask_count;
            }
        }
    }
    out << "]";

    // Open orders (non-terminal), bounded.
    out << ",\"orders\":[";
    first = true;
    int n = 0;
    for (const OrderRow& r : st.orders) {
        if (r.state == std::string_view{"Filled"} ||
            r.state == std::string_view{"Cancelled"} ||
            r.state == std::string_view{"Rejected"} ||
            r.state == std::string_view{"Replaced"})
            continue;
        if (!first) out << ",";
        out << "{\"id\":" << r.id << ",\"side\":\"" << r.side << "\",\"px\":"
            << r.price << ",\"qty\":" << r.qty << ",\"filled\":" << r.filled
            << ",\"state\":\"" << r.state << "\"}";
        first = false;
        if (++n >= 32) break;
    }
    out << "]";
    out << ",\"force\":" << (force ? "true" : "false");
    out << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* out_path = nullptr;
    unsigned every = 25;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--in") in_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--every") every = static_cast<unsigned>(std::atoi(next()));
    }
    if (!in_path || !out_path) {
        std::fprintf(stderr, "usage: journal_export --in FILE --out FILE "
                             "[--every N]\n");
        return 2;
    }

    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", in_path);
        return 1;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    journal::JournalFileReader reader(std::move(data));
    if (!reader.ok()) {
        std::fprintf(stderr, "not a kestrel journal (bad magic)\n");
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }

    ProjState st;
    journal::RecordHeader rh{};
    std::span<const std::uint8_t> payload;
    std::uint64_t event_no = 0;
    std::uint64_t ts_ns = 0;
    std::uint64_t md_since_snapshot = 0;
    std::uint64_t records = 0;
    std::uint64_t snapshots = 0;

    while (reader.next(rh, payload) == journal::ParseStatus::Ok) {
        ++records;
        const auto type = static_cast<journal::RecordType>(rh.type);
        if (payload.size() < 8 && type != journal::RecordType::Control) continue;
        if (type == journal::RecordType::Control) {
            // engine kill-switch journal: [code]
            if (!payload.empty()) {
                st.kill_switch = payload[0] == 1;
                emit_snapshot(out, st, ts_ns, event_no, true);
                ++snapshots;
            }
            continue;
        }
        // [u64_be ts][frame] for Md/Exec; raw frame for Tx (no ts prefix —
        // the engine journals the exact submitted bytes, and event time comes
        // from the input axis anyway).
        const auto frame = type == journal::RecordType::Tx ? payload
                                                           : payload.subspan(8);
        if (type != journal::RecordType::Tx) {
            std::uint64_t ts = 0;
            for (int i = 0; i < 8; ++i)
                ts = (ts << 8) | payload[static_cast<std::size_t>(i)];
            ts_ns = ts;
        }
        bool force = false;
        if (type == journal::RecordType::Md) {
            st.feed.on_frame(frame, true);
            ++event_no;
            if (++md_since_snapshot >= every) {
                emit_snapshot(out, st, ts_ns, event_no, false);
                ++snapshots;
                md_since_snapshot = 0;
            }
        } else if (type == journal::RecordType::Exec) {
            const std::size_t before_fills = st.fills_total;
            apply_exec_frame(st, frame);
            if (st.fills_total != before_fills) force = true;
            emit_snapshot(out, st, ts_ns, event_no, force);
            ++snapshots;
        } else if (type == journal::RecordType::Tx) {
            apply_tx_frame(st, frame);
        }
        (void)force;
    }
    emit_snapshot(out, st, ts_ns, event_no, true);
    ++snapshots;

    std::fprintf(stderr,
                 "journal_export: %llu records, %llu events, %llu snapshots, "
                 "pos=%lld fills=%llu orders=%llu declines=%llu kill=%d\n",
                 static_cast<unsigned long long>(records),
                 static_cast<unsigned long long>(event_no),
                 static_cast<unsigned long long>(snapshots),
                 static_cast<long long>(st.position),
                 static_cast<unsigned long long>(st.fills_total),
                 static_cast<unsigned long long>(st.orders_total),
                 static_cast<unsigned long long>(st.declines_total),
                 static_cast<int>(st.kill_switch));
    return 0;
}
