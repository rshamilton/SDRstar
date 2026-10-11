// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/card_prefs.h"
#include "test_framework.h"

using namespace sdrstar;

TEST(tips_are_visible_by_default) {
    TipPrefs t;
    CHECK(t.enabled());
    CHECK(t.visible("band-hint"));
    CHECK_EQ(t.hiddenCount(), (size_t)0);
}

TEST(hiding_one_tip_leaves_the_others) {
    TipPrefs t;
    t.hide("signal-note");
    CHECK(!t.visible("signal-note"));
    CHECK(t.visible("band-hint"));
    CHECK_EQ(t.hiddenCount(), (size_t)1);
    t.hide("signal-note");   // idempotent
    CHECK_EQ(t.hiddenCount(), (size_t)1);
    t.hide("");              // an empty key is ignored
    CHECK_EQ(t.hiddenCount(), (size_t)1);
}

TEST(turning_tips_off_hides_all_of_them_but_remembers_the_list) {
    TipPrefs t;
    t.hide("band-hint");
    t.setEnabled(false);
    CHECK(!t.visible("band-hint"));
    CHECK(!t.visible("anything"));
    t.setEnabled(true);
    CHECK(!t.visible("band-hint"));     // still individually hidden
    CHECK(t.visible("signal-note"));
}

TEST(restore_brings_everything_back) {
    TipPrefs t;
    t.hide("band-hint");
    t.hide("signal-note");
    t.setEnabled(false);
    t.restoreAll();
    CHECK(t.enabled());
    CHECK_EQ(t.hiddenCount(), (size_t)0);
    CHECK(t.visible("band-hint"));
}

TEST(hidden_list_can_be_loaded) {
    TipPrefs t;
    t.setHidden({"a", "b"});
    CHECK(!t.visible("a"));
    CHECK(!t.visible("b"));
    CHECK(t.visible("c"));
}

// ---- auto-decode ------------------------------------------------------------------------------------------------

TEST(auto_decode_needs_a_rested_cursor_on_an_identified_signal) {
    AutoDecodeGate g;
    g.cursorMoved();
    CHECK(!g.shouldStartFromCursor(true, 0.2, true, true));    // not rested long enough
    CHECK(!g.shouldStartFromCursor(true, 0.6, true, false));   // not identified
    CHECK(!g.shouldStartFromCursor(true, 0.6, false, true));   // something else is running
    CHECK(!g.shouldStartFromCursor(false, 0.6, true, true));   // checkbox off
    CHECK(g.armed());                                          // none of the above used the visit up
    CHECK(g.shouldStartFromCursor(true, 0.5, true, true));
}

TEST(auto_decode_fires_once_per_visit) {
    AutoDecodeGate g;
    g.cursorMoved();
    CHECK(g.shouldStartFromCursor(true, 0.6, true, true));
    CHECK(!g.shouldStartFromCursor(true, 0.7, true, true));    // same visit: no second start
    CHECK(!g.shouldStartFromCursor(true, 5.0, true, true));
    g.cursorMoved();                                           // the user drags the cursor again
    CHECK(g.shouldStartFromCursor(true, 0.6, true, true));
}

TEST(stopping_a_decode_does_not_restart_it_under_the_same_cursor) {
    AutoDecodeGate g;
    g.cursorMoved();
    CHECK(g.shouldStartFromCursor(true, 0.6, true, true));
    g.disarm();                                                // user pressed Stop
    CHECK(!g.shouldStartFromCursor(true, 3.0, true, true));
    g.cursorMoved();
    CHECK(g.shouldStartFromCursor(true, 0.6, true, true));
}

TEST(auto_decode_is_quiet_before_the_cursor_ever_moved) {
    AutoDecodeGate g;                                          // e.g. just after start-up
    CHECK(!g.armed());
    CHECK(!g.shouldStartFromCursor(true, 10.0, true, true));
}

TEST(identify_then_claim_starts_decoding) {
    AutoDecodeGate g;
    g.cursorMoved();
    g.identifyRequested();
    CHECK(g.identifyPending());
    CHECK(g.identifyFinished(true, true, true));
    CHECK(!g.identifyPending());
    CHECK(!g.armed());                                         // the visit is served: no duplicate start from situation 1
    CHECK(!g.shouldStartFromCursor(true, 1.0, true, true));
}

TEST(identify_without_a_claim_or_with_the_checkbox_off_starts_nothing) {
    AutoDecodeGate g;
    g.identifyRequested();
    CHECK(!g.identifyFinished(true, false, true));             // nothing recognised
    g.identifyRequested();
    CHECK(!g.identifyFinished(false, true, true));             // checkbox off
    g.identifyRequested();
    CHECK(!g.identifyFinished(true, true, false));             // the user moved on while it ran
    CHECK(!g.identifyPending());
}

TEST(a_background_check_is_not_an_identify_request) {
    AutoDecodeGate g;
    CHECK(!g.identifyFinished(true, true, true));              // nobody pressed Identify
}

TEST(cancelling_identify_forgets_the_request) {
    AutoDecodeGate g;
    g.identifyRequested();
    g.identifyCancelled();
    CHECK(!g.identifyFinished(true, true, true));
}

TEST_MAIN("card_prefs")
