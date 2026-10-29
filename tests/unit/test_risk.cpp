// Unit tests: pre-trade risk — bands, caps, reservations, token bucket,
// stale-feed protection, kill switch (spec §8)
#include <gtest/gtest.h>

#include "kestrel/risk/risk.hpp"

using namespace kestrel::risk;

namespace {
RiskConfig cfg() {
    RiskConfig c{};
    c.price_min = 90'000;
    c.price_max = 110'000;
    c.max_order_qty = 100;
    c.max_order_notional = 105 * 100'000;   // 10.5M tick-units
    c.max_position = 200;
    c.max_pending_notional = 50 * 110'000;
    c.max_pending_lots = 150;
    c.rate.capacity = 1000;
    c.rate.refill_per_sec = 1000;
    c.max_orders_per_bucket = 1;
    return c;
}
}  // namespace

TEST(TokenBucket, DeterministicRefill) {
    TokenBucket tb{{10, 5}};   // 10 tokens, 5/s
    EXPECT_TRUE(tb.try_take(10, 0));
    EXPECT_FALSE(tb.try_take(1, 100'000'000));       // 0.1s: 0.5 tokens (rounded down)
    EXPECT_TRUE(tb.try_take(1, 1'200'000'000));      // 1.2s: 6 refilled
    EXPECT_FALSE(tb.try_take(6, 1'300'000'000));     // 1.3s: 6.5 total, 6 available
    EXPECT_TRUE(tb.try_take(6, 1'400'000'000));
    // Same timeline replayed -> same decisions (determinism)
    TokenBucket tb2{{10, 5}};
    EXPECT_TRUE(tb2.try_take(10, 0));
    EXPECT_FALSE(tb2.try_take(1, 100'000'000));
    EXPECT_TRUE(tb2.try_take(1, 1'200'000'000));
}

TEST(RiskGate, ApprovesWithinLimitsAndReserves) {
    RiskGate g(cfg());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 1000), Decision::Approved);
    EXPECT_EQ(g.state().pending_buy_lots, 10);
    EXPECT_EQ(g.state().pending_buy_notional, 10 * 100'000);
}

TEST(RiskGate, PriceBand) {
    RiskGate g(cfg());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 10, 89'999, 0), Decision::RejectBand);
    EXPECT_EQ(g.approve(Side::Buy, 10, 110'001, 0), Decision::RejectBand);
    EXPECT_EQ(g.approve(Side::Buy, 10, 90'000, 0), Decision::Approved);
}

TEST(RiskGate, QtyAndNotionalCaps) {
    RiskGate g(cfg());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 101, 100'000, 0), Decision::RejectQty);
    EXPECT_EQ(g.approve(Side::Buy, 0, 100'000, 0), Decision::RejectQty);
    // notional cap = 100*110'000 = 11M tick units
    EXPECT_EQ(g.approve(Side::Buy, 100, 109'999, 0), Decision::RejectNotional);
}

namespace {
RiskConfig cfg_position() {   // notional caps high so POSITION is the binder
    RiskConfig c = cfg();
    c.max_pending_notional = 200 * 110'000;
    return c;
}
RiskConfig cfg_rate() {
    RiskConfig c = cfg_position();
    c.rate.capacity = 2;
    c.rate.refill_per_sec = 1;
    return c;
}
}  // namespace

TEST(RiskGate, PositionLimitIncludesPending) {
    RiskGate g(cfg_position());
    g.set_feed_synced(true);
    // 200 max position: 3 x 80 = 240 > 200; third must fail.
    EXPECT_EQ(g.approve(Side::Buy, 80, 100'000, 0), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 80, 100'000, 1000), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 80, 100'000, 2000), Decision::RejectPosition);
    // fill releases pending into position; still capped
    g.apply_fill(Side::Buy, 80, 100'000, 80, 100'000);
    g.apply_fill(Side::Buy, 80, 100'000, 80, 100'000);
    EXPECT_EQ(g.state().position, 160);
    EXPECT_EQ(g.approve(Side::Buy, 80, 100'000, 3000), Decision::RejectPosition);
    EXPECT_EQ(g.approve(Side::Buy, 40, 100'000, 4000), Decision::Approved);
}

TEST(RiskGate, ExposureLimits) {
    RiskGate g(cfg());   // max_pending_notional = 50*110'000 = 5.5M here
    g.set_feed_synced(true);
    // buy side: pending notional cap binds
    EXPECT_EQ(g.approve(Side::Buy, 49, 110'000, 0), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 2, 110'000, 1000), Decision::RejectExposure);
    // sell side: pending lots cap = 150 (max_order_qty = 100 per order)
    EXPECT_EQ(g.approve(Side::Sell, 100, 100'000, 2000), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Sell, 50, 100'000, 2500), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Sell, 1, 100'000, 3000), Decision::RejectExposure);
}

TEST(RiskGate, StaleFeedBlocked) {
    RiskGate g(cfg());
    g.set_feed_synced(false);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 0), Decision::RejectStale);
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 0), Decision::Approved);
}

TEST(RiskGate, KillSwitchBlocksNewExposureImmediately) {
    RiskGate g(cfg());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 0), Decision::Approved);
    g.engage_kill_switch();
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 1000), Decision::RejectKilled);
    EXPECT_EQ(g.approve(Side::Sell, 10, 100'000, 2000), Decision::RejectKilled);
    // Cancel workflow is separate: the OMS can still request cancels; the
    // risk gate has no API to block them (engine-level, spec §8).
    g.clear_kill_switch();
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 3000), Decision::Approved);
}

namespace {
RiskConfig cfg_release() {   // notional cap between 10M and 11M
    RiskConfig c = cfg();
    c.max_pending_notional = 10'500'000;
    return c;
}
}  // namespace

TEST(RiskGate, ReleaseRestoresHeadroomExactly) {
    RiskGate g(cfg_release());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 50, 100'000, 0), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 50, 100'000, 1000), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 2000), Decision::RejectExposure);
    g.release(Side::Buy, 50, 100'000);
    EXPECT_EQ(g.state().pending_buy_lots, 50);
    EXPECT_EQ(g.state().pending_buy_notional, 50 * 100'000);
    EXPECT_EQ(g.approve(Side::Buy, 10, 100'000, 3000), Decision::Approved);
}

TEST(RiskGate, SwapReservationOnReplace) {
    RiskGate g(cfg_position());
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 50, 100'000, 0), Decision::Approved);
    g.swap_reservation(Side::Buy, 50, 100'000, 40, 101'000);
    EXPECT_EQ(g.state().pending_buy_lots, 40);
    EXPECT_EQ(g.state().pending_buy_notional, 40 * 101'000);
}

TEST(RiskGate, RateLimitDeterministicInEventTime) {
    RiskGate g(cfg_rate());   // capacity 2, refill 1/s: burst 2 then 1/s
    g.set_feed_synced(true);
    EXPECT_EQ(g.approve(Side::Buy, 1, 100'000, 0), Decision::Approved);
    EXPECT_EQ(g.approve(Side::Buy, 1, 100'000, 1), Decision::Approved);   // burst 2
    EXPECT_EQ(g.approve(Side::Buy, 1, 100'000, 2), Decision::RejectRate);
    EXPECT_EQ(g.approve(Side::Buy, 1, 100'000, 1'000'000'000), Decision::Approved);  // 1s later
}
