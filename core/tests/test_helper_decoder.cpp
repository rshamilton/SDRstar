// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-tool tests of HelperDecoder.  Skipped unless the tool and its sample are provided:
//   SDRSTAR_MULTIMON (the multimon-ng program): pager audio, APRS and EAS signals.  The files are committed in
//       tests/data/signals (see its README); SDRSTAR_PAGER_RAW overrides the pager file.
//   SDRSTAR_RTL433 (the rtl_433 program): an ISM sensor capture that is NOT committed (licence): fetch it with
//       tools/fetch_test_samples.sh, or point SDRSTAR_ISM_CU8 at any 250 kS/s cu8 capture of a Nexus-TH sensor.
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "sdrstar/helper_decoder.h"
#include "test_framework.h"

using namespace sdrstar;

// Test data: an explicit environment variable wins, otherwise the committed files next to the tests.
static std::string dataFile(const char* envName, const char* relName) {
    if (envName) {
        if (const char* e = std::getenv(envName)) { return e; }
    }
    const char* dir = std::getenv("SDRSTAR_TEST_DATA_DIR");
    if (!dir) { return ""; }
    std::string p = std::string(dir) + "/signals/" + relName;
    std::ifstream f(p, std::ios::binary);
    return f ? p : "";
}

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
    CHECK(s.find("id=201") < s.find("temperature_C"));   // the sensor's own field order is kept
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

// A helper runs in a private directory with a private HOME, whatever the app's own environment is.
TEST(helpers_get_a_private_working_directory_and_home) {
    char tmpl[] = "/tmp/sdrstar-helper-XXXXXX";
    const char* base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) { return; }
    std::string dir = std::string(base) + "/work";
    HelperSpec spec;
    spec.id = "probe";
    spec.exeName = "env";
    spec.input = HelperSpec::Pcm22050;
    HelperDecoder d;
    d.setWorkDir(dir);
    std::string err;
    CHECK(d.startProcess(spec, "/usr/bin/env", 0, err));
    auto t0 = std::chrono::steady_clock::now();
    while (d.running() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool home = false, cfg = false;
    for (auto& l : d.rawLines()) {
        if (l == "HOME=" + dir) { home = true; }
        if (l == "XDG_CONFIG_HOME=" + dir + "/config") { cfg = true; }
    }
    CHECK(home);
    CHECK(cfg);
    d.stop();
    HelperDecoder p2;
    p2.setWorkDir(dir);
    spec.exeName = "pwd";
    CHECK(p2.startProcess(spec, "/bin/pwd", 0, err));
    t0 = std::chrono::steady_clock::now();
    while (p2.running() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto lines = p2.rawLines();
    CHECK(!lines.empty());
    if (!lines.empty()) {
        std::error_code ec;
        CHECK_EQ(std::filesystem::canonical(lines[0], ec).string(), std::filesystem::canonical(dir, ec).string());
    }
    p2.stop();
    std::filesystem::remove_all(base);
}

TEST(multimon_decodes_pager_audio_through_the_48k_to_22k_path) {
    const char* tool = std::getenv("SDRSTAR_MULTIMON");
    const std::string raw = dataFile("SDRSTAR_PAGER_RAW", "pocsag1200_22050.raw");
    if (!tool || raw.empty()) { std::printf("  (skipped)\n"); return; }
    std::vector<uint8_t> bytes = slurp(raw.c_str());
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

// Synthetic AFSK signals (tools/gen_afsk_samples.py): proves the APRS / EAS wiring and arguments end to end.
static bool decodesSynthetic(const char* specId, const char* file, const char* expect) {
    const char* tool = std::getenv("SDRSTAR_MULTIMON");
    const std::string path = dataFile(nullptr, file);   // committed synthetic files
    if (!tool || path.empty()) { return true; }   // skipped
    std::vector<uint8_t> bytes = slurp(path.c_str());
    if (bytes.size() < 20000) { return false; }
    std::vector<int16_t> a22((int16_t*)bytes.data(), (int16_t*)bytes.data() + bytes.size() / 2);
    PolyphaseResampler up(22050, 48000);
    std::vector<int16_t> a48;
    up.process(a22.data(), a22.size(), a48);
    HelperDecoder d;
    std::string err;
    if (!d.startProcess(*findHelperSpec(specId), tool, 0, err)) { return false; }
    for (size_t i = 0; i < a48.size(); i += 4800) {
        d.feedPcm48k(a48.data() + i, std::min<size_t>(4800, a48.size() - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    bool found = false;
    auto t0 = std::chrono::steady_clock::now();
    while (!found && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0) {
        for (auto& l : d.rawLines()) { if (l.find(expect) != std::string::npos) { found = true; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (found) { std::printf("  decoded: %s\n", d.lines(1).empty() ? "" : d.lines(1)[0].text.c_str()); }
    d.stop();
    return found;
}

TEST(aprs_packet_is_decoded_through_the_helper_path) { CHECK(decodesSynthetic("aprs", "aprs.raw", "APRS: N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W")); }
TEST(eas_alert_header_is_decoded_through_the_helper_path) { CHECK(decodesSynthetic("eas", "eas.raw", "EAS: ZCZC-WXR-RWT-012057")); }

// ADS-B is decoded inside SDRstar (no helper program): same HelperDecoder interface, lines are the aircraft table.
TEST(adsb_runs_as_a_native_helper_and_lists_aircraft) {
    const std::string cu8 = dataFile("SDRSTAR_ADSB_CU8", "adsb_modes1_2000k.cu8");
    const HelperSpec* sp = findHelperSpec("adsb");
    CHECK(sp != nullptr);
    if (sp) {
        CHECK(sp->native);
        CHECK_NEAR(sp->fixedFreqHz, 1090e6, 1.0);
    }
    if (cu8.empty() || !sp) { std::printf("  (skipped: run tools/fetch_test_samples.sh for the ADS-B capture)\n"); return; }
    std::vector<uint8_t> bytes = slurp(cu8.c_str());
    std::vector<cf> iq;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) { iq.emplace_back(((float)bytes[i] - 127.5f) / 127.5f, ((float)bytes[i + 1] - 127.5f) / 127.5f); }
    HelperDecoder d;
    std::string err;
    CHECK(d.startProcess(*sp, "", 0, err));   // no program to run
    CHECK(d.running());
    for (size_t i = 0; i < iq.size(); i += 5000) {
        d.feedIq(iq.data() + i, std::min<size_t>(5000, iq.size() - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));   // lets the table refresh timer fire
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    d.feedIq(iq.data(), 100);   // one more call after the refresh interval
    CHECK_EQ(d.messageCount(), 1LL);   // one aircraft
    auto ls = d.lines(5);
    CHECK_EQ(ls.size(), (size_t)1);
    if (!ls.empty()) {
        std::printf("  %s\n", ls[0].text.c_str());
        CHECK_CONTAINS(ls[0].text, "4D2023");
        CHECK_CONTAINS(ls[0].text, "AMC421");
    }
    d.stop();
    CHECK(!d.running());
}

TEST(rtl433_decodes_an_ism_sensor_from_float_iq_on_stdin) {
    const char* tool = std::getenv("SDRSTAR_RTL433");
    const std::string cu8 = dataFile("SDRSTAR_ISM_CU8", "nexus_g027_433.92M_250k.cu8");
    if (!tool || cu8.empty()) { std::printf("  (skipped: run tools/fetch_test_samples.sh for the sensor capture)\n"); return; }
    std::vector<uint8_t> bytes = slurp(cu8.c_str());
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
