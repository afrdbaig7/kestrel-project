#pragma once
// ============================================================================
// kestrel/sim/exchange_sim.hpp
//
// Price-time-priority exchange simulator — the engine's counterparty for
// simulation, backtesting, and the demo. VENUE-side infrastructure: allowed
// to allocate and use std containers (it is NOT the engine hot path; the
// engine's guarantees are documented separately, spec §4/§9).
//
// Assumptions (explicit, spec §9):
//   * Queue position: incoming aggressive orders match FIFO within a price
//     level; resting orders keep priority until fully filled or cancelled.
//   * Latency model: a request arriving at event time T is processed at T;
//     responses and market-data updates are scheduled at T + configured
//     latency and delivered in (delivery_ts, seq) order — a deterministic
//     "instant matching + delayed delivery" model, journaled by the host.
//   * Fills occur at the MAKER's price; taker fills report maker prices.
//   * Replace is atomic: old order removed, new order matched/rested; on
//     decline the old order remains resting.
//   * Multiple instruments per venue; two venue instances = "two venues".
//
// Fault injection (seeded, deterministic): drop/duplicate/delay responses and
// MD, probabilistic venue rejects, forced sequence gaps, disconnect windows.
// The market-data feed a venue publishes is the KITCH protocol (ADR-0004);
// responses use KOUCH.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "kestrel/core/byte_codec.hpp"
#include "kestrel/protocols/kitch.hpp"
#include "kestrel/protocols/kouch.hpp"

namespace kestrel::sim {

namespace kitch = kestrel::protocols::kitch;
namespace kouch = kestrel::protocols::kouch;
using kouch::Reason;

struct VenueFaults {
    double drop_prob = 0.0;          // responses + md dropped
    double duplicate_md_prob = 0.0;  // md frames duplicated
    double reject_prob = 0.0;        // enters declined venue-side
    double reorder_prob = 0.0;       // response delayed (see reorder_delay_ns)
    std::int64_t reorder_delay_ns = 50'000;
    std::int64_t disconnect_start_ns = -1;   // [start, end]: nothing delivered
    std::int64_t disconnect_end_ns = -1;
    bool inject_gap_next_md = false;         // explicit seq gap (test knob)
    std::uint64_t seed = 1;
};

struct VenueInstrumentCfg {
    std::uint32_t instrument = 0;
    std::int64_t min_price = 0;
    std::int64_t max_price = 0;
    std::int64_t initial_mid = 0;
    std::int64_t initial_spread_ticks = 4;
    std::int64_t initial_levels = 8;
    std::int32_t initial_level_qty = 50;
    // Background (venue-generated) flow: creates a live market around mid.
    std::int64_t background_step_ns = 250'000;
    double background_add_prob = 0.7;
    double background_cancel_prob = 0.5;
    std::int32_t background_qty = 25;
    double background_taker_prob = 0.0;   // IOC crossing orders (creates fills)
};

struct VenueConfig {
    std::int64_t egress_latency_ns = 5'000;
    std::int64_t md_publish_latency_ns = 2'000;
    VenueFaults faults{};
    VenueInstrumentCfg instruments[2]{};   // venues host up to 2 instruments
    std::size_t instrument_count = 1;
};

enum class OutputKind : std::uint8_t { MarketData, ExecReport };

struct OutFrame {
    OutputKind kind;
    std::uint64_t ts_ns;     // delivery (event) time
    std::vector<std::uint8_t> payload;
};

class ExchangeSim {
public:
    explicit ExchangeSim(const VenueConfig& cfg) : cfg_(cfg) {
        for (std::size_t i = 0; i < cfg.instrument_count; ++i) {
            VenueInstrumentCfg ic = cfg.instruments[i];
            const std::uint32_t id = ic.instrument;
            Market& m = markets_[id];
            m.cfg = ic;
            seed_liquidity(m);
        }
        rng_ = std::mt19937_64{cfg.faults.seed};
    }

    ExchangeSim(const ExchangeSim&) = delete;
    ExchangeSim& operator=(const ExchangeSim&) = delete;

    // ---- host-facing pump ----------------------------------------------------
    /// Deliver due scheduled events (md + responses) into `out`.
    void pump(std::uint64_t now_ns, std::vector<OutFrame>& out) {
        while (!scheduled_.empty() && scheduled_.front().ts <= now_ns) {
            Sched ev = std::move(scheduled_.front());
            scheduled_.pop_front();
            if (ev.is_md && reorder_now()) {
                ev.ts += static_cast<std::uint64_t>(cfg_.faults.reorder_delay_ns);
                insert_sorted(std::move(ev));
                continue;
            }
            if (!apply_faults(ev)) continue;
            OutFrame f;
            f.kind = ev.is_md ? OutputKind::MarketData : OutputKind::ExecReport;
            f.ts_ns = ev.ts;
            f.payload = std::move(ev.payload);
            if (ev.is_md) {
                // The consumer's view advances only on DELIVERED frames;
                // dropped frames (faults) leave it stale, exactly as a real
                // feed gap would.
                delivered_bb_ = ev.bb_at;
                delivered_ba_ = ev.ba_at;
            }
            out.push_back(std::move(f));
            if (ev.is_md && uni_(rng_) < cfg_.faults.duplicate_md_prob) {
                out.push_back(f);   // duplicate md (fault injection)
            }
        }
    }
    bool has_due(std::uint64_t now_ns) const {
        return !scheduled_.empty() && scheduled_.front().ts <= now_ns;
    }
    std::uint64_t next_event_ts() const {
        return scheduled_.empty() ? UINT64_MAX : scheduled_.front().ts;
    }

    /// Venue-internal background flow up to now_ns (book churn + md).
    void run_background(std::uint64_t now_ns) {
        for (auto& [instr, m] : markets_) {
            while (m.next_background_ts <= now_ns) {
                const std::uint64_t t = m.next_background_ts;
                m.next_background_ts += static_cast<std::uint64_t>(m.cfg.background_step_ns);
                background_step(m, t);
            }
        }
    }

    /// Public: schedule a full book snapshot ('B') for `instrument` as of the
    /// current state, delivered at now + publish latency. One entry PER
    /// RESTING ORDER with the real venue ref (exact FIFO + tracking restore).
    /// Used at session start and by recovery/seek tests.
    void emit_snapshot_md(std::uint32_t instrument) {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return;
        Market& m = it->second;
        constexpr std::size_t kMaxEntries = 180;
        std::uint8_t body[kitch::kHeaderLen + 6 + kMaxEntries * kitch::kSnapshotEntryLen];
        core::ByteWriter w(body);
        w.u32be(instrument);
        std::uint16_t count = 0;
        const std::size_t count_pos = w.written();
        w.u16be(0);                       // placeholder
        for (const auto& [price, lv] : m.bids) {
            for (const Resting& r : lv.q) {
                if (count >= kMaxEntries) break;
                w.u8(kitch::kSideBuy);
                w.u32be(static_cast<std::uint32_t>(r.qty));
                w.u64be(static_cast<std::uint64_t>(price));
                w.u64be(r.venue_ref);
                ++count;
            }
        }
        for (const auto& [price, lv] : m.asks) {
            for (const Resting& r : lv.q) {
                if (count >= kMaxEntries) break;
                w.u8(kitch::kSideSell);
                w.u32be(static_cast<std::uint32_t>(r.qty));
                w.u64be(static_cast<std::uint64_t>(price));
                w.u64be(r.venue_ref);
                ++count;
            }
        }
        core::ByteWriter wp(std::span<std::uint8_t>(body + count_pos, 2));
        wp.u16be(count);
        // emit_md publishes from scratch_md_: stage the snapshot body there.
        const std::size_t blen = w.written();
        static_assert(sizeof(scratch_md_) >= kitch::kHeaderLen + 6 +
                                            180 * kitch::kSnapshotEntryLen);
        std::memcpy(scratch_md_, body, blen);
        emit_md(m, kitch::MsgType::BookSnapshot, blen);
    }

    // ---- order request entry (from engine TX) --------------------------------
    /// `arrival_ns` is the event time the request reaches the venue.
    void on_request(std::span<const std::uint8_t> req, std::uint64_t arrival_ns) {
        using namespace protocols::kouch;
        event_ts_of_last_request_ = arrival_ns;
        Enter e{};
        Cancel c{};
        Replace r{};
        if (decode_request(req, e, c, r) != ReqDecode::Ok) return;   // malformed: drop
        switch (req[0]) {
            case 'E': handle_enter(e, arrival_ns); break;
            case 'C': handle_cancel(c, arrival_ns); break;
            case 'R': handle_replace(r, arrival_ns); break;
            default: break;
        }
    }

    // ---- introspection (tests / console) --------------------------------------
    std::optional<std::int64_t> best_bid(std::uint32_t instrument) const {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return std::nullopt;
        for (auto jt = it->second.bids.rbegin(); jt != it->second.bids.rend(); ++jt)
            if (!jt->second.q.empty()) return jt->first;
        return std::nullopt;
    }
    std::optional<std::int64_t> best_ask(std::uint32_t instrument) const {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return std::nullopt;
        for (const auto& [p, lv] : it->second.asks)
            if (!lv.q.empty()) return p;
        return std::nullopt;
    }
    std::size_t resting_count(std::uint32_t instrument) const {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return 0;
        std::size_t n = 0;
        for (const auto& [p, q] : it->second.bids) n += q.q.size();
        for (const auto& [p, q] : it->second.asks) n += q.q.size();
        return n;
    }
    std::uint32_t md_seq() const noexcept { return md_seq_; }
    std::uint32_t resp_seq() const noexcept { return resp_seq_; }
    /// Test aid: dump per-level (price, total, count) for one side.
    void debug_dump_bids(std::uint32_t instrument) const {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return;
        for (const auto& [price, lv] : it->second.bids) {
            std::fprintf(stderr, "  sim bid p=%lld total=%lld n=%zu\n",
                         static_cast<long long>(price),
                         static_cast<long long>(lv.total), lv.q.size());
        }
    }
    /// Debug aid (tests/tools): dump every resting order.
    void debug_dump_resting(std::uint32_t instrument) const {
        const auto it = markets_.find(instrument);
        if (it == markets_.end()) return;
        std::fprintf(stderr, "sim BIDS:\n");
        for (const auto& [price, lv] : it->second.bids)
            for (const Resting& r : lv.q)
                std::fprintf(stderr, "  p=%lld ref=%llu q=%lld c=%llu\n",
                             static_cast<long long>(price),
                             static_cast<unsigned long long>(r.venue_ref),
                             static_cast<long long>(r.qty),
                             static_cast<unsigned long long>(r.client_id));
        std::fprintf(stderr, "sim ASKS:\n");
        for (const auto& [price, lv] : it->second.asks)
            for (const Resting& r : lv.q)
                std::fprintf(stderr, "  p=%lld ref=%llu q=%lld c=%llu\n",
                             static_cast<long long>(price),
                             static_cast<unsigned long long>(r.venue_ref),
                             static_cast<long long>(r.qty),
                             static_cast<unsigned long long>(r.client_id));
    }
    /// Best prices as of the last PUBLISHED md frame (feed-consumer view).
    /// Best prices as of the last DELIVERED md frame — what a feed consumer
    /// (the engine book) converges to. Multi-instrument venues share the
    /// single delivered-state pair per venue instance (one market per venue
    /// in the current configuration; documented limitation).
    std::optional<std::int64_t> published_best_bid(std::uint32_t) const {
        return delivered_bb_;
    }
    std::optional<std::int64_t> published_best_ask(std::uint32_t) const {
        return delivered_ba_;
    }

private:
    struct Resting {
        std::uint64_t venue_ref;
        std::uint64_t client_id;
        std::int64_t qty;
    };
    struct Level {
        std::deque<Resting> q;
        std::int64_t total = 0;
    };
    // Both maps ASCENDING (std::less): best bid = *rbegin(), best ask = begin().
    struct Market {
        VenueInstrumentCfg cfg{};
        std::map<std::int64_t, Level> bids;
        std::map<std::int64_t, Level> asks;
        std::uint64_t next_background_ts = 0;
        // Best prices AS OF the most recent published md frame — this is what
        // a consumer of the feed converges to (venue-internal state may be
        // ahead by the publish latency).
        std::optional<std::int64_t> published_bb;
        std::optional<std::int64_t> published_ba;
        void refresh_published() {
            // Empty levels can transiently remain as map keys (erased after
            // the matching loop); best-price scans must skip them.
            published_bb = std::nullopt;
            for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
                if (!it->second.q.empty()) { published_bb = it->first; break; }
            }
            published_ba = std::nullopt;
            for (const auto& [p, lv] : asks) {
                if (!lv.q.empty()) { published_ba = p; break; }
            }
        }
    };
    struct ClientPos {
        std::uint32_t instrument;
        std::uint8_t side;
        std::int64_t price;
        std::int64_t qty;          // resting qty
        std::uint64_t venue_ref;
    };
    struct Sched {
        std::uint64_t ts = 0;
        std::uint64_t seq = 0;
        bool is_md = false;
        std::vector<std::uint8_t> payload;
        // Venue best prices AFTER the change this frame describes; a feed
        // consumer reaches exactly this state after applying the frame.
        std::optional<std::int64_t> bb_at;
        std::optional<std::int64_t> ba_at;
        bool operator<(const Sched& o) const {
            return ts != o.ts ? ts < o.ts : seq < o.seq;
        }
    };

    std::uint32_t market_instrument(const Market& m) const {
        for (const auto& [id, mk] : markets_)
            if (&mk == &m) return id;
        return 0;
    }

    void seed_liquidity(Market& m) {
        const std::int64_t half = m.cfg.initial_spread_ticks / 2;
        event_ts_of_last_request_ = 0;
        for (std::int64_t i = 0; i < m.cfg.initial_levels; ++i) {
            rest(m, protocols::kouch::kSideBuy, m.cfg.initial_mid - half - i,
                 m.cfg.initial_level_qty, /*client=*/0);
            rest(m, protocols::kouch::kSideSell, m.cfg.initial_mid + half + i,
                 m.cfg.initial_level_qty, /*client=*/0);
        }
        m.next_background_ts = m.cfg.background_step_ns;
    }

    static std::map<std::int64_t, Level>& book_for(Market& m, std::uint8_t side) {
        return side == protocols::kouch::kSideBuy ? m.bids : m.asks;
    }

    void rest(Market& m, std::uint8_t side, std::int64_t price, std::int64_t qty,
              std::uint64_t client_id) {
        const std::uint64_t ref = ++venue_ref_;
        auto& book = book_for(m, side);
        Level& lv = book[price];
        lv.q.push_back(Resting{ref, client_id, qty});
        lv.total += qty;
        if (client_id)
            clients_[client_id] =
                ClientPos{market_instrument(m), side, price, qty, ref};
        emit_md_add(m, market_instrument(m), ref, side, qty, price);
    }

    void background_step(Market& m, std::uint64_t ts) {
        event_ts_of_last_request_ = ts;
        const double p = uni_(rng_);
        if (p < m.cfg.background_add_prob) {
            const bool buy = (rng_() & 1) != 0;
            std::optional<std::int64_t> base =
                buy ? best_bid(market_instrument(m)) : best_ask(market_instrument(m));
            if (!base) {
                // Empty-side re-seed: without this the sim has an ABSORBING
                // state — adds anchor on the same-side best, so once a side
                // depletes (taker sweep) no background flow can ever restore
                // it. Re-populate away from the opposite touch (or the
                // reference mid when both sides are empty), the way a market
                // maker re-entering would. No extra rng consumption.
                const auto opp = buy ? best_ask(market_instrument(m))
                                     : best_bid(market_instrument(m));
                base = opp ? (buy ? *opp - m.cfg.initial_spread_ticks
                                  : *opp + m.cfg.initial_spread_ticks)
                           : (buy ? m.cfg.initial_mid - m.cfg.initial_spread_ticks
                                  : m.cfg.initial_mid + m.cfg.initial_spread_ticks);
            }
            const std::int64_t off = static_cast<std::int64_t>(uni_(rng_) * 3.0);
            std::int64_t price = buy ? *base + off : *base - off;
            price = std::clamp(price, m.cfg.min_price, m.cfg.max_price);
            // Background orders never cross (by construction).
            if (buy) {
                if (const auto ba = best_ask(market_instrument(m)); ba && price >= *ba)
                    return;
            } else {
                if (const auto bb = best_bid(market_instrument(m)); bb && price <= *bb)
                    return;
            }
            rest(m, buy ? protocols::kouch::kSideBuy : protocols::kouch::kSideSell,
                 price, m.cfg.background_qty, /*client=*/0);
        } else if (uni_(rng_) < m.cfg.background_taker_prob) {
            // Taker flow: aggressive IOC against the touch — the counterparty
            // that fills resting orders (including engine quotes).
            const bool buy = (rng_() & 1) != 0;
            const auto px = buy ? best_ask(market_instrument(m))
                                : best_bid(market_instrument(m));
            if (!px) return;
            const std::int32_t qty =
                1 + static_cast<std::int32_t>(uni_(rng_) * 30.0);
            match(m, buy ? protocols::kouch::kSideBuy : protocols::kouch::kSideSell,
                  qty, *px, /*taker_client=*/0, ts);
        } else if (p < m.cfg.background_add_prob + m.cfg.background_cancel_prob) {
            if (rng_() & 1) {
                if (m.bids.empty()) return;
                auto it = m.bids.begin();
                std::advance(it, static_cast<long>(
                                     uni_(rng_) * static_cast<double>(m.bids.size())));
                cancel_level_entry(m, it->first, protocols::kouch::kSideBuy, ts);
            } else {
                if (m.asks.empty()) return;
                auto it = m.asks.begin();
                std::advance(it, static_cast<long>(
                                     uni_(rng_) * static_cast<double>(m.asks.size())));
                cancel_level_entry(m, it->first, protocols::kouch::kSideSell, ts);
            }
        }
    }

    void cancel_level_entry(Market& m, std::int64_t price, std::uint8_t side,
                            std::uint64_t ts) {
        using namespace protocols::kouch;
        auto& book = book_for(m, side);
        auto it = book.find(price);
        if (it == book.end() || it->second.q.empty()) return;
        std::size_t victim = 0;
        bool found = false;
        for (std::size_t i = 0; i < it->second.q.size(); ++i) {
            if (it->second.q[i].client_id == 0) { victim = i; found = true; break; }
        }
        if (!found) victim = 0;
        const Resting r = it->second.q[victim];
        it->second.q.erase(it->second.q.begin() + static_cast<long>(victim));
        it->second.total -= r.qty;
        if (it->second.q.empty()) book.erase(it);
        if (r.client_id) {
            clients_.erase(r.client_id);
            // Venue-initiated cancel: the client MUST be notified, otherwise
            // its OMS keeps a "live" order that no longer exists (observed
            // as an infinite UnknownOrder replace loop).
            const Canceled xo{r.client_id, static_cast<std::uint32_t>(r.qty)};
            respond(RespType::Canceled, ts, kouch::body_canceled(scratch_resp_, xo));
        }
        const kitch::OrderCancel oc{r.venue_ref, static_cast<std::uint32_t>(r.qty)};
        emit_md(m, kitch::MsgType::OrderCancel, kitch::body_cancel(scratch_md_, oc));
    }

    // ---- request handlers ------------------------------------------------------
    void handle_enter(const protocols::kouch::Enter& e, std::uint64_t ts) {
        using namespace protocols::kouch;
        const auto mit = markets_.find(e.instrument);
        if (mit == markets_.end()) {
            respond_decline(e.client_order_id, Reason::UnknownInstrument, ts);
            return;
        }
        Market& m = mit->second;
        if (uni_(rng_) < cfg_.faults.reject_prob) {
            respond_decline(e.client_order_id, Reason::RiskRejected, ts);
            return;
        }
        if (e.price < static_cast<std::uint64_t>(m.cfg.min_price) ||
            e.price > static_cast<std::uint64_t>(m.cfg.max_price)) {
            respond_decline(e.client_order_id, Reason::InvalidPrice, ts);
            return;
        }
        if (e.qty == 0) {
            respond_decline(e.client_order_id, Reason::InvalidQty, ts);
            return;
        }
        if (clients_.contains(e.client_order_id)) {
            respond_decline(e.client_order_id, Reason::DuplicateClientOrderId, ts);
            return;
        }
        respond_accept(e, ts);

        const std::int64_t remaining =
            match(m, e.side, static_cast<std::int64_t>(e.qty),
                  static_cast<std::int64_t>(e.price), e.client_order_id, ts);
        if (remaining > 0) {
            if (e.tif == kTifIoc) {
                respond_canceled(e.client_order_id,
                                 static_cast<std::uint32_t>(remaining), ts);
            } else {
                rest(m, e.side, static_cast<std::int64_t>(e.price), remaining,
                     e.client_order_id);
            }
        }
    }

    void handle_cancel(const protocols::kouch::Cancel& c, std::uint64_t ts) {
        using namespace protocols::kouch;
        const auto it = clients_.find(c.client_order_id);
        if (it == clients_.end()) {
            respond_cancel_reject(c.client_order_id, Reason::UnknownOrder, ts);
            return;
        }
        const ClientPos cp = it->second;
        Market& m = markets_[cp.instrument];
        clients_.erase(it);
        remove_resting_no_md(m, cp);
        const kitch::OrderCancel oc{cp.venue_ref, static_cast<std::uint32_t>(cp.qty)};
        emit_md(m, kitch::MsgType::OrderCancel, kitch::body_cancel(scratch_md_, oc));
        respond_canceled(c.client_order_id, static_cast<std::uint32_t>(cp.qty), ts);
    }

    void handle_replace(const protocols::kouch::Replace& r, std::uint64_t ts) {
        using namespace protocols::kouch;
        const auto it = clients_.find(r.orig_client_id);
        if (it == clients_.end()) {
            respond_decline(r.new_client_id, Reason::UnknownOrder, ts);
            return;
        }
        const ClientPos cp = it->second;
        Market& m = markets_[cp.instrument];
        if (r.new_price < static_cast<std::uint64_t>(m.cfg.min_price) ||
            r.new_price > static_cast<std::uint64_t>(m.cfg.max_price)) {
            respond_decline(r.new_client_id, Reason::InvalidPrice, ts);
            return;   // atomic: orig stays resting
        }
        if (r.new_qty == 0) {
            respond_decline(r.new_client_id, Reason::InvalidQty, ts);
            return;
        }
        clients_.erase(it);
        remove_resting_no_md(m, cp);
        // Match FIRST (aggressor semantics), then announce the FINAL state:
        // the old code emitted the 'R' frame (cancel orig + add new) before
        // matching, so a new order that fully crossed was announced as
        // resting and then silently consumed as a taker — feed consumers
        // kept a PHANTOM order forever (observed as a crossed book view).
        const std::int64_t remaining =
            match(m, cp.side, static_cast<std::int64_t>(r.new_qty),
                  static_cast<std::int64_t>(r.new_price), r.new_client_id, ts);
        const Replaced resp{r.orig_client_id, r.new_client_id, r.new_qty, r.new_price};
        respond(RespType::Replaced, ts, kouch::body_replaced(scratch_resp_, resp));
        const Canceled xo{r.orig_client_id, static_cast<std::uint32_t>(cp.qty)};
        respond(RespType::Canceled, ts, kouch::body_canceled(scratch_resp_, xo));
        if (remaining > 0) {
            // Replace = cancel(orig) + add(new, remaining qty) in one frame.
            const std::uint64_t new_ref = ++venue_ref_;
            const kitch::OrderReplace mr{cp.venue_ref, new_ref,
                                         static_cast<std::uint32_t>(remaining),
                                         static_cast<std::uint64_t>(r.new_price)};
            emit_md(m, kitch::MsgType::OrderReplace, kitch::body_replace(scratch_md_, mr));
            auto& book = cp.side == kSideBuy ? m.bids : m.asks;
            Level& lv = book[static_cast<std::int64_t>(r.new_price)];
            lv.q.push_back(Resting{new_ref, r.new_client_id, remaining});
            lv.total += remaining;
            clients_[r.new_client_id] =
                ClientPos{cp.instrument, cp.side,
                          static_cast<std::int64_t>(r.new_price), remaining, new_ref};
        } else {
            // New order fully matched: it never rested, so the feed sees a
            // plain cancel of the orig (no phantom add).
            const kitch::OrderCancel oc{cp.venue_ref, static_cast<std::uint32_t>(cp.qty)};
            emit_md(m, kitch::MsgType::OrderCancel, kitch::body_cancel(scratch_md_, oc));
        }
    }

    /// Match an aggressive order; returns unfilled remainder.
    std::int64_t match(Market& m, std::uint8_t side, std::int64_t qty,
                       std::int64_t price, std::uint64_t taker_client,
                       std::uint64_t ts) {
        using namespace protocols::kouch;
        auto& opposite = side == kSideBuy ? m.asks : m.bids;
        const bool taker_is_buy = side == kSideBuy;
        std::int64_t remaining = qty;
        while (remaining > 0 && !opposite.empty()) {
            // Best opposite price: lowest ask (begin) or highest bid (rbegin).
            auto lit = taker_is_buy ? opposite.begin()
                                    : std::prev(opposite.end());
            const std::int64_t maker_price = lit->first;
            const bool crosses =
                taker_is_buy ? price >= maker_price : price <= maker_price;
            if (!crosses) break;
            Level& lv = lit->second;
            while (remaining > 0 && !lv.q.empty()) {
                const Resting r = lv.q.front();   // copy: deque mutates below
                const std::int64_t take = std::min(r.qty, remaining);
                // Bookkeeping FIRST: the published state captured by emit_md
                // must reflect the post-fill book (a fully-filled order is
                // already gone), matching what a real venue publishes.
                lv.q.front().qty -= take;
                remaining -= take;
                lv.total -= take;
                if (lv.q.front().qty == 0) {
                    lv.q.pop_front();
                    if (r.client_id) clients_.erase(r.client_id);
                }
                const kitch::OrderExecuted oe{r.venue_ref,
                                              static_cast<std::uint32_t>(take),
                                              ++trade_ref_};
                emit_md(m, kitch::MsgType::OrderExecuted, kitch::body_exec(scratch_md_, oe));
                // Client responses only for REAL clients: background orders
                // rest with client_id 0, and routing their fills to clients
                // would deliver exec reports for orders nobody placed.
                if (r.client_id) {
                    const Executed me{r.client_id, market_instrument(m),
                                      taker_is_buy ? kSideSell : kSideBuy,
                                      static_cast<std::uint32_t>(take),
                                      static_cast<std::uint64_t>(maker_price), trade_ref_,
                                      'A'};
                    respond(RespType::Executed, ts, kouch::body_executed(scratch_resp_, me));
                }
                if (taker_client) {
                    const Executed te{taker_client, market_instrument(m), side,
                                      static_cast<std::uint32_t>(take),
                                      static_cast<std::uint64_t>(maker_price), trade_ref_,
                                      'R'};
                    respond(RespType::Executed, ts, kouch::body_executed(scratch_resp_, te));
                }
            }
            if (lv.q.empty()) opposite.erase(lit);
        }
        return remaining;
    }

    void remove_resting_no_md(Market& m, const ClientPos& cp) {
        auto& book = cp.side == protocols::kouch::kSideBuy ? m.bids : m.asks;
        auto it = book.find(cp.price);
        if (it == book.end()) return;
        auto& q = it->second.q;
        for (auto rit = q.begin(); rit != q.end(); ++rit) {
            if (rit->venue_ref == cp.venue_ref) {
                it->second.total -= rit->qty;
                q.erase(rit);
                break;
            }
        }
        if (q.empty()) book.erase(it);
    }

    // ---- output helpers ---------------------------------------------------------
    void respond(protocols::kouch::RespType t, std::uint64_t ts, std::size_t body_len) {
        using namespace protocols::kouch;
        const std::uint64_t deliver =
            ts + static_cast<std::uint64_t>(cfg_.egress_latency_ns);
        Sched ev;
        ev.ts = deliver;
        ev.seq = ++sched_seq_;
        ev.is_md = false;
        ev.payload.resize(kRespHeaderLen + body_len);
        core::ByteWriter w(ev.payload);
        w.u8(static_cast<std::uint8_t>(t));
        w.u32le(++resp_seq_);
        w.u64le(deliver);
        if (body_len) std::memcpy(ev.payload.data() + kRespHeaderLen, scratch_resp_, body_len);
        insert_sorted(std::move(ev));
    }
    void respond_accept(const protocols::kouch::Enter& e, std::uint64_t ts) {
        using namespace protocols::kouch;
        const Accepted a{e.client_order_id, e.instrument, e.side, e.qty, e.price};
        respond(RespType::Accepted, ts, kouch::body_accepted(scratch_resp_, a));
    }
    void respond_decline(std::uint64_t client_id, Reason why, std::uint64_t ts) {
        using namespace protocols::kouch;
        const Declined d{client_id, static_cast<std::uint16_t>(why)};
        respond(RespType::Declined, ts, kouch::body_declined(scratch_resp_, d));
    }
    void respond_canceled(std::uint64_t client_id, std::uint32_t qty, std::uint64_t ts) {
        using namespace protocols::kouch;
        const Canceled c{client_id, qty};
        respond(RespType::Canceled, ts, kouch::body_canceled(scratch_resp_, c));
    }
    void respond_cancel_reject(std::uint64_t client_id, Reason why, std::uint64_t ts) {
        using namespace protocols::kouch;
        const CancelReject g{client_id, static_cast<std::uint16_t>(why)};
        respond(RespType::CancelReject, ts, kouch::body_cancel_reject(scratch_resp_, g));
    }

    void emit_md(Market& m, kitch::MsgType t, std::size_t body_len) {
        m.refresh_published();
        const std::uint64_t deliver =
            event_ts_of_last_request_ +
            static_cast<std::uint64_t>(cfg_.md_publish_latency_ns);
        Sched ev;
        ev.ts = deliver;
        ev.seq = ++sched_seq_;
        ev.is_md = true;
        ev.bb_at = m.published_bb;
        ev.ba_at = m.published_ba;
        ev.payload.resize(kitch::kHeaderLen + body_len);
        core::ByteWriter w(ev.payload);
        w.u8(static_cast<std::uint8_t>(t));
        w.u32be(++md_seq_);
        w.u64be(deliver);
        if (body_len) std::memcpy(ev.payload.data() + kitch::kHeaderLen, scratch_md_, body_len);
        insert_sorted(std::move(ev));
        if (cfg_.faults.inject_gap_next_md) {
            ++md_seq_;   // hole in the feed (recovery tests)
            cfg_.faults.inject_gap_next_md = false;
        }
    }
    void emit_md_add(Market& m, std::uint32_t instrument, std::uint64_t ref,
                     std::uint8_t side, std::int64_t qty, std::int64_t price) {
        // KOUCH side codes (1/2) map to KITCH side chars ('B'/'S').
        const std::uint8_t kside =
            side == kouch::kSideBuy ? kitch::kSideBuy : kitch::kSideSell;
        const kitch::AddOrder a{instrument, ref, kside,
                                static_cast<std::uint32_t>(qty),
                                static_cast<std::uint64_t>(price)};
        emit_md(m, kitch::MsgType::AddOrder, kitch::body_add(scratch_md_, a));
    }

    void insert_sorted(Sched&& ev) {
        auto it = std::lower_bound(scheduled_.begin(), scheduled_.end(), ev);
        scheduled_.insert(it, std::move(ev));
    }

    bool reorder_now() {
        return cfg_.faults.reorder_prob > 0.0 && uni_(rng_) < cfg_.faults.reorder_prob;
    }
    bool apply_faults(const Sched& ev) {
        if (cfg_.faults.disconnect_start_ns >= 0 &&
            ev.ts >= static_cast<std::uint64_t>(cfg_.faults.disconnect_start_ns) &&
            ev.ts <= static_cast<std::uint64_t>(cfg_.faults.disconnect_end_ns))
            return false;
        if (uni_(rng_) < cfg_.faults.drop_prob) return false;
        return true;
    }

    VenueConfig cfg_;
    std::unordered_map<std::uint32_t, Market> markets_;
    std::unordered_map<std::uint64_t, ClientPos> clients_;
    std::deque<Sched> scheduled_;
    std::mt19937_64 rng_{1};
    std::uniform_real_distribution<double> uni_{0.0, 1.0};

    std::optional<std::int64_t> delivered_bb_;
    std::optional<std::int64_t> delivered_ba_;
    std::uint64_t venue_ref_ = 0;
    std::uint64_t trade_ref_ = 0;
    std::uint64_t sched_seq_ = 0;
    std::uint32_t md_seq_ = 0;
    std::uint32_t resp_seq_ = 0;
    std::uint64_t event_ts_of_last_request_ = 0;

    std::uint8_t scratch_md_[kitch::kHeaderLen + 6 + 180 * kitch::kSnapshotEntryLen]{};
    std::uint8_t scratch_resp_[kouch::kRespHeaderLen + 64]{};
};

}  // namespace kestrel::sim
