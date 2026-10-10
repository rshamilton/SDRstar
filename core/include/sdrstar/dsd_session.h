// SPDX-License-Identifier: GPL-3.0-or-later
//
// DsdSession: runs one DSD-neo decoder as a child process.
//
//   SDRstar DSP thread --(48 kHz FM-discriminator PCM, s16le)--> dsd-neo stdin
//   dsd-neo stdout     --(decoded voice, raw s16le)------------> onAudio()
//   dsd-neo JSON API   --(NDJSON over loopback TCP)------------> state()
//   dsd-neo rigctl     --(hamlib text over loopback TCP)------> RigctlBridge (trunk following only)
//
// DSD-neo does all the protocol work (P25 P1/P2, DMR, NXDN, D-STAR, YSF, dPMR, EDACS, ProVoice, M17 ...).
// This class only supervises it and turns its telemetry into a plain struct the UI can draw.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sdrstar/subprocess.h"

namespace sdrstar {

struct DsdNeighbor {
    double freqHz = 0;
    long long wacn = -1;
    long long sysid = -1;
    int rfss = -1;
    int site = -1;
    bool currentCc = false;
};

struct DsdCall {
    int slot = 0;
    int state = 0;  // 1 idle, 2 active, 3 ended (DSD_APP_CALL_LINE_*)
    std::string name;
    std::string tgText;
    std::string srcText;
    long long tgId = 0;
    long long srcId = 0;
    bool enc = false;
    bool emergency = false;
    double freqHz = 0;
    int elapsedMs = 0;
};

struct DsdQuality {
    bool valid = false;
    long long ccOk = 0, ccErr = 0;
    double ccOkPct = 0;
    long long voiceOk = 0, voiceErr = 0;
};

struct DsdEvent {
    double timeUnix = 0;
    std::string sysid;   // "P25_293", "DMR_CC_02" ...
    std::string text;
    long long srcId = 0, tgId = 0;
    double freqHz = 0;
    int systype = 0;
    int subtype = 0;
};

struct DsdIdentity {
    // Parsed from decoder log lines; -1 / empty when not (yet) seen.
    long long nac = -1;
    long long wacn = -1;
    long long sysid = -1;
    int rfss = -1;
    int site = -1;
    int colorCode = -1;
    int ran = -1;
};

struct DsdState {
    bool processRunning = false;
    bool apiConnected = false;
    std::string protocol;        // "P25p1", "P25p2", "DMR", "NXDN", "dPMR", "D-STAR", "YSF", ...
    double ccFreqHz = 0;         // control channel the decoder believes it is on / announced
    double vcFreqHz = 0;
    bool trunking = false;
    bool trunkTuned = false;     // decoder is currently parked on a voice channel
    std::vector<DsdNeighbor> neighbors;
    std::vector<DsdCall> calls;
    DsdQuality quality;
    DsdIdentity identity;
    double snrDb = -999;
    std::string notice;          // decoder's current status message ("" = none)
    long long syncLines = 0;     // number of "Sync: +PROTO" log lines seen (evidence counter)
    long long frameLines = 0;    // decoded-frame log lines (TSBK / voice frames ...)
    std::string lastSync;        // protocol token of the last Sync line
    std::deque<DsdEvent> events; // most recent events (bounded)
    long long pcmBytesIn = 0;
    long long pcmBytesDropped = 0;
    long long audioBytesOut = 0;
    std::string lastError;
};

class DsdSession {
public:
    struct Config {
        std::string exePath;              // absolute path to dsd-neo
        std::vector<std::string> mode;    // decode preset flags, e.g. {"-f1"}, {"-ft"}, {"-fs"}, {"-fa"}
        std::vector<std::string> extra;   // extra arguments (channel map, group list ...)
        int inputRate = 48000;            // PCM sample rate pushed with pushPcm()
        bool trunking = false;            // pass -T
        int rigctlPort = 0;               // pass -U <port> when > 0
        bool wantAudio = false;           // true: -o - (decoded voice on stdout); false: -o null
        std::string workDir;              // cwd and XDG dirs for the child ("" = system temp)
        std::string label;                // for logs
    };

    using AudioCb = std::function<void(const int16_t* interleaved, size_t frames, int channels, int rate)>;
    using LogCb = std::function<void(const std::string&)>;

    DsdSession() = default;
    ~DsdSession() { stop(); }
    DsdSession(const DsdSession&) = delete;
    DsdSession& operator=(const DsdSession&) = delete;

    void setAudioCallback(AudioCb cb) { audioCb_ = std::move(cb); }
    void setLogCallback(LogCb cb) { logCb_ = std::move(cb); }

    bool start(const Config& cfg, std::string& error);
    void stop();
    bool running() const { return proc_ && proc_->running(); }

    // Thread-safe; never blocks.  `samples` are mono s16 at cfg.inputRate.
    void pushPcm(const int16_t* samples, size_t count);

    DsdState state() const;
    std::vector<std::string> recentLog(size_t maxLines = 200) const;

    // Exposed for unit tests: fold one NDJSON telemetry line / one stderr line into the state.
    void ingestApiLine(const std::string& line);
    void ingestLogLine(const std::string& line);

    const Config& config() const { return cfg_; }
    int apiPort() const { return apiPort_; }

private:
    void apiLoop();
    bool apiSend(const std::string& s);

    Config cfg_;
    std::unique_ptr<Subprocess> proc_;
    AudioCb audioCb_;
    LogCb logCb_;

    mutable std::mutex mtx_;
    DsdState st_;
    std::deque<std::string> log_;

    std::mutex pcmMtx_;
    std::vector<uint8_t> pcmPending_;

    int apiPort_ = 0;
    std::string token_;
    std::string tokenDir_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> apiFd_{-1};
    std::thread apiThread_;
    uint8_t audioCarry_[4] = {0, 0, 0, 0};
    size_t audioCarryLen_ = 0;
    int audioChannels_ = 2;
};

// A decode preset the UI can offer, with the flags DSD-neo needs.
struct DsdPreset {
    const char* id;          // stable key
    const char* title;       // "P25 Phase 1"
    std::vector<std::string> flags;
    int symbolRate;          // for display/matching
};
const std::vector<DsdPreset>& dsdPresets();
const DsdPreset* findDsdPreset(const std::string& id);

}  // namespace sdrstar
