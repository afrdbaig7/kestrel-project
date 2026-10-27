#pragma once
// ============================================================================
// kestrel/book/ref_book.hpp
//
// Simple reference book for differential testing (spec §7). Deliberately
// naive: std::map-based, allocates, no performance pretensions. Property
// tests drive LevelBook and RefBook with identical event streams and require
// identical best bid/ask and per-price totals.
//
// Semantics MUST match LevelBook exactly:
//   add: duplicate ref -> rejected (state unchanged)
//   cancel(qty<=0 or >= remaining) -> full remove, else reduce
//   execute: over-execution -> apply remaining, remove, report
//   replace: remove(old)+add(new)
// ============================================================================

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>

#include "kestrel/book/level_book.hpp"   // Side, BookResult

namespace kestrel::book {

class RefBook {
public:
    RefBook(std::int64_t min_price, std::int64_t max_price) noexcept
        : min_price_(min_price), max_price_(max_price) {}

    BookResult add(std::uint64_t ref, Side s, std::int64_t qty, std::int64_t price) {
        if (qty <= 0) return BookResult::Overflow;
        if (price < min_price_ || price > max_price_) return BookResult::InvalidPrice;
        if (orders_.contains(ref)) return BookResult::DuplicateId;
        orders_[ref] = {s, qty, price};
        level(s, price).q.push_back({ref, qty});
        return BookResult::Ok;
    }

    BookResult cancel(std::uint64_t ref, std::int64_t qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return BookResult::UnknownOrder;
        const std::int64_t remove = (qty <= 0 || qty >= it->second.qty) ? it->second.qty : qty;
        return reduce(ref, remove);
    }

    BookResult execute(std::uint64_t ref, std::int64_t qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return BookResult::UnknownOrder;
        if (qty <= 0) return BookResult::Overflow;
        if (qty > it->second.qty) {
            const std::int64_t rem = it->second.qty;
            const BookResult r = reduce(ref, rem);
            return r == BookResult::Ok ? BookResult::OverExecuted : r;
        }
        return reduce(ref, qty);
    }

    BookResult replace(std::uint64_t orig, std::uint64_t ref, std::int64_t qty,
                       std::int64_t price) {
        auto it = orders_.find(orig);
        if (it == orders_.end()) return BookResult::UnknownOrder;
        const Side s = it->second.side;
        const BookResult r = cancel(orig, it->second.qty);
        if (r != BookResult::Ok) return r;
        return add(ref, s, qty, price);
    }

    std::optional<std::int64_t> best_bid() const {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
            if (it->second.total_qty() > 0) return it->first;
        return std::nullopt;
    }
    std::optional<std::int64_t> best_ask() const {
        for (const auto& [price, lv] : asks_)
            if (lv.total_qty() > 0) return price;
        return std::nullopt;
    }
    std::int64_t level_qty(std::int64_t price, Side s) const {
        const auto& m = s == Side::Buy ? bids_ : asks_;
        const auto it = m.find(price);
        return it == m.end() ? 0 : it->second.total_qty();
    }
    std::size_t live_orders() const { return orders_.size(); }
    void clear() { orders_.clear(); bids_.clear(); asks_.clear(); }

private:
    struct Entry { std::uint64_t ref; std::int64_t qty; };
    struct Level {
        std::deque<Entry> q;
        std::int64_t total_qty() const {
            std::int64_t s = 0;
            for (const auto& e : q) s += e.qty;
            return s;
        }
    };
    struct Order { Side side; std::int64_t qty; std::int64_t price; };

    Level& level(Side s, std::int64_t price) {
        return s == Side::Buy ? bids_[price] : asks_[price];
    }

    BookResult reduce(std::uint64_t ref, std::int64_t remove) {
        auto it = orders_.find(ref);
        it->second.qty -= remove;
        auto& m = it->second.side == Side::Buy ? bids_ : asks_;
        auto lv = m.find(it->second.price);
        if (lv != m.end()) {
            for (auto& e : lv->second.q) {
                if (e.ref == ref) { e.qty -= remove; break; }
            }
            auto& q = lv->second.q;
            while (!q.empty() && q.front().qty <= 0) q.pop_front();
        }
        if (it->second.qty <= 0) orders_.erase(it);
        return BookResult::Ok;
    }

    std::int64_t min_price_;
    std::int64_t max_price_;
    std::map<std::int64_t, Level> bids_;
    std::map<std::int64_t, Level> asks_;
    std::map<std::uint64_t, Order> orders_;
};

}  // namespace kestrel::book
