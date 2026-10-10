// SPDX-License-Identifier: GPL-3.0-or-later
//
// IdentityLedger: what each frequency has been identified as, and when a new probe result is allowed to change it.
//
// Identification runs on a few seconds of signal, so now and then it can answer differently for the same channel
// (a noisy first transmission, two protocols whose decoders both sync a little).  The waterfall label must not flip
// between kinds because of that: once a signal has been identified it keeps that identity until the signal really
// changes.  The rules:
//
//   * nothing known yet                       -> take the answer;
//   * the same protocol again                 -> refresh the details (a control channel stays a control channel);
//   * a different protocol, automatic probe   -> the old identity stays; the challenger has to be seen again by a
//                                                second probe (soon after) before it replaces the old one;
//   * a different protocol, user asked        -> take it at once (the user pressed "Re-check" on purpose);
//   * "nothing recognised", automatic probe   -> never erases an identity (a channel is often just quiet);
//   * "nothing recognised", user asked        -> recorded, so the card can say so.
//
// The memory outlives the waterfall's own track of the signal (a voice channel keys up and down), so an identified
// channel that comes back is recognised again without another probe.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "sdrstar/probe_evidence.h"

namespace sdrstar {

struct IdentityEntry {
    ProbeVerdict verdict;        // claimed = identified; not claimed = probed and nothing recognised
    double freqHz = 0;           // the exact frequency the probe ran on
    double bwHz = 12500;
    double whenSec = 0;          // when a probe last confirmed or set this
    double lastSeenSec = 0;      // when a signal was last seen here
    int disagreements = 0;       // consecutive automatic probes that claimed the same OTHER protocol
    std::string challenger;      // preset id of that other protocol
    ProbeVerdict challengerVerdict;
};

enum class IdentityOutcome {
    Added,      // first answer for this frequency
    Confirmed,  // same answer again (details refreshed)
    Replaced,   // the identity changed
    KeptOld,    // a different answer was noted but the old identity stays for now
    Ignored,    // an automatic "nothing recognised" does not change a known identity
};

class IdentityLedger {
public:
    static constexpr double kForgetAfterSec = 600.0;   // an entry whose signal has not been seen for this long is dropped
    static constexpr double kVerifyEverySec = 600.0;   // an identified, present signal is re-checked this often
    static constexpr double kChallengeRetrySec = 25.0; // ...and this soon after a probe disagreed with the identity
    static constexpr int kAgreementsToChange = 2;      // automatic probes that must agree on a different protocol

    // `manual` = the user asked for this probe.
    IdentityOutcome offer(double freqHz, double bwHz, const ProbeVerdict& v, double nowSec, bool manual);

    // The entry for this frequency (same 6.25 kHz channel or the neighbouring one within 7 kHz), or nullptr.
    const IdentityEntry* find(double freqHz) const;
    // A signal is present at this frequency right now.
    void touch(double freqHz, double nowSec);
    void forget(double freqHz);
    void clear() { entries_.clear(); }
    // Drop entries whose signal has not been seen for kForgetAfterSec.
    void expire(double nowSec);
    // An identified signal that is present should be probed again (periodic check, or a disagreement to settle).
    bool verificationDue(double freqHz, double nowSec) const;
    size_t size() const { return entries_.size(); }

private:
    static long long keyFor(double freqHz);
    IdentityEntry* findMutable(double freqHz);
    std::map<long long, IdentityEntry> entries_;
};

}  // namespace sdrstar
