// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <module.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

#include <sdrstar/band_plan.h>

using namespace sdrstar;

namespace {
ConfigManager gCfg;
bool fileExecutable(const std::string& p) { return !p.empty() && ::access(p.c_str(), X_OK) == 0; }
}  // namespace

SmartEngine::SmartEngine() : follower_(&host_) {}

SmartEngine::~SmartEngine() { stop(); }

double SmartEngine::nowSec() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string SmartEngine::findHelper(const std::string& name) const {
    std::string envName = "SDRSTAR_";
    for (char c : name) { envName += (c == '-') ? '_' : (char)std::toupper((unsigned char)c); }
    if (const char* e = std::getenv(envName.c_str())) {
        if (fileExecutable(e)) { return e; }
    }
    std::string modDir;
    core::configManager.acquire();
    if (core::configManager.conf.contains("modulesDirectory")) { modDir = (std::string)core::configManager.conf["modulesDirectory"]; }
    core::configManager.release();
    const std::string cands[] = {modDir + "/../Resources/helpers/" + name, modDir + "/../helpers/" + name, modDir + "/helpers/" + name,
                                 root_ + "/helpers/" + name};
    for (const auto& c : cands) {
        if (!modDir.empty() || c.rfind(root_, 0) == 0) {
            if (fileExecutable(c)) { return std::filesystem::weakly_canonical(c).string(); }
        }
    }
    return "";
}

void SmartEngine::loadSettings() {
    json def = json::object();
    def["showLabels"] = true;
    def["autoIdentify"] = true;
    def["allowHop"] = true;
    def["sensitivityDb"] = 10.0;
    def["channelMapPath"] = "";
    def["groupListPath"] = "";
    gCfg.setPath(root_ + "/sdrstar_smart_config.json");
    gCfg.load(def);
    gCfg.acquire();
    settings_.showLabels = gCfg.conf.value("showLabels", true);
    settings_.autoIdentify = gCfg.conf.value("autoIdentify", true);
    settings_.allowHop = gCfg.conf.value("allowHop", true);
    settings_.sensitivityDb = gCfg.conf.value("sensitivityDb", 10.0);
    settings_.channelMapPath = gCfg.conf.value("channelMapPath", std::string());
    settings_.groupListPath = gCfg.conf.value("groupListPath", std::string());
    gCfg.release();
}

void SmartEngine::saveSettings() {
    gCfg.acquire();
    gCfg.conf["showLabels"] = settings_.showLabels;
    gCfg.conf["autoIdentify"] = settings_.autoIdentify;
    gCfg.conf["allowHop"] = settings_.allowHop;
    gCfg.conf["sensitivityDb"] = settings_.sensitivityDb;
    gCfg.conf["channelMapPath"] = settings_.channelMapPath;
    gCfg.conf["groupListPath"] = settings_.groupListPath;
    gCfg.release(true);
}

void SmartEngine::settingsChanged() {
    {
        std::lock_guard<std::mutex> lk(detMtx_);
        DetectorParams p = det_.params();
        p.marginDb = settings_.sensitivityDb;
        det_.setParams(p);
    }
    follower_.setAllowHop(settings_.allowHop);
    saveSettings();
}

void SmartEngine::start(const std::string& rootDir) {
    if (started_) { return; }
    root_ = rootDir;
    std::error_code ec;
    std::filesystem::create_directories(root_ + "/sdrstar_work", ec);
    loadSettings();
    dsdPath_ = findHelper("dsd-neo");
    multimonPath_ = findHelper("multimon-ng");
    rtl433Path_ = findHelper("rtl_433");
    if (multimonPath_.empty()) { flog::warn("SDRstar: multimon-ng not found; pager / APRS / weather-alert decoding is disabled"); }
    if (rtl433Path_.empty()) { flog::warn("SDRstar: rtl_433 not found; ISM sensor decoding is disabled"); }
    if (dsdPath_.empty()) {
        flog::warn("SDRstar: the dsd-neo decoder was not found; digital identification and trunk following are disabled");
    } else {
        flog::info("SDRstar: digital decoder: {0}", dsdPath_);
    }
    {
        std::lock_guard<std::mutex> lk(detMtx_);
        DetectorParams p;
        p.marginDb = settings_.sensitivityDb;
        det_.setParams(p);
    }
    follower_.setAllowHop(settings_.allowHop);
    voice_.init("SDRstar Voice");
    startSec_ = nowSec();
    tap_.start([this](const float* db, int bins, double c, double s) { onSpectrum(db, bins, c, s); });
    started_ = true;
}

void SmartEngine::stop() {
    if (!started_) { return; }
    started_ = false;
    stopJob();
    tap_.stop();
    host_.closeAll();
    voice_.shutdown();
}

void SmartEngine::onSpectrum(const float* db, int bins, double center, double span) {
    SpectrumFrame f;
    f.db = db;
    f.bins = bins;
    f.centerHz = center;
    f.spanHz = span;
    std::lock_guard<std::mutex> lk(detMtx_);
    det_.update(f, nowSec());
}

std::vector<DetectedSignal> SmartEngine::signals() const {
    std::lock_guard<std::mutex> lk(detMtx_);
    return det_.signals();
}

double SmartEngine::noiseFloorDb() const {
    std::lock_guard<std::mutex> lk(detMtx_);
    return det_.noiseFloorDb();
}

std::string SmartEngine::sourceSummary() const {
    ReceiverCaps c = host_.caps();
    char buf[200];
    double center = host_.centerHz();
    std::snprintf(buf, sizeof buf, "%s  \xC2\xB7  %s  \xC2\xB7  %s - %s", c.name.c_str(), formatSpan(c.sampleRateHz).c_str(),
                  formatFrequency(center - c.sampleRateHz / 2).c_str(), formatFrequency(center + c.sampleRateHz / 2).c_str());
    return buf;
}

// ------------------------------------------------------------------------------------------------------------
// cursor
// ------------------------------------------------------------------------------------------------------------
void SmartEngine::updateCursor(double now) {
    bool valid = false;
    double abs = 0, bw = 0;
    std::string name;
    if (gui::waterfall.selectedVFO != "") {
        auto it = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
        if (it != gui::waterfall.vfos.end()) {
            abs = gui::waterfall.getCenterFrequency() + it->second->generalOffset;
            bw = it->second->bandwidth;
            name = it->first;
            valid = true;
        }
    }
    if (valid != cursorValid_ || std::fabs(abs - cursorFreq_) > 50.0 || std::fabs(bw - cursorBw_) > 50.0) {
        cursorSince_ = now;
        if (dismissed_ && std::fabs(abs - dismissedFreq_) > 2500.0) { dismissed_ = false; }
    }
    cursorValid_ = valid;
    cursorFreq_ = abs;
    cursorBw_ = bw;
    cursorVfoName_ = name;
}

bool SmartEngine::nearestSignal(double freqHz, double bwHz, DetectedSignal& out) const {
    std::lock_guard<std::mutex> lk(detMtx_);
    const double reach = std::max(8000.0, bwHz * 1.3);
    const DetectedSignal* best = nullptr;
    double bestScore = 1e18;
    for (const auto& s : det_.signals()) {
        if (!s.present && s.identified.empty()) { continue; }
        const double d = std::fabs(s.freqHz - freqHz);
        if (d > std::max(reach, s.bwHz / 2.0)) { continue; }
        if (s.snrDb < settings_.sensitivityDb + 1.0) { continue; }
        // prefer close AND strong: distance in kHz minus a bonus for SNR
        const double score = d / 1000.0 - 0.15 * s.snrDb;
        if (score < bestScore) {
            bestScore = score;
            best = &s;
        }
    }
    if (!best) { return false; }
    out = *best;
    return true;
}

double SmartEngine::snapFrequency(double freqHz, double bwHz) const {
    DetectedSignal s;
    if (nearestSignal(freqHz, bwHz, s)) { return s.freqHz; }
    return freqHz;
}

const SmartEngine::StoredResult* SmartEngine::resultFor(double freqHz) const {
    auto it = results_.find(keyFor(freqHz));
    if (it != results_.end()) { return &it->second; }
    for (long long k = keyFor(freqHz) - 1; k <= keyFor(freqHz) + 1; ++k) {
        auto j = results_.find(k);
        if (j != results_.end() && std::fabs((double)k * 6250.0 - freqHz) < 7000.0) { return &j->second; }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------------------
// periodic work
// ------------------------------------------------------------------------------------------------------------
void SmartEngine::tick() {
    if (!started_) { return; }
    const double now = nowSec();
    updateCursor(now);
    pollProbe(now);
    autoProbe(now);
    if (job_ == JobKind::Decoding && !decoder_.running()) {
        addNote("The decoder stopped.");
        stopJob();
    }
    if (job_ == JobKind::Helper && !helper_.running()) {
        std::string why = helper_.lastError();
        addNote("The " + jobTitle_ + " decoder stopped." + (why.empty() ? "" : " (" + why + ")"));
        stopJob();
    }
    if (job_ == JobKind::Following) {
        FollowStatus st = follower_.status();
        if (st.state == FollowState::Stopped) { stopJob(); }
    }
}

void SmartEngine::addNote(const std::string& s) {
    notes_.push_back(s);
    if (notes_.size() > 50) { notes_.erase(notes_.begin()); }
    flog::info("SDRstar: {0}", s);
}

void SmartEngine::pollProbe(double now) {
    if (job_ != JobKind::Probing) { return; }
    ProbeProgress p = probe_.poll();
    lastProbe_ = p;
    if (!p.finished) { return; }
    StoredResult r;
    r.verdict = p.best;
    r.whenSec = now;
    r.bwHz = 12500;
    results_[keyFor(jobFreq_)] = r;
    if (p.best.claimed) {
        std::lock_guard<std::mutex> lk(detMtx_);
        det_.setIdentified(jobFreq_, p.best.title, p.best.detail);
    } else {
        noClaimUntil_[keyFor(jobFreq_)] = now + 90.0;
    }
    job_ = JobKind::None;
}

void SmartEngine::autoProbe(double now) {
    if (!settings_.autoIdentify || job_ != JobKind::None || dsdPath_.empty()) { return; }
    if (now - lastAutoScanSec_ < 1.0) { return; }
    lastAutoScanSec_ = now;
    if (now - lastAutoProbeSec_ < 4.0) { return; }
    std::vector<DetectedSignal> sigs = signals();
    const DetectedSignal* best = nullptr;
    for (const auto& s : sigs) {
        if (!s.present || s.hits < 8 || s.snrDb < 14.0 || !s.identified.empty()) { continue; }
        if (s.kind != SignalKind::Channel12k && s.kind != SignalKind::Narrow) { continue; }
        if (bandSuggests(s.freqHz, "wfm")) { continue; }
        auto cd = noClaimUntil_.find(keyFor(s.freqHz));
        if (cd != noClaimUntil_.end() && cd->second > now) { continue; }
        // Skip splatter/images: a much stronger signal within 40 kHz makes this one almost certainly an artifact of it.
        bool dominated = false;
        for (const auto& o : sigs) {
            if (&o != &s && std::fabs(o.freqHz - s.freqHz) < 40000.0 && o.snrDb > s.snrDb + 15.0) { dominated = true; break; }
        }
        if (dominated) { continue; }
        if (!best || s.snrDb > best->snrDb) { best = &s; }
    }
    if (best) {
        lastAutoProbeSec_ = now;
        startProbe(best->freqHz, best->bwHz, true);
    }
}

void SmartEngine::startProbe(double freqHz, double bwHz, bool automatic) {
    if (dsdPath_.empty()) {
        addNote("The digital decoder (dsd-neo) was not found, so signals cannot be identified.");
        return;
    }
    if (automatic && job_ != JobKind::None) { return; }
    stopJob();
    ProbeManager::Config c;
    c.dsdExe = dsdPath_;
    c.workDir = root_ + "/sdrstar_work";
    c.durationSec = 5.0;
    std::string err;
    auto presets = candidatePresetsFor(bwHz, freqHz);
    if (!probe_.start(&host_, freqHz, presets, c, err)) {
        addNote("Could not start identification: " + err);
        noClaimUntil_[keyFor(freqHz)] = nowSec() + 20.0;
        return;
    }
    job_ = JobKind::Probing;
    jobAutomatic_ = automatic;
    jobFreq_ = freqHz;
    lastProbe_ = ProbeProgress();
    lastProbe_.active = true;
    lastProbe_.freqHz = freqHz;
}

void SmartEngine::identifyAtCursor() {
    if (!cursorValid_) { return; }
    double f = snapFrequency(cursorFreq_, cursorBw_);
    results_.erase(keyFor(f));
    startProbe(f, std::min(cursorBw_, 25000.0), false);
}

// ------------------------------------------------------------------------------------------------------------
// jobs
// ------------------------------------------------------------------------------------------------------------
void SmartEngine::setRadioMuted(bool muted) {
    radioMuted_ = muted;
    for (auto& kv : gui::waterfall.vfos) {
        const std::string& name = kv.first;
        if (core::modComManager.interfaceExists(name) && core::modComManager.getModuleName(name) == "radio") {
            core::modComManager.callInterface(name, RADIO_IFACE_CMD_SET_MUTE, &muted, NULL);
        }
    }
}

void SmartEngine::setRadioMode(int mode, float bandwidth) {
    const std::string name = gui::waterfall.selectedVFO;
    if (name.empty() || !core::modComManager.interfaceExists(name) || core::modComManager.getModuleName(name) != "radio") { return; }
    core::modComManager.callInterface(name, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
    core::modComManager.callInterface(name, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
}

void SmartEngine::stopJob() {
    switch (job_) {
    case JobKind::Probing: probe_.cancel(); break;
    case JobKind::Following: follower_.stop(); break;
    case JobKind::Decoding: decoder_.stop(); break;
    case JobKind::Helper: helper_.stop(); break;
    default: break;
    }
    job_ = JobKind::None;
    if (radioMuted_) { setRadioMuted(false); }
    voice_.clear();
}

void SmartEngine::startFollow(const StoredResult& r) {
    if (dsdPath_.empty()) { return; }
    const double freq = jobFreq_ > 0 && job_ == JobKind::Probing ? jobFreq_ : snapFrequency(cursorFreq_, cursorBw_);
    stopJob();
    TrunkFollower::Config c;
    c.dsdExe = dsdPath_;
    c.workDir = root_ + "/sdrstar_work/trunk";
    c.controlHz = freq;
    c.allowHop = settings_.allowHop;
    const std::string& id = r.verdict.presetId;
    if (id == "p25p1" || id == "p25p2" || id == "p25") { c.mode = {"-ft"}; }
    else if (id == "dmr") { c.mode = {"-fs"}; }
    else if (id == "nxdn96") { c.mode = {"-fn"}; }
    else if (id == "nxdn48") { c.mode = {"-fi"}; }
    else if (id == "edacs" || id == "provoice") { c.mode = {"-fh"}; }
    if (!settings_.channelMapPath.empty()) { c.extraArgs.insert(c.extraArgs.end(), {"-C", settings_.channelMapPath}); }
    if (!settings_.groupListPath.empty()) { c.extraArgs.insert(c.extraArgs.end(), {"-G", settings_.groupListPath}); }
    follower_.setAudioCallback([this](const int16_t* d, size_t n, int ch, int rate) { voice_.push(d, n, ch, rate); });
    std::string err;
    if (!follower_.start(c, err)) {
        addNote("Could not start following: " + err);
        return;
    }
    job_ = JobKind::Following;
    jobFreq_ = freq;
    jobTitle_ = r.verdict.title;
    setRadioMuted(true);
}

void SmartEngine::startDecode(const StoredResult& r) {
    if (dsdPath_.empty()) { return; }
    const double freq = snapFrequency(cursorFreq_, cursorBw_);
    stopJob();
    ChannelDecoder::Config c;
    c.dsdExe = dsdPath_;
    c.workDir = root_ + "/sdrstar_work/decode";
    c.presetId = r.verdict.presetId;
    c.freqHz = freq;
    c.bwHz = (c.presetId == "nxdn48" || c.presetId == "dpmr") ? 8000 : 12500;
    decoder_.setAudioCallback([this](const int16_t* d, size_t n, int ch, int rate) { voice_.push(d, n, ch, rate); });
    std::string err;
    if (!decoder_.start(&host_, c, err)) {
        addNote("Could not start the decoder: " + err);
        return;
    }
    job_ = JobKind::Decoding;
    jobFreq_ = freq;
    jobTitle_ = r.verdict.title;
    setRadioMuted(true);
}

bool SmartEngine::helperAvailable(const std::string& id) const {
    const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(id);
    if (!sp) { return false; }
    return !(sp->exeName == "rtl_433" ? rtl433Path_ : multimonPath_).empty();
}

void SmartEngine::startHelper(const std::string& id, double freqHz) {
    const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(id);
    if (!sp) { return; }
    const std::string& exe = sp->exeName == "rtl_433" ? rtl433Path_ : multimonPath_;
    if (exe.empty()) {
        addNote("The " + sp->exeName + " helper was not found, so " + sp->title + " cannot be decoded.");
        return;
    }
    stopJob();
    std::string err;
    if (!helper_.start(&host_, *sp, exe, freqHz, err)) {
        addNote("Could not start " + sp->title + ": " + err);
        return;
    }
    job_ = JobKind::Helper;
    jobFreq_ = freqHz;
    jobTitle_ = sp->title;
}

void SmartEngine::doAction(CardAction a) {
    switch (a) {
    case ACT_HELPER_PAGER: startHelper("pager", snapFrequency(cursorFreq_, cursorBw_)); break;
    case ACT_HELPER_APRS: startHelper("aprs", snapFrequency(cursorFreq_, cursorBw_)); break;
    case ACT_HELPER_EAS: startHelper("eas", snapFrequency(cursorFreq_, cursorBw_)); break;
    case ACT_HELPER_ISM: startHelper("ism", cursorFreq_); break;
    case ACT_IDENTIFY: identifyAtCursor(); break;
    case ACT_FOLLOW:
        if (const StoredResult* r = resultFor(snapFrequency(cursorFreq_, cursorBw_))) {
            StoredResult copy = *r;
            startFollow(copy);
        }
        break;
    case ACT_DECODE:
        if (const StoredResult* r = resultFor(snapFrequency(cursorFreq_, cursorBw_))) {
            StoredResult copy = *r;
            startDecode(copy);
        }
        break;
    case ACT_LISTEN_NFM:
        stopJob();
        setRadioMode(RADIO_IFACE_MODE_NFM, 12500.0f);
        break;
    case ACT_LISTEN_WFM:
        stopJob();
        setRadioMode(RADIO_IFACE_MODE_WFM, 150000.0f);
        break;
    case ACT_LISTEN_AM:
        stopJob();
        setRadioMode(RADIO_IFACE_MODE_AM, 10000.0f);
        break;
    case ACT_CENTER: {
        DetectedSignal sig;
        if (cursorValid_ && !cursorVfoName_.empty() && nearestSignal(cursorFreq_, cursorBw_, sig)) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, cursorVfoName_, sig.freqHz);
        }
        break;
    }
    case ACT_STOP: stopJob(); break;
    case ACT_DISMISS: dismissCard(); break;
    case ACT_TOGGLE_HOP:
        settings_.allowHop = !settings_.allowHop;
        settingsChanged();
        break;
    default: break;
    }
}

std::string SmartEngine::decoderStatusLine() {
    if (dsdPath_.empty()) { return "Digital decoder (dsd-neo) not found: identification and trunk following are off."; }
    switch (job_) {
    case JobKind::Probing: return lastProbe_.status;
    case JobKind::Following: return follower_.status().headline;
    case JobKind::Decoding: return "Decoding " + jobTitle_;
    case JobKind::Helper: return jobTitle_ + " at " + formatFrequency(jobFreq_) + ": " + std::to_string(helper_.messageCount()) + " received";
    default: return "Ready.";
    }
}

// ------------------------------------------------------------------------------------------------------------
// the card
// ------------------------------------------------------------------------------------------------------------
namespace {
std::string mhz(double hz) { return formatFrequency(hz); }
}  // namespace

CardModel SmartEngine::card() {
    CardModel m;
    // --- an active job always has a pinned card ---
    if (job_ == JobKind::Following) {
        FollowStatus st = follower_.status();
        m.visible = true;
        m.pinned = true;
        m.freqHz = st.currentHz > 0 ? st.currentHz : jobFreq_;
        m.tone = !st.problem.empty() ? CardTone::Problem : (st.state == FollowState::Hopped ? CardTone::Attention : CardTone::Good);
        m.title = st.headline.empty() ? "Following trunked system" : st.headline;
        m.subtitle = st.system;
        if (!st.problem.empty()) { m.warning = st.problem; }
        if (!st.detail.empty()) { m.lines.push_back(st.detail); }
        char buf[160];
        std::snprintf(buf, sizeof buf, "Window moves: %d   Calls followed: %d   Hops: %d   Skipped: %d", st.windowRetunes, st.channelMoves + st.hops, st.hops, st.refusals);
        m.lines.push_back(buf);
        m.buttons.push_back({ACT_TOGGLE_HOP, settings_.allowHop ? "Hop to out-of-range calls: ON" : "Hop to out-of-range calls: OFF", false, true});
        m.buttons.push_back({ACT_STOP, "Stop following", true, true});
        return m;
    }
    if (job_ == JobKind::Decoding) {
        DsdState d = decoder_.state();
        m.visible = true;
        m.pinned = true;
        m.freqHz = jobFreq_;
        m.tone = CardTone::Good;
        m.title = jobTitle_;
        std::string sub = mhz(jobFreq_);
        for (const auto& c : d.calls) {
            if (c.state == 2) { sub += "   \xC2\xB7   TG " + std::to_string(c.tgId) + (c.srcId ? " from " + std::to_string(c.srcId) : ""); break; }
        }
        m.subtitle = sub;
        if (d.identity.colorCode >= 0) { m.lines.push_back("Color Code " + std::to_string(d.identity.colorCode)); }
        m.buttons.push_back({ACT_STOP, "Stop decoding", true, true});
        return m;
    }
    if (job_ == JobKind::Helper) {
        m.visible = true;
        m.pinned = true;
        m.freqHz = jobFreq_;
        m.tone = CardTone::Good;
        m.title = jobTitle_;
        m.subtitle = mhz(jobFreq_) + "   \xC2\xB7   " + std::to_string(helper_.messageCount()) + " received";
        auto ls = helper_.lines(5);
        if (ls.empty()) {
            const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(helper_.spec().id);
            m.lines.push_back(sp ? sp->idleHint : std::string("Waiting..."));
        }
        for (const auto& l : ls) {
            std::string t = l.text;
            if (t.size() > 140) { t = t.substr(0, 137) + "..."; }
            m.lines.push_back(t);
        }
        if (helper_.droppedBytes() > 0) { m.warning = "The decoder could not keep up; some signal was skipped."; }
        m.buttons.push_back({ACT_STOP, "Stop", true, true});
        return m;
    }
    if (job_ == JobKind::Probing) {
        if (jobAutomatic_) {
            // Background identification: never steal the screen. The cursor's own card wins; otherwise a small chip.
            CardModel cc = buildCursorCard();
            if (cc.visible) { return cc; }
            m.visible = true;
            m.compact = true;
            m.tone = CardTone::Working;
            char buf[128];
            std::snprintf(buf, sizeof buf, "Identifying %s ...", mhz(jobFreq_).c_str());
            m.title = buf;
            if (lastProbe_.durationSec > 0) { m.progress = (float)std::min(1.0, lastProbe_.elapsedSec / lastProbe_.durationSec); }
            return m;
        }
        m.visible = true;
        m.pinned = true;
        m.freqHz = jobFreq_;
        m.tone = CardTone::Working;
        m.title = "Identifying signal at " + mhz(jobFreq_);
        m.subtitle = lastProbe_.status;
        if (lastProbe_.durationSec > 0) { m.progress = (float)std::min(1.0, lastProbe_.elapsedSec / lastProbe_.durationSec); }
        if (lastProbe_.best.claimed) { m.lines.push_back("Leading candidate: " + lastProbe_.best.title); }
        m.buttons.push_back({ACT_STOP, "Cancel", false, true});
        return m;
    }
    return buildCursorCard();
}

std::vector<CardButton> SmartEngine::helperButtonsFor(double f) const {
    std::vector<CardButton> v;
    auto add = [&](const char* proto, const char* id, CardAction act, const char* label) {
        if (!bandSuggests(f, proto)) { return; }
        const bool ok = helperAvailable(id);
        v.push_back({act, ok ? std::string(label) : std::string(label) + " (helper missing)", false, ok});
    };
    add("pocsag", "pager", ACT_HELPER_PAGER, "Read pager messages");
    add("aprs", "aprs", ACT_HELPER_APRS, "Decode APRS");
    add("eas", "eas", ACT_HELPER_EAS, "Read weather alerts");
    add("ism", "ism", ACT_HELPER_ISM, "Read sensors");
    return v;
}

CardModel SmartEngine::buildCursorCard() {
    CardModel m;
    // --- otherwise: a card for the signal under the cursor, once the cursor has rested on it ---
    if (!cursorValid_ || dismissed_) { return m; }
    if (nowSec() - cursorSince_ < 0.45) { return m; }
    DetectedSignal sig;
    if (!nearestSignal(cursorFreq_, cursorBw_, sig)) { return m; }
    m.visible = true;
    m.freqHz = cursorFreq_;
    const StoredResult* r = resultFor(sig.freqHz);
    const bool narrow = sig.kind == SignalKind::Channel12k || sig.kind == SignalKind::Narrow;

    if (r && r->verdict.claimed) {
        m.tone = CardTone::Good;
        m.title = r->verdict.title;
        m.subtitle = r->verdict.detail.empty() ? mhz(sig.freqHz) : r->verdict.detail;
        if (r->verdict.controlChannel) {
            m.lines.push_back("This is a trunked system. Following it moves the receiver to each call's voice channel.");
            m.buttons.push_back({ACT_FOLLOW, "Follow trunked system", true, true});
        } else {
            m.buttons.push_back({ACT_DECODE, "Decode and listen", true, true});
        }
        m.buttons.push_back({ACT_IDENTIFY, "Re-check", false, true});
    } else if (r && !r->verdict.claimed) {
        m.tone = CardTone::Attention;
        m.title = "No digital protocol recognised";
        m.subtitle = mhz(sig.freqHz) + "  \xC2\xB7  " + formatSpan(sig.bwHz) + " wide  \xC2\xB7  " + std::to_string((int)std::lround(sig.snrDb)) + " dB above noise";
        m.lines.push_back("Could be analog voice, an encrypted or unsupported mode, or too weak to decode.");
        m.buttons.push_back({ACT_LISTEN_NFM, "Listen as FM", true, true});
        m.buttons.push_back({ACT_IDENTIFY, "Try again", false, !dsdPath_.empty()});
    } else {
        m.tone = CardTone::Info;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s  \xC2\xB7  %s  \xC2\xB7  %d dB", mhz(sig.freqHz).c_str(), formatSpan(sig.bwHz).c_str(), (int)std::lround(sig.snrDb));
        m.subtitle = buf;
        m.title = sig.label;
        if (!sig.hint.empty()) { m.lines.push_back("Band: " + sig.hint); }
        if (!sig.note.empty()) { m.lines.push_back(sig.note); }
        if (sig.kind == SignalKind::WideFM && bandSuggests(sig.freqHz, "wfm")) {
            m.buttons.push_back({ACT_LISTEN_WFM, "Listen (wide FM)", true, true});
        } else if (bandSuggests(sig.freqHz, "am") && !bandSuggests(sig.freqHz, "nfm") && !bandSuggests(sig.freqHz, "p25")) {
            m.buttons.push_back({ACT_LISTEN_AM, "Listen (AM)", true, true});
        } else if (narrow) {
            m.buttons.push_back({ACT_IDENTIFY, dsdPath_.empty() ? "Identify (decoder missing)" : "Identify digital signal", true, !dsdPath_.empty()});
            m.buttons.push_back({ACT_LISTEN_NFM, "Listen as FM", false, true});
        } else if (sig.kind == SignalKind::Channel25k) {
            m.lines.push_back("Wide digital modes (e.g. TETRA) are not decoded yet.");
            m.buttons.push_back({ACT_LISTEN_NFM, "Listen as FM", false, true});
        }
    }
    if (!(r && r->verdict.claimed)) {
        for (auto& b : helperButtonsFor(sig.freqHz)) { m.buttons.push_back(b); }
    }
    if (std::fabs(sig.freqHz - cursorFreq_) > 1500.0) {
        m.buttons.insert(m.buttons.begin(), {ACT_CENTER, "Center on signal", false, true});
    }
    m.buttons.push_back({ACT_DISMISS, "\xC3\x97", false, true});
    return m;
}
