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

SmartEngine::~SmartEngine() {
    stop();
    joinStopper();
}

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
    def["labelOnlyIdentified"] = true;
    def["autoIdentify"] = true;
    def["allowHop"] = true;
    def["sensitivityDb"] = 10.0;
    def["channelMapPath"] = "";
    def["groupListPath"] = "";
    gCfg.setPath(root_ + "/sdrstar_smart_config.json");
    gCfg.load(def);
    gCfg.acquire();
    try {
        settings_.showLabels = gCfg.conf.value("showLabels", true);
        settings_.labelOnlyIdentified = gCfg.conf.value("labelOnlyIdentified", true);
        settings_.autoIdentify = gCfg.conf.value("autoIdentify", true);
        settings_.allowHop = gCfg.conf.value("allowHop", true);
        settings_.sensitivityDb = gCfg.conf.value("sensitivityDb", 10.0);
        settings_.channelMapPath = gCfg.conf.value("channelMapPath", std::string());
        settings_.groupListPath = gCfg.conf.value("groupListPath", std::string());
    } catch (const json::exception&) {
        // a hand-edited settings file with a wrong type for some entry: keep the defaults rather than crash at startup
        settings_ = EngineSettings();
    }
    gCfg.release();
}

void SmartEngine::saveSettings() {
    gCfg.acquire();
    gCfg.conf["showLabels"] = settings_.showLabels;
    gCfg.conf["labelOnlyIdentified"] = settings_.labelOnlyIdentified;
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
    joinStopper();
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

std::vector<DetectedSignal> SmartEngine::labelSignals() const {
    std::vector<DetectedSignal> all = signals();
    if (!settings_.labelOnlyIdentified) { return all; }
    all.erase(std::remove_if(all.begin(), all.end(), [](const DetectedSignal& s) { return s.identified.empty(); }), all.end());
    return all;
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
            abs = gui::waterfall.getCenterFrequency() + it->second->centerOffset;   // the true centre, whatever the VFO reference (LSB/USB/RAW)
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
        if (!s.present && s.identified.empty() && s.hits < 3) { continue; }   // bursty signals stay selectable between bursts
        const double d = std::fabs(s.freqHz - freqHz);
        if (d > std::max(reach, s.bwHz / 2.0)) { continue; }
        if (s.snrDb < settings_.sensitivityDb + 1.0) { continue; }
        // prefer close AND strong: distance in kHz minus a bonus for SNR
        const double score = d / 1000.0 - 0.15 * s.snrDb + 0.04 * s.bwHz / 1000.0;   // a narrow signal on the cursor beats a wide smear near it
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

// ------------------------------------------------------------------------------------------------------------
// periodic work
// ------------------------------------------------------------------------------------------------------------
void SmartEngine::tick() {
    if (!started_) { return; }
    const double now = nowSec();
    if (stopper_.joinable() && !stopperBusy_.load()) { stopper_.join(); }
    host_.reconcile();   // the user may have moved the receiver window: keep hidden channels on their frequency
    updateCursor(now);
    if (now - lastAdoptSec_ >= 1.0) {
        lastAdoptSec_ = now;
        adoptIdentities(now, signals());
    }
    trackCursorForJob(now);
    pollProbe(now);
    autoProbe(now);
    // A job listening on one fixed frequency is useless once the user moves the window away from it: say so and stop.
    if (job_ == JobKind::Probing || job_ == JobKind::Decoding || job_ == JobKind::Helper) {
        if (!sdrstar::channelFits(host_.caps(), host_.centerHz(), sdrstar::WindowChannel{jobFreq_, jobBw_, ""})) {
            if (job_ != JobKind::Probing || !jobAutomatic_) { addNote("Stopped: the receiver window was moved away from " + formatFrequency(jobFreq_) + "."); }
            stopJob();
        }
    }
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
    // The ledger decides what the signal is called: a different answer to the one already known has to be seen twice (or be
    // asked for by the user) before the label changes, so it does not flip between protocols.
    const bool manual = !jobAutomatic_;
    ledger_.offer(jobFreq_, jobBw_, p.best, now, manual);
    const IdentityEntry* e = ledger_.find(jobFreq_);
    {
        std::lock_guard<std::mutex> lk(detMtx_);
        if (e && e->verdict.claimed) {
            det_.setIdentified(e->freqHz, e->verdict.title, e->verdict.detail);
        } else if (manual) {
            det_.setIdentified(jobFreq_, "", "");   // the user re-checked and nothing was recognised: drop the old label
        }
    }
    if (p.best.claimed) {
        noClaimCount_.erase(keyFor(jobFreq_));
        noClaimUntil_.erase(keyFor(jobFreq_));
    } else if (!(e && e->verdict.claimed)) {
        // Back off: 1.5 min, 4.5 min, 13 min, then once an hour.  An analog channel that is always on would otherwise
        // be re-tested (several decoder processes, a few seconds of CPU) forever.  The user can always ask again.
        const int n = ++noClaimCount_[keyFor(jobFreq_)];
        const double wait = std::min(3600.0, 90.0 * std::pow(3.0, n - 1));
        noClaimUntil_[keyFor(jobFreq_)] = now + wait;
    }
    job_ = JobKind::None;
}

// A channel that was identified keeps its identity: when the waterfall starts tracking it afresh (a voice channel keys up
// and down) the remembered identity is put back instead of probing again, and the ledger is told which channels are still there.
void SmartEngine::adoptIdentities(double now, const std::vector<DetectedSignal>& sigs) {
    for (const auto& s : sigs) {
        const IdentityEntry* e = ledger_.find(s.freqHz);
        if (!e) { continue; }
        if (s.present) { ledger_.touch(s.freqHz, now); }
        if (s.identified.empty() && e->verdict.claimed) {
            std::lock_guard<std::mutex> lk(detMtx_);
            det_.setIdentified(s.freqHz, e->verdict.title, e->verdict.detail);
        }
    }
    ledger_.expire(now);
}

void SmartEngine::autoProbe(double now) {
    if (!settings_.autoIdentify || job_ != JobKind::None || dsdPath_.empty()) { return; }
    if (now - lastAutoScanSec_ < 1.0) { return; }
    lastAutoScanSec_ = now;
    if (now - lastAutoProbeSec_ < 4.0) { return; }
    std::vector<DetectedSignal> sigs = signals();
    // A frequency that has been empty for a while is a different story when something shows up there again.
    for (const auto& s : sigs) { bucketSeenSec_[keyFor(s.freqHz)] = now; }
    for (auto it = bucketSeenSec_.begin(); it != bucketSeenSec_.end();) {
        if (now - it->second > 120.0) {
            noClaimUntil_.erase(it->first);
            noClaimCount_.erase(it->first);
            it = bucketSeenSec_.erase(it);
        } else {
            ++it;
        }
    }
    const DetectedSignal* best = nullptr;
    for (const auto& s : sigs) {
        if (!s.present || s.hits < 8 || s.snrDb < 14.0) { continue; }
        if (s.kind != SignalKind::Channel12k && s.kind != SignalKind::Narrow) { continue; }
        if (bandSuggests(s.freqHz, "wfm")) { continue; }
        if (!s.identified.empty()) {
            // Already identified: only look again now and then (or to settle a disagreement), so a real change is noticed.
            if (!ledger_.verificationDue(s.freqHz, now)) { continue; }
        } else {
            auto cd = noClaimUntil_.find(keyFor(s.freqHz));
            if (cd != noClaimUntil_.end() && cd->second > now) { continue; }
        }
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
    if (stopperBusy_.load()) { return; }
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
    jobBw_ = c.channelBwHz;
    lastProbe_ = ProbeProgress();
    lastProbe_.active = true;
    lastProbe_.freqHz = freqHz;
}

void SmartEngine::identifyAtCursor() {
    if (!cursorValid_) { return; }
    double f = snapFrequency(cursorFreq_, cursorBw_);
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

void SmartEngine::joinStopper() {
    if (stopper_.joinable()) { stopper_.join(); }
}

void SmartEngine::stopFollowingAsync() {
    joinStopper();   // a previous stop has long finished; this only matters if two stops come back to back
    stopperBusy_ = true;
    stopper_ = std::thread([this] {
        follower_.stop();
        stopperBusy_ = false;
    });
}

void SmartEngine::stopJob() {
    switch (job_) {
    case JobKind::Probing: probe_.cancel(); break;
    case JobKind::Following: stopFollowingAsync(); break;
    case JobKind::Decoding: decoder_.stop(); break;
    case JobKind::Helper: helper_.stop(); break;
    default: break;
    }
    job_ = JobKind::None;
    pinVfo_.clear();
    helperBlockedHz_ = 0;
    if (radioMuted_) { setRadioMuted(false); }
    voice_.end();   // closes the voice output device (nothing happens if it was never opened)
}

// ------------------------------------------------------------------------------------------------------------
// the cursor while a job runs
// ------------------------------------------------------------------------------------------------------------
// Put the selected VFO's CENTRE on a frequency.  tuner::tune() takes the frequency of the VFO's reference point (the
// centre for FM / AM, an edge for USB / LSB / RAW), so the difference is added.  It also scrolls or retunes if the VFO
// would leave the view.
bool SmartEngine::tuneCursorTo(double centerHz) {
    std::string name = (!pinVfo_.empty() && gui::waterfall.vfos.count(pinVfo_)) ? pinVfo_ : cursorVfoName_;
    auto it = gui::waterfall.vfos.find(name);
    if (name.empty() || it == gui::waterfall.vfos.end()) { return false; }
    const ImGui::WaterfallVFO* vfo = it->second;
    tuner::tune(tuner::TUNER_MODE_NORMAL, name, centerHz + (vfo->generalOffset - vfo->centerOffset));
    updateCursor(nowSec());
    return true;
}

// The cursor goes to the channel being listened to and the job is bound to it from now on.
bool SmartEngine::moveCursorTo(double centerHz) {
    const bool ok = tuneCursorTo(centerHz);
    if (ok) {
        pinHz_ = centerHz;
        pinVfo_ = cursorVfoName_;
    }
    return ok;
}

void SmartEngine::pinCursor(double hz, double bwHz) {
    pinVfo_ = cursorVfoName_;
    pinHz_ = hz;
    pinTolHz_ = std::clamp(0.5 * bwHz, 2500.0, 6250.0);   // moving the cursor off the channel's own width counts as clicking away
    lastCenterHz_ = host_.centerHz();
    settleUntil_ = 0;
}

bool SmartEngine::cursorLeftPin() const {
    if (!cursorValid_ || pinVfo_.empty()) { return false; }
    return cursorVfoName_ != pinVfo_ || std::fabs(cursorFreq_ - pinHz_) > pinTolHz_;
}

// "Click off" while following: stop.  If that happens during a call (the receiver is on a voice channel, perhaps in another
// part of the spectrum), first go back to the control channel that was originally clicked, cursor included, and then stop.
void SmartEngine::clickOffFollowing(const FollowStatus& st) {
    const bool onVoice = st.currentHz > 0 && std::fabs(st.currentHz - st.controlHz) > 700.0;
    const double control = st.controlHz;
    if (onVoice) {
        follower_.returnToControl();   // never waits; if a retune is in progress right now we just stop
        moveCursorTo(control);
        addNote("Stopped following: went back to the control channel " + formatFrequency(control) + ".");
    } else {
        addNote("Stopped following: the cursor was moved off the control channel.");
    }
    stopJob();
}

// A text decoder listens wherever the cursor is: when the cursor moves the decoder moves with it.
void SmartEngine::retargetHelper(double freqHz) {
    if (std::fabs(freqHz - helperBlockedHz_) < 400.0) { return; }   // already reported: do not try (and complain) every frame
    const sdrstar::HelperSpec& sp = helper_.spec();
    if (!sdrstar::channelFits(host_.caps(), host_.centerHz(), sdrstar::WindowChannel{freqHz, sp.channelBwHz, ""})) {
        helperBlockedHz_ = freqHz;
        addNote("The cursor is too close to the edge of the receiver window for the " + jobTitle_ + " decoder; it keeps listening at " +
                formatFrequency(jobFreq_) + ".");
        return;
    }
    std::string err;
    if (!helper_.retarget(freqHz, err)) {
        helperBlockedHz_ = freqHz;
        addNote("Could not move the " + jobTitle_ + " decoder to " + formatFrequency(freqHz) + ": " + err);
        return;
    }
    helperBlockedHz_ = 0;
    jobFreq_ = freqHz;
}

void SmartEngine::trackCursorForJob(double now) {
    const bool voiceJob = job_ == JobKind::Following || job_ == JobKind::Decoding;
    const bool textJob = job_ == JobKind::Helper && helper_.spec().fixedFreqHz <= 0;
    if (!voiceJob && !textJob) { return; }
    if (!cursorValid_) { return; }
    // A hardware retune can drag the cursor along with the window; that is not the user clicking.  Let it settle first.
    const double center = host_.centerHz();
    if (std::fabs(center - lastCenterHz_) > 1.0) {
        lastCenterHz_ = center;
        settleUntil_ = now + 0.8;
    }
    if (now < settleUntil_) { return; }

    if (textJob) {
        if (std::fabs(cursorFreq_ - jobFreq_) > 400.0 && now - cursorSince_ > 0.35) { retargetHelper(cursorFreq_); }
        return;
    }
    if (job_ == JobKind::Decoding) {
        if (cursorLeftPin()) {
            addNote("Stopped decoding: the cursor was moved off " + formatFrequency(jobFreq_) + ".");
            stopJob();
        }
        return;
    }
    // Following: the cursor sits where the receiver is listening.
    const FollowStatus st = follower_.status();
    if (st.state == FollowState::Paused || st.state == FollowState::Failed || st.state == FollowState::Stopped) { return; }
    if (st.currentHz > 0 && std::fabs(st.currentHz - pinHz_) > 700.0) {
        moveCursorTo(st.currentHz);   // the follower went to a voice channel (or back to the control channel): so does the cursor
        return;
    }
    if (cursorLeftPin()) { clickOffFollowing(st); }
}

void SmartEngine::startFollow(const IdentityEntry& r) {
    if (dsdPath_.empty()) { return; }
    if (stopperBusy_.load()) {
        addNote("Still stopping the previous trunk follow; try again in a moment.");
        return;
    }
    const double freq = r.freqHz > 0 ? r.freqHz : snapFrequency(cursorFreq_, cursorBw_);
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
    std::error_code fec;
    if (!settings_.channelMapPath.empty() && !std::filesystem::exists(settings_.channelMapPath, fec)) {
        addNote("The channel list file was not found: " + settings_.channelMapPath + ". Fix the path in the SDRstar panel (or clear it) and try again.");
        return;
    }
    if (!settings_.groupListPath.empty() && !std::filesystem::exists(settings_.groupListPath, fec)) {
        addNote("The talkgroup list file was not found: " + settings_.groupListPath + ". Fix the path in the SDRstar panel (or clear it) and try again.");
        return;
    }
    if (!settings_.channelMapPath.empty()) { c.extraArgs.insert(c.extraArgs.end(), {"-C", settings_.channelMapPath}); }
    if (!settings_.groupListPath.empty()) { c.extraArgs.insert(c.extraArgs.end(), {"-G", settings_.groupListPath}); }
    follower_.setAudioCallback([this](const int16_t* d, size_t n, int ch, int rate) { voice_.push(d, n, ch, rate); });
    moveCursorTo(freq);   // the cursor sits exactly on the control channel (the detected centre of the signal)
    std::string err;
    if (!follower_.start(c, err)) {
        addNote("Could not start following: " + err);
        pinVfo_.clear();
        return;
    }
    job_ = JobKind::Following;
    jobFreq_ = freq;
    jobTitle_ = r.verdict.title;
    pinCursor(freq, c.channelBwHz);
    setRadioMuted(true);
    voice_.begin();
}

void SmartEngine::startDecode(const IdentityEntry& r) {
    if (dsdPath_.empty()) { return; }
    if (stopperBusy_.load()) {
        addNote("Still stopping the previous trunk follow; try again in a moment.");
        return;
    }
    const double freq = r.freqHz > 0 ? r.freqHz : snapFrequency(cursorFreq_, cursorBw_);
    stopJob();
    ChannelDecoder::Config c;
    c.dsdExe = dsdPath_;
    c.workDir = root_ + "/sdrstar_work/decode";
    c.presetId = r.verdict.presetId;
    c.freqHz = freq;
    c.bwHz = (c.presetId == "nxdn48" || c.presetId == "dpmr") ? 8000 : 12500;
    decoder_.setAudioCallback([this](const int16_t* d, size_t n, int ch, int rate) { voice_.push(d, n, ch, rate); });
    moveCursorTo(freq);   // the cursor sits exactly on the channel being listened to
    std::string err;
    if (!decoder_.start(&host_, c, err)) {
        addNote("Could not start the decoder: " + err);
        pinVfo_.clear();
        return;
    }
    job_ = JobKind::Decoding;
    jobFreq_ = freq;
    jobBw_ = c.bwHz;
    jobTitle_ = r.verdict.title;
    pinCursor(freq, c.bwHz);
    setRadioMuted(true);
    voice_.begin();
}

bool SmartEngine::helperAvailable(const std::string& id) const {
    const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(id);
    if (!sp) { return false; }
    if (sp->native) { return true; }   // decoded inside SDRstar: no helper program needed
    return !(sp->exeName == "rtl_433" ? rtl433Path_ : multimonPath_).empty();
}

void SmartEngine::startHelper(const std::string& id, double freqHz) {
    const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(id);
    if (!sp) { return; }
    static const std::string kNoProgram;
    const std::string& exe = sp->native ? kNoProgram : (sp->exeName == "rtl_433" ? rtl433Path_ : multimonPath_);
    if (!sp->native && exe.empty()) {
        addNote("The " + sp->exeName + " helper was not found, so " + sp->title + " cannot be decoded.");
        return;
    }
    if (stopperBusy_.load()) {
        addNote("Still stopping the previous trunk follow; try again in a moment.");
        return;
    }
    stopJob();
    if (sp->fixedFreqHz > 0) { freqHz = sp->fixedFreqHz; }   // e.g. ADS-B is always at 1090 MHz, wherever the cursor is
    // Make sure the signal is inside the receiver window (a text decoder listens on a fixed channel). If it is not,
    // move the window there when the radio can retune, and say so.
    {
        const sdrstar::ReceiverCaps caps = host_.caps();
        const sdrstar::WindowChannel ch{freqHz, sp->channelBwHz, sp->title};
        if (!sdrstar::channelFits(caps, host_.centerHz(), ch)) {
            const sdrstar::Plan p = sdrstar::planWindow(caps, host_.centerHz(), {ch}, {});
            if (p.kind == sdrstar::PlanKind::Refuse) {
                addNote(p.summary + " " + p.detail);
                return;
            }
            if (p.hardwareRetune) {
                std::string e;
                if (!host_.setCenter(p.centerHz, e)) {
                    addNote("Could not move the receiver window to " + formatFrequency(p.centerHz) + ": " + e);
                    return;
                }
                addNote("Moved the receiver window to " + formatFrequency(p.centerHz) + " so " + formatFrequency(freqHz) + " is in view.");
            }
        }
    }
    std::string err;
    helper_.setWorkDir(root_ + "/sdrstar_work/helper-" + id);
    if (!helper_.start(&host_, *sp, exe, freqHz, err)) {
        addNote("Could not start " + sp->title + ": " + err);
        return;
    }
    job_ = JobKind::Helper;
    jobFreq_ = freqHz;
    jobBw_ = sp->channelBwHz;
    jobTitle_ = sp->title;
    helperBlockedHz_ = 0;
    lastCenterHz_ = host_.centerHz();
    settleUntil_ = 0;
}

void SmartEngine::doAction(CardAction a) {
    switch (a) {
    // Text decoders listen exactly where the user put the VFO (the detector can merge neighbouring bursts into one
    // wide "signal", so its centre is not a safe place to listen).
    case ACT_HELPER_PAGER: startHelper("pager", cursorFreq_); break;
    case ACT_HELPER_APRS: startHelper("aprs", cursorFreq_); break;
    case ACT_HELPER_EAS: startHelper("eas", cursorFreq_); break;
    case ACT_HELPER_ISM: startHelper("ism", cursorFreq_); break;
    case ACT_HELPER_ADSB: startHelper("adsb", 1090e6); break;
    case ACT_IDENTIFY: identifyAtCursor(); break;
    case ACT_FOLLOW:
        if (const IdentityEntry* r = ledger_.find(snapFrequency(cursorFreq_, cursorBw_))) {
            IdentityEntry copy = *r;
            if (copy.verdict.claimed) { startFollow(copy); }
        }
        break;
    case ACT_DECODE:
        if (const IdentityEntry* r = ledger_.find(snapFrequency(cursorFreq_, cursorBw_))) {
            IdentityEntry copy = *r;
            if (copy.verdict.claimed) { startDecode(copy); }
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
        if (cursorValid_ && !cursorVfoName_.empty() && nearestSignal(cursorFreq_, cursorBw_, sig)) { tuneCursorTo(sig.freqHz); }
        break;
    }
    case ACT_RETURN:
        if (job_ == JobKind::Following) { follower_.returnToControl(); }
        break;
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
    case JobKind::Helper: return jobTitle_ + " at " + formatFrequency(jobFreq_) + ": " + std::to_string(helper_.messageCount()) + " " + helper_.spec().countNoun;
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
        m.tone = st.state == FollowState::Paused ? CardTone::Attention
                 : !st.problem.empty()           ? CardTone::Problem
                 : (st.state == FollowState::Hopped ? CardTone::Attention : CardTone::Good);
        m.title = st.headline.empty() ? "Following trunked system" : st.headline;
        m.subtitle = st.system;
        if (!st.problem.empty()) { m.warning = st.problem; }
        if (!st.detail.empty()) { m.lines.push_back(st.detail); }
        char buf[160];
        std::snprintf(buf, sizeof buf, "Window moves: %d   Calls followed: %d   Hops: %d   Skipped: %d", st.windowRetunes, st.channelMoves + st.hops, st.hops, st.refusals);
        m.lines.push_back(buf);
        if (st.state == FollowState::Paused) { m.buttons.push_back({ACT_RETURN, "Return to control channel", true, true}); }
        {
            // DMR, NXDN and EDACS systems do not announce where their voice channels are, so SDRstar can only follow a call
            // onto another channel if it is given a channel list.  Calls that happen on THIS channel are played either way, so
            // this is a note about what a list adds, not a statement that nothing works without one.
            const std::string& pr = st.dsd.protocol;
            const bool needsList = pr.find("DMR") != std::string::npos || pr.find("NXDN") != std::string::npos || pr.find("EDACS") != std::string::npos ||
                                   pr.find("ProVoice") != std::string::npos;
            if (needsList && settings_.channelMapPath.empty()) {
                if (!st.call.empty() && st.state == FollowState::OnControl) {
                    m.lines.push_back("Hearing a call on this channel. To follow calls onto the system's other channels too, add a channel list (SDRstar panel, \"Channel list CSV\").");
                } else {
                    m.lines.push_back("Calls on this channel are played. To follow calls onto the system's other channels too, add a channel list (SDRstar panel, \"Channel list CSV\"): this kind of system does not announce them.");
                }
            }
        }
        m.lines.push_back("The cursor stays on the channel being heard. Move it off to stop.");
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
        m.lines.push_back("Move the cursor off this channel to stop.");
        m.buttons.push_back({ACT_STOP, "Stop decoding", true, true});
        return m;
    }
    if (job_ == JobKind::Helper) {
        m.visible = true;
        m.pinned = true;
        m.freqHz = jobFreq_;
        m.tone = CardTone::Good;
        m.title = jobTitle_;
        m.subtitle = mhz(jobFreq_) + "   \xC2\xB7   " + std::to_string(helper_.messageCount()) + " " + helper_.spec().countNoun;
        auto ls = helper_.lines(2);   // the card stays small (the Stop button must stay on screen); the panel lists them all
        if (ls.empty()) {
            const sdrstar::HelperSpec* sp = sdrstar::findHelperSpec(helper_.spec().id);
            m.lines.push_back(sp ? sp->idleHint : std::string("Waiting..."));
        }
        for (const auto& l : ls) {
            std::string t = l.text;
            if (t.size() > 54) { t = t.substr(0, 51) + "..."; }
            if (l.repeat > 1) { t += "  (x" + std::to_string(l.repeat) + ")"; }
            m.lines.push_back(t);
        }
        if (helper_.spec().fixedFreqHz <= 0) {
            // The decoder follows the cursor.  Say whether there is a signal under it: these signals come and go, and a
            // cursor that is a few kHz beside a pager channel hears nothing.
            DetectedSignal sig;
            if (nearestSignal(jobFreq_, 12500.0, sig)) {
                char b[128];
                std::snprintf(b, sizeof b, "Signal at the cursor: %d dB above the noise.", (int)std::lround(sig.snrDb));
                m.lines.push_back(b);
                if (std::fabs(sig.freqHz - jobFreq_) > 1500.0 && sig.bwHz < 16000.0) { m.buttons.push_back({ACT_CENTER, "Center on signal", false, true}); }
            } else {
                m.lines.push_back("No signal above the noise at the cursor right now.");
            }
            m.lines.push_back("The decoder follows the cursor: move it to listen on another frequency.");
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
    add("adsb", "adsb", ACT_HELPER_ADSB, "Read aircraft (ADS-B)");
    return v;
}

CardModel SmartEngine::buildCursorCard() {
    CardModel m;
    // --- otherwise: a card for the signal under the cursor, once the cursor has rested on it ---
    if (!cursorValid_ || dismissed_) { return m; }
    if (nowSec() - cursorSince_ < 0.45) { return m; }
    DetectedSignal sig;
    if (!nearestSignal(cursorFreq_, cursorBw_, sig)) {
        // Nothing is above the noise here right now.  Text signals (pagers, sensors, APRS) only transmit now and then, so a
        // cursor resting in a band that carries them still gets the decoder buttons: waiting on a quiet moment is the point.
        auto btns = helperButtonsFor(cursorFreq_);
        if (btns.empty() || nowSec() - cursorSince_ < 2.0) { return m; }   // a longer rest than for a signal: do not pester someone tuning around
        m.visible = true;
        m.freqHz = cursorFreq_;
        m.tone = CardTone::Info;
        m.title = "Nothing on the air here right now";
        const BandEntry* band = bestBandFor(cursorFreq_);
        m.subtitle = mhz(cursorFreq_) + (band ? std::string("  \xC2\xB7  ") + band->name : std::string());
        m.lines.push_back("Signals in this band (pagers, sensors, APRS) only transmit now and then. Start a decoder and leave the cursor here to catch the next one.");
        for (auto& b : btns) { m.buttons.push_back(b); }
        m.buttons.push_back({ACT_DISMISS, "\xC3\x97", false, true});
        return m;
    }
    m.visible = true;
    m.freqHz = cursorFreq_;
    const IdentityEntry* r = ledger_.find(sig.freqHz);
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
