#pragma once
// ============================================================================
// kestrel/core/spsc_ring.hpp
//
// Fixed-capacity single-producer/single-consumer ring buffer.
//
// Guarantees and ordering justification
// -------------------------------------
// * One producer thread, one consumer thread. No other thread may call
//   try_push/try_emplace (producer side) or try_pop (consumer side).
// * size(), empty(), full() may be called from either thread and return a
//   snapshot (valid for observability, not for synchronization decisions).
// * Storage is a fixed inline array: no allocation after construction.
// * Wait-free push/pop; no locks, no syscalls.
//
// Ordering (monotonic 64-bit cursors, wrap-free):
//   Producer: writes payload, then head_.store(h+1, release)
//             reads  tail_.load(acquire)   [pairs with consumer's tail_.store(release)]
//   Consumer: reads  head_.load(acquire)      [pairs with producer's head_.store(release)]
//             moves payload out, then tail_.store(t+1, release)
//   -> A cursor release store publishes the slot payload written before it;
//      the paired acquire load guarantees the payload is visible before use.
//      The producer's acquire read of tail_ guarantees any slot it overwrites
//      was fully consumed (moved-out + destroyed) before being reused.
// ============================================================================

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace kestrel::core {

inline constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t Capacity>
    requires(std::has_single_bit(Capacity))
class SpscRing {
    static_assert(Capacity > 0);

public:
    constexpr SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    ~SpscRing() {
        // Destroy any elements still enqueued (teardown path only).
        std::uint64_t t = tail_.cursor.load(std::memory_order_relaxed);
        std::uint64_t h = head_.cursor.load(std::memory_order_relaxed);
        for (; t != h; ++t) slot_ptr(t)->~T();
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Producer side. Returns false when the ring is full (caller decides
    /// policy: drop / backpressure / fail-closed).
    bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(v);
    }
    bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(v));
    }

    template <typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    bool try_emplace(Args&&... args) noexcept {
        const std::uint64_t h = head_.cursor.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.cursor.load(std::memory_order_acquire);
        if (h - t == Capacity) return false;               // full
        ::new (static_cast<void*>(slot_ptr(h))) T(std::forward<Args>(args)...);
        head_.cursor.store(h + 1, std::memory_order_release);  // publish payload
        return true;
    }

    /// Consumer side. Returns false when the ring is empty.
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::uint64_t t = tail_.cursor.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.cursor.load(std::memory_order_acquire);
        if (t == h) return false;                          // empty
        T* p = slot_ptr(t);
        out = std::move(*p);
        p->~T();
        tail_.cursor.store(t + 1, std::memory_order_release);  // publish freed slot
        return true;
    }

    /// Consumer-side peek without consuming. Element stays owned by the ring.
    /// Next try_pop returns the same element.
    bool peek(T* & out) noexcept {
        const std::uint64_t t = tail_.cursor.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.cursor.load(std::memory_order_acquire);
        if (t == h) return false;
        out = slot_ptr(t);
        return true;
    }

    // Observability (cross-thread snapshot; not for synchronization decisions).
    std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(
            head_.cursor.load(std::memory_order_acquire) -
            tail_.cursor.load(std::memory_order_acquire));
    }
    bool empty_approx() const noexcept { return size_approx() == 0; }
    bool full_approx() const noexcept { return size_approx() == Capacity; }
    /// Max occupancy ever observed (high-water mark, monotonic; two relaxed reads).
    std::size_t high_water_mark() const noexcept {
        const std::uint64_t h = head_.cursor.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.cursor.load(std::memory_order_relaxed);
        const std::uint64_t s = h - t;
        hwmark_.store(hwmark_.load(std::memory_order_relaxed) < s ? s : hwmark_.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
        return static_cast<std::size_t>(hwmark_.load(std::memory_order_relaxed));
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    T* slot_ptr(std::uint64_t i) noexcept {
        return std::launder(reinterpret_cast<T*>(
            storage_.data() + (i & kMask) * sizeof(T)));
    }
    const T* slot_ptr(std::uint64_t i) const noexcept {
        return std::launder(reinterpret_cast<const T*>(
            storage_.data() + (i & kMask) * sizeof(T)));
    }

    // Raw, uninitialized storage; alignment via alignas(T).
    alignas(alignof(T)) std::array<std::byte, sizeof(T) * Capacity> storage_{};

    // Producers and consumers live on different cache lines to prevent
    // false sharing on the two contended cursors.
    struct alignas(kCacheLine) Cursor {
        std::atomic<std::uint64_t> cursor{0};
    };
    Cursor head_;  // written by producer only
    Cursor tail_;  // written by consumer only
    alignas(kCacheLine) mutable std::atomic<std::uint64_t> hwmark_{0};
};

}  // namespace kestrel::core
