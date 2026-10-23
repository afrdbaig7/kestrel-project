// Unit tests: kestrel::core::ObjectPool (generation-checked slot reuse)
#include <gtest/gtest.h>

#include "kestrel/core/object_pool.hpp"

using kestrel::core::ObjectPool;

struct Node {
    int payload = 0;
    int stamp = 0;
};

TEST(ObjectPool, AcquireReleaseBasics) {
    ObjectPool<Node, 4> pool;
    EXPECT_EQ(pool.available(), 4u);
    Node* a = pool.acquire();
    ASSERT_NE(a, nullptr);
    a->payload = 1;
    EXPECT_EQ(pool.in_use(), 1u);
    Node* b = pool.acquire();
    Node* c = pool.acquire();
    Node* d = pool.acquire();
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(pool.exhausted());
    EXPECT_EQ(pool.acquire(), nullptr);   // documented exhaustion behavior
    pool.release(a);
    EXPECT_EQ(pool.in_use(), 3u);
    Node* e = pool.acquire();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload, 0);             // fresh construction, not stale data
}

TEST(ObjectPool, HandlesResolveAcrossReuse) {
    ObjectPool<Node, 2> pool;
    Node* a = pool.acquire();
    const std::uint64_t h1 = pool.handle_of(a);
    a->payload = 42;
    ASSERT_TRUE(pool.release_by_handle(h1));
    EXPECT_FALSE(pool.release_by_handle(h1));   // double release via handle: rejected

    Node* b = pool.acquire();
    const std::uint64_t h2 = pool.handle_of(b);
    // Same slot recycled but generation changed.
    EXPECT_EQ((ObjectPool<Node, 2>::index_of_handle(h1)),
              (ObjectPool<Node, 2>::index_of_handle(h2)));
    EXPECT_NE((ObjectPool<Node, 2>::gen_of_handle(h1)),
              (ObjectPool<Node, 2>::gen_of_handle(h2)));
    b->payload = 7;

    // Old handle must NOT resolve into the recycled object.
    EXPECT_EQ(pool.resolve(h1), nullptr);
    Node* r = pool.resolve(h2);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payload, 7);
}

TEST(ObjectPool, InvalidHandles) {
    ObjectPool<Node, 4> pool;
    EXPECT_EQ(pool.resolve(0), nullptr);                      // reserved invalid
    EXPECT_EQ(pool.resolve(0xFFFF'FFFF'FFFF'FFFFull), nullptr);  // out of range
    Node* a = pool.acquire();
    const std::uint64_t good = pool.handle_of(a);
    EXPECT_EQ(pool.resolve(good), a);
    const std::uint64_t bad_gen = (good + (1ull << 32));       // wrong generation
    EXPECT_EQ(pool.resolve(bad_gen), nullptr);
}

TEST(ObjectPool, ExhaustionThenFullDrain) {
    constexpr std::size_t N = 16;
    ObjectPool<Node, N> pool;
    Node* ptrs[N];
    for (std::size_t i = 0; i < N; ++i) {
        ptrs[i] = pool.acquire();
        ASSERT_NE(ptrs[i], nullptr);
        ptrs[i]->payload = static_cast<int>(i);
    }
    EXPECT_EQ(pool.acquire(), nullptr);
    for (std::size_t i = 0; i < N; ++i) pool.release(ptrs[i]);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.available(), N);
    // Everything reusable after full drain.
    Node* again = pool.acquire();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->payload, 0);
}
