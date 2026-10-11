// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small decision logic behind two card features, kept free of any GUI so it can be unit-tested:
//
//  * TipPrefs      which of the explanatory "tips" in the cursor card the user has switched off for good.
//  * AutoDecodeGate  when "auto-decode" may start a decoder on its own.
#pragma once

#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace sdrstar {

// A tip is one piece of explanatory text in a card ("This is a trunked system ...").  Each has a stable key.  The user
// can hide one tip for good (its key is remembered), or turn all tips off.
class TipPrefs {
public:
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    bool visible(const std::string& key) const { return enabled_ && hidden_.count(key) == 0; }
    void hide(const std::string& key) {
        if (!key.empty()) { hidden_.insert(key); }
    }
    // "Bring the tips back": clears every hidden tip and switches tips on again.
    void restoreAll() {
        hidden_.clear();
        enabled_ = true;
    }

    size_t hiddenCount() const { return hidden_.size(); }
    const std::set<std::string>& hidden() const { return hidden_; }
    void setHidden(std::set<std::string> keys) { hidden_ = std::move(keys); }

private:
    bool enabled_ = true;
    std::set<std::string> hidden_;
};

// Auto-decode starts decoding a signal without the user pressing the card button, in two situations:
//   1. the cursor has rested on an identified signal for kRestSec, or
//   2. the user pressed Identify (or Re-check / Try again) and the check found a protocol.
// It must never become a nuisance, so it fires once per "visit": after it fires, or after the user stops a decode
// (Stop button), it stays quiet until the cursor is moved to a new place.  It never replaces a job that is already
// running.
class AutoDecodeGate {
public:
    static constexpr double kRestSec = 0.5;

    // The cursor moved (or appeared): a new visit begins.
    void cursorMoved() { armed_ = true; }
    // The user stopped a job, or started one by hand: do not start another one under the same cursor position.
    void disarm() { armed_ = false; }

    // Situation 1.  `enabled` = the checkbox; `restedSec` = how long the cursor has been still; `idle` = no job is running
    // that must not be interrupted; `identified` = the signal under the cursor already has a claimed protocol.
    // Returns true once; the caller then starts the decoder.
    bool shouldStartFromCursor(bool enabled, double restedSec, bool idle, bool identified) {
        if (!enabled || !armed_ || !idle || !identified || restedSec < kRestSec) { return false; }
        armed_ = false;
        return true;
    }

    // Situation 2.  identifyRequested() when the user asks for identification; identifyFinished() when it is done.
    void identifyRequested() { identifyPending_ = true; }
    void identifyCancelled() { identifyPending_ = false; }
    bool identifyPending() const { return identifyPending_; }
    // `claimed` = a protocol was found; `cursorStillThere` = the cursor is still on that signal (the user did not
    // move on while the check ran).  Returns true if the decoder should now start.
    bool identifyFinished(bool enabled, bool claimed, bool cursorStillThere) {
        const bool wasPending = identifyPending_;
        identifyPending_ = false;
        if (!wasPending || !enabled || !claimed || !cursorStillThere) { return false; }
        armed_ = false;   // this visit has been served
        return true;
    }

    bool armed() const { return armed_; }

private:
    bool armed_ = false;
    bool identifyPending_ = false;
};

}  // namespace sdrstar
