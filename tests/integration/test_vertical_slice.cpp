// Milestone B vertical slice: synthetic venue feed -> replay backend ->
// decoder -> book -> strategy -> risk -> order encoder -> simulator ack ->
// fills -> positions. Asserts the full pipeline works end to end.
#include <gtest/gtest.h>

#include "host.hpp"

using namespace kestrel::test;

TEST(VerticalSlice, FeedToFillToPosition) {
    TestHost h;
    h.run(4000);   // 4000 * 250us = 1s of event time

    const auto& st = h.eng->stats();
    EXPECT_GT(st.frames_md, 1000u);       // venue published market data
    EXPECT_GT(st.intents_generated, 0u);  // strategy reacted to the book
    EXPECT_GT(st.tx_enqueued, 0u);        // orders left the engine
    EXPECT_GT(h.tx_to_venue, 0u);
    EXPECT_GT(h.sim->resp_seq(), 0u);      // venue acknowledged

    const kestrel::oms::Oms& oms = h.eng->oms_engine();
    EXPECT_GT(oms.stats().acks, 0u);      // acks flowed back
    EXPECT_GT(oms.stats().fills, 0u);     // the MM was filled by taker flow
    const kestrel::risk::RiskState& rs = h.eng->risk_gate(0).state();
    EXPECT_NE(rs.position, 0);            // inventory moved
    // Reservation sanity: resting quotes hold bounded pending exposure and
    // fills never exceeded acks (every fill required a prior ack).
    EXPECT_GE(oms.stats().acks, oms.stats().fills);
    EXPECT_LE(rs.pending_buy_lots + rs.pending_sell_lots, 200);
    EXPECT_GE(rs.released_count + rs.filled_count, rs.filled_count);
}

TEST(VerticalSlice, BooksStayValidAndConsistentWithVenue) {
    TestHost h;
    h.run(2000);
    EXPECT_TRUE(h.eng->feed(0).book().validate());
    EXPECT_TRUE(h.eng->feed(0).is_synced());
    // Engine book best prices must equal the venue's (same stream).
    EXPECT_EQ(h.eng->feed(0).book().best_bid(), h.sim->best_bid(0));
    EXPECT_EQ(h.eng->feed(0).book().best_ask(), h.sim->best_ask(0));
    EXPECT_EQ(h.input_drops, 0u);
    EXPECT_EQ(h.eng->stats().journal_gaps, 0u);
}

TEST(VerticalSlice, RunToCompletionIsDeterministic) {
    // Identical hosts (same seed/config) must produce identical engine
    // observable state after the same number of steps (invariant 5, spec §7).
    TestHost a, b;
    a.run(1500);
    b.run(1500);
    EXPECT_EQ(a.eng->risk_gate(0).state().position,
              b.eng->risk_gate(0).state().position);
    EXPECT_EQ(a.eng->oms_engine().size(), b.eng->oms_engine().size());
    EXPECT_EQ(a.eng->last_client_id(), b.eng->last_client_id());
    EXPECT_EQ(a.tx_to_venue, b.tx_to_venue);
    EXPECT_EQ(a.sim->md_seq(), b.sim->md_seq());
}

TEST(VerticalSlice, KillSwitchStopsExposureAtBoundary) {
    TestHost h;
    h.run(500);
    ASSERT_FALSE(h.eng->killed());
    const auto tx_before = h.eng->stats().tx_enqueued;
    const auto pending_before = h.eng->risk_gate(0).state().pending_buy_lots +
                                h.eng->risk_gate(0).state().pending_sell_lots;
    h.eng->engage_kill_switch();
    h.run(300);
    // No NEW exposure leaves the engine: tx count frozen (strategy emits
    // enter/replace only; both are kill-switch-rejected at the risk gate).
    EXPECT_EQ(h.eng->stats().tx_enqueued, tx_before);
    // Pending exposure may only shrink (cancels/fills complete, none start).
    const auto pending_after = h.eng->risk_gate(0).state().pending_buy_lots +
                               h.eng->risk_gate(0).state().pending_sell_lots;
    EXPECT_LE(pending_after, pending_before);
    EXPECT_TRUE(h.eng->risk_gate(0).killed());
    EXPECT_TRUE(h.eng->killed());
    // The risk gate rejects new exposure directly (unit-level boundary check).
    EXPECT_EQ(h.eng->risk_gate(0).approve(kestrel::risk::Side::Buy, 1, 100'000,
                                          h.eng->last_event_ns() + 1),
              kestrel::risk::Decision::RejectKilled);
}
