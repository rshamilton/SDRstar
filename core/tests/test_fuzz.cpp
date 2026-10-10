// SPDX-License-Identifier: GPL-3.0-or-later
//
// Robustness: every place that parses data from outside the process is fed garbage (random bytes, truncated and
// mutated realistic input, NaN / Inf / enormous numbers, very long and very deeply nested input).  The assertions are
// "does not crash, hang or corrupt memory" (run these under AddressSanitizer / UBSan) plus the few invariants that
// matter downstream (a retune request is always a finite, positive frequency).  Deterministic seeds.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "sdrstar/adsb.h"
#include "sdrstar/dsd_session.h"
#include "sdrstar/helper_decoder.h"
#include "sdrstar/probe_evidence.h"
#include "sdrstar/rigctl_bridge.h"
#include "test_framework.h"

using namespace sdrstar;

namespace {
std::string randomBytes(std::mt19937& rng, size_t n) {
    std::string s(n, '\0');
    for (auto& c : s) { c = (char)(rng() & 0xFF); }
    return s;
}
std::string mutate(std::mt19937& rng, std::string s) {
    if (s.empty()) { return s; }
    const int k = 1 + (int)(rng() % 4);
    for (int i = 0; i < k; ++i) {
        const size_t pos = rng() % s.size();
        switch (rng() % 4) {
        case 0: s[pos] = (char)(rng() & 0xFF); break;
        case 1: s.erase(pos, 1 + rng() % 5); break;
        case 2: s.insert(pos, randomBytes(rng, 1 + rng() % 8)); break;
        default: s.resize(pos); break;
        }
        if (s.empty()) { break; }
    }
    return s;
}
}  // namespace

TEST(rigctl_commands_never_crash_and_never_hand_the_planner_a_bad_frequency) {
    RigctlBridge b;
    int badFreq = 0, calls = 0;
    RigctlBridge::Handlers h;
    h.setFrequency = [&](double hz) {
        ++calls;
        if (!std::isfinite(hz) || hz <= 0.0) { ++badFreq; }
        return true;
    };
    h.getFrequency = [] { return 851.0125e6; };
    int port = 0;
    std::string err;
    CHECK(b.start(h, port, err));
    std::mt19937 rng(1);
    const char* seeds[] = {"F 851012500", "f", "M FM 12500", "m", "F 851012500.5", "\\dump_state", "q", "F", "F  ", "M", "t", "T 1", "V VFOA", "v"};
    for (int i = 0; i < 20000; ++i) {
        std::string line;
        switch (rng() % 6) {
        case 0: line = randomBytes(rng, rng() % 64); break;
        case 1: line = mutate(rng, seeds[rng() % 14]); break;
        case 2: line = std::string("F ") + (rng() % 2 ? "nan" : "inf") + (rng() % 2 ? "" : "e999"); break;
        case 3: line = "F " + std::to_string((long long)(rng()) * (rng() % 2 ? -1 : 1)); break;
        case 4: line = "F " + std::string(1 + rng() % 5000, (char)('0' + rng() % 10)); break;
        default: line = seeds[rng() % 14]; break;
        }
        bool close = false;
        std::string reply = b.handleLine(line, close);
        if (reply.size() > 4096) { CHECK(false); break; }
    }
    // explicit hostile values
    for (const char* v : {"F nan", "F -nan", "F inf", "F -inf", "F 1e999", "F -1", "F 0", "F 0x10", "F 1,5", "F 1e-320"}) {
        bool close = false;
        (void)b.handleLine(v, close);
    }
    std::printf("    %d accepted retune requests, %d bad ones reached the handler\n", calls, badFreq);
    CHECK_EQ(badFreq, 0);
    b.stop();
}

TEST(decoder_telemetry_and_log_parsing_survive_garbage) {
    DsdSession s;
    std::mt19937 rng(2);
    const char* api[] = {
        R"({"type":"state","protocol":"P25p1","nac":214,"wacn":"92065","sysid":213,"site":1})",
        R"({"event":"call","tg":1201,"src":4455,"state":2,"enc":false,"freq":853137500})",
        R"({"type":"sync","count":3})",
        R"({"id":1,"ok":true,"result":{"token":"x"}})",
    };
    const char* log[] = {
        "NAC: 0xD6 WACN: 0x92065 SYS: 0xD5 RFSS: 1 Site: 1",
        "TG: 1201 SRC: 4455 ENC",
        "Color Code: 07",
        "Sync: +P25p1 voice",
        "freq 853.1375 MHz",
    };
    for (int i = 0; i < 20000; ++i) {
        std::string a = (rng() % 3 == 0) ? randomBytes(rng, rng() % 300) : mutate(rng, api[rng() % 4]);
        std::string l = (rng() % 3 == 0) ? randomBytes(rng, rng() % 300) : mutate(rng, log[rng() % 5]);
        s.ingestApiLine(a);
        s.ingestLogLine(l);
    }
    // enormous and deeply nested input
    s.ingestApiLine(std::string(2000000, '['));
    s.ingestApiLine(std::string(200000, '{') + std::string(200000, '}'));
    s.ingestApiLine("{\"nac\":" + std::string(100000, '9') + "}");
    s.ingestLogLine(std::string(3000000, 'A'));
    s.ingestLogLine("NAC: " + std::string(100000, 'F'));
    DsdState st = s.state();
    CHECK(st.calls.size() < 1000);
    CHECK(s.recentLog(100000).size() <= 100000);
}

TEST(probe_evidence_rules_survive_garbage_logs) {
    std::mt19937 rng(3);
    DsdState st;
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::string> log;
        for (int k = 0; k < 40; ++k) { log.push_back(rng() % 2 ? randomBytes(rng, rng() % 200) : mutate(rng, "Sync: +P25p1 NAC: 0x1A2 TSBK Color Code: 5 DMR slot 1")); }
        for (const char* id : {"p25p1", "p25p2", "dmr", "nxdn96", "nxdn48", "dpmr", "dstar", "ysf", "m17", "provoice", "edacs", "nonsense"}) {
            ProbeVerdict v = evaluateProbe(id, st, log);
            CHECK(v.confidence >= 0.0 && v.confidence <= 1.01);
        }
    }
}

TEST(helper_output_summaries_survive_garbage) {
    std::mt19937 rng(4);
    const char* seeds[] = {R"({"time" : "@0.08s", "model" : "Nexus-TH", "id" : 201, "temperature_C" : 29.400, "humidity" : 30})", R"({"a":[1,2,{"b":null}],"model":1})",
                           R"({"model":"x","k":1e999})", "POCSAG1200: Address: 1  Function: 0  Alpha: hi"};
    for (int i = 0; i < 20000; ++i) {
        std::string s = (rng() % 3 == 0) ? randomBytes(rng, rng() % 400) : mutate(rng, seeds[rng() % 4]);
        std::string out = summarizeHelperLine(s);
        CHECK(out.size() < 4 * s.size() + 4096);
    }
    (void)summarizeHelperLine(std::string(1000000, '['));
    (void)summarizeHelperLine("{\"model\":\"" + std::string(1000000, 'x') + "\"}");
    std::string deep;
    for (int i = 0; i < 50000; ++i) { deep += "{\"a\":"; }
    deep += "1";
    for (int i = 0; i < 50000; ++i) { deep += "}"; }
    (void)summarizeHelperLine(deep);
}

TEST(adsb_decoding_survives_random_frames_and_samples) {
    std::mt19937 rng(5);
    AdsbTable t;
    for (int i = 0; i < 200000; ++i) {
        uint8_t m[14];
        for (auto& b : m) { b = (uint8_t)rng(); }
        m[0] = (uint8_t)((17 << 3) | (rng() & 7));   // force DF17 so the type-specific code is exercised
        if (rng() % 2) { m[4] = (uint8_t)(((rng() % 23) << 3) | (rng() & 7)); }
        t.update(m, i * 0.001, rng() % 4 == 0);
    }
    for (const auto& a : t.snapshot(1)) {
        if (a.hasPos) { CHECK(a.lat >= -90.0 && a.lat <= 90.0 && a.lon >= -180.0 && a.lon <= 180.0); }
        if (a.hasVel) { CHECK(std::isfinite(a.speedKt) && a.headingDeg >= 0.0 && a.headingDeg < 360.0 + 1e-9); }
        (void)AdsbTable::describe(a);
    }
    t.prune(1e9, 1.0);
    CHECK_EQ(t.size(), (size_t)0);
    // raw samples: random, NaN, Inf, denormals, constant, huge
    long long n = 0;
    ModeSDemod d([&](const uint8_t*, int, bool) { ++n; });
    std::vector<cf> x(50000);
    for (int round = 0; round < 40; ++round) {
        for (auto& v : x) {
            switch (round % 5) {
            case 0: v = cf((float)(rng() % 2000) / 1000.f - 1.f, (float)(rng() % 2000) / 1000.f - 1.f); break;
            case 1: v = (rng() % 50 == 0) ? cf(NAN, 0.f) : cf(0.1f, 0.2f); break;
            case 2: v = (rng() % 50 == 0) ? cf(INFINITY, -INFINITY) : cf(0.3f, 0.1f); break;
            case 3: v = cf(1e-42f, 1e-42f); break;
            default: v = cf(1e30f, -1e30f); break;
            }
        }
        d.feed(x.data(), x.size());
    }
    CHECK(n >= 0);
}

TEST_MAIN("fuzz")
