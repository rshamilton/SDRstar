// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice_out.h"

#include <algorithm>
#include <chrono>

#include <signal_path/signal_path.h>
#include <utils/flog.h>

constexpr size_t VoiceOut::kChunkFrames;
constexpr size_t VoiceOut::kStartFrames;
constexpr size_t VoiceOut::kMaxFrames;

void VoiceOut::init(const std::string& streamName) {
    if (inited_) { return; }
    name_ = streamName;
    srHandler_.handler = onSampleRate;
    srHandler_.ctx = this;
    resamp_.init(&in_, inRate_, 48000.0);
    stream_.init(&resamp_.out, &srHandler_, 48000.0f);
    sigpath::sinkManager.registerStream(name_, &stream_);
    resamp_.start();
    // The stream itself is NOT started: no output device is opened until a job needs voice (begin()).
    stop_ = false;
    active_ = false;
    feeder_ = std::thread([this] { feederLoop(); });
    inited_ = true;
}

void VoiceOut::shutdown() {
    if (!inited_) { return; }
    end();
    inited_ = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
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

void VoiceOut::begin() {
    if (!inited_ || active_.load()) { return; }
    clear();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        active_ = true;
    }
    stream_.start();   // opens the output device; the feeder is already producing silence for it
    cv_.notify_all();
}

void VoiceOut::end() {
    if (!inited_ || !active_.load()) { return; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        active_ = false;
    }
    cv_.notify_all();
    // The feeder may be blocked handing a chunk to a stream that is no longer being read: release it, wait until it has
    // gone back to waiting, then re-arm the stream for the next job.
    in_.stopWriter();
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !feederBusy_; });
    }
    in_.clearWriteStop();
    stream_.stop();    // closes the output device
    clear();
}

void VoiceOut::push(const int16_t* d, size_t frames, int channels, int rate) {
    if (!inited_ || !active_.load() || frames == 0 || channels < 1) { return; }
    (void)rate;  // DSD-neo always delivers 8 kHz decoded voice
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < frames; ++i) {
        const float l = d[i * (size_t)channels] / 32768.0f;
        const float r = channels > 1 ? d[i * (size_t)channels + 1] / 32768.0f : l;   // mono voice plays on both sides
        ring_.push_back({l, r});
    }
    while (ring_.size() > kMaxFrames) { ring_.pop_front(); }  // bounded: drop the oldest, never block
}

void VoiceOut::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    ring_.clear();
}

void VoiceOut::feederLoop() {
    using clk = std::chrono::steady_clock;
    bool playing = false;       // a call is being played out (the jitter buffer filled and has not run dry since)
    auto due = clk::now();      // pacing: never run faster than ~1.25x real time when nothing downstream pushes back
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            feederBusy_ = false;
            cv_.notify_all();   // end() waits for "idle"
            cv_.wait(lk, [this] { return stop_.load() || active_.load(); });
            if (stop_) { return; }
            feederBusy_ = true;
        }
        size_t got = 0;
        dsp::stereo_t* w = in_.writeBuf;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!playing && ring_.size() >= kStartFrames) { playing = true; }
            if (playing) {
                got = std::min(ring_.size(), kChunkFrames);
                for (size_t i = 0; i < got; ++i) { w[i] = ring_[i]; }
                ring_.erase(ring_.begin(), ring_.begin() + (long)got);
                if (got < kChunkFrames) { playing = false; }   // ran dry: refill before playing on
            }
        }
        for (size_t i = got; i < kChunkFrames; ++i) { w[i] = {0.0f, 0.0f}; }   // silence keeps the output device fed
        if (!in_.swap((int)kChunkFrames)) {
            // the writer was stopped by end(): go back to waiting (the next begin() starts from silence)
            playing = false;
            continue;
        }
        played_ += (long long)got;

        due += std::chrono::microseconds((long long)(1e6 * (double)kChunkFrames / 8000.0 / 1.25));
        const auto now = clk::now();
        if (due > now) {
            std::this_thread::sleep_until(due);
        } else if (now - due > std::chrono::milliseconds(200)) {
            due = now - std::chrono::milliseconds(100);   // we were held up by a full pipeline: do not try to catch up
        }
    }
}
