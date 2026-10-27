#pragma once
// ============================================================================
// kestrel/md/feed_handler.hpp
//
// Sequence validation, duplicate suppression, gap detection, recovery, and
// redundant A/B feed arbitration (spec §7).
//
// Model
// -----
// One market: one LevelBook fed by up to two channels (A/B). Channels carry
// the same per-market feed_seq space (redundant duplicate streams, like ITCH
// A/B feeds).
//
//   * First message establishes the sequence baseline (started_).
//   * seq <= applied_seq_                -> duplicate, dropped (either channel;
//                                          this is the A/B arbitration point:
//                                          first arrival wins, the redundant
//                                          copy is suppressed).
//   * seq == applied_seq_ + 1            -> applied (from whichever channel
//                                          delivers it first).
//   * seq >  applied_seq_ + 1            -> GAP: market enters Seeking. The
//                                          book is now INCONSISTENT: all
//                                          non-snapshot messages are dropped
//                                          (dropped_while_seeking) until the
//                                          documented recovery boundary.
//   * Recovery boundary: BookSnapshot 'B' -> book restored from the snapshot,
//                                          applied_seq_ = snapshot seq, state
//                                          = Synced. New exposure unblocks.
//
// While Seeking, is_synced() == false and the risk layer must block new
// exposure for this market (spec §7). Stats expose all counters.
//
// Determinism (ADR-0006): first-arrival-wins across A/B is a function of the
// journaled frame order; replay reproduces the same interleaving and hence
// the same decisions.
// ============================================================================

#include <cstdint>
#include <span>

#include "kestrel/book/level_book.hpp"
#include "kestrel/protocols/kitch.hpp"

namespace kitch = kestrel::protocols::kitch;
namespace kestrel::md {

enum class FeedState : std::uint8_t { Synced, Seeking };

struct FeedStats {
    std::uint64_t applied = 0;
    std::uint64_t duplicates = 0;         // seq <= applied (A/B suppression)
    std::uint64_t gaps = 0;               // forward jumps detected
    std::uint64_t dropped_while_seeking = 0;
    std::uint64_t snapshot_restores = 0;
    std::uint64_t rejects_decode = 0;     // malformed frames
    std::uint64_t rejects_anomaly = 0;    // over-execution, invalid price, etc.
    std::uint64_t pre_start_skipped = 0;  // frames dropped before first baseline
                                          // (not book-applicable while unstarted)
};

inline constexpr std::size_t kMaxSnapshotLevels = 512;

/// One market: two channels into one LevelBook.
template <std::size_t kMaxLevels = 4096, std::size_t kMaxOrders = 8192>
class FeedHandler {
public:
    using Book = book::LevelBook<kMaxLevels, kMaxOrders>;

    explicit FeedHandler(std::int64_t min_price) : book_(min_price) {}

    Book& book() noexcept { return book_; }
    const Book& book() const noexcept { return book_; }
    FeedState state() const noexcept { return state_; }
    bool is_synced() const noexcept { return state_ == FeedState::Synced; }
    const FeedStats& stats() const noexcept { return stats_; }
    std::uint32_t applied_seq() const noexcept { return applied_seq_; }
    bool started() const noexcept { return started_; }

    /// State restore (replay seeking): set sequence baseline + validity.
    /// Book contents must be restored separately (venue snapshot 'B').
    void restore_seq(std::uint32_t seq, bool started, bool synced) noexcept {
        applied_seq_ = seq;
        started_ = started;
        state_ = synced ? FeedState::Synced : FeedState::Seeking;
    }

    /// Process one KITCH payload (no outer u16 length prefix).
    /// `from_a` selects the channel (arbitration bookkeeping).
    void on_frame(std::span<const std::uint8_t> payload, bool from_a) noexcept {
        (void)from_a;   // channels are symmetric; arbitration is seq-based
        kitch::Header h{};
        if (kitch::decode_header(payload, h) != kitch::DecodeStatus::Ok) {
            ++stats_.rejects_decode;
            return;
        }
        if (h.type == kitch::MsgType::BookSnapshot) {
            apply_snapshot(payload, h);
            return;
        }
        if (!started_) {
            // Baseline: only accept the exact first message as-is.
            if (apply_body(payload, h)) {
                applied_seq_ = h.feed_seq;
                started_ = true;
                state_ = FeedState::Synced;
                ++stats_.applied;
            } else {
                ++stats_.pre_start_skipped;
            }
            return;
        }
        if (h.feed_seq <= applied_seq_) {
            ++stats_.duplicates;
            return;
        }
        if (state_ == FeedState::Seeking) {
            // Book inconsistent: nothing but a snapshot may touch it.
            ++stats_.dropped_while_seeking;
            return;
        }
        if (h.feed_seq != applied_seq_ + 1) {
            ++stats_.gaps;
            state_ = FeedState::Seeking;
            return;
        }
        if (apply_body(payload, h)) {
            applied_seq_ = h.feed_seq;
            ++stats_.applied;
        }
    }

private:
    bool apply_body(std::span<const std::uint8_t> payload, const kitch::Header& h) noexcept {
        using namespace kitch;
        switch (h.type) {
            case MsgType::AddOrder: {
                AddOrder m{};
                if (decode_add(payload, m) != DecodeStatus::Ok) { ++stats_.rejects_decode; return false; }
                const auto r = book_.add(
                    m.order_ref, m.side == kSideBuy ? book::Side::Buy : book::Side::Sell,
                    static_cast<std::int64_t>(m.qty), static_cast<std::int64_t>(m.price));
                if (r == book::BookResult::Ok) return true;
                ++stats_.rejects_anomaly;
                state_ = FeedState::Seeking;   // anomaly -> suspect until recovery
                return false;
            }
            case MsgType::OrderCancel: {
                OrderCancel m{};
                if (decode_cancel(payload, m) != DecodeStatus::Ok) { ++stats_.rejects_decode; return false; }
                const auto r =
                    book_.cancel(m.order_ref, static_cast<std::int64_t>(m.cancelled_qty));
                // Unknown cancels are tolerated (documented idempotent drop).
                return r == book::BookResult::Ok || r == book::BookResult::UnknownOrder;
            }
            case MsgType::OrderExecuted: {
                OrderExecuted m{};
                if (decode_exec(payload, m) != DecodeStatus::Ok) { ++stats_.rejects_decode; return false; }
                const auto r =
                    book_.execute(m.order_ref, static_cast<std::int64_t>(m.executed_qty));
                if (r == book::BookResult::Ok || r == book::BookResult::UnknownOrder) return true;
                ++stats_.rejects_anomaly;      // OverExecuted etc.
                state_ = FeedState::Seeking;
                return false;
            }
            case MsgType::OrderReplace: {
                OrderReplace m{};
                if (decode_replace(payload, m) != DecodeStatus::Ok) { ++stats_.rejects_decode; return false; }
                const auto r = book_.replace(m.orig_ref, m.new_ref,
                                             static_cast<std::int64_t>(m.new_qty),
                                             static_cast<std::int64_t>(m.new_price));
                if (r == book::BookResult::Ok) return true;
                ++stats_.rejects_anomaly;
                state_ = FeedState::Seeking;
                return false;
            }
            case MsgType::Trade:
                return true;      // non-printable trade: no book change
            case MsgType::SystemEvent:
                return true;
            default:
                ++stats_.rejects_decode;
                return false;
        }
    }

    void apply_snapshot(std::span<const std::uint8_t> payload, const kitch::Header& h) noexcept {
        kitch::SnapshotLevel wire[kMaxSnapshotLevels];
        std::uint16_t n = 0;
        if (kitch::decode_snapshot(payload, std::span<kitch::SnapshotLevel>(wire), n) !=
            kitch::DecodeStatus::Ok) {
            ++stats_.rejects_decode;   // includes n > kMaxSnapshotLevels
            return;
        }
        using SnapEntry = typename Book::SnapshotEntry;
        SnapEntry entries[kMaxSnapshotLevels];
        std::size_t cnt = 0;
        for (std::uint16_t i = 0; i < n; ++i) {
            entries[cnt++] = SnapEntry{
                wire[i].side == kitch::kSideBuy ? book::Side::Buy : book::Side::Sell,
                static_cast<std::int64_t>(wire[i].price),
                static_cast<std::int64_t>(wire[i].qty),
                wire[i].venue_ref};
        }
        if (book_.restore_from_snapshot(std::span<const SnapEntry>(entries, cnt))) {
            applied_seq_ = h.feed_seq;
            started_ = true;
            state_ = FeedState::Synced;
            ++stats_.snapshot_restores;
        }
    }

    Book book_;
    FeedState state_ = FeedState::Seeking;
    bool started_ = false;
    std::uint32_t applied_seq_ = 0;
    FeedStats stats_{};
};

}  // namespace kestrel::md
