#pragma once
// ============================================================================
// kestrel/risk/risk.hpp
//
// Pre-trade risk gate + deterministic token-bucket rate limiter.
//
// Reservation semantics (spec §8) — exact lifecycle:
//   * reserve()  : on risk approval of an Enter/Replace intent, BEFORE the
//                  order leaves the engine. Buys reserve notional
//                  (qty*price, tick units); sells reserve lots.
//   * fill()     : converts part of a reservation into position.
//   * release()  : on decline, cancel confirm, cancel-reject-with-no-order,
//                  replace swap (release old, reserve new), or send-failure
//                  abandonment.
//   * Outstanding (live) orders therefore reduce headroom; a fill moves
//     exposure from "pending" to "position" without double counting.
//
// Checks (in deterministic order, first failure wins):
//   1. kill switch        -> RejectKilled   (blocks new exposure immediately;
//                                            cancels remain allowed — they
//                                            are a separate workflow)
//   2. stale/invalid feed -> RejectStale    (is_synced() of the market)
//   3. token bucket       -> RejectRate     (event-time refill — DETERMINISTIC)
//   4. price band         -> RejectBand
//   5. order qty cap      -> RejectQty
//   6. order notional cap -> RejectNotional (checked 128-bit multiply)
//   7. position limit     -> RejectPosition (position + pending must fit)
//   8. exposure limit     -> RejectExposure (pending notional cap)
//
// Token bucket determinism: refill uses EVENT time (last observed feed/event
// timestamp), never wall clock (ADR-0006). Replay reproduces identical
// accept/reject decisions.
// ============================================================================

#include <cstdint>

#include "kestrel/book/instrument.hpp"

namespace kestrel::risk {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

// ---------------------------------------------------------------------------
// Deterministic token bucket (event-time).
// Capacity tokens, refill rate tokens/sec. Tokens are integer nano-tokens
// (1 token = 2^20 units) so refill math stays integer and deterministic.
// ---------------------------------------------------------------------------
class TokenBucket {
public:
    struct Config {
        std::int64_t capacity = 100;        // whole tokens
        std::int64_t refill_per_sec = 100;  // whole tokens per second
    };

    explicit TokenBucket(const Config& cfg) noexcept
        : capacity_(cfg.capacity << 20),
          // refill rate in 2^-40 token/ns units: keeps integer refill exact
          // enough that 1 token/s refills ~1.000 tokens per 1e9 ns.
          // ceil-division: never under-refill by truncation (over-estimate
          // is bounded by 1 unit per second — deterministic and documented).
          refill_per_ns_x_(static_cast<std::int64_t>(
              (static_cast<double>(cfg.refill_per_sec) * 1099511627776.0 + 1e9 - 1.0) /
              1e9)),
          refill_shift_(20) {   // (dt * x) >> 20 -> nano-tokens (token<<20)
        tokens_ = capacity_;
    }
    TokenBucket() noexcept : TokenBucket(Config{}) {}

    void reset() noexcept { tokens_ = capacity_; }

    /// Try to take `n` tokens at event time `now_ns`. Returns false when
    /// insufficient. Pure function of (tokens_, last_ns_, now).
    bool try_take(std::int64_t n, std::int64_t now_ns) noexcept {
        refill(now_ns);
        const std::int64_t need = n << 20;
        if (tokens_ < need) return false;
        tokens_ -= need;
        return true;
    }

    std::int64_t available_tokens_x1024() const noexcept { return tokens_; }
    std::int64_t capacity_tokens() const noexcept { return capacity_ >> 20; }

private:
    void refill(std::int64_t now_ns) noexcept {
        if (now_ns <= last_ns_) {
            if (last_ns_ == 0) last_ns_ = now_ns;   // first observation
            return;
        }
        const std::int64_t dt = now_ns - last_ns_;
        last_ns_ = now_ns;
        // dt <= ~8e12 stays within int64 after multiply (documented bound:
        // ~92 days between refills at 1e9 ns/s).
        const std::int64_t add = (dt * refill_per_ns_x_) >> refill_shift_;
        tokens_ = (tokens_ + add >= capacity_ || add < 0) ? capacity_ : tokens_ + add;
    }

    std::int64_t capacity_;       // nano-token units (token << 20)
    std::int64_t refill_per_ns_x_;
    int refill_shift_;
    std::int64_t tokens_ = 0;
    std::int64_t last_ns_ = 0;
};

// ---------------------------------------------------------------------------
struct RiskConfig {
    std::int64_t price_min = 0;         // band, absolute ticks
    std::int64_t price_max = 0;
    std::int64_t max_order_qty = 0;     // lots
    std::int64_t max_order_notional = 0;// tick units
    std::int64_t max_position = 0;      // lots (net short or long)
    std::int64_t max_pending_notional = 0;  // tick units (buy-side exposure)
    std::int64_t max_pending_lots = 0;      // sell-side exposure
    TokenBucket::Config rate{};         // deterministic rate limit
    std::int64_t max_orders_per_bucket = 1;
};

enum class Decision : std::uint8_t {
    Approved,
    RejectKilled,
    RejectStale,
    RejectRate,
    RejectBand,
    RejectQty,
    RejectNotional,
    RejectPosition,
    RejectExposure,
    Overflow,
};

struct RiskState {
    std::int64_t position = 0;            // filled lots (buy positive)
    std::int64_t pending_buy_notional = 0;// tick units reserved by live buys
    std::int64_t pending_buy_lots = 0;
    std::int64_t pending_sell_lots = 0;
    std::uint64_t killed_count = 0;
    std::uint64_t released_count = 0;
    std::uint64_t filled_count = 0;
};

/// Per-instrument risk gate.
class RiskGate {
public:
    explicit RiskGate(const RiskConfig& cfg) noexcept
        : cfg_(cfg), bucket_(cfg.rate) {}

    const RiskState& state() const noexcept { return st_; }
    RiskState& state() noexcept { return st_; }
    void set_feed_synced(bool synced) noexcept { feed_synced_ = synced; }
    bool feed_synced() const noexcept { return feed_synced_; }

    /// Kill switch: blocks NEW exposure at this event boundary (the call
    /// returns RejectKilled for every subsequent approve). Cancels are NOT
    /// blocked; their workflow (with ack tracking) lives in the OMS.
    void engage_kill_switch() noexcept { killed_ = true; }
    /// Disengage: explicitly cleared by a journaled CONTROL record (audited).
    void clear_kill_switch() noexcept { killed_ = false; }
    bool killed() const noexcept { return killed_; }

    /// Pre-trade check + reservation. `now_ns` is EVENT time.
    Decision approve(Side s, std::int64_t qty, std::int64_t price,
                     std::int64_t now_ns) noexcept {
        if (killed_) { ++st_.killed_count; return Decision::RejectKilled; }
        if (!feed_synced_) return Decision::RejectStale;
        if (!bucket_.try_take(cfg_.max_orders_per_bucket, now_ns))
            return Decision::RejectRate;
        if (price < cfg_.price_min || price > cfg_.price_max) return Decision::RejectBand;
        if (qty <= 0 || qty > cfg_.max_order_qty) return Decision::RejectQty;
        std::int64_t notional = 0;
        if (!book::checked_notional(qty, price, notional)) return Decision::Overflow;
        if (notional > cfg_.max_order_notional) return Decision::RejectNotional;

        if (s == Side::Buy) {
            // position + pending must stay within limits
            if (st_.position + st_.pending_buy_lots + qty > cfg_.max_position)
                return Decision::RejectPosition;
            if (st_.pending_buy_notional + notional > cfg_.max_pending_notional)
                return Decision::RejectExposure;
            st_.pending_buy_notional += notional;
            st_.pending_buy_lots += qty;
        } else {
            // net short position must stay within -max_position
            if (st_.position - st_.pending_sell_lots - qty < -cfg_.max_position)
                return Decision::RejectPosition;
            if (st_.pending_sell_lots + qty > cfg_.max_pending_lots)
                return Decision::RejectExposure;
            st_.pending_sell_lots += qty;
        }
        return Decision::Approved;
    }

    /// Exact reservation release (decline / cancel confirm / abandoned send).
    /// `qty`/`price` must be the ORDER's reserved quantity and price so the
    /// pending-notional ledger cancels exactly (no proportional estimates).
    void release(Side s, std::int64_t qty, std::int64_t price) noexcept {
        std::int64_t notional = 0;
        if (!book::checked_notional(qty, price, notional)) return;
        if (s == Side::Buy) {
            st_.pending_buy_lots -= qty;
            st_.pending_buy_notional -= notional;
            if (st_.pending_buy_lots < 0) st_.pending_buy_lots = 0;
            if (st_.pending_buy_notional < 0) st_.pending_buy_notional = 0;
        } else {
            st_.pending_sell_lots -= qty;
            if (st_.pending_sell_lots < 0) st_.pending_sell_lots = 0;
        }
        ++st_.released_count;
    }

    /// Fill: convert the reserved part into position. Reservations are
    /// released at the ORDER's price/qty (exact ledger); the position delta
    /// uses the actual fill price (may be better than the limit).
    void apply_fill(Side s, std::int64_t reserved_qty, std::int64_t reserved_price,
                    std::int64_t fill_qty, std::int64_t fill_price) noexcept {
        release(s, reserved_qty, reserved_price);
        if (s == Side::Buy) st_.position += fill_qty;
        else st_.position -= fill_qty;
        (void)fill_price;   // engine-level PnL lives in the positions service
        ++st_.filled_count;
    }

    /// Replace: atomic swap of reservations (old released, new reserved —
    /// the new side has already passed approve() semantics via the caller's
    /// replace intent check).
    void swap_reservation(Side s, std::int64_t old_qty, std::int64_t old_price,
                          std::int64_t new_qty, std::int64_t new_price) noexcept {
        release(s, old_qty, old_price);
        std::int64_t notional = 0;
        if (book::checked_notional(new_qty, new_price, notional)) {
            if (s == Side::Buy) {
                st_.pending_buy_notional += notional;
                st_.pending_buy_lots += new_qty;
            } else {
                st_.pending_sell_lots += new_qty;
            }
        }
    }

    TokenBucket& rate_limiter() noexcept { return bucket_; }

private:
    RiskConfig cfg_;
    TokenBucket bucket_;
    RiskState st_{};
    bool killed_ = false;
    bool feed_synced_ = false;
};

}  // namespace kestrel::risk
