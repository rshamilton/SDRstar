// SPDX-License-Identifier: GPL-3.0-or-later
//
// TrunkFollower: follows a trunked radio system starting from its control channel.
//
//   1. a hidden channel is opened on the control channel; its FM-discriminator audio feeds DSD-neo;
//   2. DSD-neo decodes the control channel and, when a call is granted, asks (over rigctl) to retune to the
//      voice channel;
//   3. the RigctlBridge hands the request to the window planner, which decides: just move the software channel,
//      retune the hardware once so control + voice stay in view, hop (leave the control channel for the call), or
//      refuse (out of bandwidth / outside the tuner range) -- and says so in plain English;
//   4. decoded voice comes back from DSD-neo and is delivered to the audio callback.
//
// All user-facing text is produced here so the UI only has to display it.
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sdrstar/dsd_session.h"
#include "sdrstar/radio_host.h"
#include "sdrstar/rigctl_bridge.h"
#include "sdrstar/window_planner.h"

namespace sdrstar {

enum class FollowState {
    Stopped,
    Starting,
    OnControl,      // parked on the control channel, waiting for calls
    FollowingCall,  // on a voice channel (control channel may or may not still be in view)
    Hopped,         // on a voice channel OUTSIDE the window that holds the control channel: control is lost for now
    Paused,         // the USER moved the receiver window away from the channel being followed; resumes when it is back
    Failed,
};
const char* followStateName(FollowState s);

struct FollowEvent {
    enum Level { Info, Warn, Problem };
    double tSec = 0;      // seconds since start
    Level level = Info;
    std::string text;
};

struct FollowStatus {
    FollowState state = FollowState::Stopped;
    std::string headline;          // big line for the UI
    std::string detail;            // second line: why / what the planner decided
    std::string problem;           // latest "cannot do X" message ("" = none); shown in red
    std::string system;            // "P25 Phase 1  ·  NAC 0xD6  ·  WACN 92065  ·  System 0xD5  ·  Site 1"
    std::string call;              // "Talkgroup 10384  ·  from 41183"
    double controlHz = 0;
    double currentHz = 0;          // frequency the hidden channel is on right now
    double centerHz = 0;           // hardware centre
    bool controlVisible = false;   // control channel inside the capture window right now
    int windowRetunes = 0;         // hardware retunes performed
    int channelMoves = 0;          // follow moves that needed no hardware retune
    int hops = 0;                  // times the control channel was left for a call
    int refusals = 0;              // calls we could not follow
    std::vector<FollowEvent> log;  // most recent events, oldest first
    DsdState dsd;
};

class TrunkFollower {
public:
    struct Config {
        std::string dsdExe;
        std::string workDir;
        double controlHz = 0;
        double channelBwHz = 12500;
        std::vector<std::string> mode = {"-ft"};  // DSD-neo preset flags (P25 helper by default)
        std::vector<std::string> extraArgs;       // channel map / group list for DMR/NXDN/EDACS
        bool allowHop = true;                     // follow calls that are out of the control channel's window
        bool voiceAudio = true;
        int settleMs = 120;                       // PCM held back after a hardware retune
    };

    explicit TrunkFollower(RadioHost* host) : host_(host) {}
    ~TrunkFollower() { stop(); }
    TrunkFollower(const TrunkFollower&) = delete;
    TrunkFollower& operator=(const TrunkFollower&) = delete;

    void setAudioCallback(DsdSession::AudioCb cb) { audioCb_ = std::move(cb); }

    bool start(const Config& cfg, std::string& error);
    void stop();
    bool running() const { return running_.load(); }

    void setAllowHop(bool allow) { allowHop_ = allow; }
    bool allowHop() const { return allowHop_.load(); }

    FollowStatus status() const;

    // Exposed for tests: process one retune request exactly as the rigctl bridge would.
    bool handleTuneRequest(double hz);
    // Put the window (and the follower) back on the control channel: the answer to a user who moved the receiver away.
    bool returnToControl();

private:
    void monitorLoop();
    void onIq(const cf* x, size_t n);
    void event(FollowEvent::Level lvl, const std::string& text);
    void refreshStatusText();
    void applyPlan(const Plan& plan, double targetHz, bool toControl);

    RadioHost* host_;
    Config cfg_;
    DsdSession::AudioCb audioCb_;
    std::unique_ptr<DsdSession> dsd_;
    RigctlBridge bridge_;
    int channel_ = -1;
    FmDiscriminator disc_{48000.0, 6250.0};   // touched only by the host's DSP thread (onIq)
    std::atomic<bool> discResetRequested_{false};   // other threads ask for a reset; onIq performs it
    std::vector<int16_t> pcm_;
    std::atomic<long long> muteUntilNs_{0};  // steady_clock ns; IQ before this instant is discarded
    std::chrono::steady_clock::time_point startTime_{};

    std::atomic<bool> running_{false};
    std::atomic<bool> allowHop_{true};
    std::atomic<bool> stopMonitor_{false};
    std::thread monitor_;

    mutable std::mutex mtx_;     // protects st_ and the tune decision state
    FollowStatus st_;
    double lastRefusedHz_ = 0;
    int lastRefusedCount_ = 0;
    std::chrono::steady_clock::time_point lastRefusedLog_{};
    std::mutex tuneMtx_;         // serialises tune requests
};

}  // namespace sdrstar
