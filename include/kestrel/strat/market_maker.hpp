#pragma once
// ============================================================================
// kestrel/strat/market_maker.hpp
//
// Passive market-maker strategy (spec §8, first strategy in the ordered list).
//
// Rules (deterministic, event-time only):
//   * Maintains one bid + one ask quote per instrument while feed is valid.
//   * Quotes join the best price when the spread is >= min_spread_ticks,
//     otherwise quote inside the spread (best_bid + 1 / best_ask - 1) as long
//     as that stays within the price band.
//   * Requotes when the market best moves by more than requote_ticks from the
//     order's quote-time best, or when the order leaves Live/PartialFill.
//   * Fills flip inventory; quoting size scales down as |position| approaches
//     the strategy's position cap (risk still enforces the hard cap).
//
// Constraints: strategies consume state and emit BOUNDED intents (kMaxIntents
// per invocation). They cannot bypass risk or touch networking. Client order
// ids are allocated by the ENGINE (deterministic counter) at intent
// application time — the strategy emits id=0 placeholders for new orders.
//
// Strategy demonstrations validate engine behavior; they do NOT establish
// profitable trading (spec §8).
// ============================================================================

#include <cstdint>

#include "kestrel/risk/risk.hpp"

namespace kestrel::strat {

enum class IntentKind : std::uint8_t { None, Enter, Cancel, Replace };

struct Intent {
    IntentKind kind = IntentKind::None;
    std::uint64_t client_id = 0;        // 0 = engine allocates (Enter/Replace-new)
    std::uint64_t orig_client_id = 0;   // Replace: old id
    std::uint32_t instrument = 0;
    risk::Side side = risk::Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price = 0;
};

inline constexpr std::size_t kMaxIntents = 8;

struct IntentSpan {
    Intent items[kMaxIntents];
    std::size_t count = 0;
    bool full() const noexcept { return count >= kMaxIntents; }
    void push(const Intent& i) noexcept {
        if (!full()) items[count++] = i;
    }
};

struct MarketMakerParams {
    std::int32_t quote_qty = 10;
    std::int64_t min_spread_ticks = 2;
    std::int64_t requote_ticks = 3;      // requote when best moved this far
    std::int64_t position_cap = 200;     // strategy-level soft cap (lots)
    std::int64_t price_min = 0;          // quoting band (mirrors risk band)
    std::int64_t price_max = 0;
};

/// One instrument slot of strategy state.
struct QuoteState {
    std::uint64_t bid_id = 0;            // live client ids (0 = none)
    std::uint64_t ask_id = 0;
    std::int64_t bid_quote_best = 0;     // market best at quote time
    std::int64_t ask_quote_best = 0;
};

template <std::size_t kMaxInstruments>
class MarketMakerStrategy {
public:
    void configure(std::uint32_t instrument, const MarketMakerParams& p) noexcept {
        if (instrument < kMaxInstruments) params_[instrument] = p;
    }

    /// Diagnostic read-only view (tests/bench/console): quote slot state.
    const QuoteState& quote_state(std::uint32_t instrument) const noexcept {
        return quotes_[instrument < kMaxInstruments ? instrument : 0];
    }

    void set_position(std::uint32_t instrument, std::int64_t pos) noexcept {
        if (instrument < kMaxInstruments) position_[instrument] = pos;
    }

    /// Called by the engine after a book update. Queries the book via the
    /// engine-provided accessor (compile-time interface — no virtuals).
    template <typename Book>
    void on_book_update(std::uint32_t instrument, const Book& book,
                        IntentSpan& out) noexcept {
        if (instrument >= kMaxInstruments) return;
        const MarketMakerParams& p = params_[instrument];
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (!bb || !ba) return;                    // one-sided: wait
        if ((*ba - *bb) < p.min_spread_ticks) return;   // too tight: wait
        QuoteState& q = quotes_[instrument];

        // Desired quote prices from ONE snapshot, computed PAIRED so the two
        // sides can never cross EACH OTHER (at spread==2 both naive "improve
        // by one tick" prices land on the same level — the venue would then
        // match our own bid against our own ask).
        const std::int64_t bid_want = (*bb + 1 < *ba) ? *bb + 1 : *bb;   // join if would cross
        std::int64_t ask_want = (*ba - 1 > *bb) ? *ba - 1 : *ba;         // join if would cross
        if (ask_want <= bid_want) ask_want = bid_want + 1;               // never self-cross

        // Manage the bid.
        manage_quote(instrument, risk::Side::Buy, bid_want, p, q, out);
        manage_quote(instrument, risk::Side::Sell, ask_want, p, q, out);
    }

    /// Called by the engine when an order reaches a terminal/changed state
    /// (fill, cancel, decline...) so strategy bookkeeping stays consistent.
    void on_order_gone(std::uint32_t instrument, std::uint64_t client_id) noexcept {
        if (instrument >= kMaxInstruments) return;
        QuoteState& q = quotes_[instrument];
        if (q.bid_id == client_id) q.bid_id = 0;
        if (q.ask_id == client_id) q.ask_id = 0;
    }

    /// Re-bind a quote slot to a live order after the fact (declined
    /// replace: the venue kept the ORIG resting, so the slot must point at
    /// it again, with its actual price for drift tracking).
    void rebind_quote(std::uint32_t instrument, risk::Side side,
                      std::uint64_t client_id, std::int64_t quote_price) noexcept {
        if (instrument >= kMaxInstruments) return;
        QuoteState& q = quotes_[instrument];
        if (side == risk::Side::Buy) {
            q.bid_id = client_id;
            q.bid_quote_best = quote_price;
        } else {
            q.ask_id = client_id;
            q.ask_quote_best = quote_price;
        }
    }

    /// Clear one side's slot (order gone before the engine could submit or
    /// bind: risk reject, OMS insert failure, dead replace orig). Without
    /// this the slot would stay kPendingMarker and the side would freeze.
    void clear_quote_side(std::uint32_t instrument, risk::Side side) noexcept {
        if (instrument >= kMaxInstruments) return;
        QuoteState& q = quotes_[instrument];
        if (side == risk::Side::Buy) {
            q.bid_id = 0;
            q.bid_quote_best = 0;
        } else {
            q.ask_id = 0;
            q.ask_quote_best = 0;
        }
    }

    /// Called by the engine after an Enter/Replace is submitted, binding the
    /// deterministic client id to the quote slot (resolves kPendingMarker).
    void bind_quote(std::uint32_t instrument, risk::Side side,
                    std::uint64_t client_id) noexcept {
        if (instrument >= kMaxInstruments) return;
        QuoteState& q = quotes_[instrument];
        if (side == risk::Side::Buy) q.bid_id = client_id;
        else q.ask_id = client_id;
    }

private:
    void manage_quote(std::uint32_t instrument, risk::Side side,
                      std::int64_t want,
                      const MarketMakerParams& p, QuoteState& q,
                      IntentSpan& out) noexcept {
        const std::uint64_t live_id = side == risk::Side::Buy ? q.bid_id : q.ask_id;
        if (live_id == kPendingMarker) return;   // in flight; wait for bind
        if (want < p.price_min || want > p.price_max) return;

        // Inventory-aware size: scale down toward the cap.
        const std::int64_t pos = position_[instrument];
        std::int64_t size = p.quote_qty;
        if (side == risk::Side::Buy && pos > 0)
            size = std::max<std::int64_t>(1, p.quote_qty -
                                                 (pos * p.quote_qty) / p.position_cap);
        if (side == risk::Side::Sell && pos < 0)
            size = std::max<std::int64_t>(1, p.quote_qty -
                                                 (-pos * p.quote_qty) / p.position_cap);

        const std::int64_t quote_best = side == risk::Side::Buy ? q.bid_quote_best
                                                                : q.ask_quote_best;
        // Requote when the touch moved >= requote_ticks away from our quote
        // in EITHER direction (the parameter documents "best moved this
        // far"). A one-sided test would leave quotes stale forever when the
        // market walks away from them.
        const std::int64_t drift =
            quote_best == 0 ? INT64_MAX / 2
                            : (side == risk::Side::Buy ? quote_best - want
                                                       : want - quote_best);
        const bool drifted =
            live_id != 0 && (drift < 0 ? -drift : drift) >= p.requote_ticks;
        if (live_id != 0 && !drifted) return;

        if (live_id != 0) {
            if (out.full()) return;
            // Replace keeps a resting order (venue acks U; old id cancelled).
            out.push(Intent{IntentKind::Replace, 0, live_id, instrument, side, size, want});
            if (side == risk::Side::Buy) q.bid_id = 0; else q.ask_id = 0;
        } else {
            if (out.full()) return;
            out.push(Intent{IntentKind::Enter, 0, 0, instrument, side, size, want});
        }
        if (side == risk::Side::Buy) {
            q.bid_id = kPendingMarker;   // set to real id by engine on submit
            q.bid_quote_best = want;
        } else {
            q.ask_id = kPendingMarker;
            q.ask_quote_best = want;
        }
        position_[instrument] = pos;   // unchanged here; engine pushes fills
    }

    inline static constexpr std::uint64_t kPendingMarker = 0xFFFF'FFFF'FFFF'FFFEull;

    MarketMakerParams params_[kMaxInstruments]{};
    QuoteState quotes_[kMaxInstruments]{};
    std::int64_t position_[kMaxInstruments]{};
};

}  // namespace kestrel::strat
