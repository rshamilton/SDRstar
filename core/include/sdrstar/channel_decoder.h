// SPDX-License-Identifier: GPL-3.0-or-later
//
// ChannelDecoder: decode ONE conventional (non-trunked) digital channel with a chosen decoder preset and play the
// decoded voice.  Used when the user picks "Decode this signal" after identification.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sdrstar/channel_pcm.h"
#include "sdrstar/dsd_session.h"

namespace sdrstar {

class ChannelDecoder {
public:
    struct Config {
        std::string dsdExe;
        std::string workDir;
        std::string presetId;
        double freqHz = 0;
        double bwHz = 12500;
        std::vector<std::string> extraArgs;
    };

    ChannelDecoder() = default;
    ~ChannelDecoder() { stop(); }

    void setAudioCallback(DsdSession::AudioCb cb) { audioCb_ = std::move(cb); }
    bool start(RadioHost* host, const Config& cfg, std::string& error);
    void stop();
    bool running() const { return session_ && session_->running(); }
    DsdState state() const { return session_ ? session_->state() : DsdState(); }
    std::vector<std::string> log(size_t n = 200) const { return session_ ? session_->recentLog(n) : std::vector<std::string>(); }
    double freqHz() const { return tap_ ? tap_->freqHz() : 0.0; }
    const std::string& presetId() const { return presetId_; }

private:
    DsdSession::AudioCb audioCb_;
    std::unique_ptr<ChannelPcmTap> tap_;
    std::unique_ptr<DsdSession> session_;
    std::string presetId_;
};

}  // namespace sdrstar
