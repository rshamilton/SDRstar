// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/signal_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sdrstar/band_plan.h"
#include "sdrstar/window_planner.h"  // formatSpan / formatFrequency

namespace sdrstar {

const char* signalKindName(SignalKind k) {
    switch (k) {
    case SignalKind::Carrier: return "carrier";
    case SignalKind::Narrow: return "narrow";
    case SignalKind::Channel12k: return "12.5 kHz channel";
    case SignalKind::Channel25k: return "25 kHz channel";
    case SignalKind::Medium: return "medium-band";
    case SignalKind::WideFM: return "wide FM-size";
    case SignalKind::Wideband: return "wideband";
    default: return "unknown";
    }
}

namespace {

constexpr int kMinHitsToReport = 2;

inline float fastDbToLin(float db) { return std::exp2(db * 0.33219281f); }          // 10^(db/10)
inline float fastLinToDb(float lin) { return 3.0102999566f * std::log2(lin + 1e-30f); }  // 10*log10

double percentile(std::vector<float>& v, double q) {
    if (v.empty()) { return 0; }
    size_t k = (size_t)std::min<double>(v.size() - 1, std::max(0.0, q * (double)(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

}  // namespace

void SignalDetector::reset() {
    avgLin_.clear();
    tracks_.clear();
    signals_.clear();
    lastBins_ = 0;
    scan_ = 0;
}

const DetectedSignal* SignalDetector::find(double freqHz, double tolHz) const {
    const DetectedSignal* best = nullptr;
    double bestD = 1e18;
    for (const auto& s : signals_) {
        double d = std::fabs(s.freqHz - freqHz);
        double reach = std::max(tolHz, s.bwHz / 2.0);
        if (d <= reach && d < bestD) {
            best = &s;
            bestD = d;
        }
    }
    return best;
}

bool SignalDetector::setIdentified(double freqHz, const std::string& what, const std::string& detail) {
    Track* best = nullptr;
    double bestD = 1e18;
    for (auto& t : tracks_) {
        double d = std::fabs(t.s.freqHz - freqHz);
        if (d <= std::max(3000.0, t.s.bwHz / 2.0) && d < bestD) {
            best = &t;
            bestD = d;
        }
    }
    if (!best) { return false; }
    best->s.identified = what;
    best->s.identifiedDetail = detail;
    for (auto& s : signals_) {
        if (s.id == best->s.id) {
            s.identified = what;
            s.identifiedDetail = detail;
        }
    }
    return true;
}

void classifyFromSpectrum(DetectedSignal& s) {
    const double bw = s.bwHz;
    if (bw < 3000) {
        s.kind = (s.flatTopDb > -3.0 && bw < 1800) ? SignalKind::Carrier : SignalKind::Narrow;
    } else if (bw < 6000) {
        s.kind = SignalKind::Narrow;
    } else if (bw < 16000) {
        s.kind = SignalKind::Channel12k;
    } else if (bw < 40000) {
        s.kind = SignalKind::Channel25k;
    } else if (bw < 120000) {
        s.kind = SignalKind::Medium;
    } else if (bw < 300000) {
        s.kind = SignalKind::WideFM;
    } else {
        s.kind = SignalKind::Wideband;
    }

    const BandEntry* band = bestBandFor(s.freqHz);
    s.hint = band ? band->name : "";
    const bool trunkBand = bandSuggests(s.freqHz, "p25") || bandSuggests(s.freqHz, "dmr") || bandSuggests(s.freqHz, "edacs") ||
                           bandSuggests(s.freqHz, "smartnet");
    const bool narrowChannel = s.kind == SignalKind::Channel12k || s.kind == SignalKind::Narrow;

    char buf[96];
    s.note.clear();
    switch (s.kind) {
    case SignalKind::WideFM:
        s.label = bandSuggests(s.freqHz, "wfm") ? "FM broadcast" : "wide FM-size signal";
        break;
    case SignalKind::Wideband:
        if (bandSuggests(s.freqHz, "adsb")) {
            s.label = "ADS-B";
        } else {
            std::snprintf(buf, sizeof buf, "wideband %s", formatSpan(bw).c_str());
            s.label = buf;
        }
        break;
    case SignalKind::Carrier:
        s.label = "carrier";
        break;
    case SignalKind::Narrow:
        s.label = "narrow signal";
        break;
    case SignalKind::Channel12k:
        s.label = "12.5 kHz signal";
        if (bandSuggests(s.freqHz, "am") && !bandSuggests(s.freqHz, "nfm") && !trunkBand) { s.label = "AM voice"; }
        break;
    case SignalKind::Channel25k:
        s.label = "25 kHz signal";
        break;
    case SignalKind::Medium:
        std::snprintf(buf, sizeof buf, "%s signal", formatSpan(bw).c_str());
        s.label = buf;
        break;
    default:
        s.label = "signal";
    }
    // A steady narrow carrier in a trunking band is what a control channel looks like (voice channels key up and down).
    if (narrowChannel && s.steady && trunkBand) {
        s.label = "control channel?";
        s.note = "Steady narrow carrier in a band used for trunked radio: could be a trunking control channel. Rest the cursor on it to identify.";
    } else if (narrowChannel && s.steady) {
        s.note = "Steady narrow carrier (continuous): data/beacon style rather than push-to-talk voice. Rest the cursor on it to identify.";
    } else if (narrowChannel) {
        s.note = "Narrow signal that comes and goes: typical of push-to-talk voice. Rest the cursor on it to identify.";
    }
    if (!s.identified.empty()) {
        s.label = s.identified;
        if (!s.identifiedDetail.empty()) { s.note = s.identifiedDetail; }
    }
}

void SignalDetector::update(const SpectrumFrame& f, double nowSec) {
    if (!f.db || f.bins < 64 || f.spanHz <= 0) { return; }
    const int n = f.bins;
    const double binHz = f.spanHz / (double)n;

    // Geometry change (retune / FFT size change): restart the averaging, keep the tracks.
    if (n != lastBins_ || std::fabs(f.centerHz - lastCenter_) > 1.0 || std::fabs(f.spanHz - lastSpan_) > 1.0) {
        avgLin_.assign((size_t)n, 0.0f);
        lastBins_ = n;
        lastCenter_ = f.centerHz;
        lastSpan_ = f.spanHz;
        scan_ = 0;
    }
    scan_++;

    // 1) time-averaged power spectrum (linear domain)
    const float a = (scan_ == 1) ? 1.0f : (float)p_.avgAlpha;
    for (int i = 0; i < n; ++i) { avgLin_[i] = (1.0f - a) * avgLin_[i] + a * fastDbToLin(f.db[i]); }

    // 2) frequency smoothing (boxcar, linear domain) -> dB
    const int w = std::max(1, (int)std::lround(p_.smoothHz / binHz));
    std::vector<float> sm((size_t)n);
    {
        double run = 0;
        int cnt = 0;
        std::vector<double> prefix((size_t)n + 1, 0.0);
        for (int i = 0; i < n; ++i) { prefix[i + 1] = prefix[i] + avgLin_[i]; }
        for (int i = 0; i < n; ++i) {
            int lo = std::max(0, i - w / 2), hi = std::min(n - 1, i + w / 2);
            cnt = hi - lo + 1;
            run = prefix[hi + 1] - prefix[lo];
            sm[i] = fastLinToDb((float)(run / cnt));
        }
    }

    // 3) noise floor: global robust estimate + gentle local correction
    std::vector<float> dec;
    const int step = std::max(1, n / 4096);
    for (int i = 0; i < n; i += step) { dec.push_back(sm[i]); }
    const double globalFloor = percentile(dec, 0.20);
    globalFloorDb_ = globalFloor;
    const int block = std::max(64, (int)std::lround(150e3 / binHz));
    const int nBlocks = (n + block - 1) / block;
    std::vector<double> blockFloor((size_t)nBlocks), blockCenter((size_t)nBlocks);
    for (int b = 0; b < nBlocks; ++b) {
        int lo = b * block, hi = std::min(n, lo + block);
        std::vector<float> blk(sm.begin() + lo, sm.begin() + hi);
        double q = percentile(blk, 0.25);
        blockFloor[b] = std::clamp(q, globalFloor - 3.0, globalFloor + 6.0);
        blockCenter[b] = 0.5 * (lo + hi);
    }
    std::vector<float> floorDb((size_t)n);
    for (int i = 0; i < n; ++i) {
        // piecewise-linear interpolation between block centres
        int b = std::min(nBlocks - 1, i / block);
        double x = (double)i;
        double fl;
        if (x <= blockCenter[0]) {
            fl = blockFloor[0];
        } else if (x >= blockCenter[nBlocks - 1]) {
            fl = blockFloor[nBlocks - 1];
        } else {
            int b0 = (x >= blockCenter[b]) ? b : b - 1;
            if (b0 < 0) { b0 = 0; }
            if (b0 >= nBlocks - 1) { b0 = nBlocks - 2; }
            double t = (x - blockCenter[b0]) / (blockCenter[b0 + 1] - blockCenter[b0]);
            fl = blockFloor[b0] * (1 - t) + blockFloor[b0 + 1] * t;
        }
        floorDb[i] = (float)fl;
    }

    // 4) contiguous runs above floor + margin, merging small gaps
    struct Run { int a, b; };
    std::vector<Run> runs;
    const int gapBins = std::max(1, (int)std::lround(p_.mergeGapHz / binHz));
    {
        int start = -1, lastAbove = -1;
        for (int i = 0; i < n; ++i) {
            bool above = sm[i] > floorDb[i] + p_.marginDb;
            if (above) {
                if (start < 0) { start = i; }
                lastAbove = i;
            } else if (start >= 0 && i - lastAbove > gapBins) {
                runs.push_back({start, lastAbove});
                start = -1;
            }
        }
        if (start >= 0) { runs.push_back({start, lastAbove}); }
    }

    // 5) measure each run
    std::vector<DetectedSignal> found;
    for (const Run& r : runs) {
        int a0 = r.a, b0 = r.b;
        int pk = a0;
        for (int i = a0; i <= b0; ++i) {
            if (sm[i] > sm[pk]) { pk = i; }
        }
        const float peak = sm[pk];
        // occupied bandwidth: shrink to the -25 dB points of a strong signal
        while (a0 < pk && sm[a0] < peak - 25.0f) { a0++; }
        while (b0 > pk && sm[b0] < peak - 25.0f) { b0--; }
        const double bw = (double)(b0 - a0 + 1) * binHz;
        if (bw < p_.minBwHz) { continue; }

        double num = 0, den = 0;
        for (int i = a0; i <= b0; ++i) {
            double lin = (double)fastDbToLin(sm[i]) - (double)fastDbToLin(floorDb[i]);
            if (lin < 0) { lin = 0; }
            num += lin * (double)i;
            den += lin;
        }
        const double centroidBin = den > 0 ? num / den : 0.5 * (a0 + b0);
        const double freq = f.centerHz - f.spanHz / 2.0 + (centroidBin + 0.5) * binHz;

        // informational flatness: mean power over the occupied region relative to the peak
        double acc = 0;
        for (int i = a0; i <= b0; ++i) { acc += fastDbToLin(sm[i]); }
        const double centralDb = fastLinToDb((float)(acc / (double)(b0 - a0 + 1)));

        // DC leakage: narrow thing sitting at the centre of the capture
        if (std::fabs(freq - f.centerHz) < p_.dcIgnoreHz && bw < 4 * binHz + 3000.0) { continue; }

        DetectedSignal s;
        s.freqHz = freq;
        s.bwHz = bw;
        s.peakDb = peak;
        s.floorDb = floorDb[pk];
        s.snrDb = peak - floorDb[pk];
        s.flatTopDb = centralDb - peak;
        found.push_back(s);
    }

    // 6) associate with tracks
    std::vector<char> matched(tracks_.size(), 0);
    for (DetectedSignal& s : found) {
        int best = -1;
        double bestD = 1e18;
        for (size_t t = 0; t < tracks_.size(); ++t) {
            if (matched[t]) { continue; }
            const DetectedSignal& ts = tracks_[t].s;
            double d = std::fabs(ts.freqHz - s.freqHz);
            double reach = std::max(2500.0, 0.5 * std::min(ts.bwHz, s.bwHz));
            if (d <= reach && d < bestD) {
                best = (int)t;
                bestD = d;
            }
        }
        if (best >= 0) {
            matched[best] = 1;
            Track& tr = tracks_[best];
            const double k = 0.3;
            tr.s.freqHz = tr.s.freqHz * (1 - k) + s.freqHz * k;
            tr.s.bwHz = tr.s.bwHz * (1 - k) + s.bwHz * k;
            tr.s.peakDb = s.peakDb;
            tr.s.floorDb = s.floorDb;
            tr.s.snrDb = s.snrDb;
            tr.s.flatTopDb = tr.s.flatTopDb * (1 - k) + s.flatTopDb * k;
            tr.s.lastSeenSec = nowSec;
            tr.s.hits++;
        } else {
            Track tr;
            tr.s = s;
            tr.s.id = nextId_++;
            tr.s.firstSeenSec = nowSec;
            tr.s.lastSeenSec = nowSec;
            tr.s.hits = 1;
            tr.history.assign((size_t)std::max(4, p_.activityWindow), 0);
            tracks_.push_back(std::move(tr));
            matched.push_back(1);
        }
    }
    // Instantaneous presence of every tracked signal that lies inside the current window, measured on the RAW frame
    // (the averaged spectrum above decays too slowly to see push-to-talk bursts).
    {
        const double lowHz = f.centerHz - f.spanHz / 2.0;
        for (Track& tr : tracks_) {
            const double lo = tr.s.freqHz - tr.s.bwHz * 0.3, hi = tr.s.freqHz + tr.s.bwHz * 0.3;
            if (lo < lowHz || hi > lowHz + f.spanHz) { continue; }  // outside the window: no information this scan
            int ia = std::clamp((int)((lo - lowHz) / binHz), 0, n - 1);
            int ib = std::clamp((int)((hi - lowHz) / binHz), ia, n - 1);
            double acc = 0, flo = 0;
            for (int i = ia; i <= ib; ++i) {
                acc += fastDbToLin(f.db[i]);
                flo += floorDb[i];
            }
            const int cnt = ib - ia + 1;
            const double instSnr = fastLinToDb((float)(acc / cnt)) - flo / cnt;
            const bool nowPresent = instSnr > 0.55 * p_.marginDb;
            tr.s.present = nowPresent;
            if (nowPresent) { tr.s.lastSeenSec = nowSec; }
            tr.history[tr.histPos % tr.history.size()] = nowPresent ? 1 : 0;
            tr.histPos++;
        }
    }

    // 7) forget stale tracks (identified ones are remembered much longer: a voice channel keys up and down)
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const Track& t) {
                                     double keep = t.s.identified.empty() ? p_.forgetAfterSec : 60.0;
                                     return nowSec - t.s.lastSeenSec > keep;
                                 }),
                  tracks_.end());

    // 8) publish
    signals_.clear();
    for (Track& t : tracks_) {
        if (t.s.hits < kMinHitsToReport) { continue; }
        double sum = 0;
        size_t cnt = std::min(t.histPos, t.history.size());
        for (size_t i = 0; i < t.history.size(); ++i) { sum += t.history[i]; }
        t.s.activity = cnt ? sum / (double)cnt : 0.0;
        t.s.steady = t.s.activity >= 0.9 && cnt >= 15;
        classifyFromSpectrum(t.s);
        signals_.push_back(t.s);
    }
    std::sort(signals_.begin(), signals_.end(), [](const DetectedSignal& x, const DetectedSignal& y) { return x.freqHz < y.freqHz; });
}

}  // namespace sdrstar
