// Unit tests: OMS transitions, duplicate execution reports, cancel workflow
#include <gtest/gtest.h>

#include "kestrel/oms/oms.hpp"

using namespace kestrel::oms;
using kestrel::risk::Side;

namespace {
OrderRecord rec(std::uint64_t id, std::int64_t qty, Side s = Side::Buy) {
    OrderRecord r{};
    r.client_id = id;
    r.instrument = 0;
    r.side = s;
    r.qty = qty;
    r.price = 100'000;
    r.state = OrderState::PendingNew;
    return r;
}
}  // namespace

TEST(Oms, HappyPathTransitions) {
    Oms o(16);
    EXPECT_TRUE(o.track_new(rec(1, 10)));
    EXPECT_EQ(o.find(1)->state, OrderState::PendingNew);
    EXPECT_TRUE(o.on_ack(1));
    EXPECT_EQ(o.find(1)->state, OrderState::Live);
    EXPECT_EQ(o.on_exec(1, 4, 100'000, 100), Oms::ExecResult::Applied);
    EXPECT_EQ(o.find(1)->state, OrderState::PartialFill);
    EXPECT_EQ(o.find(1)->filled, 4);
    EXPECT_EQ(o.on_exec(1, 6, 100'000, 101), Oms::ExecResult::Applied);
    EXPECT_EQ(o.find(1)->state, OrderState::Filled);
    EXPECT_EQ(o.stats().fills, 2u);
}

TEST(Oms, DuplicateExecutionReportsIgnored) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    EXPECT_EQ(o.on_exec(1, 10, 100'000, 100), Oms::ExecResult::Applied);
    // Same trade_ref again -> duplicate
    EXPECT_EQ(o.on_exec(1, 10, 100'000, 100), Oms::ExecResult::Duplicate);
    EXPECT_EQ(o.find(1)->filled, 10);          // inventory untouched
    EXPECT_EQ(o.find(1)->state, OrderState::Filled);
    // Over-fill -> duplicate path
    EXPECT_EQ(o.on_exec(1, 5, 100'000, 101), Oms::ExecResult::Duplicate);
    EXPECT_EQ(o.find(1)->filled, 10);
    EXPECT_EQ(o.stats().duplicate_execs, 2u);
}

TEST(Oms, DeclineAndCancelWorkflow) {
    Oms o(16);
    o.track_new(rec(1, 10));
    EXPECT_TRUE(o.on_decline(1));
    EXPECT_EQ(o.find(1)->state, OrderState::Rejected);

    o.track_new(rec(2, 10));
    o.on_ack(2);
    EXPECT_TRUE(o.on_cancel_request(2));
    EXPECT_EQ(o.find(2)->state, OrderState::PendingCancel);
    // Fill racing the cancel: applied, stays PendingCancel
    EXPECT_EQ(o.on_exec(2, 3, 100'000, 200), Oms::ExecResult::Applied);
    EXPECT_EQ(o.find(2)->state, OrderState::PendingCancel);
    // Cancel confirm (remaining 7) -> Cancelled
    EXPECT_EQ(o.on_cancel_confirm(2, 7), Oms::CancelConfirm::Full);
    EXPECT_EQ(o.find(2)->state, OrderState::Cancelled);
}

TEST(Oms, CancelRejectRestoresLive) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    o.on_cancel_request(1);
    EXPECT_TRUE(o.on_cancel_reject(1));
    EXPECT_EQ(o.find(1)->state, OrderState::Live);
    EXPECT_EQ(o.stats().cancel_rejects, 1u);
}

TEST(Oms, UnknownResponsesCounted) {
    Oms o(16);
    EXPECT_FALSE(o.on_ack(99));
    EXPECT_FALSE(o.on_decline(99));
    EXPECT_EQ(o.on_exec(99, 1, 1, 1), Oms::ExecResult::Unknown);
    EXPECT_EQ(o.stats().unknown_responses, 3u);
}

TEST(Oms, ReplaceLifecycle) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    OrderRecord r2 = rec(2, 20);
    r2.state = OrderState::ReplacePending;
    r2.replace_of = true;
    r2.orig_client_id = 1;
    EXPECT_TRUE(o.track_replace(r2));
    EXPECT_TRUE(o.on_replaced(2));
    EXPECT_EQ(o.find(2)->state, OrderState::Live);
    // decline path leaves orig untouched (caller manages reservations)
    o.track_new(rec(3, 10));
    o.on_ack(3);
    OrderRecord r4 = rec(4, 5);
    r4.state = OrderState::ReplacePending;
    r4.replace_of = true;
    r4.orig_client_id = 3;
    o.track_replace(r4);
    EXPECT_TRUE(o.on_decline(4));
    EXPECT_EQ(o.find(4)->state, OrderState::Rejected);
    EXPECT_EQ(o.find(3)->state, OrderState::Live);
}

TEST(Oms, SendFailureRetryTracking) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.mark_submit_result(1, false);
    o.mark_submit_result(1, false);
    EXPECT_EQ(o.find(1)->state, OrderState::PendingNew);   // still retryable
    EXPECT_EQ(o.find(1)->retries, 2u);
    o.mark_submit_result(1, true);
    EXPECT_EQ(o.find(1)->retries, 0u);
    EXPECT_TRUE(o.on_ack(1));
}

TEST(Oms, PartialCancelConfirmLeavesRemainderLive) {
    Oms o(16);
    OrderRecord r = rec(1, 10);
    r.filled = 4;              // pretend 4 filled already
    r.state = OrderState::PartialFill;
    o.track_new(r);
    o.on_ack(1);
    o.on_cancel_request(1);
    // Venue confirms only 2 of the 6 remaining (race) — remainder live.
    EXPECT_EQ(o.on_cancel_confirm(1, 2), Oms::CancelConfirm::Partial);
    EXPECT_EQ(o.find(1)->state, OrderState::PartialFill);
}

// ---------------------------------------------------------------------------
// Protocol-race semantics (added with the Milestone D benchmark findings):
// responses may arrive out of order relative to what the strategy expects.
// ---------------------------------------------------------------------------

TEST(Oms, ReplaceMovesOrigToPendingCancelImmediately) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    OrderRecord r = rec(2, 10);
    r.state = OrderState::ReplacePending;
    r.replace_of = true;
    r.orig_client_id = 1;
    ASSERT_TRUE(o.track_replace(r));
    // The orig must NOT be ackable anymore: an in-flight 'A' for it (queued
    // before the replace) can never re-ack it into a strategy slot.
    EXPECT_EQ(o.find(1)->state, OrderState::PendingCancel);
    EXPECT_FALSE(o.on_ack(1));
    EXPECT_TRUE(o.on_replaced(2));
    EXPECT_EQ(o.find(2)->state, OrderState::Live);
    EXPECT_EQ(o.find(1)->state, OrderState::Cancelled);   // orig retired
}

TEST(Oms, FillRacingReplaceAck) {
    // Response-order inversion: the venue rests the replace-child, a taker
    // fills it, and the Executed arrives BEFORE the 'U' ack.
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    OrderRecord r = rec(2, 10);
    r.state = OrderState::ReplacePending;
    r.replace_of = true;
    r.orig_client_id = 1;
    ASSERT_TRUE(o.track_replace(r));
    // Fill for the ReplacePending child must APPLY (the venue really filled
    // it) — rejecting it used to strand the order as a ghost.
    EXPECT_EQ(o.on_exec(2, 10, 100'000, 700), Oms::ExecResult::Applied);
    EXPECT_EQ(o.find(2)->state, OrderState::Filled);
    EXPECT_EQ(o.find(2)->filled, 10);
    // Late 'U': reconciles the orig (release is the engine's job) and the
    // filled child is retired.
    EXPECT_TRUE(o.on_replaced(2));
    EXPECT_EQ(o.find(1)->state, OrderState::Cancelled);
    EXPECT_TRUE(o.contains(2));           // retirement deferred to the sweep
    o.sweep_terminal();
    EXPECT_FALSE(o.contains(1));
    EXPECT_FALSE(o.contains(2));
}

TEST(Oms, DeclinedReplaceRestoresOrig) {
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    OrderRecord r = rec(2, 10);
    r.state = OrderState::ReplacePending;
    r.replace_of = true;
    r.orig_client_id = 1;
    ASSERT_TRUE(o.track_replace(r));
    EXPECT_EQ(o.find(1)->state, OrderState::PendingCancel);
    EXPECT_TRUE(o.on_decline(2));         // venue kept the orig resting
    EXPECT_EQ(o.find(2)->state, OrderState::Rejected);
    EXPECT_EQ(o.find(1)->state, OrderState::Live);   // restored
}

TEST(Oms, VenueInitiatedCancelConfirm) {
    // Real venues (and the simulator's churn) can cancel resting orders:
    // the confirm arrives for a Live order we never asked to cancel.
    Oms o(16);
    o.track_new(rec(1, 10));
    o.on_ack(1);
    EXPECT_EQ(o.on_cancel_confirm(1, 10), Oms::CancelConfirm::Full);
    EXPECT_EQ(o.find(1)->state, OrderState::Cancelled);
    // Partial venue cancel: order continues for the remainder.
    o.track_new(rec(2, 10));
    o.on_ack(2);
    EXPECT_EQ(o.on_cancel_confirm(2, 4), Oms::CancelConfirm::Partial);
    EXPECT_EQ(o.find(2)->state, OrderState::PartialFill);
    EXPECT_EQ(o.find(2)->qty - o.find(2)->filled, 10);   // total unchanged
}

TEST(Oms, TerminalRecordsRetireAndReuseCapacity) {
    Oms o(4);   // tiny capacity: forces slot reuse
    for (std::uint64_t id = 1; id <= 8; ++id) {
        ASSERT_TRUE(o.track_new(rec(id, 10))) << id;
        o.on_ack(id);
        ASSERT_EQ(o.on_exec(id, 10, 100'000, 100 * id), Oms::ExecResult::Applied);
        o.sweep_terminal();   // engine calls this once per loop
    }
    EXPECT_EQ(o.size(), 0u);   // all filled orders retired, capacity reusable
    EXPECT_EQ(o.retire_overflow(), 0u);
    // After retirement a duplicate exec is Unknown (record gone), not a
    // double fill — idempotent either way.
    EXPECT_EQ(o.on_exec(3, 10, 100'000, 300), Oms::ExecResult::Unknown);
}
