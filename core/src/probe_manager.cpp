// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/probe_manager.h"

#include <algorithm>
#include <cstdio>

#include "sdrstar/band_plan.h"

namespace sdrstar {

std::vector<std::string> candidatePresetsFor(double bwHz, double freqHz) {
    std::vector<std::string> v;
    if (bwHz > 0 && bwHz < 5500) {
        // 6.25 kHz-class signals
        v = {"nxdn48", "dpmr"};
        return v;
    }
    // Fewer decoders means less CPU and fewer chances for a false claim, so use what the band says about who lives
    // there: D-STAR / YSF / M17 are amateur-only modes, EDACS / ProVoice are land-mobile-only.  An unknown band
    // (HF, odd frequency, a band the table does not cover) tries everything.
    const bool amateur = bandSuggests(freqHz, "dstar") || bandSuggests(freqHz, "ysf") || bandSuggests(freqHz, "m17");
    const bool landMobile = bandSuggests(freqHz, "edacs") || bandSuggests(freqHz, "smartnet") ||
                            (bandSuggests(freqHz, "p25") && !amateur);
    v = {"p25p1", "dmr", "p25p2"};
    if (!landMobile) { v.insert(v.end(), {"dstar", "ysf"}); }
    v.push_back("nxdn96");
    if (!amateur) { v.insert(v.end(), {"edacs", "provoice"}); }
    if (!landMobile && (amateur || bandSuggests(freqHz, "m17") || bandSuggests(freqHz, "dstar"))) { v.push_back("m17"); }
    return v;
}

bool ProbeManager::start(RadioHost* host, double freqHz, const std::vector<std::string>& presetIds, const Config& cfg, std::string& error) {
    cancel();
    cfg_ = cfg;
    freqHz_ = freqHz;
    finished_ = false;
    last_ = ProbeProgress();
    last_.freqHz = freqHz;
    last_.durationSec = cfg.durationSec;
    if (presetIds.empty()) {
        error = "no candidate decoders for this signal";
        return false;
    }
    tap_ = std::make_unique<ChannelPcmTap>();
    if (!tap_->open(host, freqHz, cfg.channelBwHz, error)) {
        tap_.reset();
        return false;
    }
    entries_.clear();
    for (const auto& id : presetIds) {
        const DsdPreset* p = findDsdPreset(id);
        if (!p) { continue; }
        Entry e;
        e.presetId = id;
        e.session = std::make_unique<DsdSession>();
        DsdSession::Config dc;
        dc.exePath = cfg.dsdExe;
        dc.mode = p->flags;
        dc.wantAudio = false;
        dc.workDir = cfg.workDir.empty() ? "" : cfg.workDir + "/probe-" + id;
        dc.label = "probe-" + id;
        std::string err;
        if (!e.session->start(dc, err)) {
            error = "could not start decoder for " + id + ": " + err;
            entries_.clear();
            tap_.reset();
            return false;
        }
        entries_.push_back(std::move(e));
    }
    // Fan the PCM out to every session.
    tap_->addSink([this](const int16_t* pcm, size_t n) {
        for (auto& e : entries_) { e.session->pushPcm(pcm, n); }
    });
    t0_ = std::chrono::steady_clock::now();
    active_ = true;
    last_.active = true;
    return true;
}

ProbeProgress ProbeManager::poll() {
    if (!active_.load()) { return last_; }
    ProbeProgress pr;
    pr.active = true;
    pr.freqHz = freqHz_;
    pr.durationSec = cfg_.durationSec;
    pr.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    for (auto& e : entries_) {
        pr.verdicts.push_back(evaluateProbe(e.presetId, e.session->state(), e.session->recentLog(400)));
    }
    pr.best = bestVerdict(pr.verdicts);

    bool done = pr.elapsedSec >= cfg_.durationSec;
    if (pr.best.claimed && pr.best.confidence >= cfg_.earlyExitConfidence && pr.elapsedSec >= cfg_.earlyExitMinSec) { done = true; }
    if (tap_ && pr.elapsedSec > 2.0 && tap_->samplesSeen() == 0) {
        pr.error = "No signal samples arrived on this channel (is the receiver running?).";
        done = true;
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "Checking %d digital modes ... %.1f s", (int)entries_.size(), pr.elapsedSec);
    pr.status = buf;
    if (done) {
        pr.finished = true;
        pr.active = false;
        last_ = pr;
        finish();
        last_.status = pr.best.claimed ? ("Identified: " + pr.best.title) : "No digital protocol recognised";
        return last_;
    }
    last_ = pr;
    return pr;
}

void ProbeManager::finish() {
    active_ = false;
    if (tap_) { tap_->clearSinks(); }
    entries_.clear();  // destroys sessions (kills the decoder processes)
    tap_.reset();
    finished_ = true;
}

void ProbeManager::cancel() {
    if (active_.load() || !entries_.empty() || tap_) {
        active_ = false;
        if (tap_) { tap_->clearSinks(); }
        entries_.clear();
        tap_.reset();
    }
}

}  // namespace sdrstar
