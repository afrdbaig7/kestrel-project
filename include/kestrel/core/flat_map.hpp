#pragma once
// ============================================================================
// kestrel/core/flat_map.hpp
//
// Preallocated open-addressing hash map  (uint64_t key -> Value).
//
// Guarantees
// ----------
// * One allocation-free design: slot table sized at construction so that
//   `key_capacity` concurrent entries ALWAYS fit (max load factor 0.8).
//   insert() beyond key_capacity returns false — documented exhaustion.
// * No rehashing ever (deterministic memory layout; engine-friendly).
// * Linear probing + **backward-shift deletion** (no tombstones): the table
//   never accumulates garbage, so lookup cost does not degrade after churn.
// * Single-threaded by design (engine thread).
// * Default hash: splitmix64 finalizer — deterministic, decent avalanche.
//
// Complexity: O(1) expected find/insert/erase; worst case O(n) probe.
// ============================================================================

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace kestrel::core {

/// splitmix64 finalizer: strong, deterministic, cheap.
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t k) noexcept {
    k += 0x9e3779b97f4a7c15ull;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ull;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebull;
    return k ^ (k >> 31);
}

template <typename Value, float MaxLoadFactor = 0.8f>
class FlatMap {
    static_assert(MaxLoadFactor > 0.05f && MaxLoadFactor < 1.0f);

public:
    // Stored keys are (real_key + 1): slot value 0 means empty. This is a
    // BIJECTION, so real key 0 and real key 1 cannot collide. Real key
    // UINT64_MAX is unsupported (documented).
    static constexpr std::uint64_t kNpos = ~std::uint64_t{0};
    static constexpr std::uint64_t norm(std::uint64_t k) noexcept { return k + 1; }

    /// key_capacity = number of live entries that must always fit.
    explicit FlatMap(std::size_t key_capacity) : key_capacity_(key_capacity) {
        // table_size = pow2 >= key_capacity / max_load
        const double needed = static_cast<double>(key_capacity) / MaxLoadFactor;
        std::size_t ts = 16;
        while (ts < needed) ts <<= 1;
        table_size_ = ts;
        mask_ = table_size_ - 1;
        keys_ = std::make_unique<std::uint64_t[]>(table_size_);
        vals_ = std::make_unique<Value[]>(table_size_);
        used_ = std::make_unique<bool[]>(table_size_);
        for (std::size_t i = 0; i < table_size_; ++i) keys_[i] = 0;
    }
    FlatMap(const FlatMap&) = delete;
    FlatMap& operator=(const FlatMap&) = delete;

    std::size_t key_capacity() const noexcept { return key_capacity_; }
    std::size_t table_size() const noexcept { return table_size_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t memory_bytes() const noexcept {
        return table_size_ * (sizeof(std::uint64_t) + sizeof(Value) + sizeof(bool));
    }

    /// Returns true on insert; false if key present (value NOT overwritten) or
    /// if inserting would exceed key_capacity (documented exhaustion policy).
    template <typename V>
    bool insert(std::uint64_t key, V&& value) noexcept {
        if (size_ == key_capacity_) return false;          // exhausted
        const std::uint64_t k = norm(key);
        std::size_t i = index_of_key(k);
        while (used_[i]) {
            if (keys_[i] == k) return false;               // already present
            i = (i + 1) & mask_;
        }
        keys_[i] = k;
        vals_[i] = std::forward<V>(value);
        used_[i] = true;
        ++size_;
        return true;
    }

    /// Insert or overwrite. Returns false only on capacity exhaustion.
    template <typename V>
    bool insert_or_assign(std::uint64_t key, V&& value) noexcept {
        const std::uint64_t k = norm(key);
        std::size_t i = index_of_key(k);
        while (used_[i]) {
            if (keys_[i] == k) { vals_[i] = std::forward<V>(value); return true; }
            i = (i + 1) & mask_;
        }
        if (size_ == key_capacity_) return false;          // exhausted
        keys_[i] = k;
        vals_[i] = std::forward<V>(value);
        used_[i] = true;
        ++size_;
        return true;
    }

    Value* find(std::uint64_t key) noexcept {
        const std::uint64_t k = norm(key);
        std::size_t i = index_of_key(k);
        while (used_[i]) {
            if (keys_[i] == k) return &vals_[i];
            i = (i + 1) & mask_;
        }
        return nullptr;
    }
    const Value* find(std::uint64_t key) const noexcept {
        return const_cast<FlatMap*>(this)->find(key);
    }

    bool contains(std::uint64_t key) const noexcept { return find(key) != nullptr; }

    /// Backward-shift deletion (no tombstones).
    bool erase(std::uint64_t key) noexcept {
        const std::uint64_t k = norm(key);
        std::size_t i = index_of_key(k);
        while (used_[i]) {
            if (keys_[i] == k) { erase_at(i); return true; }
            i = (i + 1) & mask_;
        }
        return false;
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < table_size_; ++i) { used_[i] = false; keys_[i] = 0; }
        size_ = 0;
    }

private:
    std::size_t index_of_key(std::uint64_t k) const noexcept {
        return static_cast<std::size_t>(mix64(k)) & mask_;
    }

    void erase_at(std::size_t hole) noexcept {
        while (true) {
            std::size_t j = hole;
            // Scan forward; move the first entry whose home lies at-or-before
            // `hole` (cyclically) into the hole, making a new hole at j.
            while (true) {
                j = (j + 1) & mask_;
                if (!used_[j]) {                    // ran into empty: done
                    used_[hole] = false;
                    keys_[hole] = 0;
                    --size_;
                    return;
                }
                const std::size_t home = static_cast<std::size_t>(mix64(keys_[j])) & mask_;
                // Move j -> hole if hole is on j's probe path:
                // cyclic distance home->hole <= cyclic distance home->j.
                const std::size_t d_hole = (hole - home) & mask_;
                const std::size_t d_j = (j - home) & mask_;
                if (d_hole <= d_j) break;
            }
            keys_[hole] = keys_[j];
            vals_[hole] = std::move(vals_[j]);
            used_[hole] = true;
            hole = j;
        }
    }

    std::size_t key_capacity_;
    std::size_t table_size_;
    std::size_t mask_;
    std::size_t size_ = 0;
    std::unique_ptr<std::uint64_t[]> keys_;
    std::unique_ptr<Value[]> vals_;
    std::unique_ptr<bool[]> used_;
};

}  // namespace kestrel::core
