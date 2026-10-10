// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/resampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "sdrstar/dsp_lite.h"

namespace sdrstar {

PolyphaseResampler::PolyphaseResampler(int inRate, int outRate, int tapsPerPhase) : inRate_(inRate), outRate_(outRate) {
    const int g = std::gcd(inRate, outRate);
    L_ = outRate / g;  // interpolation factor
    M_ = inRate / g;   // decimation factor
    K_ = std::max(4, tapsPerPhase);
    const int N = L_ * K_;
    h_.assign((size_t)N, 0.0f);
    // low-pass at 0.42 x min(in, out) on a virtual rate of L * inRate; Blackman-Harris window; gain L.
    const double fs = (double)L_ * inRate;
    const double fc = 0.42 * std::min(inRate, outRate) / fs;
    const double mid = (N - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < N; ++i) {
        const double x = i - mid;
        const double s = (std::fabs(x) < 1e-9) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
        const double a = 2.0 * kPi * i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
        h_[i] = (float)(s * w);
        sum += h_[i];
    }
    for (auto& v : h_) { v = (float)(v / sum * L_); }
    hist_.assign((size_t)K_, 0.0f);
}

void PolyphaseResampler::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    histPos_ = 0;
    inIdx_ = 0;
    outIdx_ = 0;
}

void PolyphaseResampler::process(const int16_t* in, size_t n, std::vector<int16_t>& out) {
    for (size_t s = 0; s < n; ++s) {
        hist_[(size_t)histPos_] = (float)in[s];
        histPos_ = (histPos_ + 1) % K_;
        // produce every output whose (fractional) input position falls on this input sample
        while ((outIdx_ * M_) / L_ == inIdx_) {
            const int phase = (int)((outIdx_ * M_) % L_);
            float acc = 0;
            for (int k = 0; k < K_; ++k) {
                // x[inIdx - k] is the k-th most recent sample
                const int hi = ((histPos_ - 1 - k) % K_ + K_) % K_;
                acc += h_[(size_t)(phase + k * L_)] * hist_[(size_t)hi];
            }
            if (acc > 32767.0f) { acc = 32767.0f; }
            if (acc < -32768.0f) { acc = -32768.0f; }
            out.push_back((int16_t)std::lrintf(acc));
            ++outIdx_;
        }
        ++inIdx_;
    }
}

}  // namespace sdrstar
