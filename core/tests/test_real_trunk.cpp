// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trunk following on REAL recorded air: three time-aligned 2.4 MS/s slices of an 800 MHz public-safety band
// (HackRF capture of a P25 Phase 1 system; control channel 856.7101 MHz, voice channels at 857.7100 and 858.7102 MHz).
// A simulated RTL-SDR window (2.4 MS/s, 1.92 MHz usable) is moved over that air by the real follower with the real
// DSD-neo.  Skipped unless SDRSTAR_DSD_NEO and SDRSTAR_REAL_SLICES (directory holding the three .cu8 slices) are set.
#include <chrono>
#include <cstdlib>
#include <thread>

#include "sdrstar/trunk_follower.h"
#include "slice_host.h"
#include "test_framework.h"

using namespace sdrstar;
using namespace sdrstar_test;

static const double kCc = 856710100.0;

static bool loadSlices(SliceHost& h) {
    const char* d = std::getenv("SDRSTAR_REAL_SLICES");
    if (!d) { return false; }
    std::string dir = d;   // the three slice directories live side by side under SDRSTAR_REAL_SLICES
    return h.addSliceCu8(857.70e6, dir + "/p25_dmr_800mhz_hackrf_sliceA_857p70_2400k/ia862_sliceA_857.70MHz_2400k.cu8") &&
           h.addSliceCu8(859.55e6, dir + "/p25_dmr_800mhz_hackrf_sliceB_859p55_2400k/ia862_sliceB_859.55MHz_2400k.cu8") &&
           h.addSliceCu8(853.00e6, dir + "/p25_dmr_800mhz_hackrf_sliceC_853p00_2400k/ia862_sliceC_853.00MHz_2400k.cu8");
}

static void dumpLog(const FollowStatus& st) {
    for (const auto& e : st.log) { std::printf("    [%6.2fs] %s%s\n", e.tSec, e.level == FollowEvent::Problem ? "PROBLEM: " : e.level == FollowEvent::Warn ? "warn: " : "", e.text.c_str()); }
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

static bool enabled() { return std::getenv("SDRSTAR_DSD_NEO") && std::getenv("SDRSTAR_REAL_SLICES"); }

// The recording's control channel announces itself; the follower must identify the real system from the air alone.
TEST(real_p25_control_channel_is_identified) {
    if (!enabled()) { std::printf("  (skipped: set SDRSTAR_DSD_NEO and SDRSTAR_REAL_SLICES)\n"); return; }
    SliceHost host(857.2e6, 2.4e6);
    CHECK(loadSlices(host));
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = std::getenv("SDRSTAR_DSD_NEO");
    c.controlHz = kCc;
    std::string err;
    CHECK(f.start(c, err));
    bool ok = waitFor(f, 20.0, [](const FollowStatus& s) { return s.system.find("Site 21") != std::string::npos; });
    FollowStatus st = f.status();
    dumpLog(st);
    CHECK(ok);
    CHECK_CONTAINS(st.system, "NAC 0x451");
    CHECK_CONTAINS(st.system, "WACN BEE00");
    CHECK_CONTAINS(st.system, "Site 21");
    f.stop();
}

// A real grant sends the call 4 MHz away from the control channel (860.7125 MHz): impossible to see together with a
// 1.92 MHz window.  With hopping off the follower must say so and stay put; with hopping on it must leave and come back.
TEST(real_p25_grant_out_of_bandwidth_is_explained_when_hop_is_off) {
    if (!enabled()) { return; }
    SliceHost host(856.7125e6, 2.4e6);
    CHECK(loadSlices(host));
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = std::getenv("SDRSTAR_DSD_NEO");
    c.controlHz = kCc;
    c.allowHop = false;
    std::string err;
    CHECK(f.start(c, err));
    bool refused = waitFor(f, 25.0, [](const FollowStatus& s) { return s.refusals >= 1; });
    FollowStatus st = f.status();
    dumpLog(st);
    CHECK(refused);
    CHECK_EQ(st.hops, 0);
    CHECK_CONTAINS(st.log.back().text + st.problem, "out of bandwidth");
    CHECK(host.tunes().empty());
    f.stop();
}

TEST(real_p25_grant_out_of_bandwidth_hops_and_returns_when_hop_is_on) {
    if (!enabled()) { return; }
    SliceHost host(856.7125e6, 2.4e6);
    CHECK(loadSlices(host));
    TrunkFollower f(&host);
    TrunkFollower::Config c;
    c.dsdExe = std::getenv("SDRSTAR_DSD_NEO");
    c.controlHz = kCc;
    c.allowHop = true;
    std::string err;
    CHECK(f.start(c, err));
    bool hopped = waitFor(f, 25.0, [](const FollowStatus& s) { return s.hops >= 1; });
    CHECK(hopped);
    bool back = waitFor(f, 20.0, [](const FollowStatus& s) { return s.state == FollowState::OnControl && s.hops >= 1 && s.controlVisible; });
    FollowStatus st = f.status();
    dumpLog(st);
    CHECK(back);
    auto tunes = host.tunes();
    bool away = false, home = false;
    for (auto& t : tunes) {
        if (std::fabs(t.centerHz - 860.71e6) < 0.2e6) { away = true; }
        if (away && std::fabs(t.centerHz - 856.71e6) < 0.2e6) { home = true; }
    }
    CHECK(away);
    CHECK(home);
    f.stop();
}

TEST_MAIN("real_trunk")
