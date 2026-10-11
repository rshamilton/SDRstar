// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration test: drives a REAL dsd-neo process through DsdSession using the 48 kHz IQ fixtures that ship
// with dsd-neo (verified by that project's own CI).  Skipped unless:
//     SDRSTAR_DSD_NEO   = path to the dsd-neo executable
//     SDRSTAR_FIXTURES  = path to dsd-neo's tests/fixtures/iq directory
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "sdrstar/dsd_session.h"
#include "sdrstar/dsp_lite.h"
#include "audio_check.h"
#include "test_framework.h"

using namespace sdrstar;

static const char* dsdExe() { return std::getenv("SDRSTAR_DSD_NEO"); }
static const char* fixDir() { return std::getenv("SDRSTAR_FIXTURES"); }
static bool haveEnv() { return dsdExe() && fixDir(); }

// Fixture IQ (48 kHz cu8) -> FM discriminator PCM (what the SDR++ module will feed).
static std::vector<int16_t> fixturePcm(const std::string& name) {
    std::vector<cf> iq;
    std::vector<int16_t> pcm;
    if (!readCu8(std::string(fixDir()) + "/" + name + ".iq", iq)) { return pcm; }
    FmDiscriminator d(48000.0, 6250.0);
    for (auto& s : iq) { pcm.push_back(toS16(d.process(s))); }
    return pcm;
}

// Feed the PCM in real time (looping) until `done(state)` or timeout.
template <class Done>
static bool feedUntil(DsdSession& s, const std::vector<int16_t>& pcm, double timeoutS, Done done) {
    const size_t chunk = 960;  // 20 ms
    size_t pos = 0;
    auto t0 = std::chrono::steady_clock::now();
    long long sent = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < timeoutS) {
        size_t n = std::min(chunk, pcm.size() - pos);
        s.pushPcm(pcm.data() + pos, n);
        pos = (pos + n) % pcm.size();
        sent += (long long)n;
        if (done(s.state())) { return true; }
        auto due = t0 + std::chrono::microseconds((long long)(sent * 1e6 / 48000.0));
        std::this_thread::sleep_until(due);
    }
    return done(s.state());
}

TEST(missing_executable_fails_cleanly) {
    DsdSession s;
    DsdSession::Config c;
    c.exePath = "/nonexistent/dsd-neo";
    c.mode = {"-f1"};
    std::string err;
    CHECK(!s.start(c, err));
    CHECK_CONTAINS(err, "cannot execute");
}

TEST(api_and_log_ingestion_unit) {
    DsdSession s;
    s.ingestApiLine(R"({"type":"system","seq":1,"protocol":"P25p1","vc_freq_hz":0,"cc_freq_hz":381725000,"trunking":false,"p25_neighbors":[{"freq_hz":386750000,"wacn":781824,"sysid":293,"rfss":5,"site":30,"current_cc":false}]})");
    s.ingestApiLine(R"({"type":"quality","seq":2,"cc_fec":{"valid":true,"ok":525,"err":21,"ok_pct":96.15}})");
    s.ingestApiLine(R"({"type":"call","seq":3,"slots":[{"slot":0,"state":2,"name":"Dispatch","tg_text":"101","src_text":"Unit 7","tg_id":101,"enc":false,"frequency_hz":851375000,"ota_source_id":7}]})");
    s.ingestApiLine("not json at all");  // must be ignored, not crash
    s.ingestLogLine("\x1b[36m00:00:00 Sync: +P25p1 \x1b[0m WACN: BEE00; SYS: 14C; NAC/CC: 140; RFSS: 005; Site: 009; TSBK");
    s.ingestLogLine("00:00:01 Sync: +DMR   slot1  [SLOT2] | Color Code=02 | VC1");
    DsdState st = s.state();
    CHECK_EQ(st.protocol, std::string("P25p1"));
    CHECK_NEAR(st.ccFreqHz, 381725000, 0.5);
    CHECK_EQ(st.neighbors.size(), (size_t)1);
    CHECK_EQ(st.neighbors[0].sysid, 293LL);
    CHECK(st.quality.valid);
    CHECK_EQ(st.quality.ccOk, 525LL);
    CHECK_EQ(st.calls.size(), (size_t)1);
    CHECK_EQ(st.calls[0].tgId, 101LL);
    CHECK_EQ(st.identity.nac, 0x140LL);
    CHECK_EQ(st.identity.wacn, 0xBEE00LL);
    CHECK_EQ(st.identity.sysid, 0x14CLL);
    CHECK_EQ(st.identity.rfss, 5);
    CHECK_EQ(st.identity.site, 9);
    CHECK_EQ(st.identity.colorCode, 2);
    CHECK_EQ(st.syncLines, 2LL);
}

TEST(real_dsd_p25_control_channel_is_identified) {
    if (!haveEnv()) { std::printf("  (skipped: set SDRSTAR_DSD_NEO and SDRSTAR_FIXTURES)\n"); return; }
    auto pcm = fixturePcm("p25p1_c4fm_cc");
    CHECK(!pcm.empty());
    DsdSession s;
    DsdSession::Config c;
    c.exePath = dsdExe();
    c.mode = {"-f1"};
    std::string err;
    CHECK(s.start(c, err));
    bool ok = feedUntil(s, pcm, 20.0, [](const DsdState& st) { return st.quality.ccOk >= 100 && st.ccFreqHz > 0 && st.apiConnected; });
    DsdState st = s.state();
    CHECK(ok);
    CHECK_EQ(st.protocol, std::string("P25p1"));
    CHECK_NEAR(st.ccFreqHz, 381725000.0, 1.0);     // the CC announces its own frequency
    CHECK(st.quality.ccOkPct > 80.0);
    CHECK_EQ(st.identity.nac, 0x140LL);
    CHECK(!st.neighbors.empty());
    s.stop();
    CHECK(!s.running());
}

TEST(real_dsd_dmr_color_code_and_voice_audio) {
    if (!haveEnv()) { return; }
    auto pcm = fixturePcm("dmr_voice");
    CHECK(!pcm.empty());
    DsdSession s;
    DsdSession::Config c;
    c.exePath = dsdExe();
    c.mode = {"-fs"};
    c.wantAudio = true;
    std::atomic<long long> frames{0};
    std::atomic<double> energy{0};
    s.setAudioCallback([&](const int16_t* d, size_t n, int ch, int rate) {
        (void)rate;
        frames += (long long)n;
        double e = 0;
        for (size_t i = 0; i < n * ch; ++i) { e += (double)d[i] * d[i]; }
        energy = energy + e;
    });
    std::string err;
    CHECK(s.start(c, err));
    bool ok = feedUntil(s, pcm, 20.0, [&](const DsdState& st) { return st.identity.colorCode == 2 && frames.load() > 800; });
    CHECK(ok);
    DsdState st = s.state();
    CHECK_EQ(st.identity.colorCode, 2);
    CHECK(frames.load() > 800);             // decoded voice reached us (8 kHz stereo frames)
    CHECK(energy.load() > 1e6);             // and it is not silence
    s.stop();
}

// DSD-neo writes P25 Phase 1 voice as MONO.  Reading it as stereo (the old assumption) plays it back at twice the pitch
// and speed, which is what "I never heard any good audio" sounded like.  This runs the real decoder on a real P25
// voice recording and checks that what SDRstar hands to the speakers has the pitch of a human voice.
TEST(real_dsd_p25_phase1_voice_is_mono_and_speech_like) {
    if (!haveEnv()) { return; }
    auto pcm = fixturePcm("p25p1_cqpsk_vc");
    CHECK(!pcm.empty());
    DsdSession s;
    DsdSession::Config c;
    c.exePath = dsdExe();
    c.mode = {"-f1"};
    c.wantAudio = true;
    std::mutex mtx;
    std::vector<int16_t> audio;
    std::atomic<int> channels{0};
    s.setAudioCallback([&](const int16_t* d, size_t n, int ch, int rate) {
        (void)rate;
        channels = ch;
        std::lock_guard<std::mutex> lk(mtx);
        audio.insert(audio.end(), d, d + n * (size_t)ch);
    });
    std::string err;
    CHECK(s.start(c, err));
    feedUntil(s, pcm, 25.0, [&](const DsdState&) {
        std::lock_guard<std::mutex> lk(mtx);
        return audio.size() > 8000 * 4;   // a few seconds of speech
    });
    s.stop();
    CHECK_EQ(channels.load(), 1);
    std::vector<int16_t> copy;
    {
        std::lock_guard<std::mutex> lk(mtx);
        copy = audio;
    }
    int voiced = 0;
    const double pitch = sdrstar_test::medianPitchHz(sdrstar_test::firstChannel(copy, channels.load() ? channels.load() : 1), 8000, &voiced);
    std::printf("    P25p1 voice: %zu samples, %d voiced windows, median pitch %.0f Hz\n", copy.size(), voiced, pitch);
    CHECK(voiced >= 3);
    CHECK(pitch > 90.0 && pitch < 220.0);   // twice the speed would read 300 Hz and up
}

TEST(output_channel_count_follows_the_decode_mode) {
    CHECK_EQ(dsdOutputChannels({"-f1"}), 1);   // P25 Phase 1
    CHECK_EQ(dsdOutputChannels({"-fn"}), 1);   // NXDN 9600
    CHECK_EQ(dsdOutputChannels({"-fi"}), 1);   // NXDN 4800
    CHECK_EQ(dsdOutputChannels({"-fm"}), 1);   // dPMR
    CHECK_EQ(dsdOutputChannels({"-fd"}), 1);   // D-STAR
    CHECK_EQ(dsdOutputChannels({"-fy"}), 1);   // YSF
    CHECK_EQ(dsdOutputChannels({"-fz"}), 1);   // M17
    CHECK_EQ(dsdOutputChannels({"-fp"}), 1);   // ProVoice
    CHECK_EQ(dsdOutputChannels({"-fh"}), 1);   // EDACS
    CHECK_EQ(dsdOutputChannels({"-ft"}), 2);   // trunking helper (P25 1 + 2, DMR)
    CHECK_EQ(dsdOutputChannels({"-fs"}), 2);   // DMR
    CHECK_EQ(dsdOutputChannels({"-f2"}), 2);   // P25 Phase 2
    CHECK_EQ(dsdOutputChannels({}), 2);        // DSD-neo's own default
    // every preset SDRstar offers agrees with the table
    for (const auto& p : dsdPresets()) { CHECK(dsdOutputChannels(p.flags) == 1 || dsdOutputChannels(p.flags) == 2); }
}

TEST(stop_kills_the_child_and_is_idempotent) {
    if (!haveEnv()) { return; }
    DsdSession s;
    DsdSession::Config c;
    c.exePath = dsdExe();
    c.mode = {"-f1"};
    std::string err;
    CHECK(s.start(c, err));
    CHECK(s.running());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s.stop();
    CHECK(!s.running());
    s.stop();  // second stop must be harmless
    // Restart on the same object works.
    CHECK(s.start(c, err));
    s.stop();
}

TEST_MAIN("dsd_session")
