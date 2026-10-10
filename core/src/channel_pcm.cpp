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
    // A fresh block per opening (a late callback of the previous opening keeps the old block, which is closed);
    // consumers added before open() carry over.
    auto fresh = std::make_shared<Shared>();
    {
        std::lock_guard<std::mutex> lk(sh_->mtx);
        fresh->sinks = sh_->sinks;
    }
    sh_ = fresh;
    std::shared_ptr<Shared> sh = sh_;
    channel_ = host_->openChannel(freqHz, bwHz, [sh](const cf* x, size_t n) { onIq(*sh, x, n); }, error);
    return channel_ >= 0;
}

void ChannelPcmTap::close() {
    if (channel_ >= 0 && host_) {
        host_->closeChannel(channel_);
        channel_ = -1;
    }
    std::lock_guard<std::mutex> lk(sh_->mtx);
    sh_->closed = true;   // anything a host still delivers after this point is dropped
}

void ChannelPcmTap::addSink(PcmSink s) {
    std::lock_guard<std::mutex> lk(sh_->mtx);
    sh_->sinks.push_back(std::move(s));
}

void ChannelPcmTap::clearSinks() {
    std::lock_guard<std::mutex> lk(sh_->mtx);
    sh_->sinks.clear();
}

bool ChannelPcmTap::move(double freqHz) {
    if (channel_ < 0 || !host_) { return false; }
    if (!host_->moveChannel(channel_, freqHz)) { return false; }
    freq_ = freqHz;
    std::lock_guard<std::mutex> lk(sh_->mtx);
    sh_->disc.reset();
    return true;
}

void ChannelPcmTap::muteForMs(int ms) { sh_->muteUntilNs = nowNs() + (long long)ms * 1000000LL; }

void ChannelPcmTap::onIq(Shared& sh, const cf* x, size_t n) {
    if (nowNs() < sh.muteUntilNs.load()) { return; }
    std::lock_guard<std::mutex> lk(sh.mtx);
    if (sh.closed) { return; }
    sh.pcm.resize(n);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        float d = sh.disc.process(x[i]);
        acc += (double)d * d;
        sh.pcm[i] = toS16(d);
    }
    sh.level = n ? (float)std::sqrt(acc / (double)n) : 0.0f;
    sh.seen += (long long)n;
    for (auto& s : sh.sinks) { s(sh.pcm.data(), n); }
}

}  // namespace sdrstar
