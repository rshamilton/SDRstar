// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression test for the probe evidence rules.  tests/data/probe_logs holds the decoder output of a measured
// confusion matrix: every shipped test recording (2-3 s of 48 kHz I/Q, fed as FM-discriminator PCM exactly as the
// app does) run through DSD-neo with every candidate preset, plus a pure-noise recording.
#include <fstream>
#include <map>

#include "sdrstar/probe_evidence.h"
#include "test_framework.h"

using namespace sdrstar;

#ifndef SDRSTAR_TEST_DATA_DIR
#define SDRSTAR_TEST_DATA_DIR "tests/data"
#endif

static std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream f(path, std::ios::binary);
    std::string l;
    while (std::getline(f, l)) { v.push_back(l); }
    return v;
}

static const char* kPresets[] = {"p25p1", "p25p2", "dmr", "nxdn96", "nxdn48", "dpmr", "dstar", "ysf", "m17", "provoice", "edacs"};

static std::vector<ProbeVerdict> runAll(const std::string& fixture) {
    std::vector<ProbeVerdict> out;
    for (const char* p : kPresets) {
        std::string path = std::string(SDRSTAR_TEST_DATA_DIR) + "/probe_logs/" + fixture + "." + p + ".txt";
        auto lines = readLines(path);
        CHECK(std::ifstream(path).good());  // the data file must exist (an empty log is legitimate: that preset never synced)
        DsdSession s;
        for (const auto& l : lines) { s.ingestLogLine(l); }
        out.push_back(evaluateProbe(p, s.state(), lines));
    }
    return out;
}

struct Expect {
    const char* fixture;
    const char* protocolPrefix;  // "" = nothing may be claimed
    bool allowNone;              // a documented weakness: claiming nothing is acceptable
    int control;                 // 1 must be control channel, 0 must not, -1 don't care
};

TEST(every_fixture_gets_the_right_verdict_and_noise_gets_none) {
    const Expect table[] = {
        {"p25p1_c4fm_cc", "P25 Phase 1", false, 1},
        {"p25p1_c4fm_vc", "P25 Phase 1", false, 0},
        {"p25p1_cqpsk_cc", "P25 Phase 1", false, 1},
        {"p25p1_cqpsk_vc", "P25 Phase 1", false, 0},
        {"p25p2_cc", "P25 Phase 2", false, -1},
        {"dmr_t3_cc", "DMR", false, -1},
        {"dmr_voice", "DMR", false, 0},
        {"nxdn48", "NXDN", false, -1},
        {"nxdn96", "NXDN", true, -1},      // audio path decodes this one poorly (documented limitation)
        {"dpmr", "dPMR", true, -1},        // CRC-valid dPMR content not produced through the audio path (documented)
        {"dstar", "D-STAR", false, -1},
        {"ysf", "Yaesu", false, -1},
        {"m17", "M17", false, -1},
        {"edacs", "EDACS", false, 1},
        {"provoice", "EDACS / ProVoice", false, -1},
        {"noise_floor", "", true, -1},
    };
    for (const Expect& e : table) {
        auto all = runAll(e.fixture);
        ProbeVerdict best = bestVerdict(all);
        std::printf("  %-16s -> %s%s\n", e.fixture, best.claimed ? best.title.c_str() : "(no claim)", best.claimed ? "" : "");
        // No claim may ever be for a DIFFERENT protocol family than the truth.
        for (const auto& v : all) {
            if (!v.claimed) { continue; }
            if (std::string(e.protocolPrefix).empty()) {
                CHECK(false);  // noise must never be identified
                std::printf("    false claim on %s: %s (%d)\n", e.fixture, v.title.c_str(), v.evidenceCount);
            } else if (v.protocol.find(e.protocolPrefix) == std::string::npos) {
                CHECK(false);
                std::printf("    wrong claim on %s: %s (%d)\n", e.fixture, v.title.c_str(), v.evidenceCount);
            }
        }
        if (std::string(e.protocolPrefix).empty()) {
            CHECK(!best.claimed);
            continue;
        }
        if (!e.allowNone) {
            CHECK(best.claimed);
            if (best.claimed) { CHECK(best.protocol.find(e.protocolPrefix) != std::string::npos); }
        }
        if (best.claimed && e.control >= 0) { CHECK_EQ(best.controlChannel, e.control == 1); }
    }
}

TEST(p25_control_channel_detail_carries_identity) {
    auto lines = readLines(std::string(SDRSTAR_TEST_DATA_DIR) + "/probe_logs/p25p1_c4fm_cc.p25p1.txt");
    DsdSession s;
    for (const auto& l : lines) { s.ingestLogLine(l); }
    ProbeVerdict v = evaluateProbe("p25p1", s.state(), lines);
    CHECK(v.claimed);
    CHECK(v.controlChannel);
    CHECK(v.trunkable);
    CHECK_CONTAINS(v.detail, "NAC 0x140");
    CHECK_CONTAINS(v.title, "control channel");
}

TEST(dstar_detail_has_callsign) {
    auto lines = readLines(std::string(SDRSTAR_TEST_DATA_DIR) + "/probe_logs/dstar.dstar.txt");
    DsdSession s;
    for (const auto& l : lines) { s.ingestLogLine(l); }
    ProbeVerdict v = evaluateProbe("dstar", s.state(), lines);
    CHECK(v.claimed);
    CHECK_CONTAINS(v.detail, "KB7WUK");
}

TEST(too_little_evidence_is_not_a_claim) {
    DsdState st;
    std::vector<std::string> lines = {"01:00:00 Sync: +P25p1 NAC/CC: 293;  LDU1", "01:00:00 Sync: +P25p1 NAC/CC: 293;  LDU2"};
    DsdSession s;
    for (auto& l : lines) { s.ingestLogLine(l); }
    ProbeVerdict v = evaluateProbe("p25p1", s.state(), lines);
    CHECK(!v.claimed);                 // 2 syncs < 3 required
    CHECK(v.confidence < 0.5);
    CHECK(!evaluateProbe("m17", st, {"Sync: -M17 BRT", "Sync: +M17 BRT", "Sync: -M17 LSF  CRC ERR"}).claimed);
}

TEST_MAIN("probe_evidence")
