#pragma once
// ============================================================================
// kestrel/book/instrument.hpp
//
// Instrument configuration and numeric-scale rules (see docs/architecture.md):
//   * prices  : integer TICKS (int64). One tick = instrument.tick_size_exp
//               fractional currency units, 10^-tick_size_exp.
//   * quantity: integer LOTS (int32; internal accumulation int64).
//   * notional: qty * price, computed with checked 128-bit multiply then
//               verified to fit int64 -> Overflow on violation. Denominated
//               in "tick units"; multiply by 10^-tick_size_exp for currency.
//   * order qty <= kMaxOrderQty guards multiplies; qty*price <= 2^62 enforced
//     before use so downstream additions of bounded counts cannot overflow.
// ============================================================================

#include <cstdint>
#include <string_view>

namespace kestrel::book {

inline constexpr std::int64_t kMaxOrderQty = 1'000'000;          // lots
inline constexpr std::int64_t kSafeNotionalMax = 1ll << 62;      // tick units

struct InstrumentConfig {
    std::uint32_t id = 0;
    std::string_view symbol;         // short label (≤8 chars)
    std::int64_t min_price = 0;      // tick index (absolute)
    std::int64_t max_price = 0;
    std::int64_t tick_size_exp = 4;  // price unit = 10^-tick_size_exp currency
    std::int64_t reference_price = 0;  // initial/reference tick
};

/// Checked notional: qty*price with overflow detection. Returns false on
/// overflow (documented risk-reject path).
/// The 128-bit intermediate is a documented GNU extension on this target
/// (x86-64 LP64); the pedantic warning is suppressed locally.
inline bool checked_notional(std::int64_t qty, std::int64_t price,
                             std::int64_t& out) noexcept {
    if (qty < 0 || price < 0) return false;
    if (qty > kMaxOrderQty) return false;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    const __int128 m = static_cast<__int128>(qty) * static_cast<__int128>(price);
    if (m > kSafeNotionalMax) return false;
    out = static_cast<std::int64_t>(m);
#pragma GCC diagnostic pop
    return true;
}

}  // namespace kestrel::book
