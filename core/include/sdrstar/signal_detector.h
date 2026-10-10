// SPDX-License-Identifier: GPL-3.0-or-later
//
// SignalDetector: finds signals in the live waterfall spectrum.
//
// Input is the same dB spectrum SDR++ draws (one value per FFT bin across the whole capture window).
// For each frame we keep a time-averaged power spectrum, estimate the noise floor, and report contiguous
// regions that stand clear of it as "signals" with centre, occupied bandwidth, SNR and a flat-top-ness
// measure (flat = data-like, peaky = speech-like).  Signals are tracked across frames so the UI can show how
// long they have been there and how bursty they are, and so a later classification result can stay attached
// to the same signal.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdrstar {

enum class SignalKind {
    Unknown,
    Carrier,       // very narrow, steady (CW, pilot, beacon, DC leakage of something)
    Narrow,        // < ~6 kHz   (6.25 kHz digital, SSB, narrow data)
    Channel12k,    // 6-16 kHz   (12.5 kHz NFM voice or digital voice/data)
    Channel25k,    // 16-40 kHz  (25 kHz NFM, TETRA, wide data)
    Medium,        // 40-120 kHz
    WideFM,        // 120-300 kHz (broadcast FM style)
    Wideband,      // > 300 kHz   (TV, LTE, ADS-B burst energy ...)
};

const char* signalKindName(SignalKind k);

struct DetectedSignal {
    int id = 0;                 // stable while the signal is tracked
    double freqHz = 0;          // centre (power-weighted)
    double bwHz = 0;            // occupied bandwidth
    double peakDb = 0;
    double floorDb = 0;         // local noise floor
    double snrDb = 0;           // peak above floor
    double flatTopDb = 0;       // mean power over the occupied region minus peak (informational only: measured to NOT separate
                                // digital from analog on real recordings, so no label is derived from it)
    SignalKind kind = SignalKind::Unknown;
    double firstSeenSec = 0;
    double lastSeenSec = 0;
    int hits = 0;               // detections so far
    double activity = 0;        // 0..1 share of recent scans in which it was present (burstiness)
    bool steady = false;        // continuously present for several seconds (control channels, data carriers, broadcast)
    std::string note;           // longer explanation for a tooltip
    bool present = true;        // seen in the latest scan
    std::string hint;           // band-plan hint ("Broadcast FM", "800 MHz public safety ...")
    std::string label;          // short UI label ("FM 200k", "12.5k data-like", ...)
    // Set by the classifier once something was learned about this signal; survives tracking.
    std::string identified;     // "P25 control channel", "DMR", ... ("" = not identified yet)
    std::string identifiedDetail;
};

struct SpectrumFrame {
    const float* db = nullptr;   // dB per bin, low frequency first
    int bins = 0;
    double centerHz = 0;
    double spanHz = 0;           // total span covered by `bins`
};

struct DetectorParams {
    double marginDb = 8.0;        // threshold above the local floor
    double minBwHz = 1500.0;      // ignore anything narrower
    double mergeGapHz = 2500.0;   // gaps this small inside a signal do not split it
    double smoothHz = 1000.0;     // spectral smoothing width
    double avgAlpha = 0.35;       // power-domain EMA weight of the newest frame (0..1)
    double dcIgnoreHz = 4000.0;   // narrow spikes this close to the centre are DC leakage, not signals
    double forgetAfterSec = 6.0;  // drop tracked signals not seen for this long
    int activityWindow = 40;      // scans used for the activity ratio
};

class SignalDetector {
public:
    explicit SignalDetector(const DetectorParams& p = DetectorParams()) : p_(p) {}

    void setParams(const DetectorParams& p) { p_ = p; }
    const DetectorParams& params() const { return p_; }

    // Feed one spectrum frame.  `nowSec` is any monotonically increasing clock.
    void update(const SpectrumFrame& f, double nowSec);

    // Current tracked signals, sorted by frequency (includes ones not seen in the latest scan until forgotten).
    const std::vector<DetectedSignal>& signals() const { return signals_; }
    double noiseFloorDb() const { return globalFloorDb_; }
    void reset();

    // Attach a classification result to the signal nearest `freqHz` (within its bandwidth).  Returns false if none.
    bool setIdentified(double freqHz, const std::string& what, const std::string& detail);

    // The tracked signal containing (or nearest to) freqHz within `tolHz`, or nullptr.
    const DetectedSignal* find(double freqHz, double tolHz) const;

private:
    struct Track {
        DetectedSignal s;
        std::vector<uint8_t> history;  // ring of present flags
        size_t histPos = 0;
        int lastScan = 0;
    };

    DetectorParams p_;
    std::vector<float> avgLin_;      // time-averaged linear power per bin
    double lastCenter_ = 0, lastSpan_ = 0;
    int lastBins_ = 0;
    double globalFloorDb_ = -120;
    std::vector<Track> tracks_;
    std::vector<DetectedSignal> signals_;
    int nextId_ = 1;
    int scan_ = 0;
};

// Classification from spectrum shape + band plan only (no IQ).  Fills kind, hint and label.
void classifyFromSpectrum(DetectedSignal& s);

}  // namespace sdrstar
