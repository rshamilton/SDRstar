// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectrum_tap.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include <gui/gui.h>
#include <signal_path/signal_path.h>

namespace {
long long nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
size_t pickFftSize(double rate) {
    size_t n = 2048;
    while (n < 32768 && (double)n * 150.0 < rate) { n <<= 1; }
    return n;
}
}  // namespace

void SpectrumTap::start(SpectrumCb cb) {
    stop();
    cb_ = std::move(cb);
    stopWorker_ = false;
    segs_.clear();
    segFill_ = 0;
    segCount_ = 0;
    nextDueNs_ = 0;
    haveReady_ = false;
    worker_ = std::thread([this] { workerLoop(); });
    sink_.init(&in_, onBlock, this);
    sigpath::iqFrontEnd.bindIQStream(&in_);
    sink_.start();
    running_ = true;
}

void SpectrumTap::stop() {
    if (!running_.exchange(false)) { return; }
    sink_.stop();
    sigpath::iqFrontEnd.unbindIQStream(&in_);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopWorker_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
}

void SpectrumTap::onBlock(dsp::complex_t* data, int count, void* ctx) {
    SpectrumTap* t = (SpectrumTap*)ctx;
    if (!t->running_.load()) { return; }
    const long long now = nowNs();
    if (t->segCount_ == 0 && t->segFill_ == 0) {
        if (now < t->nextDueNs_) { return; }  // not due yet: skip this block entirely
        t->capCenter_ = gui::waterfall.getCenterFrequency();
        t->capRate_ = sigpath::iqFrontEnd.getSampleRate();
        t->fftSize_ = pickFftSize(t->capRate_);
        t->segs_.assign(kSegments, std::vector<sdrstar::cf>(t->fftSize_));
    }
    int i = 0;
    while (i < count && t->segCount_ < kSegments) {
        size_t take = std::min<size_t>((size_t)(count - i), t->fftSize_ - t->segFill_);
        std::memcpy(&t->segs_[t->segCount_][t->segFill_], data + i, take * sizeof(sdrstar::cf));
        t->segFill_ += take;
        i += (int)take;
        if (t->segFill_ == t->fftSize_) {
            t->segFill_ = 0;
            t->segCount_++;
        }
    }
    if (t->segCount_ == kSegments) {
        // discard frames that straddled a retune
        const double c2 = gui::waterfall.getCenterFrequency();
        const double r2 = sigpath::iqFrontEnd.getSampleRate();
        if (std::fabs(c2 - t->capCenter_) < 1.0 && std::fabs(r2 - t->capRate_) < 1.0) {
            {
                std::lock_guard<std::mutex> lk(t->mtx_);
                t->ready_.swap(t->segs_);
                t->readyCenter_ = t->capCenter_;
                t->readyRate_ = t->capRate_;
                t->haveReady_ = true;
            }
            t->cv_.notify_one();
        }
        t->segCount_ = 0;
        t->segFill_ = 0;
        t->nextDueNs_ = now + 125 * 1000000LL;  // 8 spectra per second
    }
}

void SpectrumTap::workerLoop() {
    std::vector<float> win;
    std::vector<float> db;
    std::vector<sdrstar::cf> buf;
    std::vector<double> acc;
    for (;;) {
        std::vector<std::vector<sdrstar::cf>> segs;
        double center, rate;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return haveReady_ || stopWorker_; });
            if (stopWorker_) { return; }
            segs.swap(ready_);
            center = readyCenter_;
            rate = readyRate_;
            haveReady_ = false;
        }
        if (segs.empty()) { continue; }
        const size_t n = segs[0].size();
        if (win.size() != n) { win = sdrstar::hannWindow(n); }
        double wpow = 0;
        for (float v : win) { wpow += (double)v * v; }
        acc.assign(n, 0.0);
        buf.resize(n);
        for (auto& s : segs) {
            for (size_t i = 0; i < n; ++i) { buf[i] = s[i] * win[i]; }
            sdrstar::fft(buf);
            for (size_t i = 0; i < n; ++i) { acc[(i + n / 2) % n] += std::norm(buf[i]); }
        }
        db.resize(n);
        for (size_t i = 0; i < n; ++i) { db[i] = 10.0f * std::log10((float)(acc[i] / (double)segs.size() / wpow) + 1e-20f); }
        if (cb_) { cb_(db.data(), (int)n, center, rate); }
    }
}
