// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns the output of a DSD-neo "probe" (the decoder run with ONE specific preset on a candidate signal for a few
// seconds) into a verdict: did it really decode this protocol, or just trip over noise?
//
// The rules are per protocol because DSD-neo's decoders differ in how readily they produce false syncs.  They
// were derived from a measured confusion matrix (every shipped test recording x every preset, plus pure noise):
//   * P25 Ph1/Ph2, DMR, D-STAR, YSF, EDACS/ProVoice: zero false syncs on noise or on other protocols, so a handful of
//     sync lines is trusted;
//   * NXDN, dPMR, M17: false "syncs" appear on pure noise (M17: 42 in 9 s), so only *decoded content* counts
//     (NXDN needs a decoded RAN, M17 a decoded stream link-control line, dPMR a CRC-valid talkgroup).
#pragma once

#include <string>
#include <vector>

#include "sdrstar/dsd_session.h"

namespace sdrstar {

struct ProbeVerdict {
    bool claimed = false;          // enough evidence to name the protocol
    std::string presetId;          // "p25p1", "dmr", ...
    std::string protocol;          // "P25 Phase 1"
    double confidence = 0;         // 0..1 (>= 0.5 when claimed)
    int evidenceCount = 0;         // number of qualifying decoder lines
    bool controlChannel = false;   // looks like a trunking control channel
    bool trunkable = false;        // the "Follow trunked system" action makes sense
    std::string title;             // one-line popup title ("P25 Phase 1 control channel")
    std::string detail;            // second line ("NAC 0x293 · WACN BEE00 · System 14C · Site 9")
};

// `log` = decoder log lines (ANSI already stripped or not; both work), `st` = session state from the same run.
ProbeVerdict evaluateProbe(const std::string& presetId, const DsdState& st, const std::vector<std::string>& log);

// Pick the best claimed verdict (highest confidence; ties broken by evidence count). Returns a non-claimed
// verdict if none qualifies.
ProbeVerdict bestVerdict(const std::vector<ProbeVerdict>& v);

}  // namespace sdrstar
