// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrpp_host.h"

#include <cmath>

#include <core.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

using sdrstar::ReceiverCaps;

SdrppRadioHost::~SdrppRadioHost() { closeAll(); }

std::string SdrppRadioHost::sourceName() const {
    core::configManager.acquire();
    std::string s = core::configManager.conf.contains("source") ? (std::string)core::configManager.conf["source"] : std::string();
    core::configManager.release();
    return s;
}

bool SdrppRadioHost::retunable() const {
    const std::string s = sourceName();
    // Recordings and network replays have no tuner to move.
    return !(s == "File" || s == "Network" || s == "");
}

ReceiverCaps SdrppRadioHost::caps() const {
    ReceiverCaps c;
    c.sampleRateHz = sigpath::iqFrontEnd.getSampleRate();  // after any decimation the user chose = what the waterfall shows
    const std::string s = sourceName();
    c.name = s.empty() ? "receiver" : s;
    const double center = gui::waterfall.getCenterFrequency();
    if (s == "RTL-SDR" || s == "RTL-TCP") {
        c.name = "RTL-SDR";
        c.minHz = 500e3;    // RTL-SDR Blog V4 HF upconverter
        c.maxHz = 1766e6;
        c.retuneMs = 250.0; // PLL + USB buffers in flight
    } else if (!retunable()) {
        c.name = "recording";
        c.minHz = center - c.sampleRateHz / 2.0;
        c.maxHz = center + c.sampleRateHz / 2.0;
    } else {
        c.minHz = 1e6;
        c.maxHz = 7.5e9;
    }
    return c;
}

double SdrppRadioHost::centerHz() const { return gui::waterfall.getCenterFrequency(); }

bool SdrppRadioHost::fitsLocked(double absHz, double bwHz) const {
    const double half = caps().usableSpanHz() / 2.0;
    return std::fabs(absHz - centerHz()) + bwHz / 2.0 <= half + 1.0;
}

void SdrppRadioHost::applyOffsetsLocked() {
    const double center = centerHz();
    for (auto& kv : chans_) {
        Chan& c = *kv.second;
        c.vfo->setOffset(c.absHz - center);
        c.vfo->reset();
    }
}

bool SdrppRadioHost::setCenter(double hz, std::string& error) {
    if (!retunable()) {
        error = "the current source (" + sourceName() + ") is a recording and cannot be retuned";
        return false;
    }
    const ReceiverCaps c = caps();
    if (hz < c.minHz || hz > c.maxHz) {
        error = "frequency outside the receiver's tuning range";
        return false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    const double oldCenter = centerHz();

    // Remember where every visible VFO is in ABSOLUTE terms so we can put it back after the window moves.
    struct Saved { std::string name; double abs; double bw; };
    std::vector<Saved> vis;
    for (auto& kv : gui::waterfall.vfos) {
        vis.push_back({kv.first, oldCenter + kv.second->generalOffset, kv.second->bandwidth});
    }

    tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", hz);  // moves waterfall + hardware (synchronous for the RTL-SDR)
    sigpath::iqFrontEnd.flushInputBuffer();

    applyOffsetsLocked();  // hidden channels keep their absolute frequency

    const double half = sigpath::iqFrontEnd.getSampleRate() / 2.0;
    for (const auto& v : vis) {
        double off = v.abs - hz;
        if (std::fabs(off) + v.bw / 2.0 < half) { sigpath::vfoManager.setOffset(v.name, off); }
    }
    return true;
}

void SdrppRadioHost::onBlock(dsp::complex_t* data, int count, void* ctx) {
    Chan* c = (Chan*)ctx;
    static_assert(sizeof(dsp::complex_t) == sizeof(sdrstar::cf), "complex layout");
    if (c->sink) { c->sink(reinterpret_cast<const sdrstar::cf*>(data), (size_t)count); }
}

int SdrppRadioHost::openChannel(double absFreqHz, double bwHz, IqSinkFn sink, std::string& error, double rateHz) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (rateHz <= 0) { rateHz = channelRateHz(); }
    if (rateHz > caps().sampleRateHz) {
        error = "the radio's current sample rate is too low for this decoder";
        return -1;
    }
    if (!fitsLocked(absFreqHz, bwHz)) {
        error = "channel is outside the visible window";
        return -1;
    }
    const int id = nextId_++;
    auto c = std::make_unique<Chan>();
    c->name = "sdrstar_ch_" + std::to_string(id);
    c->absHz = absFreqHz;
    c->bwHz = bwHz;
    c->sink = std::move(sink);
    c->vfo = sigpath::iqFrontEnd.addVFO(c->name, rateHz, bwHz, absFreqHz - centerHz());
    if (!c->vfo) {
        error = "could not create a hidden channel";
        return -1;
    }
    c->handler.init(&c->vfo->out, onBlock, c.get());
    c->handler.start();
    chans_[id] = std::move(c);
    return id;
}

bool SdrppRadioHost::moveChannel(int channel, double absFreqHz) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = chans_.find(channel);
    if (it == chans_.end()) { return false; }
    Chan& c = *it->second;
    if (!fitsLocked(absFreqHz, c.bwHz)) { return false; }
    c.absHz = absFreqHz;
    c.vfo->setOffset(absFreqHz - centerHz());
    c.vfo->reset();  // drop filter history from the previous frequency
    return true;
}

void SdrppRadioHost::closeChannel(int channel) {
    std::unique_ptr<Chan> c;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = chans_.find(channel);
        if (it == chans_.end()) { return; }
        c = std::move(it->second);
        chans_.erase(it);
    }
    // Stop (and join) the DSP-thread handler FIRST: only then is nothing running inside `sink`, so it can be released.
    c->handler.stop();
    c->sink = nullptr;
    sigpath::iqFrontEnd.removeVFO(c->name);
}

void SdrppRadioHost::closeAll() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : chans_) { ids.push_back(kv.first); }
    }
    for (int id : ids) { closeChannel(id); }
}
