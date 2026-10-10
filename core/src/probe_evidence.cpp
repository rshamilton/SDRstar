// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/probe_evidence.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>

namespace sdrstar {

namespace {

std::string stripAnsiLocal(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == ';')) { ++j; }
            if (j < s.size()) { i = j; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// A line only a DMR *trunking* control channel produces.  A bare "CSBK" is not one: conventional repeaters print
// "Preamble CSBK" before data and "Channel Timing CSBK" on ordinary traffic channels, so counting those called plain DMR
// channels "control channels (trunked system)".  These are DSD-neo's own labels for the control messages: Tier III
// (C_ALOHA / C_BCAST / C_MOVE / C_AHOY / C_ACKVIT / P_MAINT and the *_GRANT family), Capacity Plus and Connect Plus.
bool isDmrTrunkingLine(const std::string& l) {
    static const char* const kMarkers[] = {"C_ALOHA", "C_BCAST", "C_MOVE", "C_AHOY", "C_ACKVIT", "P_MAINT", "_GRANT", "Capacity Plus CSBK",
                                           "Connect Plus Adjacent", "Connect Plus Group Voice", "Connect Plus Private Voice",
                                           "Connect Plus Data Channel", "SLCO Connect Plus Control"};
    for (const char* m : kMarkers) {
        if (has(l, m)) { return true; }
    }
    return false;
}

struct Counts {
    int sync = 0;        // lines that contain the protocol's sync token
    int clean = 0;       // ... and carry no error marker
    int content = 0;     // lines carrying decoded content specific to the protocol
    int control = 0;     // lines that only a control channel produces
};

double confidenceFor(int hits, int minHits) {
    if (hits < minHits) { return 0.4 * (double)hits / (double)std::max(1, minHits); }
    double c = 0.5 + 0.5 * (double)(hits - minHits) / (double)(2 * minHits);
    return std::min(1.0, c);
}

std::string hex(long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llX", v);
    return b;
}

std::string p25Detail(const DsdState& st) {
    std::string d;
    auto add = [&](const std::string& s) {
        if (!d.empty()) { d += "  \xC2\xB7  "; }
        d += s;
    };
    if (st.identity.nac >= 0) { add("NAC 0x" + hex(st.identity.nac)); }
    if (st.identity.wacn >= 0) { add("WACN " + hex(st.identity.wacn)); }
    if (st.identity.sysid >= 0) { add("System " + hex(st.identity.sysid)); }
    if (st.identity.site >= 0) { add("Site " + std::to_string(st.identity.site)); }
    if (st.identity.rfss >= 0) { add("RFSS " + std::to_string(st.identity.rfss)); }
    return d;
}

}  // namespace

ProbeVerdict evaluateProbe(const std::string& presetId, const DsdState& st, const std::vector<std::string>& rawLog) {
    ProbeVerdict v;
    v.presetId = presetId;

    std::vector<std::string> log;
    log.reserve(rawLog.size());
    for (const auto& l : rawLog) { log.push_back(stripAnsiLocal(l)); }

    Counts c;
    auto each = [&](auto fn) {
        for (const auto& l : log) { fn(l); }
    };

    if (presetId == "p25p1" || presetId == "p25") {
        each([&](const std::string& l) {
            if (has(l, "Sync: +P25p1")) { c.sync++; }
            if (has(l, "TSBK")) { c.control++; }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 3);
        if (st.identity.nac < 0) { v.confidence *= 0.5; }  // a real P25 frame always carries a NAC
        v.claimed = v.confidence >= 0.5;
        v.protocol = "P25 Phase 1";
        v.controlChannel = c.control >= 3 || st.quality.ccOk >= 10;
        v.trunkable = v.controlChannel;
        v.title = v.controlChannel ? "P25 Phase 1 control channel (trunked system)" : "P25 Phase 1 channel";
        v.detail = p25Detail(st);
    } else if (presetId == "p25p2") {
        each([&](const std::string& l) {
            if (has(l, "Sync: +P25p2")) { c.sync++; }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 3);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "P25 Phase 2";
        v.title = "P25 Phase 2 (TDMA) channel";
        v.detail = p25Detail(st);
        v.trunkable = false;
    } else if (presetId == "dmr") {
        each([&](const std::string& l) {
            if (has(l, "Sync: +DMR") || has(l, "Sync: DMR")) { c.sync++; }
            if (isDmrTrunkingLine(l)) { c.control++; }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 3);
        if (st.identity.colorCode < 0) { v.confidence *= 0.5; }
        v.claimed = v.confidence >= 0.5;
        v.protocol = "DMR";
        v.controlChannel = c.control >= 3;
        v.trunkable = v.controlChannel;
        v.title = v.controlChannel ? "DMR control channel (trunked system)" : "DMR channel";
        if (st.identity.colorCode >= 0) { v.detail = "Color Code " + std::to_string(st.identity.colorCode); }
    } else if (presetId == "dstar") {
        std::string calls;
        each([&](const std::string& l) {
            if (has(l, "Sync: +DSTAR")) { c.sync++; }
            if (calls.empty() && has(l, "DSTAR VOICE")) {
                size_t p = l.find("SRC:");
                if (p != std::string::npos) {
                    std::string s = l.substr(p + 4);
                    std::string clean;
                    for (unsigned char ch : s) { if (ch >= 32 && ch < 127) { clean.push_back((char)ch); } }
                    while (!clean.empty() && clean.back() == ' ') { clean.pop_back(); }
                    size_t a = clean.find_first_not_of(' ');
                    calls = a == std::string::npos ? "" : clean.substr(a);
                }
            }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 3);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "D-STAR";
        v.title = "D-STAR digital voice";
        if (!calls.empty()) { v.detail = "From: " + calls; }
    } else if (presetId == "ysf") {
        each([&](const std::string& l) {
            if (has(l, "Sync: +YSF")) { c.sync++; }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 8);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "Yaesu System Fusion";
        v.title = "Yaesu System Fusion (C4FM) channel";
    } else if (presetId == "edacs" || presetId == "provoice") {
        bool edacs = false, pv = false;
        each([&](const std::string& l) {
            if (has(l, "Sync: -EDACS") || has(l, "Sync: +EDACS")) { c.sync++; edacs = true; }
            if (has(l, "Sync: -PV") || has(l, "Sync: +PV")) { c.sync++; pv = true; }
            if (has(l, "Control Channel") || has(l, "CC LCN")) { c.control++; }
        });
        v.evidenceCount = c.sync;
        v.confidence = confidenceFor(c.sync, 6);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "EDACS / ProVoice";
        v.controlChannel = edacs && c.control >= 2;
        v.trunkable = v.controlChannel;
        v.title = v.controlChannel ? "EDACS control channel (trunked system)" : (pv && !edacs ? "ProVoice channel" : "EDACS / ProVoice channel");
    } else if (presetId == "nxdn96" || presetId == "nxdn48") {
        each([&](const std::string& l) {
            if (has(l, "Sync: NXDN")) { c.sync++; }
            // A decoded RAN (CRC-protected) is the proof; noise produces syncs but never a RAN.
            if (has(l, "Sync: NXDN") && has(l, " RAN ") && !has(l, "CRC ERR")) { c.content++; }
        });
        v.evidenceCount = c.content;
        v.confidence = confidenceFor(c.content, 3);
        v.claimed = v.confidence >= 0.5;
        v.protocol = presetId == "nxdn48" ? "NXDN 4800 (6.25 kHz)" : "NXDN 9600 (12.5 kHz)";
        v.title = v.protocol + " channel";
        if (st.identity.ran >= 0) { v.detail = "RAN " + std::to_string(st.identity.ran); }
    } else if (presetId == "m17") {
        each([&](const std::string& l) {
            if (has(l, "STR LC:")) { c.content++; }
        });
        v.evidenceCount = c.content;
        v.confidence = confidenceFor(c.content, 6);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "M17";
        v.title = "M17 digital voice";
    } else if (presetId == "dpmr") {
        each([&](const std::string& l) {
            if (has(l, "TG=") && !has(l, "CRC ERR")) { c.content++; }
        });
        v.evidenceCount = c.content;
        v.confidence = confidenceFor(c.content, 2);
        v.claimed = v.confidence >= 0.5;
        v.protocol = "dPMR";
        v.title = "dPMR digital voice";
    }
    return v;
}

ProbeVerdict bestVerdict(const std::vector<ProbeVerdict>& all) {
    // Highest confidence wins; then more evidence; then the more informative claim (control channel, has detail).
    auto better = [](const ProbeVerdict& a, const ProbeVerdict& b) {
        if (std::fabs(a.confidence - b.confidence) > 1e-9) { return a.confidence > b.confidence; }
        if (a.evidenceCount != b.evidenceCount) { return a.evidenceCount > b.evidenceCount; }
        if (a.controlChannel != b.controlChannel) { return a.controlChannel; }
        return !a.detail.empty() && b.detail.empty();
    };
    ProbeVerdict best;
    for (const auto& v : all) {
        if (!v.claimed) { continue; }
        if (!best.claimed || better(v, best)) { best = v; }
    }
    return best;
}

}  // namespace sdrstar
