// SPDX-License-Identifier: GPL-3.0-or-later
//
// SimHost: a simulated radio for tests.  It plays recorded narrowband signals (looped) at ABSOLUTE frequencies,
// enforces the capture window like a real tuner (channels must lie inside centre +/- usable/2), goes silent while
// "retuning", and delivers 48 kHz baseband to open channels in real time (20 ms blocks).  No hardware needed.
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "sdrstar/radio_host.h"

namespace sdrstar_test {

using namespace sdrstar;

class SimHost : public RadioHost {
public:
    struct Signal {
        std::string name;
        double freqHz;
        std::vector<cf> iq;  // 48 kHz baseband centred on the signal
        size_t pos = 0;
        bool loop = true;            // false: plays once, then silence (a call that ends)
        bool startOnListen = false;  // true: starts when a channel first covers it (the call begins when you arrive)
        bool started = true;
        bool done = false;
    };
    struct TuneRecord {
        double tSec;
        double centerHz;
    };

    SimHost(double centerHz, double sampleRate) : center_(centerHz), rate_(sampleRate), t0_(std::chrono::steady_clock::now()) {
        pump_ = std::thread([this] { pumpLoop(); });
    }
    ~SimHost() override {
        stop_ = true;
        if (pump_.joinable()) { pump_.join(); }
    }

    void addSignal(const std::string& name, double freqHz, std::vector<cf> iq, bool loop = true, bool startOnListen = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        Signal s;
        s.name = name;
        s.freqHz = freqHz;
        s.iq = std::move(iq);
        s.loop = loop;
        s.startOnListen = startOnListen;
        s.started = !startOnListen;
        signals_.push_back(std::move(s));
    }
    void setSampleRate(double r) { rate_ = r; }

    ReceiverCaps caps() const override {
        ReceiverCaps c;
        c.sampleRateHz = rate_.load();
        c.name = "simulated RTL-SDR";
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
        std::this_thread::sleep_for(std::chrono::milliseconds(retuneMs_));  // tuner settling: channels are silent meanwhile
        center_ = hz;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            retuning_ = false;
            tunes_.push_back({elapsed(), hz});
            // channels that fell out of the window stay silent until moved back inside
        }
        return true;
    }
    double channelRateHz() const override { return 48000.0; }
    int openChannel(double absFreqHz, double bwHz, IqSinkFn sink, std::string& error) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!fits(absFreqHz, bwHz)) {
            error = "channel outside the capture window";
            return -1;
        }
        int id = nextId_++;
        channels_[id] = {absFreqHz, bwHz, sink, 0.0};
        return id;
    }
    bool moveChannel(int ch, double absFreqHz) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = channels_.find(ch);
        if (it == channels_.end()) { return false; }
        if (!fits(absFreqHz, it->second.bw)) { return false; }
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
    struct Chan {
        double freq, bw;
        IqSinkFn sink;
        double phase;
    };
    bool fits(double f, double bw) const {
        ReceiverCaps c = caps();
        return std::fabs(f - center_.load()) + bw / 2.0 <= c.usableSpanHz() / 2.0 + 1.0;
    }
    double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    void pumpLoop() {
        const size_t block = 960;  // 20 ms at 48 kHz
        std::mt19937 rng(1234);
        std::normal_distribution<float> nd(0.0f, 0.02f);
        auto next = std::chrono::steady_clock::now();
        std::vector<cf> out(block);
        while (!stop_.load()) {
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
            std::vector<std::pair<IqSinkFn, std::vector<cf>>> deliveries;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                // Advance every signal by one block (the air keeps going whether or not anyone listens).
                std::vector<std::vector<cf>> sigBlock(signals_.size());
                // one-shot calls start the moment a channel first covers them
                for (auto& sg : signals_) {
                    if (sg.started) { continue; }
                    for (auto& kv : channels_) {
                        if (!retuning_ && fits(kv.second.freq, kv.second.bw) && std::fabs(sg.freqHz - kv.second.freq) <= 3000.0) { sg.started = true; }
                    }
                }
                for (size_t s = 0; s < signals_.size(); ++s) {
                    auto& sg = signals_[s];
                    sigBlock[s].assign(block, cf(0, 0));
                    if (!sg.started || sg.done) { continue; }
                    for (size_t i = 0; i < block; ++i) {
                        size_t idx = sg.pos + i;
                        if (idx >= sg.iq.size()) {
                            if (!sg.loop) { break; }
                            idx %= sg.iq.size();
                        }
                        sigBlock[s][i] = sg.iq[idx];
                    }
                    sg.pos += block;
                    if (sg.pos >= sg.iq.size()) {
                        if (sg.loop) { sg.pos %= sg.iq.size(); } else { sg.done = true; }
                    }
                }
                for (auto& kv : channels_) {
                    Chan& c = kv.second;
                    std::fill(out.begin(), out.end(), cf(0, 0));
                    if (!retuning_ && fits(c.freq, c.bw)) {
                        for (size_t i = 0; i < block; ++i) { out[i] = cf(nd(rng), nd(rng)); }
                        for (size_t s = 0; s < signals_.size(); ++s) {
                            double off = signals_[s].freqHz - c.freq;
                            if (std::fabs(off) > 3000.0) { continue; }  // outside this channel's filter
                            double ph = c.phase;
                            for (size_t i = 0; i < block; ++i) {
                                out[i] += sigBlock[s][i] * cf((float)std::cos(ph), (float)std::sin(ph));
                                ph += 2.0 * kPi * off / 48000.0;
                            }
                        }
                        c.phase = std::fmod(c.phase + 2.0 * kPi * 0.0, 2.0 * kPi);
                    }
                    deliveries.emplace_back(c.sink, out);
                }
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
    std::vector<Signal> signals_;
    std::map<int, Chan> channels_;
    std::vector<TuneRecord> tunes_;
    bool retuning_ = false;
    int nextId_ = 1;
};

}  // namespace sdrstar_test
