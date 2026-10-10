// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ledger keeps a signal's identity steady: it changes only when the signal really changed (or the user asked).
#include "sdrstar/identity_ledger.h"
#include "test_framework.h"

using namespace sdrstar;

static ProbeVerdict claim(const char* preset, const char* title, bool control = false, const char* detail = "") {
    ProbeVerdict v;
    v.claimed = true;
    v.presetId = preset;
    v.protocol = preset;
    v.title = title;
    v.detail = detail;
    v.confidence = 0.9;
    v.controlChannel = control;
    v.trunkable = control;
    return v;
}
static ProbeVerdict none() { return ProbeVerdict(); }

static const double F = 851.0125e6;

TEST(first_answer_is_taken_and_found_again_nearby) {
    IdentityLedger l;
    CHECK(l.find(F) == nullptr);
    CHECK(l.offer(F, 12500, claim("dmr", "DMR channel"), 10, false) == IdentityOutcome::Added);
    const IdentityEntry* e = l.find(F + 2000);   // the cursor / track is a little off
    CHECK(e != nullptr);
    if (e) { CHECK_EQ(e->verdict.title, std::string("DMR channel")); }
    CHECK(l.find(F + 25000) == nullptr);         // another channel
}

TEST(a_single_different_probe_does_not_flip_the_identity) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 10, false);
    CHECK(l.offer(F, 12500, claim("p25p2", "P25 Phase 2 (TDMA) channel"), 700, false) == IdentityOutcome::KeptOld);
    CHECK_EQ(l.find(F)->verdict.presetId, std::string("dmr"));
    // ... and the original protocol coming back clears the doubt
    CHECK(l.offer(F, 12500, claim("dmr", "DMR channel"), 730, false) == IdentityOutcome::Confirmed);
    CHECK_EQ(l.find(F)->disagreements, 0);
    // a different protocol seen once, then ANOTHER different one: still no change
    l.offer(F, 12500, claim("p25p2", "x"), 800, false);
    CHECK(l.offer(F, 12500, claim("nxdn96", "y"), 830, false) == IdentityOutcome::KeptOld);
    CHECK_EQ(l.find(F)->verdict.presetId, std::string("dmr"));
}

TEST(two_agreeing_probes_for_another_protocol_do_change_it) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 10, false);
    CHECK(l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel"), 700, false) == IdentityOutcome::KeptOld);
    CHECK(l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel"), 730, false) == IdentityOutcome::Replaced);
    CHECK_EQ(l.find(F)->verdict.presetId, std::string("p25p1"));
    CHECK_EQ(l.find(F)->disagreements, 0);
    CHECK_EQ(l.size(), (size_t)1);
}

TEST(the_user_asking_again_gets_the_new_answer_immediately) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 10, false);
    CHECK(l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel"), 20, true) == IdentityOutcome::Replaced);
    CHECK_EQ(l.find(F)->verdict.presetId, std::string("p25p1"));
}

TEST(a_quiet_automatic_probe_never_erases_an_identity_but_a_manual_one_is_recorded) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 10, false);
    CHECK(l.offer(F, 12500, none(), 700, false) == IdentityOutcome::Ignored);
    CHECK(l.find(F)->verdict.claimed);
    CHECK(l.offer(F, 12500, none(), 800, true) == IdentityOutcome::Replaced);
    CHECK(!l.find(F)->verdict.claimed);
    // a channel that was never identified: automatic "nothing" is remembered, a later claim replaces it
    CHECK(l.offer(F + 50000, 12500, none(), 5, false) == IdentityOutcome::Added);
    CHECK(l.offer(F + 50000, 12500, claim("p25p1", "P25 Phase 1 channel"), 50, false) == IdentityOutcome::Replaced);
    CHECK(l.find(F + 50000)->verdict.claimed);
}

TEST(a_control_channel_stays_a_control_channel_when_one_probe_sees_less_control_traffic) {
    IdentityLedger l;
    l.offer(F, 12500, claim("p25p1", "P25 Phase 1 control channel (trunked system)", true, "NAC 0x293"), 10, false);
    l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel", false, ""), 700, false);
    const IdentityEntry* e = l.find(F);
    CHECK(e->verdict.controlChannel);
    CHECK(e->verdict.trunkable);
    CHECK_CONTAINS(e->verdict.title, "control channel");
    CHECK_EQ(e->verdict.detail, std::string("NAC 0x293"));   // kept when the new probe has no detail
    // the other way: a plain channel that turns out to carry control traffic is upgraded
    IdentityLedger m;
    m.offer(F, 12500, claim("dmr", "DMR channel"), 10, false);
    m.offer(F, 12500, claim("dmr", "DMR control channel (trunked system)", true), 700, false);
    CHECK(m.find(F)->verdict.controlChannel);
    // ... and a user's re-check can downgrade it
    l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel"), 900, true);
    CHECK(!l.find(F)->verdict.controlChannel);
}

TEST(identified_signals_are_reverified_on_a_schedule_and_sooner_after_a_disagreement) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 100, false);
    CHECK(!l.verificationDue(F, 100 + 599));
    CHECK(l.verificationDue(F, 100 + 601));
    l.offer(F, 12500, claim("p25p1", "P25 Phase 1 channel"), 800, false);   // disagreement
    CHECK(!l.verificationDue(F, 800 + 10));
    CHECK(l.verificationDue(F, 800 + 26));
    CHECK(!l.verificationDue(F + 99000, 5000));                              // unknown frequency
    IdentityLedger u;
    u.offer(F, 12500, none(), 0, false);
    CHECK(!u.verificationDue(F, 99999));                                     // never identified: nothing to verify
}

TEST(memory_outlives_a_vanished_signal_for_a_while_then_expires) {
    IdentityLedger l;
    l.offer(F, 12500, claim("dmr", "DMR channel"), 100, false);
    l.touch(F, 150);
    l.expire(150 + 500);
    CHECK(l.find(F) != nullptr);            // keyed up again within minutes: still known
    l.expire(150 + 601);
    CHECK(l.find(F) == nullptr);            // long gone: a new signal there gets a fresh probe
    CHECK_EQ(l.size(), (size_t)0);
    l.offer(F, 12500, claim("dmr", "DMR channel"), 1000, false);
    l.forget(F);
    CHECK(l.find(F) == nullptr);
}

TEST_MAIN("identity_ledger")
