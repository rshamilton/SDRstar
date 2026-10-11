// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small, dependency-free DSP helpers shared by the analysis code, the unit tests and the headless tools.
// (Inside SDR++ the heavy lifting is done by SDR++'s own optimised DSP blocks; this is for analysis-sized work.)
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sdrstar {

using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;

// ---- file loading (test fixtures and recordings) ----------------------------------------------------------
// Interleaved unsigned 8-bit I/Q (RTL-SDR native "cu8"), centred on 127.5.
inline bool readCu8(const std::string& path, std::vector<cf>& out, size_t maxSamples = 0, size_t skipSamples = 0) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { return false; }
    if (skipSamples) { std::fseek(f, (long)(skipSamples * 2), SEEK_SET); }
    std::vector<uint8_t> raw;
    const size_t chunk = 1 << 20;
    raw.resize(chunk);
    out.clear();
    for (;;) {
        size_t want = chunk;
        if (maxSamples) {
            size_t remain = (maxSamples - out.size()) * 2;
            if (remain == 0) { break; }
            want = std::min(want, remain);
        }
        size_t n = std::fread(raw.data(), 1, want, f);
        if (n < 2) { break; }
        for (size_t i = 0; i + 1 < n; i += 2) { out.emplace_back(((float)raw[i] - 127.5f) / 127.5f, ((float)raw[i + 1] - 127.5f) / 127.5f); }
    }
    std::fclose(f);
    return !out.empty();
}

// ---- FFT ---------------------------------------------------------------------------------------------------
inline bool isPow2(size_t n) { return n && !(n & (n - 1)); }

namespace detail {
// Twiddle factors and bit-reversal order for one FFT size.  The analysis code runs the same size over and over (the
// spectrum tap does thousands of 16384-point FFTs), so they are built once per thread and size, in double precision
// (a running product of float twiddles drifts over thousands of steps).  Twiddles are stored per stage, contiguous:
// the stage with half-length h uses wr/wi[h + k], k < h.
struct FftPlan {
    size_t n = 0;
    std::vector<float> wr, wi;
    std::vector<uint32_t> rev;
};

inline const FftPlan& fftPlan(size_t n) {
    thread_local FftPlan plan;
    if (plan.n == n) { return plan; }
    plan.n = n;
    plan.wr.assign(n, 0.0f);
    plan.wi.assign(n, 0.0f);
    for (size_t half = 1; half < n; half <<= 1) {
        for (size_t k = 0; k < half; ++k) {
            const double ang = -kPi * (double)k / (double)half;   // -2*pi*k/len with len = 2*half
            plan.wr[half + k] = (float)std::cos(ang);
            plan.wi[half + k] = (float)std::sin(ang);
        }
    }
    plan.rev.assign(n, 0);
    size_t bits = 0;
    while (((size_t)1 << bits) < n) { ++bits; }
    for (size_t i = 0; i < n; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < bits; ++b) { r |= ((i >> b) & 1u) << (bits - 1 - b); }
        plan.rev[i] = (uint32_t)r;
    }
    return plan;
}
}  // namespace detail

// In-place iterative radix-2 FFT (forward).  n must be a power of two (anything else is left unchanged).
// Real/imaginary parts are multiplied out by hand: std::complex's operator* also checks for NaN results (a library call
// when it fails), which makes it several times slower than the four multiplications it stands for.
inline void fft(std::vector<cf>& a) {
    const size_t n = a.size();
    if (n < 2 || !isPow2(n)) { return; }
    const detail::FftPlan& pl = detail::fftPlan(n);
    float* d = reinterpret_cast<float*>(a.data());
    for (size_t i = 0; i < n; ++i) {
        const size_t j = pl.rev[i];
        if (i < j) {
            std::swap(d[2 * i], d[2 * j]);
            std::swap(d[2 * i + 1], d[2 * j + 1]);
        }
    }
    for (size_t half = 1; half < n; half <<= 1) {
        const float* wr = pl.wr.data() + half;
        const float* wi = pl.wi.data() + half;
        for (size_t i = 0; i < n; i += 2 * half) {
            float* p = d + 2 * i;
            float* q = d + 2 * (i + half);
            for (size_t k = 0; k < half; ++k) {
                const float xr = q[2 * k], xi = q[2 * k + 1];
                const float vr = xr * wr[k] - xi * wi[k];
                const float vi = xr * wi[k] + xi * wr[k];
                const float ur = p[2 * k], ui = p[2 * k + 1];
                p[2 * k] = ur + vr;
                p[2 * k + 1] = ui + vi;
                q[2 * k] = ur - vr;
                q[2 * k + 1] = ui - vi;
            }
        }
    }
}

inline std::vector<float> hannWindow(size_t n) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i) { w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(n - 1))); }
    return w;
}

// Averaged power spectrum in dB (fftshifted: bin 0 = -fs/2).  Welch with 50 % overlap and a Hann window.
inline std::vector<float> powerSpectrumDb(const std::vector<cf>& x, size_t nfft, size_t maxSegments = 0) {
    std::vector<float> acc(nfft, 0.0f);
    if (x.size() < nfft || !isPow2(nfft)) { return acc; }
    std::vector<float> win = hannWindow(nfft);
    double wpow = 0;
    for (float v : win) { wpow += (double)v * v; }
    size_t segs = 0;
    std::vector<cf> buf(nfft);
    for (size_t off = 0; off + nfft <= x.size(); off += nfft / 2) {
        for (size_t i = 0; i < nfft; ++i) { buf[i] = x[off + i] * win[i]; }
        fft(buf);
        for (size_t i = 0; i < nfft; ++i) { acc[(i + nfft / 2) % nfft] += std::norm(buf[i]); }
        if (++segs >= maxSegments && maxSegments) { break; }
    }
    for (size_t i = 0; i < nfft; ++i) { acc[i] = 10.0f * std::log10(acc[i] / (float)segs / (float)wpow + 1e-20f); }
    return acc;
}

// ---- mixing / filtering / FM -----------------------------------------------------------------------------
// Shift the spectrum by -shiftHz (i.e. a signal at +shiftHz lands at DC).
inline std::vector<cf> freqShift(const std::vector<cf>& x, double shiftHz, double fs) {
    std::vector<cf> y(x.size());
    double ph = 0.0;
    const double inc = -2.0 * kPi * shiftHz / fs;
    for (size_t i = 0; i < x.size(); ++i) {
        y[i] = x[i] * cf((float)std::cos(ph), (float)std::sin(ph));
        ph += inc;
        if (ph < -kPi) { ph += 2 * kPi; }
    }
    return y;
}

// Windowed-sinc low-pass FIR (Hamming), cutoff in Hz.
inline std::vector<float> designLowpass(double cutoffHz, double fs, int taps) {
    if (taps % 2 == 0) { taps++; }
    std::vector<float> h(taps);
    const double fc = cutoffHz / fs;
    const int m = taps / 2;
    double sum = 0;
    for (int i = 0; i < taps; ++i) {
        const int k = i - m;
        double s = (k == 0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * k) / (kPi * k);
        double w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (taps - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (auto& v : h) { v = (float)(v / sum); }
    return h;
}

// FIR filter + integer decimation.
inline std::vector<cf> firDecimate(const std::vector<cf>& x, const std::vector<float>& h, int decim) {
    std::vector<cf> y;
    if (x.size() < h.size()) { return y; }
    y.reserve(x.size() / decim);
    for (size_t n = h.size() - 1; n < x.size(); n += decim) {
        cf acc(0, 0);
        for (size_t k = 0; k < h.size(); ++k) { acc += x[n - k] * h[k]; }
        y.push_back(acc);
    }
    return y;
}

// Quadrature FM discriminator.  Output is scaled so that `deviationHz` of deviation = 1.0 (like SDR++ NFM).
class FmDiscriminator {
public:
    FmDiscriminator(double sampleRate, double deviationHz) : scale_((float)(sampleRate / (2.0 * kPi * deviationHz))) {}
    float process(cf x) {
        float out = std::arg(x * std::conj(prev_)) * scale_;
        prev_ = x;
        return out;
    }
    void reset() { prev_ = cf(1, 0); }

private:
    float scale_;
    cf prev_{1, 0};
};

inline int16_t toS16(float v) {
    float s = v * 32767.0f;
    if (s > 32767.0f) { s = 32767.0f; }
    if (s < -32768.0f) { s = -32768.0f; }
    return (int16_t)std::lrintf(s);
}

}  // namespace sdrstar
