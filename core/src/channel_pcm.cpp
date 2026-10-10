// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/channel_pcm.h"

#include <chrono>
#include <cmath>

namespace sdrstar {

namespace {
long long nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

bool ChannelPcmTap::open(RadioHost* host, double freqHz, double bwHz, std::string& error) {
    close();
    host_ = host;
    freq_ = freqHz;
    disc_.reset();
    channel_ = host_->openChannel(freqHz, bwHz, [this](const cf* x, size_t n) { onIq(x, n); }, error);
    return channel_ >= 0;
}

void ChannelPcmTap::close() {
    if (channel_ >= 0 && host_) {
        host_->closeChannel(channel_);
        channel_ = -1;
    }
}

void ChannelPcmTap::addSink(PcmSink s) {
    std::lock_guard<std::mutex> lk(sinkMtx_);
    sinks_.push_back(std::move(s));
}

void ChannelPcmTap::clearSinks() {
    std::lock_guard<std::mutex> lk(sinkMtx_);
    sinks_.clear();
}

bool ChannelPcmTap::move(double freqHz) {
    if (channel_ < 0 || !host_) { return false; }
    if (!host_->moveChannel(channel_, freqHz)) { return false; }
    freq_ = freqHz;
    disc_.reset();
    return true;
}

void ChannelPcmTap::muteForMs(int ms) { muteUntilNs_ = nowNs() + (long long)ms * 1000000LL; }

void ChannelPcmTap::onIq(const cf* x, size_t n) {
    if (nowNs() < muteUntilNs_.load()) { return; }
    pcm_.resize(n);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        float d = disc_.process(x[i]);
        acc += (double)d * d;
        pcm_[i] = toS16(d);
    }
    level_ = n ? (float)std::sqrt(acc / (double)n) : 0.0f;
    seen_ += (long long)n;
    std::lock_guard<std::mutex> lk(sinkMtx_);
    for (auto& s : sinks_) { s(pcm_.data(), n); }
}

}  // namespace sdrstar
