// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>

#include "sdrstar/resampler.h"
#include "test_framework.h"

using namespace sdrstar;

// RMS and dominant frequency of a mono signal.
static double rms(const std::vector<int16_t>& v, size_t skip) {
    double a = 0; size_t n = 0;
    for (size_t i = skip; i < v.size(); ++i) { a += (double)v[i] * v[i]; ++n; }
    return std::sqrt(a / std::max<size_t>(1, n));
}
static double zeroCrossFreq(const std::vector<int16_t>& v, double rate, size_t skip) {
    int zc = 0;
    for (size_t i = skip + 1; i < v.size(); ++i) { if ((v[i - 1] < 0) != (v[i] < 0)) { zc++; } }
    return zc / 2.0 / ((v.size() - skip) / rate);
}

static std::vector<int16_t> tone(double hz, double rate, double secs, double amp = 12000) {
    std::vector<int16_t> v((size_t)(rate * secs));
    for (size_t i = 0; i < v.size(); ++i) { v[i] = (int16_t)(amp * std::sin(2 * M_PI * hz * (double)i / rate)); }
    return v;
}

TEST(downsample_48k_to_22050_preserves_in_band_tones) {
    for (double f : {300.0, 1200.0, 2200.0, 4000.0, 6000.0}) {   // the band pagers / AFSK / DTMF audio lives in
        PolyphaseResampler r(48000, 22050);
        auto in = tone(f, 48000, 1.0);
        std::vector<int16_t> out;
        r.process(in.data(), in.size(), out);
        CHECK(std::fabs((double)out.size() - 22050.0) < 4.0);
        CHECK_NEAR(rms(out, 400), 12000 / std::sqrt(2.0), 12000 / std::sqrt(2.0) * 0.06);   // within 0.5 dB
        CHECK_NEAR(zeroCrossFreq(out, 22050, 400), f, f * 0.01 + 3);
    }
}

TEST(downsample_rejects_out_of_band_tones) {
    PolyphaseResampler r(48000, 22050);
    auto in = tone(15000, 48000, 1.0);   // above the output Nyquist (aliases to 7.05 kHz if not removed)
    std::vector<int16_t> out;
    r.process(in.data(), in.size(), out);
    CHECK(rms(out, 400) < 12000 / std::sqrt(2.0) * 0.03);   // > 30 dB down
}

TEST(upsample_22050_to_48k_roundtrip) {
    PolyphaseResampler up(22050, 48000), down(48000, 22050);
    auto in = tone(1700, 22050, 1.0);
    std::vector<int16_t> mid, back;
    up.process(in.data(), in.size(), mid);
    CHECK(std::fabs((double)mid.size() - 48000.0) < 6.0);
    down.process(mid.data(), mid.size(), back);
    CHECK(std::fabs((double)back.size() - 22050.0) < 6.0);
    CHECK_NEAR(rms(back, 800), 12000 / std::sqrt(2.0), 12000 / std::sqrt(2.0) * 0.06);
}

TEST(chunked_input_gives_identical_output) {
    auto in = tone(900, 48000, 0.5);
    PolyphaseResampler a(48000, 22050), b(48000, 22050);
    std::vector<int16_t> oa, ob;
    a.process(in.data(), in.size(), oa);
    for (size_t i = 0; i < in.size(); i += 777) { b.process(in.data() + i, std::min<size_t>(777, in.size() - i), ob); }
    CHECK_EQ(oa.size(), ob.size());
    bool same = oa == ob;
    CHECK(same);
}

TEST_MAIN("resampler")
