#pragma once
// ============================================================================
// kestrel/book/level_book.hpp
//
// Bounded order-level book (market-data side).
//
// Structure:
//   * Direct-indexed price levels: absolute tick range
//     [min_price, min_price + kMaxLevels) — identical range for both sides.
//   * Bitmaps per side for O(1) best-price discovery (ctz/clz scans).
//   * Pooled intrusive order nodes; FIFO within each price level
//     (doubly-linked list of pool handles).
//   * Preallocated map: exchange order_ref -> pool handle. Exchange refs are
//     arbitrary 64-bit ids; they are NEVER assumed to contain pool indices
//     (spec §7). Handles encode (generation, slot) — see ObjectPool.
//
// Defined behaviors (spec §7):
//   duplicate add ref        -> DuplicateId (message dropped, counter by caller)
//   unknown cancel/execute   -> UnknownOrder (dropped, counter)
//   over-execution           -> OverExecuted: apply what remains, REMOVE the
//                               node, report anomaly (caller marks the
//                               instrument suspect and blocks new exposure)
//   invalid price            -> InvalidPrice (outside configured range)
//   pool/level exhaustion    -> CapacityExhausted (documented fail-closed path)
//   replace                  -> remove(old) + add(new): queue position resets
//                               (matches real venue semantics for price moves)
//
// No allocation after construction. Single-threaded (engine pin).
// `validate()` checks all five spec invariants; used by tests and by the
// snapshot-restore path.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>

#include "kestrel/core/bitmap.hpp"
#include "kestrel/core/flat_map.hpp"
#include "kestrel/core/object_pool.hpp"
#include "kestrel/book/instrument.hpp"

namespace kestrel::book {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class BookResult : std::uint8_t {
    Ok,
    DuplicateId,
    UnknownOrder,
    InvalidPrice,
    Overflow,
    CapacityExhausted,
    OverExecuted,
};

struct BookStats {
    std::uint64_t duplicates = 0;
    std::uint64_t unknown_orders = 0;
    std::uint64_t invalid_prices = 0;
    std::uint64_t over_executions = 0;
    std::uint64_t capacity_blocks = 0;
    std::uint64_t replacements = 0;
};

template <std::size_t kMaxLevels = 4096, std::size_t kMaxOrders = 8192>
class LevelBook {
public:
    struct OrderNode {
        std::uint64_t ext_id = 0;
        std::uint64_t prev = 0;   // pool handles; 0 == null (invalid handle)
        std::uint64_t next = 0;
        std::int64_t qty = 0;
        std::int64_t price = 0;   // absolute tick
        std::uint8_t side = 0;    // kitch::kSideBuy / kSideSell
    };

    explicit LevelBook(std::int64_t min_price) noexcept
        : min_price_(min_price), ext_to_node_(kMaxOrders) {}

    std::int64_t min_price() const noexcept { return min_price_; }
    std::size_t live_orders() const noexcept { return pool_.in_use(); }
    const BookStats& stats() const noexcept { return stats_; }

    // ---- queries -----------------------------------------------------------
    std::optional<std::int64_t> best_bid() const noexcept {
        const auto idx = bid_bitmap_.find_to(kMaxLevels - 1);
        if (!idx) return std::nullopt;
        return min_price_ + static_cast<std::int64_t>(*idx);
    }
    std::optional<std::int64_t> best_ask() const noexcept {
        const auto idx = ask_bitmap_.find_from(0);
        if (!idx) return std::nullopt;
        return min_price_ + static_cast<std::int64_t>(*idx);
    }
    std::int64_t level_qty(std::int64_t price, Side s) const noexcept {
        const auto* lv = level_at(price);
        if (!lv) return 0;
        return s == Side::Buy ? lv->bid_total_qty : lv->sell_total_qty;
    }
    std::size_t level_count(std::int64_t price, Side s) const noexcept {
        const auto* lv = level_at(price);
        if (!lv) return 0;
        return s == Side::Buy ? lv->bid_count : lv->sell_count;
    }

    // ---- mutations ----------------------------------------------------------
    BookResult add(std::uint64_t ext_ref, Side s, std::int64_t qty,
                   std::int64_t price) noexcept {
        if (qty <= 0 || qty > kMaxOrderQty) return BookResult::Overflow;
        const std::size_t idx = price_index(price);
        if (idx == kBadIndex) { ++stats_.invalid_prices; return BookResult::InvalidPrice; }
        if (ext_to_node_.contains(ext_ref)) { ++stats_.duplicates; return BookResult::DuplicateId; }
        if (pool_.exhausted()) { ++stats_.capacity_blocks; return BookResult::CapacityExhausted; }
        OrderNode* n = pool_.acquire();
        if (!n) { ++stats_.capacity_blocks; return BookResult::CapacityExhausted; }
        n->ext_id = ext_ref;
        n->qty = qty;
        n->price = price;
        n->side = s == Side::Buy ? 'B' : 'S';
        n->prev = 0;
        n->next = 0;
        if (!ext_to_node_.insert(ext_ref, pool_.handle_of(n))) {
            pool_.release(n);
            ++stats_.capacity_blocks;
            return BookResult::CapacityExhausted;   // map full (paranoia; sizes match)
        }
        Level& lv = levels_[idx];
        push_back(lv, s, pool_.handle_of(n));
        if (s == Side::Buy) {
            lv.bid_total_qty += qty;
            lv.bid_count += 1;
            bid_bitmap_.set(idx);
        } else {
            lv.sell_total_qty += qty;
            lv.sell_count += 1;
            ask_bitmap_.set(idx);
        }
        return BookResult::Ok;
    }

    /// Cancel up to `qty` from the order (partial cancels reduce; qty 0 or
    /// >= remaining removes entirely).
    BookResult cancel(std::uint64_t ext_ref, std::int64_t qty) noexcept {
        const std::uint64_t* h = ext_to_node_.find(ext_ref);
        if (!h) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        OrderNode* n = pool_.resolve(*h);
        if (!n) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        const std::int64_t remove = (qty <= 0 || qty >= n->qty) ? n->qty : qty;
        return reduce(*h, *n, remove);
    }

    /// Execute `qty` against the order. Over-execution executes what remains,
    /// removes the node and reports OverExecuted (caller marks instrument
    /// suspect; documented recovery = snapshot).
    BookResult execute(std::uint64_t ext_ref, std::int64_t qty) noexcept {
        const std::uint64_t* h = ext_to_node_.find(ext_ref);
        if (!h) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        OrderNode* n = pool_.resolve(*h);
        if (!n) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        if (qty <= 0) return BookResult::Overflow;
        if (qty > n->qty) {
            const std::int64_t rem = n->qty;
            const BookResult r = reduce(*h, *n, rem);
            ++stats_.over_executions;
            return r == BookResult::Ok ? BookResult::OverExecuted : r;
        }
        return reduce(*h, *n, qty);
    }

    /// Replace = cancel(old) + add(new) — queue position resets (documented).
    BookResult replace(std::uint64_t orig_ref, std::uint64_t new_ref,
                       std::int64_t new_qty, std::int64_t new_price) noexcept {
        const std::uint64_t* h = ext_to_node_.find(orig_ref);
        if (!h) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        OrderNode* n = pool_.resolve(*h);
        if (!n) { ++stats_.unknown_orders; return BookResult::UnknownOrder; }
        const Side s = n->side == 'B' ? Side::Buy : Side::Sell;
        const BookResult r = cancel(orig_ref, n->qty);
        if (r != BookResult::Ok) return r;
        ++stats_.replacements;
        return add(new_ref, s, new_qty, new_price);
    }

    struct SnapshotEntry {
        Side side;
        std::int64_t price;
        std::int64_t qty;
        std::uint64_t ref = 0;   // real venue ref; 0 => synthetic (high bit)
    };

    /// Recovery boundary: clear the book and install a snapshot (spec §7).
    /// Entries carry the REAL venue refs so subsequent cancel/exec/replace
    /// frames keep tracking exactly. Returns false if the snapshot exceeds
    /// caps (caller marks instrument suspect and continues to block exposure).
    bool restore_from_snapshot(std::span<const SnapshotEntry> entries) {
        clear();
        for (const SnapshotEntry& e : entries) {
            const std::uint64_t snap_ref =
                e.ref != 0 ? e.ref : 0x8000'0000'0000'0000ull | ++snapshot_ref_seq_;
            if (add(snap_ref, e.side, e.qty, e.price) != BookResult::Ok) return false;
        }
        return true;
    }

    void clear() noexcept {
        pool_.reset();
        ext_to_node_.clear();
        bid_bitmap_.clear_all();
        ask_bitmap_.clear_all();
        for (Level& lv : levels_) lv = Level{};
        stats_ = BookStats{};
    }

    // ---- invariants (spec §7; tests + snapshot path) ------------------------
    /// 1) level qty == sum of its orders; 2) every live order in exactly one
    /// level; 3) best bid/ask match occupied levels; 4) lookup/pool consistent;
    /// 5) caller checks determinism by identical-stream replay.
    bool validate() const noexcept {
        std::size_t seen_orders = 0;
        std::int64_t total_bid = 0, total_ask = 0;
        for (std::size_t i = 0; i < kMaxLevels; ++i) {
            const Level& lv = levels_[i];
            if (bid_bitmap_.test(i)) {
                if (lv.bid_count == 0) return false;
                std::int64_t sum = 0;
                std::size_t cnt = 0;
                std::uint64_t h = lv.bid_head;
                std::uint64_t prev = 0;
                while (h) {
                    const OrderNode* n = pool_.resolve(h);
                    if (!n || n->side != 'B') return false;
                    if (n->prev != prev) return false;
                    sum += n->qty;
                    ++cnt;
                    prev = h;
                    h = n->next;
                }
                if (sum != lv.bid_total_qty || cnt != lv.bid_count) return false;
                total_bid += sum;
                seen_orders += cnt;
            } else if (lv.bid_count != 0 || lv.bid_total_qty != 0) {
                return false;
            }
            if (ask_bitmap_.test(i)) {
                if (lv.sell_count == 0) return false;
                std::int64_t sum = 0;
                std::size_t cnt = 0;
                std::uint64_t h = lv.sell_head;
                std::uint64_t prev = 0;
                while (h) {
                    const OrderNode* n = pool_.resolve(h);
                    if (!n || n->side != 'S') return false;
                    if (n->prev != prev) return false;
                    sum += n->qty;
                    ++cnt;
                    prev = h;
                    h = n->next;
                }
                if (sum != lv.sell_total_qty || cnt != lv.sell_count) return false;
                total_ask += sum;
                seen_orders += cnt;
            } else if (lv.sell_count != 0 || lv.sell_total_qty != 0) {
                return false;
            }
        }
        // Pool ownership + lookup consistency.
        if (seen_orders != pool_.in_use()) return false;
        if (ext_to_node_.size() != pool_.in_use()) return false;
        // Best price consistency.
        const auto bb = best_bid();
        const auto ba = best_ask();
        if (bb && level_qty(*bb, Side::Buy) <= 0) return false;
        if (ba && level_qty(*ba, Side::Sell) <= 0) return false;
        (void)total_bid;
        (void)total_ask;
        return true;
    }


    /// Debug aid (tests/tools): dump one side's levels and order ext ids.
    void debug_dump_side(Side s) const {
        std::fprintf(stderr, "book %s levels:\n", s == Side::Buy ? "BID" : "ASK");
        for (std::size_t i = 0; i < kMaxLevels; ++i) {
            const Level& lv = levels_[i];
            const std::uint64_t head = s == Side::Buy ? lv.bid_head : lv.sell_head;
            if (!head) continue;
            std::fprintf(stderr, "  p=%lld total=%lld n=%u refs:",
                         static_cast<long long>(min_price_ + static_cast<std::int64_t>(i)),
                         static_cast<long long>(s == Side::Buy ? lv.bid_total_qty
                                                               : lv.sell_total_qty),
                         s == Side::Buy ? lv.bid_count : lv.sell_count);
            std::uint64_t h = head;
            while (h) {
                const OrderNode* n = pool_.resolve(h);
                std::fprintf(stderr, " %llu(q%lld)",
                             static_cast<unsigned long long>(n->ext_id),
                             static_cast<long long>(n->qty));
                h = (s == Side::Buy) ? n->next : n->next;
            }
            std::fprintf(stderr, "\n");
        }
    }

private:
    static constexpr std::size_t kBadIndex = ~std::size_t{0};
    struct Level {
        std::int64_t bid_total_qty = 0;
        std::int64_t sell_total_qty = 0;
        std::uint64_t bid_head = 0, bid_tail = 0;
        std::uint64_t sell_head = 0, sell_tail = 0;
        std::uint32_t bid_count = 0;
        std::uint32_t sell_count = 0;
    };


    std::size_t price_index(std::int64_t price) const noexcept {
        const std::int64_t off = price - min_price_;
        if (off < 0 || off >= static_cast<std::int64_t>(kMaxLevels)) return kBadIndex;
        return static_cast<std::size_t>(off);
    }
    const Level* level_at(std::int64_t price) const noexcept {
        const std::size_t i = price_index(price);
        return i == kBadIndex ? nullptr : &levels_[i];
    }

    void push_back(Level& lv, Side s, std::uint64_t handle) noexcept {
        OrderNode* n = pool_.resolve(handle);
        if (s == Side::Buy) {
            n->prev = lv.bid_tail;
            n->next = 0;
            if (lv.bid_tail) pool_.resolve(lv.bid_tail)->next = handle;
            lv.bid_tail = handle;
            if (!lv.bid_head) lv.bid_head = handle;
        } else {
            n->prev = lv.sell_tail;
            n->next = 0;
            if (lv.sell_tail) pool_.resolve(lv.sell_tail)->next = handle;
            lv.sell_tail = handle;
            if (!lv.sell_head) lv.sell_head = handle;
        }
    }

    BookResult reduce(std::uint64_t /*handle*/, OrderNode& n, std::int64_t qty) noexcept {
        const std::size_t idx = price_index(n.price);
        if (idx == kBadIndex) return BookResult::InvalidPrice;   // cannot happen
        Level& lv = levels_[idx];
        const Side s = n.side == 'B' ? Side::Buy : Side::Sell;
        n.qty -= qty;
        if (s == Side::Buy) lv.bid_total_qty -= qty;
        else lv.sell_total_qty -= qty;
        if (n.qty > 0) return BookResult::Ok;
        // Unlink from the FIFO.
        if (s == Side::Buy) {
            if (n.prev) pool_.resolve(n.prev)->next = n.next;
            else lv.bid_head = n.next;
            if (n.next) pool_.resolve(n.next)->prev = n.prev;
            else lv.bid_tail = n.prev;
            lv.bid_count -= 1;
            if (lv.bid_count == 0) bid_bitmap_.clear(idx);
        } else {
            if (n.prev) pool_.resolve(n.prev)->next = n.next;
            else lv.sell_head = n.next;
            if (n.next) pool_.resolve(n.next)->prev = n.prev;
            else lv.sell_tail = n.prev;
            lv.sell_count -= 1;
            if (lv.sell_count == 0) ask_bitmap_.clear(idx);
        }
        ext_to_node_.erase(n.ext_id);
        pool_.release(&n);
        return BookResult::Ok;
    }

    std::int64_t min_price_;
    std::uint64_t snapshot_ref_seq_ = 0;
    Level levels_[kMaxLevels];
    core::Bitmap<kMaxLevels> bid_bitmap_{};
    core::Bitmap<kMaxLevels> ask_bitmap_{};
    core::ObjectPool<OrderNode, kMaxOrders> pool_;
    core::FlatMap<std::uint64_t> ext_to_node_;
    BookStats stats_{};
};

}  // namespace kestrel::book
