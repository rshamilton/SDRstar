// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/trunk_follower.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sdrstar {

using clk = std::chrono::steady_clock;

const char* followStateName(FollowState s) {
    switch (s) {
    case FollowState::Stopped: return "stopped";
    case FollowState::Starting: return "starting";
    case FollowState::OnControl: return "on control channel";
    case FollowState::FollowingCall: return "following call";
    case FollowState::Hopped: return "hopped (control channel out of view)";
    case FollowState::Failed: return "failed";
    }
    return "?";
}

namespace {
constexpr size_t kMaxEvents = 120;
constexpr double kSameFreqHz = 700.0;  // two frequencies this close are "the same channel"

long long nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count(); }

std::string hexs(long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llX", v);
    return b;
}
}  // namespace

void TrunkFollower::event(FollowEvent::Level lvl, const std::string& text) {
    // caller holds mtx_ OR is single-threaded setup; use a dedicated lock-free path via mtx_ in public callers
    FollowEvent e;
    e.tSec = std::chrono::duration<double>(clk::now() - startTime_).count();
    e.level = lvl;
    e.text = text;
    st_.log.push_back(e);
    while (st_.log.size() > kMaxEvents) { st_.log.erase(st_.log.begin()); }
}

bool TrunkFollower::start(const Config& cfg, std::string& error) {
    if (running_.load()) {
        error = "already following a system";
        return false;
    }
    cfg_ = cfg;
    allowHop_ = cfg.allowHop;
    startTime_ = clk::now();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_ = FollowStatus();
        st_.state = FollowState::Starting;
        st_.controlHz = cfg.controlHz;
        st_.currentHz = cfg.controlHz;
        st_.headline = "Starting to follow the system at " + formatFrequency(cfg.controlHz);
        st_.detail = "Opening the control channel and starting the decoder.";
    }

    // Make sure the control channel is inside the capture window (recentre if the user dragged near the edge).
    ReceiverCaps caps = host_->caps();
    WindowChannel cc{cfg.controlHz, cfg.channelBwHz, "control channel"};
    Plan p = planWindow(caps, host_->centerHz(), {cc}, {});
    if (p.kind == PlanKind::Refuse) {
        error = p.summary;
        std::lock_guard<std::mutex> lk(mtx_);
        st_.state = FollowState::Failed;
        st_.problem = p.summary;
        st_.detail = p.detail;
        return false;
    }
    if (p.hardwareRetune) {
        std::string err;
        if (!host_->setCenter(p.centerHz, err)) {
            error = "could not move the receiver window: " + err;
            return false;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        st_.windowRetunes++;
        event(FollowEvent::Info, "Moved the receiver window to " + formatFrequency(p.centerHz) + " so the control channel is comfortably in view.");
    }

    std::string err;
    channel_ = host_->openChannel(cfg.controlHz, cfg.channelBwHz, [this](const cf* x, size_t n) { onIq(x, n); }, err);
    if (channel_ < 0) {
        error = "could not open the control channel: " + err;
        return false;
    }

    // Rigctl bridge: DSD-neo asks for retunes here.
    RigctlBridge::Handlers h;
    h.getFrequency = [this] {
        std::lock_guard<std::mutex> lk(mtx_);
        return st_.currentHz;
    };
    h.setFrequency = [this](double hz) { return handleTuneRequest(hz); };
    int port = 0;
    if (!bridge_.start(h, port, err)) {
        host_->closeChannel(channel_);
        channel_ = -1;
        error = "could not start the tuning bridge: " + err;
        return false;
    }

    dsd_ = std::make_unique<DsdSession>();
    if (audioCb_) { dsd_->setAudioCallback(audioCb_); }
    DsdSession::Config dc;
    dc.exePath = cfg.dsdExe;
    dc.mode = cfg.mode;
    dc.extra = cfg.extraArgs;
    dc.trunking = true;
    dc.rigctlPort = port;
    dc.wantAudio = cfg.voiceAudio;
    dc.workDir = cfg.workDir;
    dc.label = "trunk";
    if (!dsd_->start(dc, err)) {
        bridge_.stop();
        host_->closeChannel(channel_);
        channel_ = -1;
        dsd_.reset();
        error = "could not start the decoder: " + err;
        return false;
    }

    running_ = true;
    stopMonitor_ = false;
    monitor_ = std::thread([this] { monitorLoop(); });
    return true;
}

void TrunkFollower::stop() {
    if (!running_.exchange(false)) {
        if (monitor_.joinable()) { monitor_.join(); }
        return;
    }
    stopMonitor_ = true;
    if (monitor_.joinable()) { monitor_.join(); }
    if (dsd_) { dsd_->stop(); }
    bridge_.stop();
    if (channel_ >= 0) {
        host_->closeChannel(channel_);
        channel_ = -1;
    }
    dsd_.reset();
    std::lock_guard<std::mutex> lk(mtx_);
    st_.state = FollowState::Stopped;
    st_.headline = "Stopped";
    st_.detail = "";
}

FollowStatus TrunkFollower::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_;
}

void TrunkFollower::onIq(const cf* x, size_t n) {
    if (!running_.load() || !dsd_) { return; }
    if (nowNs() < muteUntilNs_.load()) { return; }  // tuner still settling after a retune: those samples are junk
    pcm_.resize(n);
    if (discResetRequested_.exchange(false)) { disc_.reset(); }
    for (size_t i = 0; i < n; ++i) { pcm_[i] = toS16(disc_.process(x[i])); }
    dsd_->pushPcm(pcm_.data(), n);
}

bool TrunkFollower::handleTuneRequest(double hz) {
    std::lock_guard<std::mutex> tl(tuneMtx_);
    if (!dsd_ || channel_ < 0) { return false; }

    double controlHz;
    double announcedCc = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        controlHz = st_.controlHz;
    }
    if (dsd_) { announcedCc = dsd_->state().ccFreqHz; }

    bool toControl = std::fabs(hz - controlHz) < kSameFreqHz;
    if (!toControl && announcedCc > 0 && std::fabs(hz - announcedCc) < kSameFreqHz) {
        toControl = true;
        std::lock_guard<std::mutex> lk(mtx_);
        st_.controlHz = hz;  // the system's own announcement wins over where the user clicked
        controlHz = hz;
    }

    const ReceiverCaps caps = host_->caps();
    WindowChannel cc{controlHz, cfg_.channelBwHz, "control channel"};
    WindowChannel vc{hz, cfg_.channelBwHz, "voice channel"};
    PlanOptions opt;
    opt.allowHop = allowHop_.load();
    opt.centreOnControl = false;
    Plan plan = toControl ? planWindow(caps, host_->centerHz(), {cc}, {}, opt) : planWindow(caps, host_->centerHz(), {cc}, {vc}, opt);

    if (plan.kind == PlanKind::Refuse) {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.refusals++;
        st_.problem = plan.summary;
        st_.detail = plan.detail;
        // Grants repeat every second or so: log a given refusal once, then count.
        const auto now = clk::now();
        if (std::fabs(lastRefusedHz_ - hz) > kSameFreqHz || now - lastRefusedLog_ > std::chrono::seconds(30)) {
            lastRefusedHz_ = hz;
            lastRefusedCount_ = 1;
            lastRefusedLog_ = now;
            event(FollowEvent::Problem, plan.summary);
        } else {
            lastRefusedCount_++;
        }
        return false;
    }

    applyPlan(plan, hz, toControl);
    return true;
}

void TrunkFollower::applyPlan(const Plan& plan, double targetHz, bool toControl) {
    bool retuned = false;
    if (plan.hardwareRetune) {
        muteUntilNs_ = nowNs() + 3600LL * 1000000000LL;  // mute until the new window is live
        std::string err;
        const bool ok = host_->setCenter(plan.centerHz, err);
        muteUntilNs_ = nowNs() + (long long)cfg_.settleMs * 1000000LL;
        if (!ok) {
            std::lock_guard<std::mutex> lk(mtx_);
            st_.problem = "The receiver could not retune to " + formatFrequency(plan.centerHz) + ": " + err;
            event(FollowEvent::Problem, st_.problem);
            return;
        }
        retuned = true;
    }
    if (!host_->moveChannel(channel_, targetHz)) {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.problem = "Internal error: " + formatFrequency(targetHz) + " is not inside the receiver window after planning.";
        event(FollowEvent::Problem, st_.problem);
        return;
    }
    discResetRequested_ = true;

    std::lock_guard<std::mutex> lk(mtx_);
    const double prev = st_.currentHz;
    const FollowState prevState = st_.state;
    st_.currentHz = targetHz;
    st_.centerHz = host_->centerHz();
    st_.problem.clear();
    if (retuned) { st_.windowRetunes++; } else if (std::fabs(prev - targetHz) > kSameFreqHz) { st_.channelMoves++; }
    if (toControl) {
        st_.state = FollowState::OnControl;
        st_.controlVisible = true;
    } else if (plan.kind == PlanKind::HopAway) {
        st_.state = FollowState::Hopped;
        st_.controlVisible = false;
        if (prevState != FollowState::Hopped) { st_.hops++; }
    } else {
        st_.state = FollowState::FollowingCall;
        st_.controlVisible = plan.controlStillVisible;
    }
    st_.detail = plan.detail;
    // log only real changes (the same grant is repeated many times)
    if (std::fabs(prev - targetHz) > kSameFreqHz || prevState != st_.state) {
        FollowEvent::Level lvl = plan.kind == PlanKind::HopAway ? FollowEvent::Warn : FollowEvent::Info;
        event(lvl, toControl ? ("Back on the control channel " + formatFrequency(targetHz) + (retuned ? " (window moved)." : "."))
                             : plan.summary);
    }
}

void TrunkFollower::monitorLoop() {
    bool announcedLock = false;
    while (!stopMonitor_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!dsd_) { continue; }
        DsdState d = dsd_->state();
        std::lock_guard<std::mutex> lk(mtx_);
        st_.dsd = d;
        st_.centerHz = host_->centerHz();

        // System identity line
        std::string sys;
        auto add = [&](const std::string& s) {
            if (!sys.empty()) { sys += "  \xC2\xB7  "; }
            sys += s;
        };
        if (!d.protocol.empty()) { add(d.protocol); }
        if (d.identity.nac >= 0) { add("NAC 0x" + hexs(d.identity.nac)); }
        if (d.identity.wacn >= 0) { add("WACN " + hexs(d.identity.wacn)); }
        if (d.identity.sysid >= 0) { add("System 0x" + hexs(d.identity.sysid)); }
        if (d.identity.site >= 0) { add("Site " + std::to_string(d.identity.site)); }
        if (d.identity.colorCode >= 0) { add("Color Code " + std::to_string(d.identity.colorCode)); }
        st_.system = sys;

        // Call line
        st_.call.clear();
        for (const auto& c : d.calls) {
            if (c.state == 2) {
                st_.call = (c.name.empty() ? "Talkgroup " + std::to_string(c.tgId) : c.name + " (TG " + std::to_string(c.tgId) + ")");
                if (c.srcId) { st_.call += "  \xC2\xB7  from " + std::to_string(c.srcId); }
                if (c.enc) { st_.call += "  \xC2\xB7  ENCRYPTED"; }
                break;
            }
        }

        if (!d.processRunning && running_.load()) {
            st_.state = FollowState::Failed;
            st_.problem = d.lastError.empty() ? "The decoder stopped unexpectedly." : "The decoder stopped: " + d.lastError;
            st_.headline = "Decoder stopped";
            continue;
        }

        const double sinceStart = std::chrono::duration<double>(clk::now() - startTime_).count();
        switch (st_.state) {
        case FollowState::Starting:
            if (d.syncLines >= 3 && !d.protocol.empty()) {
                st_.state = FollowState::OnControl;
                st_.controlVisible = true;
                if (!announcedLock) {
                    announcedLock = true;
                    event(FollowEvent::Info, "Locked to the control channel: " + sys);
                }
            } else if (sinceStart > 15.0) {
                st_.problem = "No digital control channel could be decoded at " + formatFrequency(st_.controlHz) +
                              ". Check that this really is a control channel, that the signal is strong enough, and the decoder preset.";
                st_.headline = "Not decoding yet";
            } else {
                st_.headline = "Listening for the control channel at " + formatFrequency(st_.controlHz);
            }
            break;
        case FollowState::OnControl:
            st_.headline = "Waiting for calls on the control channel " + formatFrequency(st_.controlHz);
            if (st_.problem.empty()) { st_.detail = sys.empty() ? "Decoder is locked." : sys; }
            break;
        case FollowState::FollowingCall:
            st_.headline = (st_.call.empty() ? "Listening to a call" : st_.call) + "  on  " + formatFrequency(st_.currentHz);
            break;
        case FollowState::Hopped:
            st_.headline = (st_.call.empty() ? "Listening to a call" : st_.call) + "  on  " + formatFrequency(st_.currentHz) +
                           "  (control channel out of view)";
            break;
        default:
            break;
        }
    }
}

}  // namespace sdrstar
