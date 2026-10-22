#pragma once
// ============================================================================
// kestrel/core/bitmap.hpp
//
// Fixed-size word bitmap with forward/backward set-bit scans.
//
// Used for occupied price levels: best-ask = first set bit at-or-after an
// index (ctz on masked words), best-bid = last set bit at-or-before an index
// (clz on masked words). No allocation after construction.
// ============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace kestrel::core {

template <std::size_t Bits>
    requires(std::has_single_bit(Bits))
class Bitmap {
    static constexpr std::size_t kWords = Bits / 64;
    static_assert(kWords * 64 == Bits);

public:
    constexpr Bitmap() = default;
    static constexpr std::size_t size() noexcept { return Bits; }
    std::size_t memory_bytes() const noexcept { return sizeof(words_); }

    void set(std::size_t i) noexcept {
        words_[i >> 6] |= (std::uint64_t{1} << (i & 63));
    }
    void clear(std::size_t i) noexcept {
        words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
    }
    bool test(std::size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1ull;
    }
    void clear_all() noexcept { words_.fill(0); }

    /// First set bit with index >= from. nullopt when none.
    [[nodiscard]] std::optional<std::size_t> find_from(std::size_t from) const noexcept {
        if (from >= Bits) return std::nullopt;
        const std::size_t w = from >> 6;
        const std::uint64_t word = words_[w] & (~std::uint64_t{0} << (from & 63));
        if (word) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
        for (std::size_t i = w + 1; i < kWords; ++i) {
            if (words_[i]) return (i << 6) + static_cast<std::size_t>(std::countr_zero(words_[i]));
        }
        return std::nullopt;
    }

    /// Last set bit with index <= to. nullopt when none.
    [[nodiscard]] std::optional<std::size_t> find_to(std::size_t to) const noexcept {
        if (to >= Bits) to = Bits - 1;
        const std::size_t w = to >> 6;
        const std::uint64_t word = words_[w] & (~std::uint64_t{0} >> (63 - (to & 63)));
        if (word) return (w << 6) + static_cast<std::size_t>(63 - std::countl_zero(word));
        for (std::size_t i = w; i-- > 0;) {
            if (words_[i]) return (i << 6) + static_cast<std::size_t>(63 - std::countl_zero(words_[i]));
        }
        return std::nullopt;
    }

    std::size_t count() const noexcept {
        std::size_t n = 0;
        for (const std::uint64_t w : words_) n += static_cast<std::size_t>(std::popcount(w));
        return n;
    }

    const std::array<std::uint64_t, kWords>& words() const noexcept { return words_; }

private:
    std::array<std::uint64_t, kWords> words_{};
};

}  // namespace kestrel::core
