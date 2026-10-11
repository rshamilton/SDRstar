// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceOut: plays decoded digital voice (8 kHz, interleaved stereo s16 from DSD-neo) through SDR++'s normal audio
// system as its own stream ("SDRstar Voice"), so the user gets the usual per-stream volume and output-device
// selection in the Sinks menu.  push() never blocks the caller: data goes through a bounded jitter buffer and a
// feeder thread.
//
// The feeder produces audio ONLY while there is voice to play (and a short run of silence at the end of a
// transmission to flush the resampler).  An idle voice stream therefore costs nothing; patch 0005 makes the audio
// sink tolerate a stream that is silent for minutes without stalling the other stream on the same output device.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <dsp/multirate/rational_resampler.h>
#include <dsp/stream.h>
#include <sdrstar/voice_jitter.h>
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
    bool initialised() const { return inited_.load(); }

private:
    void feederLoop();
    static void onSampleRate(float sr, void* ctx);

    std::string name_;
    std::atomic<bool> inited_{false};
    dsp::stream<dsp::stereo_t> in_;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp_;
    SinkManager::Stream stream_;
    EventHandler<float> srHandler_;
    double inRate_ = 8000.0;

    sdrstar::VoiceJitterBuffer jitter_;
    std::mutex wakeMtx_;
    std::condition_variable cv_;
    std::thread feeder_;
    std::atomic<bool> stop_{false};
    std::atomic<long long> played_{0};
};
