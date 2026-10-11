// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers to check that decoded voice is really speech played at the right speed: the pitch of a human voice is
// 70-300 Hz, and audio played back at twice the speed (mono data read as stereo) shows up at twice that.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sdrstar_test {

// Median fundamental frequency (Hz) over the voiced 80 ms windows of `x` (autocorrelation, 60-400 Hz search).
// `voicedWindows` receives how many windows had a clear pitch; returns 0 when there were none.
inline double medianPitchHz(const std::vector<float>& x, int rate, int* voicedWindows = nullptr) {
    const size_t n = (size_t)(rate * 0.08);
    const size_t lagMin = (size_t)(rate / 400.0), lagMax = (size_t)(rate / 60.0);
    std::vector<double> pitches;
    for (size_t s = 0; s + 2 * n <= x.size(); s += n) {
        double mean = 0;
        for (size_t i = 0; i < n; ++i) { mean += x[s + i]; }
        mean /= (double)n;
        double energy = 0;
        for (size_t i = 0; i < n; ++i) { energy += (x[s + i] - mean) * (x[s + i] - mean); }
        if (std::sqrt(energy / (double)n) < 300.0) { continue; }   // too quiet to judge
        double best = 0;
        size_t bestLag = 0;
        for (size_t lag = lagMin; lag <= lagMax && lag < n; ++lag) {
            double ac = 0;
            for (size_t i = 0; i + lag < n; ++i) { ac += (x[s + i] - mean) * (x[s + i + lag] - mean); }
            ac /= energy;
            if (ac > best) {
                best = ac;
                bestLag = lag;
            }
        }
        if (best > 0.5 && bestLag) { pitches.push_back((double)rate / (double)bestLag); }
    }
    if (voicedWindows) { *voicedWindows = (int)pitches.size(); }
    if (pitches.empty()) { return 0.0; }
    std::sort(pitches.begin(), pitches.end());
    return pitches[pitches.size() / 2];
}

// First channel of interleaved audio as float samples.
inline std::vector<float> firstChannel(const std::vector<int16_t>& interleaved, int channels) {
    std::vector<float> out;
    for (size_t i = 0; i + (size_t)channels <= interleaved.size(); i += (size_t)channels) { out.push_back((float)interleaved[i]); }
    return out;
}

}  // namespace sdrstar_test
