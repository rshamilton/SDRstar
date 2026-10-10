// SPDX-License-Identifier: GPL-3.0-or-later
//
// SpectrumTap: our own view of the capture spectrum, independent of the waterfall's zoom/FFT settings.
// It binds to SDR++'s IQ splitter, periodically snapshots a few FFT blocks, and (on a worker thread) averages them
// into one dB spectrum that is handed to a callback.  CPU use is a few percent of one core.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <dsp/sink/handler_sink.h>
#include <dsp/stream.h>
#include <sdrstar/dsp_lite.h>

class SpectrumTap {
public:
    using SpectrumCb = std::function<void(const float* db, int bins, double centerHz, double spanHz)>;

    SpectrumTap() = default;
    ~SpectrumTap() { stop(); }

    void start(SpectrumCb cb);
    void stop();
    bool running() const { return running_.load(); }

private:
    static void onBlock(dsp::complex_t* data, int count, void* ctx);
    void workerLoop();

    dsp::stream<dsp::complex_t> in_;
    dsp::sink::Handler<dsp::complex_t> sink_;
    SpectrumCb cb_;
    std::atomic<bool> running_{false};

    // capture state (DSP thread)
    static constexpr int kSegments = 4;
    size_t fftSize_ = 16384;
    std::vector<std::vector<sdrstar::cf>> segs_;
    size_t segFill_ = 0;
    int segCount_ = 0;
    long long nextDueNs_ = 0;
    double capCenter_ = 0, capRate_ = 0;

    // hand-off to the worker
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::vector<sdrstar::cf>> ready_;
    double readyCenter_ = 0, readyRate_ = 0;
    bool haveReady_ = false;
    std::thread worker_;
    std::atomic<bool> stopWorker_{false};
};
