// SPDX-License-Identifier: GPL-3.0-or-later
//
// SliceHost: a simulated RTL-SDR whose "air" is made of REAL wideband recordings (2.4 MS/s cu8 slices cut from one
// 20 MS/s capture of an 800 MHz public-safety band, all time-aligned).  Like a real receiver it only delivers channels
// inside the current capture window (centre +/- usable/2), is silent while retuning, and channels are produced by
// mixing + filtering + decimating the recording exactly as the SDR++ hidden channels do (to 48 kHz).
//
// What it cannot do: the recording is only a few seconds long and loops, and a window can only be as wide as the
// recorded slice (a channel more than ~1.15 MHz from every slice centre has no signal in it).
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "sdrstar/dsp_lite.h"
#include "sdrstar/radio_host.h"

namespace sdrstar_test {

using namespace sdrstar;

class SliceHost : public RadioHost {
public:
    struct Slice {
        double centerHz;
        std::vector<cf> iq;
    };
    struct TuneRecord {
        double tSec;
        double centerHz;
    };

    SliceHost(double windowCenterHz, double sampleRate) : center_(windowCenterHz), rate_(sampleRate), t0_(std::chrono::steady_clock::now()) {
        h_ = designLowpass(8000.0, kSliceRate, 281);
        pump_ = std::thread([this] { pumpLoop(); });
    }
    ~SliceHost() override {
        stop_ = true;
        if (pump_.joinable()) { pump_.join(); }
    }

    bool addSliceCu8(double centerHz, const std::string& path) {
        Slice s;
        s.centerHz = centerHz;
        if (!readCu8(path, s.iq) || s.iq.empty()) { return false; }
        std::lock_guard<std::mutex> lk(mtx_);
        if (!slices_.empty() && slices_[0].iq.size() != s.iq.size()) { return false; }  // must be time aligned
        slices_.push_back(std::move(s));
        return true;
    }
    double durationSec() const { return slices_.empty() ? 0.0 : (double)slices_[0].iq.size() / kSliceRate; }

    ReceiverCaps caps() const override {
        ReceiverCaps c;
        c.sampleRateHz = rate_.load();
        c.name = "simulated RTL-SDR (recorded air)";
        return c;
    }
    double centerHz() const override { return center_.load(); }
    bool setCenter(double hz, std::string& error) override {
        if (hz < caps().minHz || hz > caps().maxHz) {
            error = "outside tuner range";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            retuning_ = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retuneMs_));
        center_ = hz;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            retuning_ = false;
            tunes_.push_back({elapsed(), hz});
        }
        return true;
    }
    double channelRateHz() const override { return 48000.0; }
    int openChannel(double absFreqHz, double bwHz, IqSinkFn sink, std::string& error, double rateHz = 0) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (rateHz != 0 && std::fabs(rateHz - 48000.0) > 1) {
            error = "simulated host only provides 48 kHz channels";
            return -1;
        }
        if (!fits(absFreqHz, bwHz)) {
            error = "channel outside the capture window";
            return -1;
        }
        int id = nextId_++;
        Chan c;
        c.freq = absFreqHz;
        c.bw = bwHz;
        c.sink = sink;
        channels_[id] = c;
        return id;
    }
    bool moveChannel(int ch, double absFreqHz) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = channels_.find(ch);
        if (it == channels_.end() || !fits(absFreqHz, it->second.bw)) { return false; }
        it->second.freq = absFreqHz;
        return true;
    }
    void closeChannel(int ch) override {
        std::lock_guard<std::mutex> lk(mtx_);
        channels_.erase(ch);
    }
    std::vector<TuneRecord> tunes() {
        std::lock_guard<std::mutex> lk(mtx_);
        return tunes_;
    }
    int retuneMs_ = 150;

private:
    static constexpr double kSliceRate = 2400000.0;
    static constexpr int kDecim = 50;  // 2.4 MS/s -> 48 kS/s
    struct Chan {
        double freq = 0, bw = 0;
        IqSinkFn sink;
    };
    bool fits(double f, double bw) const {
        ReceiverCaps c = caps();
        return std::fabs(f - center_.load()) + bw / 2.0 <= c.usableSpanHz() / 2.0 + 1.0;
    }
    double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    // One output block (960 samples) of the channel at absolute frequency `f`, starting at input sample `pos`.
    void channelize(const Slice& s, double f, size_t pos, std::vector<cf>& out) const {
        const size_t N = s.iq.size();
        const size_t taps = h_.size();
        const double inc = -2.0 * kPi * (f - s.centerHz) / kSliceRate;  // phase step per input sample (moves f to DC)
        std::vector<cf> w(taps);
        for (size_t k = 0; k < taps; ++k) { w[k] = h_[k] * cf((float)std::cos(-(double)k * inc), (float)std::sin(-(double)k * inc)); }
        for (size_t n = 0; n < out.size(); ++n) {
            const size_t idx = pos + n * kDecim + (kDecim - 1) + N;  // + N keeps the modulo positive for the history
            float ar = 0, ai = 0;   // explicit real arithmetic: std::complex multiply is slow without -ffast-math
            for (size_t k = 0; k < taps; ++k) {
                const cf& x = s.iq[(idx - k) % N];
                ar += x.real() * w[k].real() - x.imag() * w[k].imag();
                ai += x.real() * w[k].imag() + x.imag() * w[k].real();
            }
            const double ph = inc * (double)(idx % N);  // absolute phase: no drift across blocks or retunes
            const float c = (float)std::cos(ph), sn = (float)std::sin(ph);
            out[n] = cf(ar * c - ai * sn, ar * sn + ai * c);
        }
    }

    void pumpLoop() {
        const size_t block = 960;
        std::vector<cf> out(block);
        auto next = std::chrono::steady_clock::now();
        size_t pos = 0;
        while (!stop_.load()) {
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
            std::vector<std::pair<IqSinkFn, std::vector<cf>>> deliveries;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                const size_t N = slices_.empty() ? 1 : slices_[0].iq.size();
                for (auto& kv : channels_) {
                    Chan& c = kv.second;
                    std::fill(out.begin(), out.end(), cf(0, 0));
                    if (!retuning_ && fits(c.freq, c.bw)) {
                        const Slice* best = nullptr;
                        for (const auto& s : slices_) {
                            const double d = std::fabs(c.freq - s.centerHz);
                            if (d < 1.15e6 && (!best || d < std::fabs(c.freq - best->centerHz))) { best = &s; }
                        }
                        if (best) { channelize(*best, c.freq, pos, out); }
                    }
                    deliveries.emplace_back(c.sink, out);
                }
                pos = (pos + block * kDecim) % N;  // the air keeps moving whether or not anyone listens
            }
            for (auto& d : deliveries) { d.first(d.second.data(), d.second.size()); }
        }
    }

    std::atomic<double> center_;
    std::atomic<double> rate_;
    std::atomic<bool> stop_{false};
    std::chrono::steady_clock::time_point t0_;
    std::thread pump_;
    std::mutex mtx_;
    std::vector<Slice> slices_;
    std::vector<float> h_;
    std::map<int, Chan> channels_;
    std::vector<TuneRecord> tunes_;
    bool retuning_ = false;
    int nextId_ = 1;
};

}  // namespace sdrstar_test
