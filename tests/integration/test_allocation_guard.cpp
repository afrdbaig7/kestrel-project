// D1: allocation-guard — the engine event path must perform ZERO heap
// allocations after initialization (spec §4). Implemented with a process-wide
// counting operator new, gated by an atomic flag so only the guarded window
// counts (gtest and host setup may allocate freely outside it).
//
// IMPORTANT: the override set below must be COMPLETE (sized + unsized +
// nothrow + aligned variants). A partial set lets some news route to the
// compiler/ASan interceptors while deletes route to free() — which ASan
// flags as alloc-dealloc-mismatch (seen with gtest's temporary buffers).
// Every operator allocates and frees through the SAME allocator family
// (malloc/free family), so pairs always match.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <gtest/gtest.h>

#include "host.hpp"

using namespace kestrel::test;

namespace {
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_alloc_bytes{0};

inline void count_alloc(std::size_t n) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
    }
}
inline void* alloc(std::size_t n) noexcept {
    if (n == 0) n = 1;   // operator new must return a unique non-null pointer
    void* p = std::malloc(n);
    if (!p) std::abort();  // nothrow path needs a "null means OOM" contract
    return p;
}
inline void* alloc_aligned(std::size_t n, std::size_t a) noexcept {
    if (n == 0) n = 1;
    void* p = nullptr;
    if (posix_memalign(&p, a, n) != 0) std::abort();
    return p;
}
}  // namespace

// --- unsized + sized scalar/array ------------------------------------------
void* operator new(std::size_t n) { count_alloc(n); void* p = std::malloc(n == 0 ? 1 : n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { count_alloc(n); void* p = std::malloc(n == 0 ? 1 : n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// --- nothrow variants --------------------------------------------------------
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { count_alloc(n); return alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { count_alloc(n); return alloc(n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// --- aligned variants (over-aligned types) -----------------------------------
void* operator new(std::size_t n, std::align_val_t a) { count_alloc(n); void* p = alloc_aligned(n, static_cast<std::size_t>(a)); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n, std::align_val_t a) { count_alloc(n); void* p = alloc_aligned(n, static_cast<std::size_t>(a)); if (!p) throw std::bad_alloc(); return p; }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { count_alloc(n); return alloc_aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { count_alloc(n); return alloc_aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
struct Guard {
    Guard() { g_alloc_count.store(0); g_alloc_bytes.store(0); g_counting.store(true); }
    ~Guard() { g_counting.store(false); }
    std::uint64_t count() const { return g_alloc_count.load(); }
};
}  // namespace

TEST(AllocationGuard, ZeroAllocationsOnEngineEventPath) {
    TestHost h;
    h.run(100);   // warm: strategy quoting, venue responses flowing

    Guard guard;
    for (int i = 0; i < 1000; ++i) {
        h.eng->run_once();
    }
    // Note: venue feed continues arriving via the backend (already in its
    // preallocated arena). TX frames land in preallocated TX slots. The
    // engine path itself must not allocate.
    EXPECT_EQ(guard.count(), 0u)
        << "engine event path allocated " << g_alloc_bytes.load()
        << " bytes across " << guard.count() << " allocations";
}

TEST(AllocationGuard, ZeroAllocationsWithStageTimingEnabled) {
    // The diagnostic stage-timing engine must also stay allocation-free on
    // the event path (histograms are allocated at init).
    TestHost h;   // default host uses non-timing engine; construct a timing one
    using TimingEngine =
        kestrel::engine::Engine<kInstr, Backend, MM, /*kStageTiming=*/true>;
    auto eng = std::make_unique<TimingEngine>(*h.backend, h.ecfg);
    // Prime the pipeline with a few frames.
    h.run(50);
    Guard guard;
    for (int i = 0; i < 1000; ++i) eng->run_once();
    EXPECT_EQ(guard.count(), 0u);
}

TEST(AllocationGuard, ZeroAllocationsOnTxSubmitAndJournalWrite) {
    TestHost h;
    h.enable_journal();
    h.run(100);
    Guard guard;
    for (int i = 0; i < 1000; ++i) {
        h.eng->run_once();     // may tx_submit + write_critical (ring push)
    }
    EXPECT_EQ(guard.count(), 0u);
}
