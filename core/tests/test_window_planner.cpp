// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/window_planner.h"
#include "test_framework.h"

using namespace sdrstar;

static ReceiverCaps v4() { return ReceiverCaps(); }  // 2.4 MS/s -> 1.92 MHz usable

TEST(usable_span_is_80_percent) { CHECK_NEAR(v4().usableSpanHz(), 1.92e6, 1.0); }

TEST(voice_inside_window_just_moves_channel) {
    // Control channel 851.0125, voice 851.9 MHz, window centred between them.
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{851.9e6, 12500, "voice channel"};
    Plan p = planWindow(v4(), 851.4e6, {cc}, {vc});
    CHECK(p.kind == PlanKind::KeepWindow);
    CHECK(!p.hardwareRetune);
    CHECK(p.controlStillVisible);
    CHECK_NEAR(p.gapMs, 0, 0.001);
}

TEST(voice_beyond_edge_but_pair_fits_shifts_window_and_keeps_control) {
    // Window centred on control; voice 1.5 MHz away: outside +/-0.96 MHz, but the pair spans only 1.5 MHz.
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{852.5125e6, 12500, "voice channel"};
    Plan p = planWindow(v4(), 851.0125e6 + 200e3, {cc}, {vc});
    CHECK(p.kind == PlanKind::ShiftWindow);
    CHECK(p.hardwareRetune);
    CHECK(p.controlStillVisible);
    CHECK(channelFits(v4(), p.centerHz, cc));
    CHECK(channelFits(v4(), p.centerHz, vc));
    CHECK_NEAR(p.gapMs, v4().retuneMs, 0.001);
    // The shifted window should be roughly midway so neighbours on either side still fit.
    CHECK_NEAR(p.centerHz, 0.5 * (851.0125e6 + 852.5125e6), 20e3);
}

TEST(span_too_wide_refuses_when_hopping_disabled_and_explains) {
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{856.2125e6, 12500, "voice channel"};  // 5.2 MHz away
    PlanOptions o; o.allowHop = false;
    Plan p = planWindow(v4(), 851.0125e6 + 100e3, {cc}, {vc}, o);
    CHECK(p.kind == PlanKind::Refuse);
    CHECK(p.refuse == RefuseReason::OutOfBandwidth);
    CHECK_CONTAINS(p.summary, "out of bandwidth");
    CHECK_CONTAINS(p.detail, "5.2 MHz");
    CHECK_CONTAINS(p.detail, "1.92 MHz");
    CHECK(p.neededSpanHz > 5.2e6);
}

TEST(span_too_wide_hops_when_allowed_and_control_is_lost) {
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{856.2125e6, 12500, "voice channel"};
    Plan p = planWindow(v4(), 851.0125e6 + 100e3, {cc}, {vc});
    CHECK(p.kind == PlanKind::HopAway);
    CHECK(!p.controlStillVisible);
    CHECK(p.hardwareRetune);
    CHECK(channelFits(v4(), p.centerHz, vc));
    CHECK(!channelFits(v4(), p.centerHz, cc));
    CHECK_CONTAINS(p.detail, "return when it ends");
}

TEST(return_to_control_after_hop_is_a_shift) {
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    Plan p = planWindow(v4(), 856.2125e6, {cc}, {});
    CHECK(p.kind == PlanKind::ShiftWindow);
    CHECK(channelFits(v4(), p.centerHz, cc));
}

TEST(frequency_above_tuner_range_is_refused) {
    WindowChannel cc{851e6, 12500, "control channel"};
    WindowChannel vc{1850e6, 12500, "voice channel"};
    Plan p = planWindow(v4(), 851e6, {cc}, {vc});
    CHECK(p.kind == PlanKind::Refuse);
    CHECK(p.refuse == RefuseReason::OutOfTunerRange);
    CHECK_CONTAINS(p.summary, "1.850 GHz");
    CHECK_CONTAINS(p.detail, "1.766 GHz");
}

TEST(frequency_below_hf_floor_is_refused) {
    WindowChannel vc{100e3, 12500, "signal"};
    Plan p = planWindow(v4(), 7e6, {}, {vc});
    CHECK(p.kind == PlanKind::Refuse);
    CHECK(p.refuse == RefuseReason::OutOfTunerRange);
}

TEST(channel_wider_than_receiver_is_refused_even_with_hop) {
    ReceiverCaps c = v4();
    c.sampleRateHz = 250e3;  // 200 kHz usable
    WindowChannel wfm{100.1e6, 200e3 + 50e3, "broadcast FM"};
    Plan p = planWindow(c, 100.1e6, {}, {wfm});
    CHECK(p.kind == PlanKind::Refuse);
    CHECK(p.refuse == RefuseReason::OutOfBandwidth);
}

TEST(window_near_tuner_limits_clamps_centre) {
    ReceiverCaps c = v4();
    WindowChannel a{1765.5e6, 12500, "a"};
    WindowChannel b{1764.0e6, 12500, "b"};
    Plan p = planWindow(c, 1700e6, {a}, {b});
    CHECK(p.kind == PlanKind::ShiftWindow);
    CHECK(p.centerHz <= c.maxHz);
    CHECK(channelFits(c, p.centerHz, a));
    CHECK(channelFits(c, p.centerHz, b));
}

TEST(dc_spike_is_avoided_when_a_new_centre_is_chosen) {
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{852.5125e6, 12500, "voice channel"};
    Plan p = planWindow(v4(), 800e6, {cc}, {vc});
    CHECK(p.kind == PlanKind::ShiftWindow);
    CHECK(!p.nearDcSpike);
    CHECK(std::fabs(p.centerHz - cc.freqHz) >= v4().dcGuardHz);
    CHECK(std::fabs(p.centerHz - vc.freqHz) >= v4().dcGuardHz);
}

TEST(sample_rate_changes_what_fits) {
    WindowChannel cc{851.0125e6, 12500, "control channel"};
    WindowChannel vc{852.5125e6, 12500, "voice channel"};  // 1.5 MHz apart
    ReceiverCaps slow = v4(); slow.sampleRateHz = 1.024e6;  // ~819 kHz usable
    PlanOptions o; o.allowHop = false;
    Plan p = planWindow(slow, 851.0125e6 + 50e3, {cc}, {vc}, o);
    CHECK(p.kind == PlanKind::Refuse);
    ReceiverCaps fast = v4(); fast.sampleRateHz = 3.2e6;
    Plan q = planWindow(fast, 851.0125e6 + 50e3, {cc}, {vc}, o);
    CHECK(q.kind != PlanKind::Refuse);
}

TEST(wide_voice_pair_exactly_at_limit) {
    // Pair exactly fills the usable window (edge to edge) -> fits; 100 kHz more does not.
    ReceiverCaps c = v4();
    const double usable = c.usableSpanHz();
    WindowChannel a{851e6, 12500, "a"};
    WindowChannel b{851e6 + usable - 12500, 12500, "b"};
    Plan p = planWindow(c, 800e6, {a}, {b});
    CHECK(p.kind == PlanKind::ShiftWindow);
    WindowChannel b2{b.freqHz + 100e3, 12500, "b2"};
    PlanOptions o; o.allowHop = false;
    Plan q = planWindow(c, 800e6, {a}, {b2}, o);
    CHECK(q.kind == PlanKind::Refuse);
}

TEST(empty_request_is_invalid) {
    Plan p = planWindow(v4(), 100e6, {}, {});
    CHECK(p.kind == PlanKind::Refuse);
    CHECK(p.refuse == RefuseReason::InvalidRequest);
}

TEST(frequency_formatting) {
    CHECK_EQ(formatFrequency(851.0125e6), std::string("851.0125 MHz"));
    CHECK_EQ(formatFrequency(162.55e6), std::string("162.550 MHz"));
    CHECK_EQ(formatFrequency(1090e6), std::string("1.090 GHz"));
    CHECK_EQ(formatFrequency(7.074e6), std::string("7.074 MHz"));
    CHECK_EQ(formatSpan(2.4e6), std::string("2.4 MHz"));
    CHECK_EQ(formatSpan(1.92e6), std::string("1.92 MHz"));
    CHECK_EQ(formatSpan(250e3), std::string("250 kHz"));
}

TEST_MAIN("window_planner")
