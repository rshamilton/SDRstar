// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <random>

#include "sdrstar/band_plan.h"
#include "sdrstar/signal_detector.h"
#include "test_framework.h"

using namespace sdrstar;

namespace {

struct Lump {
    double offsetHz;   // from centre
    double bwHz;
    double snrDb;
    bool flat = true;  // flat-top (digital) vs peaky (analog voice)
};

// Synthetic dB spectrum: noise (averaged periodogram statistics) + signal lumps.
std::vector<float> makeSpectrum(int bins, double spanHz, const std::vector<Lump>& lumps, std::mt19937& rng, double noiseDb = -80.0,
                                bool dcSpike = false) {
    std::vector<float> db((size_t)bins);
    std::exponential_distribution<double> ex(1.0);
    const double binHz = spanHz / bins;
    for (int i = 0; i < bins; ++i) {
        double f = (i + 0.5) * binHz - spanHz / 2.0;
        double lin = 0;
        const int K = 6;
        for (int k = 0; k < K; ++k) { lin += ex(rng); }
        lin /= K;
        lin *= std::pow(10.0, noiseDb / 10.0);
        for (const Lump& l : lumps) {
            double d = std::fabs(f - l.offsetHz) / (l.bwHz / 2.0);
            if (d >= 1.0) { continue; }
            double shape;
            if (l.flat) {
                shape = d < 0.8 ? 1.0 : 0.5 * (1 + std::cos(M_PI * (d - 0.8) / 0.2));
            } else {
                shape = std::pow(std::cos(M_PI * d / 2.0), 2.0) * 0.98 + 0.02;  // bell
                shape *= (0.5 + 0.5 * ex(rng) / 2.0);                            // speech-like raggedness
            }
            lin += std::pow(10.0, (noiseDb + l.snrDb) / 10.0) * shape;
        }
        if (dcSpike && std::fabs(f) < 1.5 * binHz) { lin += std::pow(10.0, (noiseDb + 30) / 10.0); }
        db[i] = (float)(10.0 * std::log10(lin));
    }
    return db;
}

void feed(SignalDetector& det, const std::vector<float>& db, double center, double span, double t) {
    SpectrumFrame f;
    f.db = db.data();
    f.bins = (int)db.size();
    f.centerHz = center;
    f.spanHz = span;
    det.update(f, t);
}

}  // namespace

TEST(noise_only_finds_nothing) {
    std::mt19937 rng(1);
    SignalDetector det;
    for (int i = 0; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {}, rng), 851e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)0);
    CHECK_NEAR(det.noiseFloorDb(), -80.0, 2.0);
}

TEST(single_digital_channel_is_found_with_correct_centre_and_width) {
    std::mt19937 rng(2);
    SignalDetector det;
    for (int i = 0; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{300e3, 12500, 30}}, rng), 851e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    if (det.signals().size() == 1) {
        const auto& s = det.signals()[0];
        CHECK_NEAR(s.freqHz, 851e6 + 300e3, 600.0);
        CHECK(s.bwHz > 9000 && s.bwHz < 14500);
        CHECK(s.snrDb > 24 && s.snrDb < 34);
        CHECK(s.kind == SignalKind::Channel12k);
        CHECK(s.activity > 0.9);
        CHECK(s.steady);
        CHECK_CONTAINS(s.hint, "800 MHz");           // band-plan hint for 851 MHz
        CHECK_EQ(s.label, std::string("control channel?"));  // steady narrow carrier in a trunking band
    }
}

TEST(spectrum_shape_alone_never_claims_digital) {
    std::mt19937 rng(3);
    SignalDetector det;
    for (int i = 0; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{-200e3, 12500, 35, false}}, rng), 146e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    if (det.signals().size() == 1) {
        // 146 MHz is the 2 m amateur band: a steady 12.5 kHz signal there is just "a 12.5 kHz signal", never "digital" or "control channel".
        CHECK_EQ(det.signals()[0].label, std::string("12.5 kHz signal"));
    }
}

TEST(fm_broadcast_scene) {
    std::mt19937 rng(4);
    SignalDetector det;
    std::vector<Lump> l;
    for (int k = -2; k <= 2; ++k) { l.push_back({k * 400e3, 180e3, 40}); }
    for (int i = 0; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, l, rng), 98.1e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)5);
    for (const auto& s : det.signals()) {
        CHECK(s.kind == SignalKind::WideFM);
        CHECK(s.bwHz > 130e3 && s.bwHz < 200e3);
        CHECK_EQ(s.label, std::string("FM broadcast"));
    }
}

TEST(adjacent_channels_stay_separate) {
    std::mt19937 rng(5);
    SignalDetector det;
    for (int i = 0; i < 30; ++i) {
        feed(det, makeSpectrum(16384, 2.4e6, {{-12500, 9000, 30}, {12500, 9000, 30}}, rng), 460e6, 2.4e6, i * 0.2);
    }
    CHECK_EQ(det.signals().size(), (size_t)2);
}

TEST(dc_spike_is_not_a_signal) {
    std::mt19937 rng(6);
    SignalDetector det;
    for (int i = 0; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {}, rng, -80, true), 100e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)0);
}

TEST(bursty_signal_has_partial_activity_and_stable_id) {
    std::mt19937 rng(7);
    SignalDetector det;
    int id = -1;
    for (int i = 0; i < 100; ++i) {
        bool on = (i % 20) < 12;  // 2.4 s on / 1.6 s off at 5 scans/s -> 60 % duty
        std::vector<Lump> l;
        if (on) { l.push_back({100e3, 12500, 28}); }
        feed(det, makeSpectrum(8192, 2.4e6, l, rng), 155e6, 2.4e6, i * 0.2);
        if (i == 30 && !det.signals().empty()) { id = det.signals()[0].id; }
    }
    CHECK(!det.signals().empty());
    if (!det.signals().empty()) {
        CHECK_EQ(det.signals()[0].id, id);
        CHECK(det.signals()[0].activity > 0.4 && det.signals()[0].activity < 0.85);
        CHECK(!det.signals()[0].steady);
        CHECK_EQ(det.signals()[0].label, std::string("12.5 kHz signal"));   // bursty (voice-like) at 155 MHz
    }
}

TEST(signal_disappears_then_is_forgotten) {
    std::mt19937 rng(8);
    SignalDetector det;
    for (int i = 0; i < 20; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{0 + 500e3, 12500, 30}}, rng), 155e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    for (int i = 20; i < 120; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {}, rng), 155e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)0);
}

TEST(identification_sticks_to_the_tracked_signal) {
    std::mt19937 rng(9);
    SignalDetector det;
    for (int i = 0; i < 20; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{250e3, 12500, 30}}, rng), 851e6, 2.4e6, i * 0.2); }
    CHECK(det.setIdentified(851e6 + 250e3 + 800, "P25 control channel", "NAC 293"));
    for (int i = 20; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{250e3, 12500, 30}}, rng), 851e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    CHECK_EQ(det.signals()[0].label, std::string("P25 control channel"));
    CHECK(!det.setIdentified(900e6, "x", "y"));
}

TEST(retune_keeps_signal_by_absolute_frequency) {
    std::mt19937 rng(10);
    SignalDetector det;
    for (int i = 0; i < 20; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{200e3, 12500, 30}}, rng), 851e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    int id = det.signals()[0].id;
    // window moves by +500 kHz: the signal is now at -300 kHz offset, same absolute frequency
    for (int i = 20; i < 30; ++i) { feed(det, makeSpectrum(8192, 2.4e6, {{-300e3, 12500, 30}}, rng), 851.5e6, 2.4e6, i * 0.2); }
    CHECK_EQ(det.signals().size(), (size_t)1);
    if (det.signals().size() == 1) {
        CHECK_EQ(det.signals()[0].id, id);
        CHECK_NEAR(det.signals()[0].freqHz, 851.2e6, 1500.0);
    }
}

TEST(band_plan_hints) {
    CHECK(bandSuggests(98.5e6, "wfm"));
    CHECK(bandSuggests(852e6, "p25"));
    CHECK(bandSuggests(131.55e6, "acars"));
    CHECK(bandSuggests(1090e6, "adsb"));
    CHECK(bandSuggests(433.92e6, "ism"));
    CHECK(bandSuggests(929.5e6, "pocsag"));
    CHECK(!bandSuggests(98.5e6, "p25"));
    const BandEntry* b = bestBandFor(162.5e6);
    CHECK(b != nullptr);
    if (b) { CHECK_EQ(std::string(b->name), std::string("NOAA weather radio")); }  // narrowest entry wins
    CHECK(bestBandFor(3.0e9) == nullptr);
}

TEST_MAIN("signal_detector")
