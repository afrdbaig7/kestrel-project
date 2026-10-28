#pragma once
// ============================================================================
// kestrel/oms/oms.hpp
//
// Order management state machine (spec §8).
//
// States and transitions:
//   PendingNew   --ack(A)----------------------> Live
//                --decline(D)------------------> Rejected        [release]
//                --send-fail-abandon-----------> SendFailed      [release]
//   Live         --exec(E partial)-------------> PartialFill      [fill]
//                --exec(E full)----------------> Filled           [fill]
//                --cancel-request(C)-----------> PendingCancel
//   PartialFill  --exec(E full)----------------> Filled           [fill]
//                --exec(E partial)-------------> PartialFill      [fill]
//                --cancel-request(C)-----------> PendingCancel
//                --cancel-confirm(x)-----------> Cancelled        [release rem]
//   PendingCancel--exec(E)---------------------> apply fill, stay/st -> Done
//                --cancel-confirm(x)-----------> Cancelled        [release rem]
//                --cancel-reject(G)------------> back (Live/PartialFill)
//   ReplacePending(new id) --ack(U)-------------> Live(new) [swap reservations]
//                          --decline(D)---------> Rejected(new); orig restored
//   (replace submit also moves the orig to PendingCancel so a late in-flight
//    'A' ack for the orig can never re-ack it; 'U' then retires the orig to
//    Cancelled, and a declined replace restores the orig to Live — the venue
//    kept it resting.)
//
// Duplicate execution reports (spec §8): idempotent by cumulative fill
// accounting — an exec that would exceed the order's remaining qty, or a
// trade_ref equal to the LAST applied trade_ref, is a duplicate: ignored with
// a counter, inventory and reservations untouched. (The simulator emits
// strictly increasing trade refs; a duplicate is a transport anomaly.)
//
// Send failures (TxSink::kBusy persists / kError): the order REMAINS
// PendingNew and the engine retries next iteration with identical bytes
// (documented retry semantics). The strategy's client_id does not change, so
// a late duplicate ack is impossible; the venue treats the retried message
// idempotently by client_order_id (DuplicateClientOrderId -> Declined, which
// the OMS reconciles to the existing order).
// ============================================================================

#include <cstdint>

#include "kestrel/core/flat_map.hpp"
#include "kestrel/risk/risk.hpp"

namespace kestrel::oms {

// Terminal orders are ERASED from the map (bounded preallocated storage) so
// long sessions cannot saturate capacity with Filled/Cancelled rows. Erase
// is DEFERRED: transitions push the id into a bounded retire ring, drained
// by sweep_terminal() once per engine loop — response handlers may keep
// their OrderRecord pointers valid until the end of the event.
inline constexpr std::size_t kRetireRingCap = 256;

enum class OrderState : std::uint8_t {
    PendingNew,
    Live,
    PartialFill,
    Filled,
    Rejected,
    PendingCancel,
    Cancelled,
    SendFailed,
    ReplacePending,
};

inline const char* state_name(OrderState s) noexcept {
    switch (s) {
        case OrderState::PendingNew: return "PendingNew";
        case OrderState::Live: return "Live";
        case OrderState::PartialFill: return "PartialFill";
        case OrderState::Filled: return "Filled";
        case OrderState::Rejected: return "Rejected";
        case OrderState::PendingCancel: return "PendingCancel";
        case OrderState::Cancelled: return "Cancelled";
        case OrderState::SendFailed: return "SendFailed";
        case OrderState::ReplacePending: return "ReplacePending";
    }
    return "?";
}

struct OrderRecord {
    std::uint64_t client_id = 0;
    std::uint32_t instrument = 0;
    risk::Side side = risk::Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price = 0;
    std::int64_t filled = 0;
    OrderState state = OrderState::PendingNew;
    std::uint64_t last_trade_ref = 0;
    std::uint32_t retries = 0;
    bool replace_of = false;    // created by a replace (has orig)
    std::uint64_t orig_client_id = 0;
};

struct OmsStats {
    std::uint64_t submits = 0;
    std::uint64_t acks = 0;
    std::uint64_t declines = 0;
    std::uint64_t fills = 0;
    std::uint64_t duplicate_execs = 0;
    std::uint64_t cancel_requests = 0;
    std::uint64_t cancel_confirms = 0;
    std::uint64_t cancel_rejects = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t unknown_responses = 0;
    std::uint64_t send_retries = 0;
};

/// OMS works on OrderRecords; reservations/positions live in the RiskGate.
class Oms {
public:
    explicit Oms(std::size_t max_orders) : orders_(max_orders) {}

    const OrderRecord* find(std::uint64_t client_id) const noexcept {
        return orders_.find(client_id);
    }
    OrderRecord* find_mut(std::uint64_t client_id) noexcept {
        return orders_.find(client_id);
    }
    bool contains(std::uint64_t client_id) const noexcept {
        return orders_.contains(client_id);
    }
    std::size_t size() const noexcept { return orders_.size(); }
    const OmsStats& stats() const noexcept { return stats_; }

    /// Erase queued terminal records (call once per engine loop, BEFORE the
    /// batch is processed — bounded map ops, no allocation, no dangling
    /// handler pointers since nothing else holds records across events).
    void sweep_terminal() noexcept {
        for (std::size_t i = 0; i < retire_n_; ++i) {
            if (OrderRecord* r = orders_.find(retire_ring_[i])) {
                if (is_terminal_state(r->state)) orders_.erase(retire_ring_[i]);
            }
        }
        retire_n_ = 0;
    }
    std::uint64_t retire_overflow() const noexcept { return retire_overflow_; }

    /// Register a new pending order (after risk approval + reservation).
    bool track_new(const OrderRecord& rec) noexcept {
        if (!orders_.insert(rec.client_id, rec)) return false;
        ++stats_.submits;
        return true;
    }

    /// Mark as sent (bytes handed to TxSink). Failed submit keeps PendingNew.
    void mark_submit_result(std::uint64_t client_id, bool enqueued) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r) return;
        if (enqueued) {
            r->retries = 0;
        } else {
            ++r->retries;
            ++stats_.send_retries;
            // Abandonment is the engine's decision (bounded retries); see
            // abandon_pending().
        }
    }

    /// Bounded-retry policy: give up after N consecutive send failures,
    /// release the reservation, and record SendFailed (journaled CONTROL).
    bool abandon_pending(std::uint64_t client_id, std::uint32_t max_retries) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r || r->state != OrderState::PendingNew || r->retries < max_retries)
            return false;
        r->state = OrderState::SendFailed;
        ++stats_.send_failures;
        retire_later_(client_id);
        return true;
    }

    // ---- response handlers (return true when state changed) -----------------
    bool on_ack(std::uint64_t client_id) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r) { ++stats_.unknown_responses; return false; }
        if (r->state != OrderState::PendingNew && r->state != OrderState::ReplacePending) {
            ++stats_.unknown_responses;
            return false;
        }
        r->state = OrderState::Live;
        ++stats_.acks;
        return true;
    }

    bool on_decline(std::uint64_t client_id) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r) { ++stats_.unknown_responses; return false; }
        if (r->state == OrderState::PendingNew) {
            r->state = OrderState::Rejected;
            ++stats_.declines;
            retire_later_(client_id);
            return true;
        }
        if (r->state == OrderState::ReplacePending) {
            // Replace declined: new order dead; the venue kept the orig
            // resting, so restore it (unless it reached a terminal state in
            // the meantime — a raced fill/cancel wins).
            r->state = OrderState::Rejected;
            ++stats_.declines;
            retire_later_(client_id);
            if (OrderRecord* o = orders_.find(r->orig_client_id)) {
                if (o->state == OrderState::PendingCancel) {
                    o->state = OrderState::Live;
                }
            }
            return true;
        }
        ++stats_.unknown_responses;
        return false;
    }

    enum class ExecResult { Applied, Duplicate, Unknown, Overflow };

    /// Venue-initiated cancels (risk checks, churn in the simulator) deliver
    /// cancel CONFIRMS for orders in Live/PartialFill — not just our own
    /// PendingCancel requests. All three are valid confirm states.

    ExecResult on_exec(std::uint64_t client_id, std::int64_t exec_qty,
                       std::int64_t exec_price, std::uint64_t trade_ref) noexcept {
        (void)exec_price;  // PnL accounting lives in the position projection (Milestone E)
        OrderRecord* r = orders_.find(client_id);
        if (!r) { ++stats_.unknown_responses; return ExecResult::Unknown; }
        if (r->state != OrderState::Live && r->state != OrderState::PartialFill &&
            r->state != OrderState::PendingCancel &&
            r->state != OrderState::ReplacePending &&
            r->state != OrderState::Filled) {
            // Filled included: a further exec must reach the duplicate /
            // over-fill path below (documented anomaly semantics).
            ++stats_.unknown_responses;
            return ExecResult::Unknown;
        }
        // Duplicate detection: same last trade_ref, or over-fill.
        if (trade_ref != 0 && trade_ref == r->last_trade_ref) {
            ++stats_.duplicate_execs;
            return ExecResult::Duplicate;
        }
        const std::int64_t remaining = r->qty - r->filled;
        if (exec_qty <= 0 || exec_qty > remaining) {
            ++stats_.duplicate_execs;   // over-fill treated as anomaly/dup
            return ExecResult::Duplicate;
        }
        r->last_trade_ref = trade_ref;
        r->filled += exec_qty;
        const bool done = r->filled == r->qty;
        if (done) {
            r->state = OrderState::Filled;
            // A filled replace-child must WAIT for its 'U' (the orig's
            // reservation release and the strategy slot reconciliation
            // depend on it) — retirement happens in on_replaced.
            if (!r->replace_of) retire_later_(client_id);
        } else if (r->state != OrderState::PendingCancel) {
            r->state = OrderState::PartialFill;
        }
        ++stats_.fills;
        return ExecResult::Applied;
    }

    bool on_cancel_request(std::uint64_t client_id) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r) return false;
        if (r->state != OrderState::Live && r->state != OrderState::PartialFill)
            return false;
        r->state = OrderState::PendingCancel;
        ++stats_.cancel_requests;
        return true;
    }

    /// Cancel confirm carries the cancelled qty (may be partial if a fill
    /// raced the cancel — remaining then stays live per venue semantics;
    /// our simulator never does this, but the OMS handles it).
    enum class CancelConfirm { Full, Partial, Unknown };
    CancelConfirm on_cancel_confirm(std::uint64_t client_id,
                                    std::int64_t cancelled_qty) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r || (r->state != OrderState::PendingCancel &&
                   r->state != OrderState::Live &&
                   r->state != OrderState::PartialFill)) {
            ++stats_.unknown_responses;
            return CancelConfirm::Unknown;
        }
        const std::int64_t rem = r->qty - r->filled;
        if (cancelled_qty >= rem) {
            r->state = OrderState::Cancelled;
            ++stats_.cancel_confirms;
            retire_later_(client_id);
            return CancelConfirm::Full;
        }
        // Partial cancel confirm: order continues live for the remainder.
        r->state = rem > r->filled ? OrderState::PartialFill : OrderState::Filled;
        ++stats_.cancel_confirms;
        return CancelConfirm::Partial;
    }

    bool on_cancel_reject(std::uint64_t client_id) noexcept {
        OrderRecord* r = orders_.find(client_id);
        if (!r || r->state != OrderState::PendingCancel) {
            ++stats_.unknown_responses;
            return false;
        }
        r->state = r->filled > 0 ? OrderState::PartialFill : OrderState::Live;
        ++stats_.cancel_rejects;
        return true;
    }

    /// Register a replace: new id enters ReplacePending (risk approved the
    /// new reservation before submission). The orig moves to PendingCancel
    /// NOW (not on the venue's 'U'): an atomic replace means the orig must
    /// die, and leaving it ackable lets an in-flight 'A' ack re-bind a stale
    /// client id into the strategy (observed as ghost replaces in trace).
    /// The venue acks 'U' for the NEW id and cancels the old.
    bool track_replace(const OrderRecord& rec) noexcept {
        OrderRecord* orig = orders_.find(rec.orig_client_id);
        if (!orig) return false;
        if (orig->state != OrderState::Live &&
            orig->state != OrderState::PartialFill &&
            orig->state != OrderState::PendingNew &&
            orig->state != OrderState::PendingCancel) {
            return false;   // orig already terminal (filled/cancelled)
        }
        if (!orders_.insert(rec.client_id, rec)) return false;
        orig->state = OrderState::PendingCancel;
        ++stats_.submits;
        return true;
    }
    bool on_replaced(std::uint64_t new_client_id) noexcept {
        OrderRecord* r = orders_.find(new_client_id);
        if (!r) { ++stats_.unknown_responses; return false; }
        const bool late = r->state == OrderState::Live ||
                          r->state == OrderState::PartialFill ||
                          r->state == OrderState::Filled;
        if (r->state != OrderState::ReplacePending &&
            !(r->replace_of && late)) {
            // Not ours / already reconciled (duplicate 'U').
            ++stats_.unknown_responses;
            return false;
        }
        // A fill may have raced the 'U' (response-order inversion): the
        // venue rested the new order and filled it before we saw the ack.
        // Accept the late ack without disturbing the fill state.
        if (r->state == OrderState::ReplacePending) {
            r->state = OrderState::Live;
            ++stats_.acks;
        }
        // 'U' implies the venue cancelled the orig atomically: retire it.
        if (OrderRecord* o = orders_.find(r->orig_client_id)) {
            if (o->state == OrderState::PendingCancel) {
                o->state = OrderState::Cancelled;
                ++stats_.cancel_confirms;
                retire_later_(r->orig_client_id);
            }
        }
        if (r->state == OrderState::Filled) {
            retire_later_(new_client_id);   // filled child: now fully done
        }
        return true;
    }

private:
    static bool is_terminal_state(OrderState s) noexcept {
        return s == OrderState::Filled || s == OrderState::Rejected ||
               s == OrderState::Cancelled || s == OrderState::SendFailed;
    }
    void retire_later_(std::uint64_t client_id) noexcept {
        if (retire_n_ < kRetireRingCap) {
            retire_ring_[retire_n_++] = client_id;
        } else {
            ++retire_overflow_;   // record stays terminal in-map (leak-checked)
        }
    }

    core::FlatMap<OrderRecord> orders_;
    OmsStats stats_{};
    std::uint64_t retire_ring_[kRetireRingCap]{};
    std::size_t retire_n_ = 0;
    std::uint64_t retire_overflow_ = 0;
};

}  // namespace kestrel::oms
