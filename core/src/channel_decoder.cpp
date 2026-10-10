// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/channel_decoder.h"

namespace sdrstar {

bool ChannelDecoder::start(RadioHost* host, const Config& cfg, std::string& error) {
    stop();
    const DsdPreset* p = findDsdPreset(cfg.presetId);
    if (!p) {
        error = "unknown decoder preset '" + cfg.presetId + "'";
        return false;
    }
    presetId_ = cfg.presetId;
    tap_ = std::make_unique<ChannelPcmTap>();
    if (!tap_->open(host, cfg.freqHz, cfg.bwHz, error)) {
        tap_.reset();
        return false;
    }
    session_ = std::make_unique<DsdSession>();
    if (audioCb_) { session_->setAudioCallback(audioCb_); }
    DsdSession::Config dc;
    dc.exePath = cfg.dsdExe;
    dc.mode = p->flags;
    dc.extra = cfg.extraArgs;
    dc.wantAudio = true;
    dc.workDir = cfg.workDir;
    dc.label = "decode-" + cfg.presetId;
    if (!session_->start(dc, error)) {
        session_.reset();
        tap_.reset();
        return false;
    }
    DsdSession* s = session_.get();
    tap_->addSink([s](const int16_t* pcm, size_t n) { s->pushPcm(pcm, n); });
    return true;
}

void ChannelDecoder::stop() {
    if (tap_) { tap_->clearSinks(); }
    session_.reset();  // kills the decoder
    tap_.reset();
}

}  // namespace sdrstar
