// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/identity_ledger.h"

#include <cmath>

namespace sdrstar {

constexpr double IdentityLedger::kForgetAfterSec;
constexpr double IdentityLedger::kVerifyEverySec;
constexpr double IdentityLedger::kChallengeRetrySec;
constexpr int IdentityLedger::kAgreementsToChange;

long long IdentityLedger::keyFor(double freqHz) { return (long long)std::llround(freqHz / 6250.0); }

IdentityEntry* IdentityLedger::findMutable(double freqHz) {
    const long long k0 = keyFor(freqHz);
    IdentityEntry* best = nullptr;
    double bestD = 1e18;
    for (long long k = k0 - 1; k <= k0 + 1; ++k) {
        auto it = entries_.find(k);
        if (it == entries_.end()) { continue; }
        const double d = std::fabs(it->second.freqHz - freqHz);
        if (d < 7000.0 && d < bestD) {
            best = &it->second;
            bestD = d;
        }
    }
    return best;
}

const IdentityEntry* IdentityLedger::find(double freqHz) const { return const_cast<IdentityLedger*>(this)->findMutable(freqHz); }

IdentityOutcome IdentityLedger::offer(double freqHz, double bwHz, const ProbeVerdict& v, double nowSec, bool manual) {
    IdentityEntry* e = findMutable(freqHz);
    auto fresh = [&](IdentityOutcome o) {
        // Re-key on the new exact frequency (an old neighbouring entry for the same channel is replaced, not duplicated).
        if (e) { entries_.erase(keyFor(e->freqHz)); }
        IdentityEntry n;
        n.verdict = v;
        n.freqHz = freqHz;
        n.bwHz = bwHz;
        n.whenSec = nowSec;
        n.lastSeenSec = nowSec;
        entries_[keyFor(freqHz)] = n;
        return o;
    };

    if (!v.claimed) {
        if (!e) { return fresh(IdentityOutcome::Added); }
        if (manual) { return fresh(IdentityOutcome::Replaced); }
        if (!e->verdict.claimed) {
            e->whenSec = nowSec;   // still nothing: remember that we looked
            return IdentityOutcome::Confirmed;
        }
        return IdentityOutcome::Ignored;   // a quiet moment does not un-identify a channel
    }

    if (!e) { return fresh(IdentityOutcome::Added); }
    if (!e->verdict.claimed) { return fresh(IdentityOutcome::Replaced); }

    if (e->verdict.presetId == v.presetId) {
        ProbeVerdict merged = v;
        if (e->verdict.controlChannel && !v.controlChannel && !manual) {
            // One probe without enough control traffic does not turn a known control channel back into a plain channel.
            merged.controlChannel = true;
            merged.trunkable = e->verdict.trunkable;
            merged.title = e->verdict.title;
        }
        if (merged.detail.empty()) { merged.detail = e->verdict.detail; }
        e->verdict = merged;
        e->whenSec = nowSec;
        e->lastSeenSec = nowSec;
        e->disagreements = 0;
        e->challenger.clear();
        e->freqHz = freqHz;
        return IdentityOutcome::Confirmed;
    }

    // A different protocol.
    if (manual) { return fresh(IdentityOutcome::Replaced); }
    if (e->challenger == v.presetId) {
        e->disagreements++;
    } else {
        e->challenger = v.presetId;
        e->disagreements = 1;
    }
    e->challengerVerdict = v;
    e->whenSec = nowSec;   // the probe happened: the next verification is measured from here
    if (e->disagreements >= kAgreementsToChange) { return fresh(IdentityOutcome::Replaced); }
    return IdentityOutcome::KeptOld;
}

void IdentityLedger::touch(double freqHz, double nowSec) {
    if (IdentityEntry* e = findMutable(freqHz)) { e->lastSeenSec = nowSec; }
}

void IdentityLedger::forget(double freqHz) {
    if (IdentityEntry* e = findMutable(freqHz)) { entries_.erase(keyFor(e->freqHz)); }
}

void IdentityLedger::expire(double nowSec) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (nowSec - it->second.lastSeenSec > kForgetAfterSec) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

bool IdentityLedger::verificationDue(double freqHz, double nowSec) const {
    const IdentityEntry* e = find(freqHz);
    if (!e || !e->verdict.claimed) { return false; }
    const double since = nowSec - e->whenSec;
    if (e->disagreements > 0) { return since >= kChallengeRetrySec; }
    return since >= kVerifyEverySec;
}

}  // namespace sdrstar
