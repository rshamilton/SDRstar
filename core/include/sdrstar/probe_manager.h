// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProbeManager: identifies a digital signal by running DSD-neo with several *specific* decoder presets on it in
// parallel for a few seconds and weighing the evidence (see probe_evidence.h for why not the decoder's own
// auto-hunt).  One probe at a time; the decoder processes are loopback-only, receive PCM only, and are killed when
// the probe ends.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "sdrstar/channel_pcm.h"
#include "sdrstar/dsd_session.h"
#include "sdrstar/probe_evidence.h"

namespace sdrstar {

struct ProbeProgress {
    bool active = false;
    bool finished = false;
    double freqHz = 0;
    double elapsedSec = 0;
    double durationSec = 0;
    std::vector<ProbeVerdict> verdicts;  // latest evaluation of every preset
    ProbeVerdict best;                   // best claimed verdict so far (claimed=false if none)
    std::string status;                  // "Trying 8 digital modes ... 2.1 s"
    std::string error;
};

// Candidate presets for a signal of the given occupied bandwidth at the given frequency.
std::vector<std::string> candidatePresetsFor(double bwHz, double freqHz);

class ProbeManager {
public:
    struct Config {
        std::string dsdExe;
        std::string workDir;
        double durationSec = 5.0;
        double earlyExitConfidence = 0.9;   // stop early once a verdict is this strong...
        double earlyExitMinSec = 1.8;       // ...and at least this long has passed
        double channelBwHz = 12500;
        int niceLevel = 0;                  // > 0: run the decoder processes at a lower CPU priority (background probes)
    };

    ProbeManager() = default;
    ~ProbeManager() { cancel(); }

    bool start(RadioHost* host, double freqHz, const std::vector<std::string>& presetIds, const Config& cfg, std::string& error);
    // Poll from any thread; cheap.  Call repeatedly until finished.
    ProbeProgress poll();
    void cancel();
    bool active() const { return active_.load(); }

private:
    struct Entry {
        std::string presetId;
        std::unique_ptr<DsdSession> session;
    };
    void finish();

    // Re-reading every decoder's log and re-weighing the evidence is not free (several hundred lines per decoder), and the
    // GUI polls once per frame, so the evaluation is refreshed a few times a second only.
    static constexpr std::chrono::milliseconds kEvalInterval{200};
    std::vector<ProbeVerdict> evalVerdicts_;
    ProbeVerdict evalBest_;
    std::chrono::steady_clock::time_point evalAt_{};

    Config cfg_;
    std::unique_ptr<ChannelPcmTap> tap_;
    std::vector<Entry> entries_;
    std::atomic<bool> active_{false};
    bool finished_ = false;
    std::chrono::steady_clock::time_point t0_;
    ProbeProgress last_;
    double freqHz_ = 0;
};

}  // namespace sdrstar
