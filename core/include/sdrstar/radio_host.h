// SPDX-License-Identifier: GPL-3.0-or-later
//
// RadioHost: everything the smart layer needs from "the radio".
//
// Inside the app this is implemented on top of SDR++ (hidden channels via iqFrontEnd.addVFO, hardware retune via
// the source manager).  In the test-suite it is implemented by a simulator that plays recorded signals, so the
// whole trunk-following loop can be tested with the real decoder and no hardware.
#pragma once

#include <functional>
#include <string>

#include "sdrstar/dsp_lite.h"
#include "sdrstar/window_planner.h"

namespace sdrstar {

class RadioHost {
public:
    virtual ~RadioHost() = default;

    // Receiver description including the CURRENT sample rate (so the planner always uses live numbers).
    virtual ReceiverCaps caps() const = 0;
    // Current hardware centre frequency (Hz).
    virtual double centerHz() const = 0;
    // Retune the hardware.  Blocks until the tuner has settled (or failed).  The host must flush stale samples.
    virtual bool setCenter(double hz, std::string& error) = 0;

    // Sample rate of the channel streams delivered by openChannel() (e.g. 48000).
    virtual double channelRateHz() const = 0;
    // Open a hidden down-converted channel at an ABSOLUTE frequency.  `sink` is called on the host's DSP thread with
    // complex baseband at channelRateHz().  Returns a channel id >= 0, or -1 (error filled in).
    using IqSinkFn = std::function<void(const cf* samples, size_t count)>;
    virtual int openChannel(double absFreqHz, double bwHz, IqSinkFn sink, std::string& error) = 0;
    // Retarget an open channel to another ABSOLUTE frequency inside the current window.  Returns false if the
    // frequency is not inside the window.
    virtual bool moveChannel(int channel, double absFreqHz) = 0;
    virtual void closeChannel(int channel) = 0;
};

}  // namespace sdrstar
