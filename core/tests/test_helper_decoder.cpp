// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-tool tests of HelperDecoder.  Skipped unless the tool and its sample are provided:
//   SDRSTAR_MULTIMON + SDRSTAR_PAGER_RAW  (22.05 kHz s16le mono audio of a POCSAG/FLEX recording)
//   SDRSTAR_RTL433   + SDRSTAR_ISM_CU8    (250 kS/s cu8 capture of an ISM sensor, e.g. rtl_433_tests nexus/03)
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "sdrstar/helper_decoder.h"
#include "test_framework.h"

using namespace sdrstar;

static std::vector<uint8_t> slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(summaries_are_readable) {
    CHECK_EQ(summarizeHelperLine("POCSAG1200: Address: 1 Function: 0"), std::string("POCSAG1200: Address: 1 Function: 0"));
    std::string s = summarizeHelperLine(R"({"time" : "@0.08s", "model" : "Nexus-TH", "id" : 201, "channel" : 3, "battery_ok" : 1, "temperature_C" : 29.400, "humidity" : 30})");
    CHECK_CONTAINS(s, "Nexus-TH");
    CHECK_CONTAINS(s, "temperature_C=29.4");
    CHECK_CONTAINS(s, "humidity=30");
    CHECK(s.find("time") == std::string::npos);
    CHECK(s.find("29.400") == std::string::npos);
}

TEST(specs_are_complete) {
    for (const char* id : {"pager", "aprs", "eas", "ism"}) {
        const HelperSpec* sp = findHelperSpec(id);
        CHECK(sp != nullptr);
        if (sp) { CHECK(!sp->args.empty()); }
    }
    CHECK(findHelperSpec("nope") == nullptr);
}

TEST(missing_tool_fails_cleanly) {
    HelperDecoder d;
    std::string err;
    CHECK(!d.startProcess(*findHelperSpec("pager"), "/nonexistent/multimon-ng", 929.5e6, err));
}

TEST(multimon_decodes_pager_audio_through_the_48k_to_22k_path) {
    const char* tool = std::getenv("SDRSTAR_MULTIMON");
    const char* raw = std::getenv("SDRSTAR_PAGER_RAW");
    if (!tool || !raw) { std::printf("  (skipped)\n"); return; }
    std::vector<uint8_t> bytes = slurp(raw);
    CHECK(bytes.size() > 40000);
    std::vector<int16_t> a22((int16_t*)bytes.data(), (int16_t*)bytes.data() + bytes.size() / 2);
    // 22.05 kHz -> 48 kHz (what the FM discriminator would deliver); HelperDecoder converts back down.
    PolyphaseResampler up(22050, 48000);
    std::vector<int16_t> a48;
    up.process(a22.data(), a22.size(), a48);
    HelperDecoder d;
    std::string err;
    CHECK(d.startProcess(*findHelperSpec("pager"), tool, 929.5e6, err));
    for (size_t i = 0; i < a48.size(); i += 4800) {
        d.feedPcm48k(a48.data() + i, std::min<size_t>(4800, a48.size() - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(4));  // ~25x faster than real time
    }
    auto t0 = std::chrono::steady_clock::now();
    while (d.messageCount() < 1 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto ls = d.lines(5);
    CHECK(d.messageCount() >= 1);
    if (!ls.empty()) { std::printf("  first message: %s\n", ls[0].text.c_str()); }
    bool pager = false;
    for (auto& l : d.lines(50)) { if (l.text.find("POCSAG") != std::string::npos || l.text.find("FLEX") != std::string::npos) { pager = true; } }
    CHECK(pager);
    d.stop();
}

TEST(rtl433_decodes_an_ism_sensor_from_float_iq_on_stdin) {
    const char* tool = std::getenv("SDRSTAR_RTL433");
    const char* cu8 = std::getenv("SDRSTAR_ISM_CU8");
    if (!tool || !cu8) { return; }
    std::vector<uint8_t> bytes = slurp(cu8);
    CHECK(bytes.size() > 100000);
    std::vector<cf> iq;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) { iq.emplace_back(((float)bytes[i] - 127.5f) / 127.5f, ((float)bytes[i + 1] - 127.5f) / 127.5f); }
    std::string err;
    HelperDecoder e;
    CHECK(e.startProcess(*findHelperSpec("ism"), tool, 433.92e6, err));
    for (size_t i = 0; i < iq.size(); i += 5000) {
        e.feedIq(iq.data() + i, std::min<size_t>(5000, iq.size() - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ~10x real time
    }
    CHECK_EQ(e.droppedBytes(), 0LL);
    auto t0 = std::chrono::steady_clock::now();
    while (e.messageCount() < 1 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto ls = e.lines(5);
    CHECK(e.messageCount() >= 1);
    if (!ls.empty()) {
        std::printf("  decoded: %s\n", ls[0].text.c_str());
        CHECK_CONTAINS(ls[0].text, "Nexus-TH");
    }
    e.stop();
}

TEST_MAIN("helper_decoder")
