// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end trunk-following test with the REAL decoder and a SIMULATED radio.
// World: the control channel of a real 851 MHz P25 system (recorded CQPSK control channel at 851.1125 MHz, whose real
// grants send calls to 853.1375 MHz -- 2.025 MHz away) plus a recorded voice transmission placed at 853.1375 MHz.
// Skipped unless SDRSTAR_DSD_NEO and SDRSTAR_FIXTURES are set.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "sdrstar/trunk_follower.h"
#include "sim_host.h"
#include "test_framework.h"

using namespace sdrstar;
using namespace sdrstar_test;

static const char* dsdExe() { return std::getenv("SDRSTAR_DSD_NEO"); }
static const char* fixDir() { return std::getenv("SDRSTAR_FIXTURES"); }

static std::vector<cf> loadFixture(const std::string& name) {
    std::vector<cf> iq;
    readCu8(std::string(fixDir()) + "/" + name + ".iq", iq);
    return iq;
}

template <class Pred>
static bool waitFor(TrunkFollower& f, double timeoutS, Pred pred) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < timeoutS) {
        if (pred(f.status())) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return pred(f.status());
}

static void dumpLog(const FollowStatus& st) {
    for (const auto& e : st.log) { std::printf("    [%6.2fs] %s%s\n", e.tSec, e.level == FollowEvent::Problem ? "PROBLEM: " : e.level == FollowEvent::Warn ? "warn: " : "", e.text.c_str()); }
}

static const double kCc = 851112500.0;
static const double kVc = 853137500.0;

TEST(out_of_bandwidth_with_hop_disabled_is_refused_and_explained) {
    if (!dsdExe() || !fixDir()) { std::printf("  (skipped)\n"); return; }
    SimHost host(851.2e6, 2.4e6);  // 1.92 MHz usable: control + voice (2.025 MHz apart) cannot both fit
    host.addSignal("CC", kCc, loadFixture("p25p1_cqpsk_cc"));
    host.addSignal("VC", kVc, loadFixture("p25p1_cqpsk_vc"), /*loop=*/false, /*startOnListen=*/true);
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = dsdExe();
    c.controlHz = kCc;
    c.allowHop = false;
    std::string err;
    CHECK(f.start(c, err));
    bool refused = waitFor(f, 25.0, [](const FollowStatus& s) { return s.refusals >= 1; });
    FollowStatus st = f.status();
    dumpLog(st);
    CHECK(refused);
    CHECK(st.state != FollowState::Hopped);
    CHECK(st.state != FollowState::FollowingCall);
    CHECK_EQ(st.hops, 0);
    CHECK_CONTAINS(st.log.back().text + st.problem, "out of bandwidth");
    CHECK_CONTAINS(st.detail, "1.92 MHz");
    CHECK_NEAR(host.centerHz(), 851.2e6, 1.0);  // the window must NOT have moved
    CHECK(host.tunes().empty());
    f.stop();
}

TEST(out_of_bandwidth_hops_to_the_voice_channel_then_returns) {
    if (!dsdExe() || !fixDir()) { return; }
    SimHost host(851.2e6, 2.4e6);
    host.addSignal("CC", kCc, loadFixture("p25p1_cqpsk_cc"));
    host.addSignal("VC", kVc, loadFixture("p25p1_cqpsk_vc"), /*loop=*/false, /*startOnListen=*/true);
    TrunkFollower f(&host);
    std::atomic<long long> audioFrames{0};
    f.setAudioCallback([&](const int16_t*, size_t n, int, int) { audioFrames += (long long)n; });
    TrunkFollower::Config c;
    c.dsdExe = dsdExe();
    c.controlHz = kCc;
    c.allowHop = true;
    std::string err;
    CHECK(f.start(c, err));
    bool hopped = waitFor(f, 25.0, [](const FollowStatus& s) { return s.hops >= 1; });
    FollowStatus st = f.status();
    CHECK(hopped);
    CHECK(st.windowRetunes >= 1);
    // wait to be back on the control channel
    bool back = waitFor(f, 20.0, [](const FollowStatus& s) { return s.state == FollowState::OnControl && s.hops >= 1 && std::fabs(s.currentHz - kCc) < 800; });
    st = f.status();
    dumpLog(st);
    CHECK(back);
    CHECK(st.controlVisible);
    auto tunes = host.tunes();
    CHECK(tunes.size() >= 2);  // out to the voice window, back to the control window
    bool sawVoiceWindow = false;
    for (auto& t : tunes) { if (std::fabs(t.centerHz - kVc) < 0.9e6 && std::fabs(t.centerHz - kCc) > 0.9e6) { sawVoiceWindow = true; } }
    CHECK(sawVoiceWindow);
    std::printf("    decoded voice frames received: %lld\n", (long long)audioFrames.load());
    f.stop();
}

TEST(faster_sample_rate_keeps_control_and_voice_in_one_window) {
    if (!dsdExe() || !fixDir()) { return; }
    SimHost host(851.2e6, 2.56e6);  // 2.048 MHz usable: pair (2.0375 MHz span) just fits -> one retune, control stays in view
    host.addSignal("CC", kCc, loadFixture("p25p1_cqpsk_cc"));
    host.addSignal("VC", kVc, loadFixture("p25p1_cqpsk_vc"), /*loop=*/false, /*startOnListen=*/true);
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = dsdExe();
    c.controlHz = kCc;
    std::string err;
    CHECK(f.start(c, err));
    bool followed = waitFor(f, 25.0, [](const FollowStatus& s) { return s.state == FollowState::FollowingCall || s.channelMoves + s.windowRetunes >= 2; });
    FollowStatus st = f.status();
    dumpLog(st);
    CHECK(followed);
    CHECK_EQ(st.hops, 0);              // never had to leave the control channel
    CHECK_EQ(st.refusals, 0);
    f.stop();
}

// The USER moves the receiver window (types another frequency, drags the waterfall) while following.
TEST(user_moving_the_window_away_pauses_following_and_it_resumes) {
    if (!dsdExe() || !fixDir()) { return; }
    SimHost host(851.2e6, 2.4e6);
    host.addSignal("CC", kCc, loadFixture("p25p1_cqpsk_cc"));
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = dsdExe();
    c.controlHz = kCc;
    std::string err;
    CHECK(f.start(c, err));
    CHECK(waitFor(f, 25.0, [](const FollowStatus& s) { return s.state == FollowState::OnControl; }));
    // 1. the user retunes far away: following pauses, says why, and the control channel is not claimed to be visible
    CHECK(host.setCenter(700e6, err));
    CHECK(waitFor(f, 5.0, [](const FollowStatus& s) { return s.state == FollowState::Paused; }));
    FollowStatus st = f.status();
    CHECK_CONTAINS(st.problem, "moved away");
    CHECK_CONTAINS(st.headline, "paused");
    CHECK(!st.controlVisible);
    // 2. the user moves back by hand: it resumes by itself and the message goes away
    CHECK(host.setCenter(851.2e6, err));
    CHECK(waitFor(f, 5.0, [](const FollowStatus& s) { return s.state == FollowState::OnControl && s.problem.empty(); }));
    // 3. away again, then the "Return to control channel" button
    CHECK(host.setCenter(700e6, err));
    CHECK(waitFor(f, 5.0, [](const FollowStatus& s) { return s.state == FollowState::Paused; }));
    CHECK(f.returnToControl());
    CHECK(waitFor(f, 5.0, [](const FollowStatus& s) { return s.state == FollowState::OnControl && s.controlVisible; }));
    CHECK(std::fabs(host.centerHz() - kCc) < 0.96e6);
    dumpLog(f.status());
    f.stop();
}

TEST(bad_inputs_fail_cleanly) {
    SimHost host(851.2e6, 2.4e6);
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = "/nonexistent/dsd-neo";
    c.controlHz = kCc;
    std::string err;
    CHECK(!f.start(c, err));
    CHECK(!f.running());
    TrunkFollower g(&host);
    TrunkFollower::Config d;
    d.dsdExe = "/bin/true";
    d.controlHz = 3.0e9;  // above the tuner range
    CHECK(!g.start(d, err));
    CHECK_CONTAINS(err, "tuning range");
}

TEST_MAIN("trunk_follower")
