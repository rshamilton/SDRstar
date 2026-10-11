// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice_out.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <signal_path/signal_path.h>
#include <utils/flog.h>

namespace {
constexpr size_t kChunkFrames = 160;   // 20 ms at 8 kHz: small enough that the audio sink's own buffering sets the latency
constexpr size_t kFlushFrames = 80;    // 10 ms of silence after a transmission pushes the resampler's filter tail out
}  // namespace

void VoiceOut::init(const std::string& streamName) {
    if (inited_) { return; }
    name_ = streamName;
    srHandler_.handler = onSampleRate;
    srHandler_.ctx = this;
    stop_ = false;
    // The feeder exists before the stream can start pulling: nothing waits on a thread that is not there yet.
    feeder_ = std::thread([this] { feederLoop(); });
    resamp_.init(&in_, inRate_, 48000.0);
    stream_.init(&resamp_.out, &srHandler_, 48000.0f);
    sigpath::sinkManager.registerStream(name_, &stream_);
    resamp_.start();
    stream_.start();
    inited_ = true;
}

void VoiceOut::shutdown() {
    if (!inited_.exchange(false)) { return; }
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);
        stop_ = true;
    }
    cv_.notify_all();
    in_.stopWriter();  // release a feeder blocked in swap()
    if (feeder_.joinable()) { feeder_.join(); }
    stream_.stop();
    resamp_.stop();
    sigpath::sinkManager.unregisterStream(name_);
}

void VoiceOut::onSampleRate(float sr, void* ctx) {
    VoiceOut* v = (VoiceOut*)ctx;
    if (sr > 8000.0f) { v->resamp_.setOutSamplerate(sr); }
}

void VoiceOut::push(const int16_t* d, size_t frames, int channels, int rate) {
    if (!inited_ || frames == 0 || channels < 1) { return; }
    (void)rate;  // DSD-neo always delivers 8 kHz decoded voice
    jitter_.push(d, frames, channels);
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);   // pairs with the feeder's predicate check: no lost wake-up
    }
    cv_.notify_one();
}

void VoiceOut::clear() { jitter_.clear(); }

void VoiceOut::feederLoop() {
    std::vector<sdrstar::StereoFrame> chunk(kChunkFrames);
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(wakeMtx_);
            cv_.wait(lk, [this] { return stop_.load() || jitter_.pending(); });
            if (stop_) { return; }
        }
        const size_t n = jitter_.takeReady(chunk.data(), chunk.size());
        if (n == 0) {
            // Voice is queued but still being pre-buffered: look again shortly.
            std::unique_lock<std::mutex> lk(wakeMtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(10), [this] { return stop_.load(); });
            if (stop_) { return; }
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            in_.writeBuf[i].l = chunk[i].l;
            in_.writeBuf[i].r = chunk[i].r;
        }
        if (!in_.swap((int)n)) { return; }
        played_ += (long long)n;
        if (!jitter_.pending()) {
            // The transmission is over.  Silence behind it flushes the resampler, otherwise its last few milliseconds
            // would sit in the filter and come out as a click at the start of the next call.
            for (size_t i = 0; i < kFlushFrames; ++i) { in_.writeBuf[i].l = in_.writeBuf[i].r = 0.0f; }
            if (!in_.swap((int)kFlushFrames)) { return; }
        }
    }
}
