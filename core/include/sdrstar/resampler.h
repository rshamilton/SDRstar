// SPDX-License-Identifier: GPL-3.0-or-later
//
// PolyphaseResampler: rational sample-rate conversion for mono audio (e.g. 48 kHz FM-discriminator audio ->
// 22.05 kHz for multimon-ng).  Windowed-sinc prototype, 32 taps per phase.
#pragma once

#include <cstdint>
#include <vector>

namespace sdrstar {

class PolyphaseResampler {
public:
    PolyphaseResampler(int inRate, int outRate, int tapsPerPhase = 32);
    // Appends the converted samples to `out`.
    void process(const int16_t* in, size_t n, std::vector<int16_t>& out);
    void reset();
    int inRate() const { return inRate_; }
    int outRate() const { return outRate_; }

private:
    int inRate_, outRate_;
    int L_, M_, K_;
    std::vector<float> h_;     // L*K taps (prototype designed at L * inRate)
    std::vector<float> hist_;  // last K input samples (ring)
    int histPos_ = 0;
    long long inIdx_ = 0;      // index of the NEXT input sample
    long long outIdx_ = 0;     // index of the NEXT output sample
};

}  // namespace sdrstar
