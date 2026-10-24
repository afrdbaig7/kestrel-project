// Micro-benchmarks for Kestrel foundational primitives (Google Benchmark).
//
// Methodology notes (see docs/benchmarks/methodology.md):
//  * These measure *operations*, not end-to-end trading latency.
//  * Run under the `bench` preset (-O3 -march=native) on a pinned, quiet host;
//    record environment metadata with kestrel_inspect alongside results.
#include <benchmark/benchmark.h>
#include <cstdint>
#include <random>

#include "kestrel/core/bitmap.hpp"
#include "kestrel/core/clock.hpp"
#include "kestrel/core/flat_map.hpp"
#include "kestrel/core/histogram.hpp"
#include "kestrel/core/object_pool.hpp"
#include "kestrel/core/spsc_ring.hpp"

using namespace kestrel::core;

namespace {
struct Packet {
    std::uint64_t ts;
    std::uint64_t seq;
    const std::uint8_t* data;
    std::uint32_t len;
};
struct BookNode {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t next;
};
}  // namespace

static void BM_SpscRingPushPop(benchmark::State& state) {
    SpscRing<Packet, 1024> ring;
    Packet p{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.try_push(p));
        benchmark::DoNotOptimize(ring.try_pop(p));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRingPushPop);

static void BM_ObjectPoolAcquireRelease(benchmark::State& state) {
    ObjectPool<BookNode, 1024> pool;
    for (auto _ : state) {
        BookNode* n = pool.acquire(1, 2, 0);
        benchmark::DoNotOptimize(n);
        pool.release(n);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolAcquireRelease);

static void BM_FlatMapFindHot(benchmark::State& state) {
    FlatMap<std::uint32_t> map(1024);
    std::mt19937_64 rng(1);
    for (std::uint64_t k = 1; k <= 700; ++k) map.insert(rng(), static_cast<std::uint32_t>(k));
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k = 1; k <= 700; ++k) keys.push_back(rng());
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(keys[i % keys.size()]));
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FlatMapFindHot);

static void BM_BitmapBestBidScan(benchmark::State& state) {
    // Worst realistic case: occupied levels far apart, scan the whole ladder.
    Bitmap<4096> bid;
    bid.set(1);
    bid.set(2000);
    for (auto _ : state) {
        benchmark::DoNotOptimize(bid.find_to(4095));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BitmapBestBidScan);

static void BM_HistogramRecord(benchmark::State& state) {
    Histogram h(1'000'000'000, 3);
    std::uint64_t v = 500;
    for (auto _ : state) {
        v = v * 6364136223846793005ull + 1442695040888963407ull;
        h.record(static_cast<std::int64_t>(v % 1'000'000));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecord);

static void BM_ClockReadOrdered(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(TscClock::now());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ClockReadOrdered);

static void BM_ClockSpanPair(benchmark::State& state) {
    for (auto _ : state) {
        const auto t0 = TscClock::now();
        benchmark::ClobberMemory();
        const auto t1 = TscClock::now();
        benchmark::DoNotOptimize(t1 - t0);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ClockSpanPair);
