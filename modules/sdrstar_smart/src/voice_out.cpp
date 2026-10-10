// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice_out.h"

#include <algorithm>

#include <signal_path/signal_path.h>
#include <utils/flog.h>

namespace {
constexpr size_t kMaxRingFrames = 8000 * 3;  // 3 s at 8 kHz
}

void VoiceOut::init(const std::string& streamName) {
    if (inited_) { return; }
    name_ = streamName;
    srHandler_.handler = onSampleRate;
    srHandler_.ctx = this;
    resamp_.init(&in_, inRate_, 48000.0);
    stream_.init(&resamp_.out, &srHandler_, 48000.0f);
    sigpath::sinkManager.registerStream(name_, &stream_);
    resamp_.start();
    stream_.start();
    stop_ = false;
    feeder_ = std::thread([this] { feederLoop(); });
    inited_ = true;
}

void VoiceOut::shutdown() {
    if (!inited_) { return; }
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

void VoiceOut::push(const int16_t* d, size_t frames, int channels, int rate) {
    if (!inited_ || frames == 0 || channels < 1) { return; }
    (void)rate;  // DSD-neo always delivers 8 kHz decoded voice
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < frames; ++i) {
        float l = d[i * channels] / 32768.0f;
        float r = channels > 1 ? d[i * channels + 1] / 32768.0f : l;
        ring_.push_back({l, r});
    }
    while (ring_.size() > kMaxRingFrames) { ring_.pop_front(); }  // bounded: drop the oldest, never block
    cv_.notify_one();
}

void VoiceOut::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    ring_.clear();
}

void VoiceOut::feederLoop() {
    std::vector<dsp::stereo_t> chunk;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !ring_.empty(); });
            if (stop_) { return; }
            size_t n = std::min<size_t>(ring_.size(), 2048);
            chunk.assign(ring_.begin(), ring_.begin() + (long)n);
            ring_.erase(ring_.begin(), ring_.begin() + (long)n);
        }
        for (size_t i = 0; i < chunk.size(); ++i) { in_.writeBuf[i] = chunk[i]; }
        if (!in_.swap((int)chunk.size())) { return; }
        played_ += (long long)chunk.size();
    }
}
