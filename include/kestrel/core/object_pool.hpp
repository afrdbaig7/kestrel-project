#pragma once
// ============================================================================
// kestrel/core/object_pool.hpp
//
// Fixed-capacity object pool with generation-checked slot reuse.
//
// Guarantees
// ----------
// * No allocation after construction (one contiguous slot array).
// * Single-threaded by design (engine thread). Not synchronized.
// * acquire() returns nullptr when exhausted (no growth, documented policy).
// * Safe slot reuse: every slot carries a generation counter bumped on
//   release(); 64-bit handles encode (slot, generation) so a stale handle can
//   never resolve into a recycled object. A stale/double release via handle is
//   detected (mismatched generation) rather than corrupting another object.
// * Freelist is an intrusive index chain stored in the slot itself.
// ============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace kestrel::core {

template <typename T, std::size_t Capacity>
    requires(std::has_single_bit(Capacity) && Capacity <= 0xFFFF'FFFFull)
class ObjectPool {
public:
    static constexpr std::uint32_t kIndexBits = 32;   // slot index
    static constexpr std::uint32_t kGenBits   = 32;   // generation
    /// handle 0 is reserved as "invalid"
    static constexpr std::uint64_t kInvalidHandle = 0;

    constexpr ObjectPool() {
        // Chain slots 0 -> 1 -> ... -> Capacity-1; free_head_ = 0.
        for (std::uint32_t i = 0; i + 1 < Capacity; ++i) slot(i).next_free = i + 1;
        slot(Capacity - 1).next_free = kEndOfList;
    }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    ~ObjectPool() {
        // Destroy live objects (teardown only; normally all are released).
        for (std::uint32_t i = 0; i < Capacity; ++i) {
            if (slot_live_[i]) slot_ptr(i)->~T();
        }
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }
    std::size_t in_use() const noexcept { return live_count_; }
    std::size_t available() const noexcept { return Capacity - live_count_; }
    bool exhausted() const noexcept { return live_count_ == Capacity; }

    /// Destroy all live objects and rebuild the freelist (init/teardown path).
    void reset() noexcept {
        for (std::uint32_t i = 0; i < Capacity; ++i) {
            if (slot_live_[i]) { slot_ptr(i)->~T(); slot_live_[i] = false; }
            slots_[i].gen += 1;   // invalidate every outstanding handle
            slots_[i].next_free = (i + 1 < Capacity) ? i + 1 : kEndOfList;
        }
        free_head_ = 0;
        live_count_ = 0;
    }

    /// Allocate and construct. Returns nullptr when exhausted.
    template <typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    T* acquire(Args&&... args) noexcept {
        if (free_head_ == kEndOfList) return nullptr;
        const std::uint32_t idx = free_head_;
        Slot& s = slot(idx);
        free_head_ = s.next_free;
        s.gen += 1;                       // generation changes on every acquire
        T* obj = slot_ptr(idx);
        ::new (static_cast<void*>(obj)) T(std::forward<Args>(args)...);
        slot_live_[idx] = true;
        ++live_count_;
        return obj;
    }

    /// Release via pointer. The pointer must have been returned by acquire()
    /// and must not have been released before (single owner: the engine).
    void release(T* p) noexcept {
        const std::uint32_t idx = index_of(p);
        p->~T();
        Slot& s = slot(idx);
        s.gen += 1;                       // bump so stale handles stop resolving
        s.next_free = free_head_;
        free_head_ = idx;
        slot_live_[idx] = false;
        --live_count_;
    }

    /// Opaque 64-bit handle = (gen << 32) | index, never zero.
    std::uint64_t handle_of(const T* p) const noexcept {
        const std::uint32_t idx = index_of(p);
        const std::uint64_t gen = slots_[idx].gen;
        return (gen << kIndexBits) | idx;
    }

    /// Resolve a handle. Returns nullptr for invalid/stale handles
    /// (wrong generation = slot was reused, or never-issued handle).
    T* resolve(std::uint64_t handle) noexcept {
        return const_cast<T*>(static_cast<const ObjectPool*>(this)->resolve(handle));
    }
    const T* resolve(std::uint64_t handle) const noexcept {
        if (handle == kInvalidHandle) return nullptr;
        const std::uint32_t idx = static_cast<std::uint32_t>(handle & 0xFFFF'FFFFull);
        const std::uint32_t gen = static_cast<std::uint32_t>(handle >> kIndexBits);
        if (idx >= Capacity) return nullptr;
        const Slot& s = slots_[idx];
        if (s.gen != gen || !slot_live_[idx]) return nullptr;
        return slot_ptr(idx);
    }

    /// Release via handle (generation-checked). Returns false on stale handle.
    bool release_by_handle(std::uint64_t handle) noexcept {
        T* p = resolve(handle);
        if (!p) return false;
        release(p);
        return true;
    }

    static std::uint32_t index_of_handle(std::uint64_t handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xFFFF'FFFFull);
    }
    static std::uint32_t gen_of_handle(std::uint64_t handle) noexcept {
        return static_cast<std::uint32_t>(handle >> kIndexBits);
    }

private:
    static constexpr std::uint32_t kEndOfList = 0xFFFF'FFFFu;

    struct Slot {
        std::uint32_t next_free;      // intrusive freelist (when not live)
        std::uint32_t gen = 0;        // generation; changes on acquire/release
    };

    std::uint32_t index_of(const T* p) const noexcept {
        return static_cast<std::uint32_t>(
            (reinterpret_cast<const std::byte*>(static_cast<const void*>(p)) -
             reinterpret_cast<const std::byte*>(static_cast<const void*>(storage_))) /
            sizeof(T));
    }
    Slot& slot(std::uint32_t i) noexcept { return slots_[i]; }
    const Slot& slot(std::uint32_t i) const noexcept { return slots_[i]; }
    T* slot_ptr(std::uint32_t i) noexcept {
        return std::launder(reinterpret_cast<T*>(storage_ + i * sizeof(T)));
    }
    const T* slot_ptr(std::uint32_t i) const noexcept {
        return std::launder(reinterpret_cast<const T*>(storage_ + i * sizeof(T)));
    }

    alignas(alignof(T)) std::byte storage_[sizeof(T) * Capacity];
    Slot slots_[Capacity];
    bool slot_live_[Capacity] = {};
    std::uint32_t free_head_ = 0;
    std::size_t live_count_ = 0;
};

}  // namespace kestrel::core
