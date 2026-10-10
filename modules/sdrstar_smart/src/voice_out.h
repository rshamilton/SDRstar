// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceOut: plays decoded digital voice (8 kHz, interleaved stereo s16 from DSD-neo) through SDR++'s normal audio
// system as its own stream ("SDRstar Voice"), so the user gets the usual per-stream volume and output-device
// selection in the Sinks menu.  push() never blocks the caller: data goes through a bounded ring and a feeder thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <dsp/multirate/rational_resampler.h>
#include <dsp/stream.h>
#include <signal_path/sink.h>
#include <utils/event.h>

class VoiceOut {
public:
    VoiceOut() = default;
    ~VoiceOut() { shutdown(); }

    void init(const std::string& streamName);
    void shutdown();

    // Thread-safe, non-blocking.  `interleaved` holds frames*channels s16 samples at `rate` Hz.
    void push(const int16_t* interleaved, size_t frames, int channels, int rate);
    // Drop anything queued (e.g. when a call is abandoned).
    void clear();

    long long framesPlayed() const { return played_.load(); }
    bool initialised() const { return inited_; }

private:
    void feederLoop();
    static void onSampleRate(float sr, void* ctx);

    std::string name_;
    bool inited_ = false;
    dsp::stream<dsp::stereo_t> in_;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp_;
    SinkManager::Stream stream_;
    EventHandler<float> srHandler_;
    double inRate_ = 8000.0;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<dsp::stereo_t> ring_;
    std::thread feeder_;
    std::atomic<bool> stop_{false};
    std::atomic<long long> played_{0};
};
