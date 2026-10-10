// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADS-B: published reference messages (J. Sun, "The 1090 MHz Riddle"), a synthetic Mode S modulator (frames, noise,
// bit errors, back-to-back frames, chunked delivery) and, when provided, a real 2 MS/s recording.
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>

#include "sdrstar/adsb.h"
#include "test_framework.h"

using namespace sdrstar;

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) { v.push_back((uint8_t)std::strtol(std::string(s + i, 2).c_str(), nullptr, 16)); }
    return v;
}

// Mode S transmitter at 2 MS/s: 8 us preamble + 112 bits PPM, as float IQ (carrier phase irrelevant: magnitude is used).
static std::vector<cf> modulate(const std::vector<uint8_t>& frame, float amp, size_t padBefore, size_t padAfter) {
    std::vector<float> m(padBefore, 0.0f);
    const int pre[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) { m.push_back(pre[i] ? amp : 0.0f); }
    for (int i = 0; i < 112; ++i) {
        const bool bit = frame[i / 8] & (0x80 >> (i % 8));
        m.push_back(bit ? amp : 0.0f);
        m.push_back(bit ? 0.0f : amp);
    }
    for (size_t i = 0; i < padAfter; ++i) { m.push_back(0.0f); }
    std::vector<cf> out(m.size());
    for (size_t i = 0; i < m.size(); ++i) { out[i] = cf(m[i], 0.0f); }
    return out;
}

static void addNoise(std::vector<cf>& x, float sigma, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, sigma);
    for (auto& v : x) { v += cf(nd(rng), nd(rng)); }
}

TEST(crc24_accepts_the_published_messages_and_rejects_a_flipped_bit) {
    for (const char* h : {"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F"}) {
        auto m = hex(h);
        CHECK_EQ(m.size(), (size_t)14);
        CHECK_EQ(modeSCrc24(m.data(), 112), 0u);
        m[6] ^= 0x10;
        CHECK(modeSCrc24(m.data(), 112) != 0u);
    }
}

TEST(identification_position_and_velocity_decode_to_the_published_values) {
    AdsbTable t;
    t.update(hex("8D4840D6202CC371C32CE0576098").data(), 1.0);
    t.update(hex("8D40621D58C382D690C8AC2863A7").data(), 2.0);   // even frame
    t.update(hex("8D40621D58C386435CC412692AD6").data(), 3.0);   // odd frame (newest)
    t.update(hex("8D485020994409940838175B284F").data(), 4.0);
    auto s = t.snapshot(1);
    CHECK_EQ(s.size(), (size_t)3);
    const AdsbAircraft *klm = nullptr, *pos = nullptr, *vel = nullptr;
    for (auto& a : s) {
        if (a.icao == 0x4840D6) { klm = &a; }
        if (a.icao == 0x40621D) { pos = &a; }
        if (a.icao == 0x485020) { vel = &a; }
    }
    CHECK(klm && pos && vel);
    if (klm) { CHECK_EQ(klm->callsign, std::string("KLM1023")); }
    if (pos) {
        CHECK(pos->hasAlt);
        CHECK_EQ(pos->altFt, 38000);
        CHECK(pos->hasPos);
        // The odd frame is the newest here, so the position is the one decoded from it (the book lists both).
        CHECK_NEAR(pos->lat, 52.26578, 0.0005);
        CHECK_NEAR(pos->lon, 3.93891, 0.0005);
    }
    // ...and with the even frame newest the same pair gives the even-frame position (52.25720, 3.91937).
    AdsbTable t2;
    t2.update(hex("8D40621D58C386435CC412692AD6").data(), 2.0);
    t2.update(hex("8D40621D58C382D690C8AC2863A7").data(), 3.0);
    auto s2 = t2.snapshot(1);
    CHECK_EQ(s2.size(), (size_t)1);
    if (!s2.empty()) {
        CHECK_NEAR(s2[0].lat, 52.25720, 0.0005);
        CHECK_NEAR(s2[0].lon, 3.91937, 0.0005);
    }
    if (vel) {
        CHECK(vel->hasVel);
        CHECK_NEAR(vel->speedKt, 159.2, 0.5);
        CHECK_NEAR(vel->headingDeg, 182.88, 0.1);
        CHECK(vel->hasVr);
        CHECK_EQ(vel->vrFpm, -832);
    }
}

TEST(local_cpr_decoding_follows_an_aircraft_with_a_single_frame) {
    AdsbTable t;
    t.update(hex("8D40621D58C382D690C8AC2863A7").data(), 1.0);
    t.update(hex("8D40621D58C386435CC412692AD6").data(), 2.0);   // global fix
    // the next even frame alone (> 10 s later, so no pair): decoded locally against the last position
    t.update(hex("8D40621D58C382D690C8AC2863A7").data(), 30.0);
    auto s = t.snapshot(1);
    CHECK_EQ(s.size(), (size_t)1);
    CHECK(s[0].hasPos);
    CHECK_NEAR(s[0].lat, 52.2572, 0.001);
    CHECK_NEAR(s[0].lon, 3.9194, 0.002);
}

TEST(cpr_zone_count_function_matches_the_standard) {
    CHECK_EQ(adsbNL(0.0), 59);
    CHECK_EQ(adsbNL(10.0), 59);
    CHECK_EQ(adsbNL(52.0), 36);
    CHECK_EQ(adsbNL(87.0), 2);
    CHECK_EQ(adsbNL(89.0), 1);
    CHECK_EQ(adsbNL(-52.0), 36);
}

TEST(demodulator_finds_frames_in_noise_across_chunk_boundaries) {
    std::vector<std::vector<uint8_t>> frames = {hex("8D4840D6202CC371C32CE0576098"), hex("8D40621D58C382D690C8AC2863A7"), hex("8D485020994409940838175B284F")};
    std::vector<cf> sig(700 + 3 * 1200 + 600, cf(0, 0));
    size_t at = 700;
    for (auto& f : frames) {
        auto fm = modulate(f, 1.0f, 0, 0);
        for (size_t i = 0; i < fm.size(); ++i) { sig[at + i] += fm[i]; }
        at += 1200;
    }
    addNoise(sig, 0.05f, 7);   // SNR around 23 dB per sample
    std::vector<std::vector<uint8_t>> got;
    ModeSDemod d([&](const uint8_t* m, int n, bool) { got.emplace_back(m, m + n); });
    // deliver in odd-sized chunks so frames straddle chunk boundaries
    size_t pos = 0;
    const size_t chunk[] = {137, 999, 61, 2500};
    int k = 0;
    while (pos < sig.size()) {
        size_t n = std::min(chunk[k++ % 4], sig.size() - pos);
        d.feed(sig.data() + pos, n);
        pos += n;
    }
    CHECK_EQ(got.size(), (size_t)3);
    for (size_t i = 0; i < got.size() && i < 3; ++i) { CHECK(got[i] == frames[i]); }
    CHECK_EQ(d.repaired(), 0LL);
}

TEST(a_single_corrupted_bit_is_repaired_and_two_are_not) {
    auto f = hex("8D40621D58C382D690C8AC2863A7");
    // flip one data bit AFTER modulation by flipping the PPM pair for bit 60
    auto sig = modulate(f, 1.0f, 100, 100);
    const size_t bitSample = 100 + 16 + 2 * 60;
    std::swap(sig[bitSample], sig[bitSample + 1]);
    std::vector<std::vector<uint8_t>> got;
    ModeSDemod d([&](const uint8_t* m, int n, bool) { got.emplace_back(m, m + n); });
    d.feed(sig.data(), sig.size());
    CHECK_EQ(got.size(), (size_t)1);
    if (!got.empty()) { CHECK(got[0] == f); }
    CHECK_EQ(d.repaired(), 1LL);
    // two flipped bits: must not be accepted (and certainly not silently "repaired" into a wrong message)
    std::swap(sig[100 + 16 + 2 * 20], sig[100 + 16 + 2 * 20 + 1]);
    ModeSDemod d2([&](const uint8_t* m, int n, bool) { got.emplace_back(m, m + n); });
    size_t before = got.size();
    d2.feed(sig.data(), sig.size());
    CHECK_EQ(got.size(), before);
}

TEST(noise_alone_never_produces_a_frame) {
    std::vector<cf> noise(400000, cf(0, 0));
    addNoise(noise, 0.1f, 99);
    long long n = 0;
    ModeSDemod d([&](const uint8_t*, int, bool) { ++n; });
    d.feed(noise.data(), noise.size());
    CHECK_EQ(n, 0LL);
}

TEST(the_real_recording_decodes_aircraft_if_provided) {
    const char* path = std::getenv("SDRSTAR_ADSB_CU8");
    if (!path) {
        const char* dir = std::getenv("SDRSTAR_TEST_DATA_DIR");
        static std::string p;
        if (dir) { p = std::string(dir) + "/signals/adsb_modes1_2000k.cu8"; std::ifstream f(p); if (f) { path = p.c_str(); } }
    }
    if (!path) { std::printf("  (skipped: run tools/fetch_test_samples.sh for the ADS-B capture)\n"); return; }
    std::vector<cf> iq;
    CHECK(readCu8(path, iq));
    AdsbTable t;
    ModeSDemod d([&](const uint8_t* m, int, bool r) { t.update(m, 1.0, r); });
    for (size_t i = 0; i < iq.size(); i += 4096) { d.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i)); }
    std::printf("  %lld frames (%lld repaired) from %zu aircraft\n", d.frames(), d.repaired(), t.size());
    // The reference decoder (antirez dump1090) finds 120 DF17 frames in this 0.18 s capture (217 messages counting the
    // other Mode S types, which we do not decode). Every frame is CRC-verified and they all come from one aircraft.
    CHECK(d.frames() >= 120);
    CHECK_EQ(t.size(), (size_t)1);
    auto s = t.snapshot(1);
    bool found = false;
    for (auto& a : s) {
        std::printf("    %s\n", AdsbTable::describe(a).c_str());
        if (a.icao == 0x4D2023) { found = true; CHECK_CONTAINS(a.callsign, "AMC421"); }
    }
    CHECK(found);
}

// The hidden channel SDRstar opens is band-limited (1.9 MHz wide: what fits the usable window of the default
// 2.4 MS/s). Measured limit: the 0.5 us pulses blur together below about 1.8 MHz and bits become undecidable, which is
// why ADS-B needs the 2.4 MS/s sample rate.
TEST(a_band_limited_channel_still_decodes_the_real_recording) {
    const char* path = std::getenv("SDRSTAR_ADSB_CU8");
    static std::string p;
    if (!path) {
        const char* dir = std::getenv("SDRSTAR_TEST_DATA_DIR");
        if (dir) { p = std::string(dir) + "/signals/adsb_modes1_2000k.cu8"; std::ifstream f(p); if (f) { path = p.c_str(); } }
    }
    if (!path) { return; }
    std::vector<cf> iq;
    CHECK(readCu8(path, iq));
    for (double bw : {1.9e6, 1.8e6, 1.5e6}) {
        auto h = designLowpass(bw / 2.0, 2.0e6, 63);
        std::vector<cf> padded(31, cf(0, 0));
        padded.insert(padded.end(), iq.begin(), iq.end());
        std::vector<cf> y = firDecimate(padded, h, 1);
        long long n = 0;
        ModeSDemod d([&](const uint8_t*, int, bool) { ++n; });
        d.feed(y.data(), y.size());
        std::printf("    %.1f MHz wide channel: %lld frames%s\n", bw / 1e6, n, bw < 1.8e6 ? "  (known limit: too narrow)" : "");
        if (bw >= 1.8e6) { CHECK(n >= 100); }
    }
}

TEST_MAIN("adsb")
