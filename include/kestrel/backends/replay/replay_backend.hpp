#pragma once
// ============================================================================
// kestrel/backends/replay/replay_backend.hpp
//
// In-memory replay backend (ADR-0005): the universal RxSource/TxSink for
// simulation, backtesting, and journal replay. Version 1's workhorse.
//
// Semantics (backend contract, net/backend.hpp):
//   RX  : frames live in a fixed arena slot until release_rx; poll_rx hands
//         out views into slots. Single-threaded by design (the feeder and
//         the engine run on the same pinned thread in sim/replay/backtest).
//   TX  : tx_submit COPIES into a TX arena slot and enqueues — kEnqueued
//         means "copied + enqueued", NOT flushed/doorbell (no wire exists).
//         The host drains via pop_tx; completion processing = the drain.
//         kBusy when arena or ring is exhausted (engine retries per policy).
//
// No allocation after construction.
// ============================================================================

#include <cstdint>
#include <span>

#include "kestrel/core/spsc_ring.hpp"
#include "kestrel/net/backend.hpp"

namespace kestrel::backends {

template <std::size_t kSlots = 512, std::size_t kFrameMax = 4608,
          std::size_t kRingCap = 512>
class ReplayBackend {
    static_assert((kSlots & (kSlots - 1)) == 0);
    static_assert((kRingCap & (kRingCap - 1)) == 0);

public:
    ReplayBackend() {
        for (std::size_t i = 0; i < kSlots; ++i) rxfree_[i] = static_cast<std::uint16_t>(i);
        for (std::size_t i = 0; i < kSlots; ++i) txfree_[i] = static_cast<std::uint16_t>(i);
    }

    // ---- producer side (sim host / journal reader) --------------------------
    bool push_rx(net::FrameKind kind, std::uint64_t ts_ns,
                 std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.size() > kFrameMax || rxfree_count_ == 0) return false;
        if (!rx_ring_.try_push(rxfree_[rxfree_count_ - 1])) return false;
        Slot& s = slots_[rxfree_[--rxfree_count_]];
        s.ts = ts_ns;
        s.kind = kind;
        s.len = static_cast<std::uint16_t>(bytes.size());
        if (bytes.size()) __builtin_memcpy(s.bytes, bytes.data(), bytes.size());
        return true;
    }

    /// Host drain of engine output: pops one submitted TX frame (copies out,
    /// returns the slot). Returns false when empty.
    bool pop_tx(std::span<std::uint8_t> out, std::size_t& len) noexcept {
        std::uint16_t idx{};
        if (!tx_ring_.try_pop(idx)) return false;
        TxSlot& s = tx_slots_[idx];
        len = s.len;
        if (len) __builtin_memcpy(out.data(), s.bytes, len);
        txfree_[txfree_count_++] = idx;
        return true;
    }
    std::size_t tx_backlog() const noexcept { return tx_ring_.size_approx(); }
    std::size_t rx_backlog() const noexcept { return rx_ring_.size_approx(); }

    // ---- RxSource ------------------------------------------------------------
    std::size_t poll_rx(net::FrameView* out, std::size_t max) noexcept {
        std::size_t n = 0;
        std::uint16_t idx{};
        while (n < max && rx_ring_.try_pop(idx)) {
            const Slot& s = slots_[idx];
            out[n].ts_event_ns = s.ts;
            out[n].kind = s.kind;
            out[n].data = s.bytes;
            out[n].len = s.len;
            in_flight_[in_flight_count_++] = idx;
            ++n;
        }
        return n;
    }
    void release_rx(std::size_t n) noexcept {
        for (std::size_t i = 0; i < n && in_flight_count_; ++i) {
            rxfree_[rxfree_count_++] = in_flight_[--in_flight_count_];
        }
    }

    // ---- TxSink ----------------------------------------------------------------
    net::TxResult tx_submit(const std::uint8_t* data, std::size_t len) noexcept {
        if (len > kFrameMax || txfree_count_ == 0) return net::TxResult::kBusy;
        const std::uint16_t slot = txfree_[txfree_count_ - 1];
        if (!tx_ring_.try_push(slot)) return net::TxResult::kBusy;
        --txfree_count_;
        TxSlot& s = tx_slots_[slot];
        s.len = static_cast<std::uint16_t>(len);
        if (len) __builtin_memcpy(s.bytes, data, len);
        return net::TxResult::kEnqueued;
    }
    std::size_t tx_poll_completed(std::size_t) noexcept {
        // Replay semantics: sends complete at enqueue; the host drain
        // (pop_tx) is the completion processing. Nothing to reclaim here.
        return tx_ring_.size_approx();
    }

private:
    struct Slot {
        std::uint64_t ts = 0;
        net::FrameKind kind = net::FrameKind::MarketData;
        std::uint16_t len = 0;
        std::uint8_t bytes[kFrameMax]{};
    };
    struct TxSlot {
        std::uint16_t len = 0;
        std::uint8_t bytes[kFrameMax]{};
    };

    Slot slots_[kSlots];
    std::uint16_t rxfree_[kSlots];
    std::size_t rxfree_count_ = kSlots;
    std::uint16_t in_flight_[kRingCap]{};
    std::size_t in_flight_count_ = 0;
    core::SpscRing<std::uint16_t, kRingCap> rx_ring_;

    TxSlot tx_slots_[kSlots];
    std::uint16_t txfree_[kSlots];
    std::size_t txfree_count_ = kSlots;
    core::SpscRing<std::uint16_t, kRingCap> tx_ring_;
};

static_assert(net::RxSource<ReplayBackend<>>);
static_assert(net::TxSink<ReplayBackend<>>);

}  // namespace kestrel::backends
