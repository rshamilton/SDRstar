// SPDX-License-Identifier: GPL-3.0-or-later
//
// ChannelPcmTap: a hidden down-converted channel whose FM-discriminator output (48 kHz, signed 16-bit) is handed
// to any number of consumers (decoder sessions, level meters ...).  Also keeps a rolling window of raw channel IQ
// for analysis.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "sdrstar/radio_host.h"

namespace sdrstar {

class ChannelPcmTap {
public:
    using PcmSink = std::function<void(const int16_t* pcm, size_t n)>;

    ChannelPcmTap() = default;
    ~ChannelPcmTap() { close(); }
    ChannelPcmTap(const ChannelPcmTap&) = delete;
    ChannelPcmTap& operator=(const ChannelPcmTap&) = delete;

    bool open(RadioHost* host, double freqHz, double bwHz, std::string& error);
    void close();
    bool isOpen() const { return channel_ >= 0; }

    // Consumers must be added before samples are expected; they are called on the host's DSP thread.
    void addSink(PcmSink s);
    void clearSinks();

    // Retarget (absolute frequency inside the current window).  Resets the discriminator.
    bool move(double freqHz);
    double freqHz() const { return freq_.load(); }

    // Hold back samples until the given monotonic time (ns) (used after hardware retunes).
    void muteForMs(int ms);

    // Level of the last block, 0..1 (RMS of discriminator output / full scale) -- cheap activity indicator.
    float lastLevel() const { return level_.load(); }
    long long samplesSeen() const { return seen_.load(); }

private:
    void onIq(const cf* x, size_t n);

    RadioHost* host_ = nullptr;
    int channel_ = -1;
    std::atomic<double> freq_{0};
    FmDiscriminator disc_{48000.0, 6250.0};
    std::vector<int16_t> pcm_;
    std::mutex sinkMtx_;
    std::vector<PcmSink> sinks_;
    std::atomic<long long> muteUntilNs_{0};
    std::atomic<float> level_{0};
    std::atomic<long long> seen_{0};
};

}  // namespace sdrstar
