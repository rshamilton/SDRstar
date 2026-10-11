// SPDX-License-Identifier: GPL-3.0-or-later
#include <random>

#include "sdrstar/dsp_lite.h"
#include "test_framework.h"

using namespace sdrstar;

namespace {
// Reference: the textbook O(n^2) DFT in double precision.
std::vector<std::complex<double>> naiveDft(const std::vector<cf>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> y(n);
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> acc(0, 0);
        for (size_t t = 0; t < n; ++t) {
            const double ang = -2.0 * kPi * (double)((k * t) % n) / (double)n;
            acc += std::complex<double>(x[t].real(), x[t].imag()) * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        y[k] = acc;
    }
    return y;
}
}  // namespace

TEST(fft_matches_the_reference_dft_for_every_size) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    for (size_t n : {2u, 4u, 8u, 16u, 64u, 256u, 1024u}) {
        std::vector<cf> x(n);
        for (auto& v : x) { v = cf(nd(rng), nd(rng)); }
        const auto ref = naiveDft(x);
        std::vector<cf> y = x;
        fft(y);
        double worst = 0, scale = 0;
        for (size_t k = 0; k < n; ++k) {
            worst = std::max(worst, std::abs(std::complex<double>(y[k].real(), y[k].imag()) - ref[k]));
            scale = std::max(scale, std::abs(ref[k]));
        }
        CHECK(worst < 1e-4 * scale + 1e-4);
    }
}

TEST(fft_puts_a_pure_tone_in_exactly_one_bin) {
    const size_t n = 4096;
    const size_t bin = 1234;
    std::vector<cf> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * (double)bin * (double)i / (double)n;
        x[i] = cf((float)std::cos(ph), (float)std::sin(ph));
    }
    fft(x);
    CHECK_NEAR(std::abs(x[bin]), (double)n, 0.05);
    double leak = 0;
    for (size_t k = 0; k < n; ++k) {
        if (k != bin) { leak = std::max(leak, (double)std::abs(x[k])); }
    }
    CHECK(leak < 0.05);   // float twiddles stay accurate over a long transform (a recurrence would drift)
}

TEST(fft_handles_changing_sizes_and_ignores_bad_ones) {
    std::vector<cf> a(8, cf(1, 0)), b(16, cf(1, 0));
    fft(a);
    fft(b);
    fft(a);   // back to the first size: the cached plan must be rebuilt, not reused wrongly
    CHECK_NEAR(std::abs(b[0]), 16.0, 1e-5);
    std::vector<cf> odd(6, cf(1, 2));
    fft(odd);   // not a power of two: untouched
    CHECK_NEAR(odd[3].real(), 1.0, 1e-9);
    std::vector<cf> empty;
    fft(empty);
    CHECK(empty.empty());
}

TEST(power_spectrum_of_noise_is_flat_and_scaled) {
    // unit-variance complex noise -> about 0 dB per bin after Hann normalisation
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.0f, 0.7071068f);
    std::vector<cf> x(1 << 16);
    for (auto& v : x) { v = cf(nd(rng), nd(rng)); }
    auto db = powerSpectrumDb(x, 4096);
    double mean = 0;
    for (float v : db) { mean += v; }
    mean /= (double)db.size();
    CHECK(mean > -3.0 && mean < 0.5);   // mean of dB of an exponential distribution is -2.5 dB below its mean power
}

TEST_MAIN("dsp_lite")
