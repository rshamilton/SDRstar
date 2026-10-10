// SPDX-License-Identifier: GPL-3.0-or-later
//
// SmartEngine: the brain of the SDRstar module.  Owns the spectrum tap + signal detector, the decoder-probe /
// trunk-follow / conventional-decode jobs, decoded-voice playback and the settings, and turns all of that into
// plain data (signals to label, a "card" to show next to the cursor, status text) for the UI layer.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sdrstar/channel_decoder.h>
#include <sdrstar/helper_decoder.h>
#include <sdrstar/probe_manager.h>
#include <sdrstar/signal_detector.h>
#include <sdrstar/trunk_follower.h>

#include "sdrpp_host.h"
#include "spectrum_tap.h"
#include "voice_out.h"

enum CardAction {
    ACT_NONE = 0,
    ACT_IDENTIFY,
    ACT_FOLLOW,
    ACT_DECODE,
    ACT_LISTEN_NFM,
    ACT_LISTEN_WFM,
    ACT_LISTEN_AM,
    ACT_STOP,
    ACT_DISMISS,
    ACT_TOGGLE_HOP,
    ACT_CENTER,
    ACT_HELPER_PAGER,
    ACT_HELPER_APRS,
    ACT_HELPER_EAS,
    ACT_HELPER_ISM,
    ACT_HELPER_ADSB,
    ACT_RETURN,   // put the receiver window back on the trunk's control channel
};

struct CardButton {
    CardAction action = ACT_NONE;
    std::string label;
    bool primary = false;
    bool enabled = true;
};

enum class CardTone { Info, Working, Good, Attention, Problem };

struct CardModel {
    bool visible = false;
    CardTone tone = CardTone::Info;
    std::string title;
    std::string subtitle;
    std::vector<std::string> lines;   // extra info lines (grey)
    std::string warning;              // red line ("" = none)
    float progress = -1.0f;           // 0..1, or < 0 for none
    std::vector<CardButton> buttons;
    double freqHz = 0;
    bool pinned = false;              // shown for an active job even if the cursor moved away
    bool compact = false;             // small unobtrusive chip (background activity), no buttons
};

struct EngineSettings {
    bool showLabels = true;
    bool autoIdentify = true;     // quietly identify steady narrowband signals as they appear
    bool allowHop = true;         // follow calls that need the receiver to leave the control channel
    double sensitivityDb = 10.0;  // detection margin above the noise floor
    std::string channelMapPath;   // CSV for DMR / NXDN / EDACS trunking
    std::string groupListPath;
};

enum class JobKind { None, Probing, Following, Decoding, Helper };

class SmartEngine {
public:
    SmartEngine();
    ~SmartEngine();

    // lifecycle (GUI thread)
    void start(const std::string& rootDir);
    void stop();

    // called every GUI frame (GUI thread)
    void tick();

    // data for the UI (GUI thread)
    std::vector<sdrstar::DetectedSignal> signals() const;
    CardModel card();                           // card for the cursor / active job
    JobKind jobKind() const { return job_; }
    sdrstar::FollowStatus followStatus() const { return follower_.status(); }
    sdrstar::ProbeProgress lastProbe() const { return lastProbe_; }
    std::string decoderStatusLine();
    bool decoderAvailable() const { return !dsdPath_.empty(); }
    const std::string& decoderPath() const { return dsdPath_; }
    double noiseFloorDb() const;
    std::string sourceSummary() const;

    // text decoders (pagers, APRS, weather alerts, ISM sensors)
    bool helperAvailable(const std::string& id) const;
    void startHelper(const std::string& id, double freqHz);
    std::vector<sdrstar::HelperLine> helperLines(size_t maxLines = 200) const { return helper_.lines(maxLines); }
    long long helperMessageCount() const { return helper_.messageCount(); }
    std::string helperTitle() const { return job_ == JobKind::Helper ? jobTitle_ : std::string(); }

    // user actions
    void doAction(CardAction a);
    void identifyAtCursor();
    EngineSettings& settings() { return settings_; }
    void settingsChanged();   // push settings into the detector / follower and save
    void dismissCard() { dismissedFreq_ = cursorFreq_; dismissed_ = true; }

    // cursor info
    bool cursorValid() const { return cursorValid_; }
    double cursorFreq() const { return cursorFreq_; }
    double cursorBw() const { return cursorBw_; }

private:
    struct StoredResult {
        sdrstar::ProbeVerdict verdict;
        double whenSec = 0;
        double bwHz = 0;
        double freqHz = 0;   // the exact frequency this result was established for (NOT whatever the cursor / another job is on now)
    };

    void onSpectrum(const float* db, int bins, double center, double span);
    void updateCursor(double now);
    void pollProbe(double now);
    void autoProbe(double now);
    void startProbe(double freqHz, double bwHz, bool automatic);
    CardModel buildCursorCard();
    std::vector<CardButton> helperButtonsFor(double freqHz) const;
    void startFollow(const StoredResult& r);
    void startDecode(const StoredResult& r);
    void stopJob();
    void setRadioMuted(bool muted);
    void setRadioMode(int mode, float bandwidth);
    std::string findHelper(const std::string& name) const;
    static long long keyFor(double freqHz) { return (long long)std::llround(freqHz / 6250.0); }
    const StoredResult* resultFor(double freqHz) const;
    double snapFrequency(double freqHz, double bwHz) const;
    bool nearestSignal(double freqHz, double bwHz, sdrstar::DetectedSignal& out) const;
    double nowSec() const;
    void loadSettings();
    void saveSettings();
    void addNote(const std::string& s);

    SdrppRadioHost host_;
    SpectrumTap tap_;
    VoiceOut voice_;
    mutable std::mutex detMtx_;
    sdrstar::SignalDetector det_;
    sdrstar::ProbeManager probe_;
    sdrstar::TrunkFollower follower_;
    sdrstar::ChannelDecoder decoder_;
    sdrstar::HelperDecoder helper_;

    EngineSettings settings_;
    std::string root_;
    std::string dsdPath_;
    std::string multimonPath_;
    std::string rtl433Path_;
    bool started_ = false;

    JobKind job_ = JobKind::None;
    bool jobAutomatic_ = false;
    double jobFreq_ = 0;
    double jobBw_ = 12500;   // bandwidth of the hidden channel the current job listens on
    std::string jobTitle_;
    sdrstar::ProbeProgress lastProbe_;
    std::map<long long, StoredResult> results_;
    std::map<long long, double> noClaimUntil_;   // auto-probe cooldown per frequency bucket
    std::map<long long, int> noClaimCount_;      // how many times in a row a bucket was probed without a claim (back-off)
    std::map<long long, double> bucketSeenSec_;  // last time a tracked signal was on this bucket (to forget stale cooldowns)
    double lastAutoProbeSec_ = 0;
    double lastAutoScanSec_ = 0;
    std::vector<std::string> notes_;

    // cursor (selected VFO) tracking
    bool cursorValid_ = false;
    double cursorFreq_ = 0, cursorBw_ = 0;
    double cursorSince_ = 0;
    bool dismissed_ = false;
    double dismissedFreq_ = 0;
    std::string cursorVfoName_;
    bool radioMuted_ = false;
    double startSec_ = 0;
};
