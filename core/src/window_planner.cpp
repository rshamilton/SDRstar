// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/window_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace sdrstar {

namespace {

constexpr double kEps = 1.0;  // 1 Hz slack when comparing frequencies

struct Interval {
    double lo;
    double hi;
    bool empty() const { return lo > hi; }
};

// Interval of window centres for which `ch` lies fully inside the usable window.
Interval centresFor(const ReceiverCaps& caps, const WindowChannel& ch) {
    const double half = caps.usableSpanHz() / 2.0;
    return {ch.freqHz + ch.bwHz / 2.0 - half, ch.freqHz - ch.bwHz / 2.0 + half};
}

Interval intersect(Interval a, Interval b) { return {std::max(a.lo, b.lo), std::min(a.hi, b.hi)}; }

bool nearDc(const ReceiverCaps& caps, double centerHz, const std::vector<const WindowChannel*>& chans) {
    for (const WindowChannel* c : chans) {
        if (std::fabs(c->freqHz - centerHz) < caps.dcGuardHz) { return true; }
    }
    return false;
}

// Choose a centre inside `feasible` that (1) respects the tuner range, (2) avoids the DC spike if
// possible and (3) leaves the most headroom to the window edges.  `prefer` biases towards a given
// frequency (used when the control channel should sit mid-window).
double pickCentre(const ReceiverCaps& caps, Interval feasible, const std::vector<const WindowChannel*>& chans,
                  bool haveBias, double biasHz) {
    feasible.lo = std::max(feasible.lo, caps.minHz);
    feasible.hi = std::min(feasible.hi, caps.maxHz);
    if (feasible.empty()) {
        // Tuner range clips the interval away; fall back to the nearest legal centre.
        return std::clamp(0.5 * (feasible.lo + feasible.hi), caps.minHz, caps.maxHz);
    }

    std::vector<double> cand;
    cand.push_back(0.5 * (feasible.lo + feasible.hi));
    cand.push_back(feasible.lo);
    cand.push_back(feasible.hi);
    if (haveBias) { cand.push_back(std::clamp(biasHz, feasible.lo, feasible.hi)); }
    for (const WindowChannel* c : chans) {
        // Just outside each channel's DC guard zone.
        cand.push_back(std::clamp(c->freqHz - caps.dcGuardHz - kEps, feasible.lo, feasible.hi));
        cand.push_back(std::clamp(c->freqHz + caps.dcGuardHz + kEps, feasible.lo, feasible.hi));
    }

    const double half = caps.usableSpanHz() / 2.0;
    double best = cand[0];
    double bestScore = -std::numeric_limits<double>::infinity();
    for (double c : cand) {
        double minSlack = std::numeric_limits<double>::infinity();
        for (const WindowChannel* ch : chans) {
            const double slack = half - (std::fabs(ch->freqHz - c) + ch->bwHz / 2.0);
            minSlack = std::min(minSlack, slack);
        }
        double score = minSlack;
        if (nearDc(caps, c, chans)) { score -= 1e9; }  // heavy but not infinite penalty
        if (haveBias) { score -= 0.25 * std::fabs(c - biasHz); }
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

std::string labelOr(const WindowChannel& c, const char* fallback) { return c.label.empty() ? fallback : c.label; }

}  // namespace

std::string formatFrequency(double hz) {
    char buf[64];
    const double a = std::fabs(hz);
    if (a >= 1e9) {
        std::snprintf(buf, sizeof buf, "%.4f", hz / 1e9);
        std::string s(buf);
        while (s.size() > 5 && s.back() == '0') { s.pop_back(); }
        return s + " GHz";
    }
    if (a >= 1e6) {
        std::snprintf(buf, sizeof buf, "%.4f", hz / 1e6);
        std::string s(buf);
        // keep at least three decimals
        while (s.size() > 3 && s.back() == '0' && s[s.size() - 4] != '.') { s.pop_back(); }
        return s + " MHz";
    }
    if (a >= 1e3) {
        std::snprintf(buf, sizeof buf, "%.3f kHz", hz / 1e3);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.0f Hz", hz);
    return buf;
}

std::string formatSpan(double hz) {
    char buf[64];
    if (hz >= 1e6) {
        std::snprintf(buf, sizeof buf, "%.2f", hz / 1e6);
        std::string s(buf);
        while (s.size() > 1 && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
        return s + " MHz";
    }
    std::snprintf(buf, sizeof buf, "%.0f kHz", hz / 1e3);
    return buf;
}

bool channelFits(const ReceiverCaps& caps, double centerHz, const WindowChannel& ch) {
    const double half = caps.usableSpanHz() / 2.0;
    return std::fabs(ch.freqHz - centerHz) + ch.bwHz / 2.0 <= half + kEps;
}

Plan planWindow(const ReceiverCaps& caps, double currentCenterHz, const std::vector<WindowChannel>& mustKeep,
                const std::vector<WindowChannel>& wanted, const PlanOptions& opt) {
    Plan p;
    p.usableSpanHz = caps.usableSpanHz();
    p.centerHz = currentCenterHz;

    std::vector<const WindowChannel*> all;
    for (const auto& c : mustKeep) { all.push_back(&c); }
    for (const auto& c : wanted) { all.push_back(&c); }

    if (all.empty() || caps.sampleRateHz <= 0.0) {
        p.kind = PlanKind::Refuse;
        p.refuse = RefuseReason::InvalidRequest;
        p.summary = "Nothing to plan.";
        p.detail = "No channels were given to the window planner.";
        return p;
    }

    // 1) Is everything inside what the hardware can tune at all?
    for (const WindowChannel* c : all) {
        if (c->freqHz < caps.minHz || c->freqHz > caps.maxHz) {
            p.kind = PlanKind::Refuse;
            p.refuse = RefuseReason::OutOfTunerRange;
            p.summary = "Cannot follow: " + formatFrequency(c->freqHz) + " is outside the " + caps.name + " tuning range.";
            p.detail = "The " + caps.name + " can tune from " + formatFrequency(caps.minHz) + " to " + formatFrequency(caps.maxHz) +
                       ". The " + labelOr(*c, "signal") + " is at " + formatFrequency(c->freqHz) + ", which this receiver cannot reach.";
            return p;
        }
    }

    // Span needed to hold everything.
    double lowEdge = std::numeric_limits<double>::infinity();
    double highEdge = -std::numeric_limits<double>::infinity();
    for (const WindowChannel* c : all) {
        lowEdge = std::min(lowEdge, c->freqHz - c->bwHz / 2.0);
        highEdge = std::max(highEdge, c->freqHz + c->bwHz / 2.0);
    }
    p.neededSpanHz = highEdge - lowEdge;

    auto controlVisibleAt = [&](double centre) {
        for (const auto& m : mustKeep) {
            if (!channelFits(caps, centre, m)) { return false; }
        }
        return !mustKeep.empty();
    };

    // 2) Does everything already fit where we are?
    bool allFit = true;
    for (const WindowChannel* c : all) { allFit = allFit && channelFits(caps, currentCenterHz, *c); }
    if (allFit) {
        p.kind = PlanKind::KeepWindow;
        p.centerHz = currentCenterHz;
        p.hardwareRetune = false;
        p.controlStillVisible = !mustKeep.empty();
        p.gapMs = 0.0;
        p.nearDcSpike = nearDc(caps, currentCenterHz, all);
        if (!wanted.empty()) {
            p.summary = "Following " + labelOr(wanted.front(), "channel") + " at " + formatFrequency(wanted.front().freqHz) +
                        (mustKeep.empty() ? "." : "; control channel still in view.");
        } else {
            p.summary = "Window unchanged.";
        }
        p.detail = "Everything needed fits in the " + formatSpan(p.usableSpanHz) + " usable window, so the receiver does not need to retune.";
        return p;
    }

    // 3) Can one window hold everything?
    Interval feas = {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    for (const WindowChannel* c : all) { feas = intersect(feas, centresFor(caps, *c)); }
    Interval clipped = {std::max(feas.lo, caps.minHz), std::min(feas.hi, caps.maxHz)};
    if (!feas.empty() && !clipped.empty()) {
        double bias = 0.0;
        bool haveBias = false;
        if (opt.centreOnControl && !mustKeep.empty()) {
            bias = mustKeep.front().freqHz;
            haveBias = true;
        }
        p.kind = PlanKind::ShiftWindow;
        p.centerHz = pickCentre(caps, feas, all, haveBias, bias);
        p.hardwareRetune = std::fabs(p.centerHz - currentCenterHz) > 0.5;
        p.controlStillVisible = !mustKeep.empty();
        p.gapMs = p.hardwareRetune ? caps.retuneMs : 0.0;
        p.nearDcSpike = nearDc(caps, p.centerHz, all);
        p.summary = "Moving the receiver window to " + formatFrequency(p.centerHz) + " to fit " +
                    (wanted.empty() ? "the channel" : labelOr(wanted.front(), "the channel") + " at " + formatFrequency(wanted.front().freqHz)) +
                    (mustKeep.empty() ? "." : " alongside the control channel.");
        p.detail = "The channels span " + formatSpan(p.neededSpanHz) + ", which fits inside the " + formatSpan(p.usableSpanHz) +
                   " this receiver can use at once. The window moves, and nothing is lost except about " +
                   std::to_string((int)std::lround(p.gapMs)) + " ms while the tuner settles.";
        return p;
    }

    // 4) Everything does not fit in one window.
    // Is the wanted channel alone feasible?
    Interval wfeas = {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    std::vector<const WindowChannel*> wantedPtrs;
    for (const auto& c : wanted) {
        wantedPtrs.push_back(&c);
        wfeas = intersect(wfeas, centresFor(caps, c));
    }
    const bool wantedAloneFits = !wanted.empty() && !wfeas.empty() && !intersect(wfeas, {caps.minHz, caps.maxHz}).empty();

    // Describe how far apart things are (control vs. first wanted).
    std::string apart;
    if (!mustKeep.empty() && !wanted.empty()) {
        apart = formatSpan(std::fabs(mustKeep.front().freqHz - wanted.front().freqHz));
    }

    if (!wantedAloneFits) {
        p.kind = PlanKind::Refuse;
        p.refuse = RefuseReason::OutOfBandwidth;
        p.summary = "Cannot follow: the signal is wider than this receiver can capture.";
        p.detail = "The signal needs " + formatSpan(p.neededSpanHz) + " but this receiver can only provide " + formatSpan(p.usableSpanHz) +
                   " at its current sample rate.";
        return p;
    }

    if (!opt.allowHop) {
        p.kind = PlanKind::Refuse;
        p.refuse = RefuseReason::OutOfBandwidth;
        p.controlStillVisible = controlVisibleAt(currentCenterHz);
        p.summary = "Not following: " + labelOr(wanted.front(), "voice channel") + " is out of bandwidth (" + apart + " from the control channel).";
        p.detail = "The control channel and the voice channel are " + apart + " apart, but a single " + caps.name + " can only watch " +
                   formatSpan(p.usableSpanHz) + " at once (" + formatSpan(caps.sampleRateHz) + " sample rate, edges unusable). Following this call would mean leaving the control channel, "
                   "and you have chosen to stay on it. Turn on 'Hop to out-of-range calls' to follow such calls anyway.";
        return p;
    }

    p.kind = PlanKind::HopAway;
    p.centerHz = pickCentre(caps, wfeas, wantedPtrs, false, 0.0);
    p.hardwareRetune = std::fabs(p.centerHz - currentCenterHz) > 0.5;
    p.controlStillVisible = controlVisibleAt(p.centerHz);
    p.gapMs = caps.retuneMs;
    p.nearDcSpike = nearDc(caps, p.centerHz, wantedPtrs);
    p.summary = "Hopping to " + formatFrequency(wanted.front().freqHz) + ": it is " + apart + " from the control channel, outside the " +
                formatSpan(p.usableSpanHz) + " window.";
    p.detail = "One " + caps.name + " cannot see both the control channel and this voice channel at the same time. SDRstar will leave the control channel "
               "for the length of the call and return when it ends. New call announcements are missed meanwhile.";
    return p;
}

}  // namespace sdrstar
