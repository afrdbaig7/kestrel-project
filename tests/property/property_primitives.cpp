// Property tests: FlatMap vs unordered_map oracle, Bitmap vs brute force,
// ObjectPool generation safety under random churn.
#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "kestrel/core/bitmap.hpp"
#include "kestrel/core/flat_map.hpp"
#include "kestrel/core/object_pool.hpp"

using kestrel::core::Bitmap;
using kestrel::core::FlatMap;
using kestrel::core::ObjectPool;

namespace {
struct Node {
    std::uint64_t v = 0;
};
}  // namespace

TEST(PropertyFlatMap, OracleRandomOps) {
    std::mt19937_64 rng(2026'11'01);
    constexpr std::size_t CAP = 512;
    FlatMap<std::uint64_t> m(CAP);
    std::unordered_map<std::uint64_t, std::uint64_t> oracle;
    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t key = rng() % (CAP * 2);
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {  // insert
            const std::uint64_t val = rng();
            const bool got = m.insert(key, val);
            const bool want = oracle.emplace(key, val).second &&
                              oracle.size() <= CAP;
            // insert() refuses at capacity; oracle model must match
            if (oracle.size() > CAP) {
                EXPECT_FALSE(got);
                oracle.erase(key);
            } else {
                EXPECT_EQ(got, want) << "step " << step << " key " << key;
            }
        } else if (op == 1) {  // erase
            const bool got = m.erase(key);
            const bool want = oracle.erase(key) > 0;
            EXPECT_EQ(got, want) << "step " << step << " key " << key;
        } else {  // find
            const std::uint64_t* got = m.find(key);
            const auto it = oracle.find(key);
            if (it == oracle.end()) {
                EXPECT_EQ(got, nullptr) << "step " << step << " key " << key;
            } else {
                ASSERT_NE(got, nullptr) << "step " << step << " key " << key;
                EXPECT_EQ(*got, it->second) << "step " << step << " key " << key;
            }
        }
        ASSERT_EQ(m.size(), oracle.size()) << "step " << step;
    }
}

TEST(PropertyFlatMap, HeavyChurnNeverLosesEntries) {
    // Adversarial: insert dense cluster, delete forward, verify survivors.
    std::mt19937_64 rng(7);
    for (int round = 0; round < 20; ++round) {
        constexpr std::size_t CAP = 256;
        FlatMap<std::uint64_t> m(CAP);
        std::unordered_map<std::uint64_t, std::uint64_t> oracle;
        const std::uint64_t base = rng() << 40;
        for (std::size_t i = 0; i < CAP; ++i) {
            const std::uint64_t k = base + i;
            ASSERT_TRUE(m.insert(k, k * 3));
            oracle[k] = k * 3;
        }
        // Delete random half
        for (std::size_t d = 0; d < CAP / 2; ++d) {
            const std::uint64_t k = base + (rng() % CAP);
            const bool got = m.erase(k);
            const bool want = oracle.erase(k) > 0;
            ASSERT_EQ(got, want);
        }
        for (const auto& [k, v] : oracle) {
            const std::uint64_t* got = m.find(k);
            ASSERT_NE(got, nullptr) << "lost key " << k;
            EXPECT_EQ(*got, v);
        }
    }
}

TEST(PropertyBitmap, OracleRandomBits) {
    std::mt19937_64 rng(99);
    constexpr std::size_t BITS = 4096;
    Bitmap<BITS> b;
    std::vector<bool> oracle(BITS, false);
    for (int step = 0; step < 100'000; ++step) {
        const std::size_t i = static_cast<std::size_t>(rng() % BITS);
        const bool set = rng() & 1;
        if (set) { b.set(i); oracle[i] = true; }
        else { b.clear(i); oracle[i] = false; }
        // verify forward/backward scans against brute force every 97 steps
        if (step % 97 == 0) {
            std::optional<std::size_t> want_from;
            for (std::size_t k = i; k < BITS; ++k)
                if (oracle[k]) { want_from = k; break; }
            EXPECT_EQ(b.find_from(i), want_from);
            std::optional<std::size_t> want_to;
            for (std::size_t k = i + 1; k-- > 0;)
                if (oracle[k]) { want_to = k; break; }
            EXPECT_EQ(b.find_to(i), want_to);
            std::size_t cnt = 0;
            for (bool v : oracle) cnt += v ? 1 : 0;
            EXPECT_EQ(b.count(), cnt);
        }
    }
}

TEST(PropertyPool, GenerationSafetyUnderChurn) {
    std::mt19937_64 rng(1234);
    constexpr std::size_t CAP = 64;
    ObjectPool<Node, CAP> pool;
    std::vector<std::uint64_t> handles;
    for (int step = 0; step < 50'000; ++step) {
        if (handles.size() < CAP && (rng() & 1)) {
            Node* n = pool.acquire();
            ASSERT_NE(n, nullptr);
            n->v = step;
            handles.push_back(pool.handle_of(n));
        } else if (!handles.empty()) {
            const std::size_t idx = static_cast<std::size_t>(rng() % handles.size());
            const std::uint64_t h = handles[idx];
            Node* n = pool.resolve(h);
            ASSERT_NE(n, nullptr) << "step " << step;
            EXPECT_EQ(n->v * 0, 0u);   // object alive at this handle
            pool.release(n);
            handles.erase(handles.begin() + static_cast<long>(idx));
            // Stale handle must no longer resolve.
            EXPECT_EQ(pool.resolve(h), nullptr) << "step " << step;
        }
    }
    EXPECT_EQ(pool.in_use(), handles.size());
}
