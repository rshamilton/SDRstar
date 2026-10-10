// SPDX-License-Identifier: GPL-3.0-or-later
//
// Capture-window planner.
//
// A single RTL-SDR can only "see" a slice of spectrum at a time (about 2.4 MHz
// of raw bandwidth, of which roughly 80 % is usable once the filter roll-off
// is accounted for).  A trunked radio system, on the other hand, can scatter
// its voice channels over many megahertz.  This module answers one question:
//
//     "Given what I must keep listening to (the control channel) and what I
//      now want to hear (a voice channel), where should the receiver's centre
//      frequency be, and if that is impossible, why?"
//
// It is pure logic (no I/O, no SDR++ dependency) so that every rule can be
// unit-tested and so that the user-facing explanation text is generated in one
// place.
#pragma once

#include <string>
#include <vector>

namespace sdrstar {

// What the attached receiver can do.  Defaults describe an RTL-SDR Blog V4.
struct ReceiverCaps {
    double minHz = 500e3;           // lowest tunable frequency (V4: HF upconverter)
    double maxHz = 1766e6;          // highest tunable frequency (R828D tuner)
    double sampleRateHz = 2.4e6;    // current raw sample rate
    double usableFraction = 0.80;   // share of the raw span that is clean (filter roll-off)
    double dcGuardHz = 12e3;        // keep signals this far from the DC spike if at all possible
    double retuneMs = 150.0;        // typical time the stream is unusable after a hardware retune
    std::string name = "RTL-SDR";

    double usableSpanHz() const { return sampleRateHz * usableFraction; }
};

// A signal that has to fit in the capture window.
struct WindowChannel {
    double freqHz = 0.0;
    double bwHz = 12500.0;
    std::string label;  // "control channel", "voice channel", ...
};

enum class PlanKind {
    KeepWindow,   // everything already fits: just move the software channel
    ShiftWindow,  // retune the hardware once; all channels (incl. control) still fit
    HopAway,      // cannot hold control channel and voice channel together: control is lost
                  // for the duration of the call (only produced when hopping is allowed)
    Refuse,       // the request is rejected; see summary/detail
};

enum class RefuseReason {
    None,
    OutOfTunerRange,    // frequency outside what the hardware can tune
    OutOfBandwidth,     // control + voice span more than the usable window and hopping is off
    InvalidRequest,
};

struct PlanOptions {
    bool allowHop = true;   // allow leaving the control channel to follow an out-of-window call
    // Prefer centres that keep the control channel near the middle of the window,
    // leaving headroom on both sides for neighbouring voice channels.
    bool centreOnControl = false;
};

struct Plan {
    PlanKind kind = PlanKind::Refuse;
    RefuseReason refuse = RefuseReason::None;
    double centerHz = 0.0;            // hardware centre frequency to use (== current if unchanged)
    bool hardwareRetune = false;      // true when centerHz differs from the current centre
    bool controlStillVisible = false; // is the control channel inside the window after this plan?
    double neededSpanHz = 0.0;        // span required to hold everything that was requested
    double usableSpanHz = 0.0;        // span this receiver can actually provide
    double gapMs = 0.0;               // expected interruption to channels that stay visible
    bool nearDcSpike = false;         // some channel lies within the DC guard (soft warning)
    std::string summary;              // one line, plain English, for the status bar
    std::string detail;               // a few sentences, plain English, for the info panel
};

// Plan the capture window.
//   current:   the hardware centre frequency right now
//   mustKeep:  channels that should stay visible if at all possible (control channel)
//   wanted:    channels we want to hear now (voice channel being followed)
Plan planWindow(const ReceiverCaps& caps, double currentCenterHz, const std::vector<WindowChannel>& mustKeep,
                const std::vector<WindowChannel>& wanted, const PlanOptions& opt = PlanOptions());

// Convenience used by the UI: does a single channel lie inside the window centred at centerHz?
bool channelFits(const ReceiverCaps& caps, double centerHz, const WindowChannel& ch);

// "851.012 MHz", "162.550 MHz", "1.090 GHz", "7.074 MHz" etc.
std::string formatFrequency(double hz);
// "2.4 MHz", "1.92 MHz", "250 kHz"
std::string formatSpan(double hz);

}  // namespace sdrstar
