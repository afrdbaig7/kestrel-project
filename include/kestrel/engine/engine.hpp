#pragma once
// ============================================================================
// kestrel/engine/engine.hpp
//
// One pinned thread, run-to-completion (spec §4). Each input event is fully
// processed — RX -> decode -> book update -> strategy -> pre-trade risk ->
// order encoding -> TX — before the next one; execution reports and session
// events feed back into the OMS and risk state.
//
// Post-init guarantees (enforced by allocation-guard tests):
//   * no heap allocation on the event path (all storage preallocated),
//   * no locks, no blocking, no synchronous I/O, no virtual dispatch on the
//     event chain (compile-time Backend/Strategy parameters),
//   * integer prices/quantities/notionals with documented scales and checked
//     multiplies (book/instrument.hpp).
//
// Journaling (ADR-0006): every nondeterministic input (MD, EXEC) is journaled
// BEFORE processing; outbound order bytes are journaled on successful TX
// submission. Critical overflow => fail-closed (kill switch + gap flag).
// Live time never reaches a decision path; decisions use event time.
//
// Exhaustion behavior (documented):
//   * intent buffer full            -> strategy re-emits on the next update
//                                      (bounded, counted)
//   * OMS map full                  -> intent dropped + counter + reservation
//                                      released (config bug if hit)
//   * TX kBusy                      -> bounded in-place retries, then a fixed
//                                      retry queue flushed on next run_once;
//                                      queue exhaustion abandons the order
//                                      (SendFailed + reservation release)
//   * journal critical overflow     -> kill switch (fail-closed)
// ============================================================================

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "kestrel/core/clock.hpp"
#include "kestrel/core/flat_map.hpp"
#include "kestrel/core/histogram.hpp"
#include "kestrel/journal/journal.hpp"
#include "kestrel/md/feed_handler.hpp"
#include "kestrel/net/backend.hpp"
#include "kestrel/oms/oms.hpp"
#include "kestrel/protocols/kitch.hpp"
#include "kestrel/protocols/kouch.hpp"
#include "kestrel/risk/risk.hpp"
#include "kestrel/strat/market_maker.hpp"

namespace kestrel::engine {

namespace kitch = kestrel::protocols::kitch;
namespace kouch = kestrel::protocols::kouch;
using oms::OrderRecord;
using oms::Oms;

inline constexpr std::size_t kMaxBatch = 64;
inline constexpr std::size_t kMaxMdRoutes = 16384;   // ext order_ref -> instrument
inline constexpr std::uint32_t kMaxSendRetries = 3;
inline constexpr std::size_t kMaxRetryQueue = 64;
inline constexpr std::size_t kMaxBookLevels = 32768;   // covers 20k-tick trading range
inline constexpr std::size_t kMaxBookOrders = 8192;

template <std::size_t kMaxInstruments>
struct EngineConfig {
    std::int64_t book_min_price[kMaxInstruments]{};
    risk::RiskConfig risk[kMaxInstruments]{};
    strat::MarketMakerParams strat[kMaxInstruments]{};
    std::size_t oms_capacity = 256;
};

template <std::size_t kMaxInstruments, typename Backend, typename Strategy,
          bool kStageTiming = false>
    requires net::RxSource<Backend> && net::TxSink<Backend>
class Engine {
public:
    using Feed = md::FeedHandler<kMaxBookLevels, kMaxBookOrders>;

    struct Stats {
        std::uint64_t frames_md = 0;
        std::uint64_t frames_exec = 0;
        std::uint64_t frames_md_invalid = 0;   // header/decode failures
        std::uint64_t md_route_misses = 0;     // unknown instrument/order ref
        std::uint64_t intents_generated = 0;
        std::uint64_t intents_rejected = 0;    // risk declines
        std::uint64_t intents_dropped = 0;     // oms map full
        std::uint64_t tx_enqueued = 0;
        std::uint64_t tx_retries = 0;
        std::uint64_t send_abandoned = 0;
        std::uint64_t kill_switch_events = 0;
        std::uint64_t journal_gaps = 0;
    };

    Engine(Backend& backend, const EngineConfig<kMaxInstruments>& cfg)
        : backend_(backend), oms_(cfg.oms_capacity), md_routes_(kMaxMdRoutes) {
        if (kStageTiming) {
            // 0..100 ms range covers pathological VM pauses; init-time alloc.
            h_tick_to_trade_ = std::make_unique<core::Histogram>(100'000'000, 3);
            h_journal_ = std::make_unique<core::Histogram>(100'000'000, 3);
            h_processing_ = std::make_unique<core::Histogram>(100'000'000, 3);
        }
        for (std::size_t i = 0; i < kMaxInstruments; ++i) {
            feeds_[i] = new (static_cast<void*>(&feed_storage_[i]))
                Feed(cfg.book_min_price[i]);
            risks_[i] = new (static_cast<void*>(&risk_storage_[i]))
                risk::RiskGate(cfg.risk[i]);
            strategy_.configure(static_cast<std::uint32_t>(i), cfg.strat[i]);
        }
    }
    ~Engine() {
        for (std::size_t i = 0; i < kMaxInstruments; ++i) {
            feeds_[i]->~Feed();
            risks_[i]->~RiskGate();
        }
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void attach_journal(journal::JournalWriter<>* jw) noexcept { journal_ = jw; }


    // ---- instrumentation access --------------------------------------------
    Feed& feed(std::size_t i) noexcept { return *feeds_[i]; }
    risk::RiskGate& risk_gate(std::size_t i) noexcept { return *risks_[i]; }
    Strategy& strategy() noexcept { return strategy_; }
    oms::Oms& oms_engine() noexcept { return oms_; }
    const Stats& stats() const noexcept { return stats_; }
    std::uint64_t last_event_ns() const noexcept { return last_event_ns_; }
    bool killed() const noexcept { return killed_; }
    std::uint64_t last_client_id() const noexcept { return next_client_id_; }

    /// Kill switch: blocks NEW exposure at this event boundary. Cancels are a
    /// separate workflow (spec §8) and remain permitted.
    void engage_kill_switch() noexcept {
        if (killed_) return;
        killed_ = true;
        ++stats_.kill_switch_events;
        for (std::size_t i = 0; i < kMaxInstruments; ++i) risks_[i]->engage_kill_switch();
        if (journal_) {
            const std::uint8_t code = 1;   // 1 = engage
            journal_->write_telemetry(journal::RecordType::Control,
                                      std::span<const std::uint8_t>(&code, 1));
        }
    }
    /// Disengage: journaled CONTROL record (audited path).
    void clear_kill_switch() noexcept {
        if (!killed_) return;
        killed_ = false;
        for (std::size_t i = 0; i < kMaxInstruments; ++i) risks_[i]->clear_kill_switch();
        if (journal_) {
            const std::uint8_t code = 2;   // 2 = clear
            journal_->write_telemetry(journal::RecordType::Control,
                                      std::span<const std::uint8_t>(&code, 1));
        }
    }

    // ---- main loop ----------------------------------------------------------
    /// Process up to `max_batch` input events (run-to-completion each).
    std::size_t run_once(std::size_t max_batch = kMaxBatch) noexcept {
        oms_.sweep_terminal();   // retire terminal orders (bounded, no alloc)
        flush_retry_queue();
        const std::size_t n = backend_.poll_rx(views_, max_batch < kMaxBatch ? max_batch : kMaxBatch);
        for (std::size_t i = 0; i < n; ++i) {
            const net::FrameView& v = views_[i];
            if (v.ts_event_ns > last_event_ns_) last_event_ns_ = v.ts_event_ns;
            const core::TscClock::ticks t_ev = kStageTiming ? core::TscClock::now() : 0;
            if (!journal_input(v)) break;      // fail-closed: kill switch set
            const core::TscClock::ticks t_in = kStageTiming ? core::TscClock::now() : 0;
            process_frame(v);
            if (kStageTiming) {
                const core::TscClock::ticks t_done = core::TscClock::now();
                h_journal_->record(t_in > t_ev ? t_in - t_ev : 0);
                h_tick_to_trade_->record(t_done > t_ev ? t_done - t_ev : 0);
                h_processing_->record(t_done > t_in ? t_done - t_in : 0);
            }
        }
        if (n) backend_.release_rx(n);
        return n;
    }

    /// Diagnostic stage histograms (only meaningful with kStageTiming=true).
    const core::Histogram* stage_tick_to_trade() const noexcept {
        return kStageTiming ? h_tick_to_trade_.get() : nullptr;
    }
    const core::Histogram* stage_journal() const noexcept {
        return kStageTiming ? h_journal_.get() : nullptr;
    }
    const core::Histogram* stage_processing() const noexcept {
        return kStageTiming ? h_processing_.get() : nullptr;
    }

private:
    using FeedStorage = alignas(Feed) unsigned char[sizeof(Feed)];
    using RiskStorage =
        alignas(risk::RiskGate) unsigned char[sizeof(risk::RiskGate)];

    // ---- journal ------------------------------------------------------------
    /// MD/EXEC records are journaled as [u64_be ts_event_ns][frame bytes] so a
    /// replay reconstructs both the bytes and the event-time ordering axis.
    bool journal_input(const net::FrameView& v) noexcept {
        if (!journal_) return true;
        const journal::RecordType t = v.kind == net::FrameKind::MarketData
                                          ? journal::RecordType::Md
                                          : journal::RecordType::Exec;
        std::uint8_t buf[8 + 4608];
        if (v.len > 4608) return false;
        core::ByteWriter w(buf);
        w.u64be(v.ts_event_ns);
        if (v.len) std::memcpy(buf + 8, v.data, v.len);
        if (journal_->write_critical(t, std::span<const std::uint8_t>(buf, 8 + v.len)))
            return true;
        // Critical journal overflow: fail-closed NOW, at this event boundary.
        ++stats_.journal_gaps;
        engage_kill_switch();
        return false;
    }

    // ---- frame dispatch -------------------------------------------------------
    void process_frame(const net::FrameView& v) noexcept {
        if (v.kind == net::FrameKind::MarketData) {
            ++stats_.frames_md;
            process_md(std::span<const std::uint8_t>(v.data, v.len));
        } else {
            ++stats_.frames_exec;
            process_exec(std::span<const std::uint8_t>(v.data, v.len));
        }
        // Run to completion: intents from THIS event go through risk and out.
        generate_and_submit_intents();
    }

    void process_md(std::span<const std::uint8_t> payload) noexcept {
        kitch::Header h{};
        if (kitch::decode_header(payload, h) != kitch::DecodeStatus::Ok) {
            ++stats_.frames_md_invalid;
            return;
        }
        const std::size_t idx = route_instrument(payload, h);
        if (idx >= kMaxInstruments) {
            ++stats_.md_route_misses;
            return;
        }
        feeds_[idx]->on_frame(payload, /*from_a=*/true);
        risks_[idx]->set_feed_synced(feeds_[idx]->is_synced());
        strategy_.set_position(static_cast<std::uint32_t>(idx),
                               risks_[idx]->state().position);
        intents_.count = 0;
        strategy_.on_book_update(static_cast<std::uint32_t>(idx), feeds_[idx]->book(),
                                 intents_);
        stats_.intents_generated += intents_.count;
    }

    std::size_t route_instrument(std::span<const std::uint8_t> payload,
                                 const kitch::Header& h) noexcept {
        using namespace kitch;
        std::uint32_t instrument = 0;
        switch (h.type) {
            case MsgType::AddOrder: {
                AddOrder m{};
                if (decode_add(payload, m) != DecodeStatus::Ok) return kMaxInstruments;
                instrument = m.instrument;
                md_routes_.insert(m.order_ref, instrument);
                break;
            }
            case MsgType::OrderReplace: {
                OrderReplace m{};
                if (decode_replace(payload, m) != DecodeStatus::Ok) return kMaxInstruments;
                const std::uint32_t* p = md_routes_.find(m.orig_ref);
                if (!p) return kMaxInstruments;
                instrument = *p;
                md_routes_.erase(m.orig_ref);
                md_routes_.insert(m.new_ref, instrument);
                break;
            }
            case MsgType::OrderCancel: {
                OrderCancel m{};
                if (decode_cancel(payload, m) != DecodeStatus::Ok) return kMaxInstruments;
                const std::uint32_t* p = md_routes_.find(m.order_ref);
                if (!p) return kMaxInstruments;
                instrument = *p;
                break;
            }
            case MsgType::OrderExecuted: {
                OrderExecuted m{};
                if (decode_exec(payload, m) != DecodeStatus::Ok) return kMaxInstruments;
                const std::uint32_t* p = md_routes_.find(m.order_ref);
                if (!p) return kMaxInstruments;
                instrument = *p;
                break;
            }
            case MsgType::Trade: {
                Trade m{};
                if (decode_trade(payload, m) != DecodeStatus::Ok) return kMaxInstruments;
                instrument = m.instrument;
                break;
            }
            case MsgType::BookSnapshot: {
                if (payload.size() < kHeaderLen + 4) return kMaxInstruments;
                core::ByteReader r(payload.subspan(kHeaderLen));
                if (r.u32be(instrument) != core::CodecStatus::Ok) return kMaxInstruments;
                break;
            }
            default:
                return kMaxInstruments;
        }
        if (instrument < kMaxInstruments) return static_cast<std::size_t>(instrument);
        return kMaxInstruments;
    }

    void process_exec(std::span<const std::uint8_t> payload) noexcept {
        using namespace protocols::kouch;
        RespHeader h{};
        if (decode_response_header(payload, h) != ReqDecode::Ok) return;
        switch (h.type) {
            case RespType::Accepted: {
                Accepted m{};
                if (decode_accepted(payload, m) != ReqDecode::Ok) return;
                if (oms_.on_ack(m.client_order_id)) {
                    strategy_.bind_quote(m.instrument,
                                         m.side == kSideBuy ? risk::Side::Buy : risk::Side::Sell,
                                         m.client_order_id);
                }
                break;
            }
            case RespType::Declined: {
                Declined m{};
                if (decode_declined(payload, m) != ReqDecode::Ok) return;
                OrderRecord* rec = oms_.find_mut(m.client_order_id);
                if (rec && oms_.on_decline(m.client_order_id)) {
                    const risk::Side s = rec->side;
                    const bool was_replace = rec->replace_of;
                    const std::uint64_t orig_id = rec->orig_client_id;
                    const std::int64_t q =
                        rec->replace_of ? rec->qty : rec->qty - rec->filled;
                    risks_[rec->instrument]->release(s, q, rec->price);
                    strategy_.on_order_gone(rec->instrument, m.client_order_id);
                    if (was_replace) {
                        // Declined atomic replace: the venue kept the orig
                        // resting (OMS restored it to Live). Re-bind the
                        // strategy slot to the orig so it is not orphaned.
                        // If the orig is ALREADY GONE (raced fill / venue
                        // cancel retired it), the slot must be CLEARED —
                        // leaving kPendingMarker freezes the side forever.
                        bool rebound = false;
                        if (const OrderRecord* o = oms_.find(orig_id);
                            o && (o->state == oms::OrderState::Live ||
                                  o->state == oms::OrderState::PartialFill)) {
                            strategy_.rebind_quote(rec->instrument, s, orig_id,
                                                   o->price);
                            rebound = true;
                        }
                        if (!rebound) {
                            strategy_.clear_quote_side(rec->instrument, s);
                        }
                    }
                    strategy_.set_position(rec->instrument,
                                           risks_[rec->instrument]->state().position);
                    intents_.count = 0;
                    strategy_.on_book_update(rec->instrument, feeds_[rec->instrument]->book(),
                                             intents_);
                    stats_.intents_generated += intents_.count;
                }
                break;
            }
            case RespType::Executed: {
                Executed m{};
                if (decode_executed(payload, m) != ReqDecode::Ok) return;
                OrderRecord* rec = oms_.find_mut(m.client_order_id);
                if (!rec) return;
                const auto res =
                    oms_.on_exec(m.client_order_id, static_cast<std::int64_t>(m.fill_qty),
                                 static_cast<std::int64_t>(m.fill_price), m.trade_ref);
                if (res == oms::Oms::ExecResult::Applied) {
                    risks_[rec->instrument]->apply_fill(
                        rec->side, static_cast<std::int64_t>(m.fill_qty), rec->price,
                        static_cast<std::int64_t>(m.fill_qty),
                        static_cast<std::int64_t>(m.fill_price));
                    strategy_.set_position(rec->instrument,
                                           risks_[rec->instrument]->state().position);
                    if (rec->state == oms::OrderState::Filled) {
                        strategy_.on_order_gone(rec->instrument, m.client_order_id);
                    }
                    intents_.count = 0;
                    strategy_.on_book_update(rec->instrument, feeds_[rec->instrument]->book(),
                                             intents_);
                    stats_.intents_generated += intents_.count;
                }
                break;
            }
            case RespType::Canceled: {
                Canceled m{};
                if (decode_canceled(payload, m) != ReqDecode::Ok) return;
                OrderRecord* rec = oms_.find_mut(m.client_order_id);
                if (!rec) return;
                const auto res = oms_.on_cancel_confirm(
                    m.client_order_id, static_cast<std::int64_t>(m.cancelled_qty));
                if (res == oms::Oms::CancelConfirm::Full) {
                    const std::int64_t rem = rec->qty - rec->filled;
                    risks_[rec->instrument]->release(rec->side, rem, rec->price);
                    strategy_.on_order_gone(rec->instrument, m.client_order_id);
                } else if (res == oms::Oms::CancelConfirm::Partial) {
                    risks_[rec->instrument]->release(
                        rec->side, static_cast<std::int64_t>(m.cancelled_qty), rec->price);
                }
                strategy_.set_position(rec->instrument,
                                       risks_[rec->instrument]->state().position);
                break;
            }
            case RespType::CancelReject: {
                CancelReject m{};
                if (decode_cancel_reject(payload, m) != ReqDecode::Ok) return;
                oms_.on_cancel_reject(m.client_order_id);
                break;
            }
            case RespType::Replaced: {
                Replaced m{};
                if (decode_replaced(payload, m) != ReqDecode::Ok) return;
                OrderRecord* rec = oms_.find_mut(m.new_client_id);
                if (!rec) return;
                if (oms_.on_replaced(m.new_client_id) && rec->replace_of) {
                    // 'U' implies the venue cancelled the orig (atomic
                    // replace): release the OLD reservation (remaining qty —
                    // the orig may have partially filled before the replace).
                    if (const OrderRecord* orig = oms_.find(rec->orig_client_id)) {
                        risks_[rec->instrument]->release(orig->side,
                                                         orig->qty - orig->filled,
                                                         orig->price);
                    }
                    strategy_.on_order_gone(rec->instrument, rec->orig_client_id);
                    if (rec->state == oms::OrderState::Filled) {
                        // A fill raced the 'U' (response-order inversion):
                        // the new order is already dead — clear the slot so
                        // the side re-quotes (binding it would ghost).
                        strategy_.clear_quote_side(rec->instrument, rec->side);
                    } else {
                        strategy_.bind_quote(rec->instrument, rec->side,
                                             m.new_client_id);
                    }
                }
                break;
            }
        }
    }

    // ---- intents -> risk -> encode -> TX --------------------------------------
    void generate_and_submit_intents() noexcept {
        for (std::size_t i = 0; i < intents_.count; ++i) {
            apply_intent(intents_.items[i]);
        }
        intents_.count = 0;
    }

    void apply_intent(const strat::Intent& it) noexcept {
        using namespace protocols::kouch;
        if (it.kind == strat::IntentKind::None || it.instrument >= kMaxInstruments) return;
        risk::RiskGate& rg = *risks_[it.instrument];

        switch (it.kind) {
            case strat::IntentKind::Enter: {
                const auto d = rg.approve(it.side, it.qty, it.price, event_now());
                if (d != risk::Decision::Approved) {
                    ++stats_.intents_rejected;
                    strategy_.clear_quote_side(it.instrument, it.side);
                    return;
                }
                const std::uint64_t id = next_client_id();
                OrderRecord rec;
                rec.client_id = id;
                rec.instrument = it.instrument;
                rec.side = it.side;
                rec.qty = it.qty;
                rec.price = it.price;
                rec.state = oms::OrderState::PendingNew;
                if (!oms_.track_new(rec)) {
                    ++stats_.intents_dropped;
                    rg.release(it.side, it.qty, it.price);
                    strategy_.clear_quote_side(it.instrument, it.side);
                    return;
                }
                Enter msg{};
                msg.client_order_id = id;
                msg.instrument = it.instrument;
                msg.side = it.side == risk::Side::Buy ? kSideBuy : kSideSell;
                msg.qty = static_cast<std::uint32_t>(it.qty);
                msg.price = static_cast<std::uint64_t>(it.price);
                msg.tif = kTifDay;
                submit_request(id, msg);
                track_pending(id);
                strategy_.bind_quote(it.instrument, it.side, id);
                break;
            }
            case strat::IntentKind::Cancel: {
                if (!oms_.on_cancel_request(it.client_id)) return;
                Cancel msg{it.client_id};
                submit_request(it.client_id, msg);
                break;
            }
            case strat::IntentKind::Replace: {
                const OrderRecord* orig = oms_.find(it.orig_client_id);
                if (!orig ||
                    (orig->state != oms::OrderState::Live &&
                     orig->state != oms::OrderState::PartialFill &&
                     orig->state != oms::OrderState::PendingNew &&
                     orig->state != oms::OrderState::PendingCancel)) {
                    // Orig already gone (raced fill/cancel): the strategy's
                    // slot points at a dead order — clear it so the side
                    // re-quotes on the next event. (A silent return here
                    // used to freeze the side at kPendingMarker forever.)
                    strategy_.clear_quote_side(it.instrument, it.side);
                    return;
                }
                // Replace re-checks risk for the NEW terms; the old reservation
                // is swapped on the venue's atomic 'U' ack.
                const auto d = rg.approve(it.side, it.qty, it.price, event_now());
                if (d != risk::Decision::Approved) {
                    ++stats_.intents_rejected;
                    // Orig untouched and still live: re-bind the slot to it.
                    strategy_.rebind_quote(it.instrument, it.side,
                                           orig->client_id, orig->price);
                    return;
                }
                const std::uint64_t new_id = next_client_id();
                OrderRecord rec;
                rec.client_id = new_id;
                rec.instrument = it.instrument;
                rec.side = it.side;
                rec.qty = it.qty;
                rec.price = it.price;
                rec.state = oms::OrderState::ReplacePending;
                rec.replace_of = true;
                rec.orig_client_id = it.orig_client_id;
                if (!oms_.track_replace(rec)) {
                    ++stats_.intents_dropped;
                    rg.release(it.side, it.qty, it.price);
                    // Insert failure = OMS full: orig still live, re-bind.
                    strategy_.rebind_quote(it.instrument, it.side,
                                           orig->client_id, orig->price);
                    return;
                }
                Replace msg{it.orig_client_id, new_id,
                            static_cast<std::uint32_t>(it.qty),
                            static_cast<std::uint64_t>(it.price)};
                submit_request(new_id, msg);
                track_pending(new_id);
                break;
            }
            case strat::IntentKind::None:
                break;
        }
    }

    /// Encode + submit with bounded retries; persistent kBusy goes to a fixed
    /// retry queue flushed on the next run_once (documented semantics).
    template <typename Msg>
    void submit_request(std::uint64_t client_id, const Msg& msg) noexcept {
        std::uint8_t buf[kouch::kLenReplace];   // max request size
        const std::size_t len = encode_any(buf, msg);
        if (len == 0) return;
        for (std::uint32_t attempt = 0; attempt <= kMaxSendRetries; ++attempt) {
            const net::TxResult r = backend_.tx_submit(buf, len);
            if (r == net::TxResult::kEnqueued) {
                ++stats_.tx_enqueued;
                oms_.mark_submit_result(client_id, true);
                journal_tx(buf, len);
                return;
            }
            ++stats_.tx_retries;
        }
        oms_.mark_submit_result(client_id, false);
        push_retry_queue(client_id, buf, len);
    }

    static std::size_t encode_any(std::uint8_t* buf, const kouch::Enter& m) noexcept {
        return kouch::encode_enter(std::span<std::uint8_t>(buf, kouch::kLenEnter), m);
    }
    static std::size_t encode_any(std::uint8_t* buf, const kouch::Cancel& m) noexcept {
        return kouch::encode_cancel(std::span<std::uint8_t>(buf, kouch::kLenCancel), m);
    }
    static std::size_t encode_any(std::uint8_t* buf, const kouch::Replace& m) noexcept {
        return kouch::encode_replace(std::span<std::uint8_t>(buf, kouch::kLenReplace), m);
    }

    void journal_tx(const std::uint8_t* buf, std::size_t len) noexcept {
        if (!journal_) return;
        journal_->write_critical(journal::RecordType::Tx,
                                 std::span<const std::uint8_t>(buf, len));
    }

    struct RetryItem {
        std::uint64_t client_id = 0;
        std::uint16_t len = 0;
        std::uint8_t bytes[kouch::kLenReplace]{};
    };

    void push_retry_queue(std::uint64_t client_id, const std::uint8_t* bytes,
                          std::size_t len) noexcept {
        if (retry_count_ >= kMaxRetryQueue) {
            // Bounded queue exhausted: abandon (documented exhaustion path).
            abandon_send(client_id);
            return;
        }
        RetryItem& it = retry_queue_[retry_count_++];
        it.client_id = client_id;
        it.len = static_cast<std::uint16_t>(len);
        std::memcpy(it.bytes, bytes, len);
    }

    // ---- pending-id tracking (for state snapshots) ---------------------------
    static constexpr std::size_t kMaxPendingIds = 128;
    void track_pending(std::uint64_t id) noexcept {
        if (pending_count_ < kMaxPendingIds) pending_ids_[pending_count_++] = id;
    }
    void prune_pending() noexcept {
        std::size_t i = 0;
        while (i < pending_count_) {
            const OrderRecord* r = oms_.find(pending_ids_[i]);
            const bool terminal = !r || r->state == oms::OrderState::Filled ||
                                  r->state == oms::OrderState::Cancelled ||
                                  r->state == oms::OrderState::Rejected ||
                                  r->state == oms::OrderState::SendFailed;
            if (terminal) pending_ids_[i] = pending_ids_[--pending_count_];
            else ++i;
        }
    }

    void abandon_send(std::uint64_t client_id) noexcept {
        OrderRecord* rec = oms_.find_mut(client_id);
        if (rec && rec->state == oms::OrderState::PendingNew) {
            rec->state = oms::OrderState::SendFailed;
            risks_[rec->instrument]->release(rec->side, rec->qty - rec->filled, rec->price);
            strategy_.on_order_gone(rec->instrument, client_id);
            ++stats_.send_abandoned;
        }
    }

    void flush_retry_queue() noexcept {
        std::size_t i = 0;
        while (i < retry_count_) {
            RetryItem& it = retry_queue_[i];
            const net::TxResult r = backend_.tx_submit(it.bytes, it.len);
            if (r == net::TxResult::kEnqueued) {
                ++stats_.tx_enqueued;
                oms_.mark_submit_result(it.client_id, true);
                journal_tx(it.bytes, it.len);
                retry_queue_[i] = retry_queue_[--retry_count_];
                continue;
            }
            ++i;
        }
    }

    std::uint64_t event_now() const noexcept { return last_event_ns_; }
    std::uint64_t next_client_id() noexcept { return ++next_client_id_; }

public:
    // ---- state snapshot (journal SNAPSHOT records; replay-debugger seeking) ---
    // Layout (v1):
    //   u16 version | u64 last_event_ns | u64 next_client_id
    //   per instrument: i64 position | i64 pending_buy_lots | i64 pending_buy_notional
    //                   | i64 pending_sell_lots | u32 applied_seq | u8 started | u8 synced
    //   u16 live_orders | live_orders x OrderRecordPOD (client_id, instrument,
    //       side, qty, price, filled, state, retries, replace_of, orig)
    // Book state is NOT serialized: seeking replays books from the venue 'B'
    // snapshot record nearest the seek point (documented in docs/replay.md).
    static constexpr std::uint16_t kStateVersion = 1;
    std::size_t save_state(std::span<std::uint8_t> out) noexcept {
        core::ByteWriter w(out);
        if (w.u16le(kStateVersion) != core::CodecStatus::Ok ||
            w.u64le(last_event_ns_) != core::CodecStatus::Ok ||
            w.u64le(next_client_id_) != core::CodecStatus::Ok) return 0;
        for (std::size_t i = 0; i < kMaxInstruments; ++i) {
            const risk::RiskState& rs = risks_[i]->state();
            const Feed& fh = *feeds_[i];
            if (w.u64le(static_cast<std::uint64_t>(rs.position)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(rs.pending_buy_lots)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(rs.pending_buy_notional)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(rs.pending_sell_lots)) != core::CodecStatus::Ok ||
                w.u32le(fh.applied_seq()) != core::CodecStatus::Ok ||
                w.u8(fh.started() ? 1 : 0) != core::CodecStatus::Ok ||
                w.u8(fh.is_synced() ? 1 : 0) != core::CodecStatus::Ok) return 0;
        }
        // Non-terminal OMS records (lazy prune keeps the list bounded).
        prune_pending();
        if (w.u16le(static_cast<std::uint16_t>(pending_count_)) != core::CodecStatus::Ok) return 0;
        for (std::size_t i = 0; i < pending_count_; ++i) {
            const OrderRecord* r = oms_.find(pending_ids_[i]);
            if (!r) continue;
            if (w.u64le(r->client_id) != core::CodecStatus::Ok ||
                w.u32le(r->instrument) != core::CodecStatus::Ok ||
                w.u8(static_cast<std::uint8_t>(r->side)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(r->qty)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(r->price)) != core::CodecStatus::Ok ||
                w.u64le(static_cast<std::uint64_t>(r->filled)) != core::CodecStatus::Ok ||
                w.u8(static_cast<std::uint8_t>(r->state)) != core::CodecStatus::Ok ||
                w.u64le(r->last_trade_ref) != core::CodecStatus::Ok ||
                w.u8(r->replace_of ? 1 : 0) != core::CodecStatus::Ok ||
                w.u64le(r->orig_client_id) != core::CodecStatus::Ok) return 0;
        }
        return w.written();
    }

    bool load_state(std::span<const std::uint8_t> in) noexcept {
        core::ByteReader r(in);
        std::uint16_t ver = 0;
        if (r.u16le(ver) != core::CodecStatus::Ok || ver != kStateVersion) return false;
        std::uint64_t ev = 0, cid = 0;
        if (r.u64le(ev) != core::CodecStatus::Ok || r.u64le(cid) != core::CodecStatus::Ok)
            return false;
        last_event_ns_ = ev;
        next_client_id_ = cid;
        for (std::size_t i = 0; i < kMaxInstruments; ++i) {
            std::uint64_t pos = 0, pbl = 0, pbn = 0, psl = 0;
            std::uint32_t seq = 0;
            std::uint8_t started = 0, synced = 0;
            if (r.u64le(pos) != core::CodecStatus::Ok ||
                r.u64le(pbl) != core::CodecStatus::Ok ||
                r.u64le(pbn) != core::CodecStatus::Ok ||
                r.u64le(psl) != core::CodecStatus::Ok ||
                r.u32le(seq) != core::CodecStatus::Ok ||
                r.u8(started) != core::CodecStatus::Ok ||
                r.u8(synced) != core::CodecStatus::Ok) return false;
            risk::RiskState& rs = risks_[i]->state();
            rs.position = static_cast<std::int64_t>(pos);
            rs.pending_buy_lots = static_cast<std::int64_t>(pbl);
            rs.pending_buy_notional = static_cast<std::int64_t>(pbn);
            rs.pending_sell_lots = static_cast<std::int64_t>(psl);
            feeds_[i]->restore_seq(seq, started != 0, synced != 0);
            strategy_.set_position(static_cast<std::uint32_t>(i), rs.position);
        }
        std::uint16_t live = 0;
        if (r.u16le(live) != core::CodecStatus::Ok) return false;
        pending_count_ = 0;
        for (std::uint16_t i = 0; i < live; ++i) {
            OrderRecord rec{};
            std::uint64_t cid2 = 0, qty = 0, price = 0, filled = 0, tref = 0, orig = 0;
            std::uint32_t instr = 0;
            std::uint8_t side = 0, state = 0, rof = 0;
            if (r.u64le(cid2) != core::CodecStatus::Ok ||
                r.u32le(instr) != core::CodecStatus::Ok ||
                r.u8(side) != core::CodecStatus::Ok ||
                r.u64le(qty) != core::CodecStatus::Ok ||
                r.u64le(price) != core::CodecStatus::Ok ||
                r.u64le(filled) != core::CodecStatus::Ok ||
                r.u8(state) != core::CodecStatus::Ok ||
                r.u64le(tref) != core::CodecStatus::Ok ||
                r.u8(rof) != core::CodecStatus::Ok ||
                r.u64le(orig) != core::CodecStatus::Ok) return false;
            rec.client_id = cid2;
            rec.instrument = instr;
            rec.side = static_cast<risk::Side>(side);
            rec.qty = static_cast<std::int64_t>(qty);
            rec.price = static_cast<std::int64_t>(price);
            rec.filled = static_cast<std::int64_t>(filled);
            rec.state = static_cast<oms::OrderState>(state);
            rec.last_trade_ref = tref;
            rec.replace_of = rof != 0;
            rec.orig_client_id = orig;
            if (!oms_.track_new(rec)) return false;
            track_pending(cid2);
        }
        return true;
    }

    /// Write a SNAPSHOT record (telemetry policy) if a journal is attached.
    void write_snapshot_record() noexcept {
        if (!journal_) return;
        std::uint8_t buf[8192];
        const std::size_t n = save_state(buf);
        if (n) journal_->write_telemetry(journal::RecordType::Snapshot, {buf, n});
    }

private:
    Backend& backend_;
    Strategy strategy_;
    oms::Oms oms_;
    core::FlatMap<std::uint32_t> md_routes_;
    journal::JournalWriter<>* journal_ = nullptr;

    FeedStorage feed_storage_[kMaxInstruments];
    RiskStorage risk_storage_[kMaxInstruments];
    Feed* feeds_[kMaxInstruments];
    risk::RiskGate* risks_[kMaxInstruments];

    strat::IntentSpan intents_{};
    net::FrameView views_[kMaxBatch]{};
    RetryItem retry_queue_[kMaxRetryQueue]{};
    std::size_t retry_count_ = 0;
    std::uint64_t pending_ids_[kMaxPendingIds]{};
    std::size_t pending_count_ = 0;

    std::uint64_t next_client_id_ = 0;
    std::uint64_t last_event_ns_ = 0;
    bool killed_ = false;
    Stats stats_{};

    // Diagnostic stage timing (empty when !kStageTiming).
    std::unique_ptr<core::Histogram> h_tick_to_trade_;
    std::unique_ptr<core::Histogram> h_journal_;
    std::unique_ptr<core::Histogram> h_processing_;
};

}  // namespace kestrel::engine
