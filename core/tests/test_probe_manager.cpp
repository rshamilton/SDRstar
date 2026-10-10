// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-decoder test of the probe manager and channel decoder on the simulated radio.
// Skipped unless SDRSTAR_DSD_NEO and SDRSTAR_FIXTURES are set.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "sdrstar/channel_decoder.h"
#include "sdrstar/probe_manager.h"
#include "sim_host.h"
#include "test_framework.h"

using namespace sdrstar;
using namespace sdrstar_test;

static const char* dsdExe() { return std::getenv("SDRSTAR_DSD_NEO"); }
static const char* fixDir() { return std::getenv("SDRSTAR_FIXTURES"); }
static std::vector<cf> load(const std::string& n) {
    std::vector<cf> iq;
    readCu8(std::string(fixDir()) + "/" + n + ".iq", iq);
    return iq;
}

static ProbeProgress runProbe(SimHost& host, double freq, double bw) {
    ProbeManager pm;
    ProbeManager::Config c;
    c.dsdExe = dsdExe();
    std::string err;
    auto presets = candidatePresetsFor(bw, freq);
    if (!pm.start(&host, freq, presets, c, err)) { std::printf("  probe start failed: %s\n", err.c_str()); return ProbeProgress(); }
    ProbeProgress p;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0) {
        p = pm.poll();
        if (p.finished) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return p;
}

TEST(candidate_presets_follow_bandwidth_and_band) {
    auto narrow = candidatePresetsFor(4000, 460e6);
    CHECK_EQ(narrow.size(), (size_t)2);
    auto wide = candidatePresetsFor(9000, 852e6);
    CHECK(std::find(wide.begin(), wide.end(), "p25p1") != wide.end());
    CHECK(std::find(wide.begin(), wide.end(), "m17") == wide.end());
    auto ham = candidatePresetsFor(9000, 440e6);
    CHECK(std::find(ham.begin(), ham.end(), "m17") != ham.end());
    CHECK(std::find(ham.begin(), ham.end(), "dstar") != ham.end());
    CHECK(std::find(ham.begin(), ham.end(), "edacs") == ham.end());       // land-mobile-only modes are not tried on a ham band
    CHECK(std::find(wide.begin(), wide.end(), "dstar") == wide.end());    // amateur-only modes are not tried on public-safety bands
    CHECK(std::find(wide.begin(), wide.end(), "edacs") != wide.end());
    CHECK(candidatePresetsFor(9000, 30e6).size() >= 8);                   // unknown band: try everything
}

TEST(probe_identifies_p25_control_channel_in_a_busy_window) {
    if (!dsdExe() || !fixDir()) { std::printf("  (skipped)\n"); return; }
    SimHost host(851.4e6, 2.4e6);
    host.addSignal("CC", 851.1125e6, load("p25p1_cqpsk_cc"));
    host.addSignal("DMR", 852.0e6, load("dmr_voice"));
    ProbeProgress p = runProbe(host, 851.1125e6, 12500);
    std::printf("  %s | %s | %s (%.1f s)\n", p.status.c_str(), p.best.title.c_str(), p.best.detail.c_str(), p.elapsedSec);
    CHECK(p.finished);
    CHECK(p.best.claimed);
    CHECK(p.best.controlChannel);
    CHECK(p.best.trunkable);
    CHECK_CONTAINS(p.best.detail, "NAC 0xD6");
    CHECK(p.elapsedSec < 8.0);
}

TEST(probe_identifies_dmr_and_dstar_and_ignores_noise) {
    if (!dsdExe() || !fixDir()) { return; }
    SimHost host(851.4e6, 2.4e6);
    host.addSignal("DMR", 852.0e6, load("dmr_voice"));
    SimHost hamHost(440.4e6, 2.4e6);   // D-STAR is amateur-only, so it is only tried (and tested) on an amateur band
    hamHost.addSignal("DSTAR", 440.0e6, load("dstar"));
    ProbeProgress a = runProbe(host, 852.0e6, 12500);
    std::printf("  852.0 MHz: %s\n", a.best.claimed ? a.best.title.c_str() : "(none)");
    CHECK(a.best.claimed);
    CHECK_CONTAINS(a.best.protocol, "DMR");
    CHECK(!a.best.trunkable);
    ProbeProgress b = runProbe(hamHost, 440.0e6, 12500);
    std::printf("  440.0 MHz: %s\n", b.best.claimed ? b.best.title.c_str() : "(none)");
    CHECK(b.best.claimed);
    CHECK_CONTAINS(b.best.protocol, "D-STAR");
    // an empty channel must never be identified as anything
    ProbeProgress n = runProbe(host, 851.7e6, 12500);
    std::printf("  851.7 MHz (empty): %s\n", n.best.claimed ? n.best.title.c_str() : "(none)");
    CHECK(!n.best.claimed);
}

TEST(channel_decoder_plays_voice_from_a_conventional_digital_channel) {
    if (!dsdExe() || !fixDir()) { return; }
    SimHost host(851.4e6, 2.4e6);
    host.addSignal("DMR", 852.0e6, load("dmr_voice"));
    ChannelDecoder cd;
    std::atomic<long long> frames{0};
    cd.setAudioCallback([&](const int16_t*, size_t n, int, int) { frames += (long long)n; });
    ChannelDecoder::Config c;
    c.dsdExe = dsdExe();
    c.presetId = "dmr";
    c.freqHz = 852.0e6;
    std::string err;
    CHECK(cd.start(&host, c, err));
    auto t0 = std::chrono::steady_clock::now();
    while (frames.load() < 2000 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 15.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(frames.load() >= 2000);
    CHECK_EQ(cd.state().identity.colorCode, 2);
    cd.stop();
    CHECK(!cd.running());
    CHECK(!cd.start(&host, ChannelDecoder::Config{dsdExe(), "", "nonsense", 852e6, 12500, {}}, err));
}

TEST_MAIN("probe_manager")
