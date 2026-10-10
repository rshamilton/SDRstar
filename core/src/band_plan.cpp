// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/band_plan.h"

#include <algorithm>
#include <cstring>

namespace sdrstar {

static const std::vector<BandEntry>& table() {
    static const std::vector<BandEntry> t = {
        {0.53e6, 1.71e6, "AM broadcast (MW)", "AM, 10 kHz spacing (HF mode of the V4)", {"am"}},
        {1.8e6, 30e6, "HF", "AM / SSB / CW / digital modes (V4 HF upconverter)", {"am", "usb", "lsb", "cw"}},
        {76e6, 108e6, "Broadcast FM", "wideband FM, 200 kHz stations", {"wfm"}},
        {108e6, 118e6, "Aeronautical navigation", "VOR / ILS (AM-based)", {"am"}},
        {118e6, 137e6, "Airband (voice + ACARS)", "AM voice; ACARS data near 131 MHz", {"am", "acars"}},
        {137e6, 138e6, "Weather satellites", "NOAA APT / Meteor LRPT downlinks", {"apt", "lrpt"}},
        {144e6, 148e6, "2 m amateur", "NFM, APRS at 144.390 MHz (US)", {"nfm", "afsk1200", "aprs"}},
        {150.8e6, 152e6, "Business / pagers", "NFM, POCSAG/FLEX pagers", {"nfm", "pocsag", "flex"}},
        {152e6, 159.5e6, "VHF land mobile / pagers", "NFM, P25, DMR, POCSAG/FLEX", {"nfm", "p25", "dmr", "nxdn", "pocsag", "flex"}},
        {156e6, 162.025e6, "Marine VHF", "NFM voice; AIS at 161.975 / 162.025 MHz", {"nfm", "ais"}},
        {159.5e6, 174e6, "VHF public safety / land mobile", "NFM, P25, DMR, NXDN", {"nfm", "p25", "dmr", "nxdn"}},
        {162.4e6, 162.55e6, "NOAA weather radio", "NFM, 7 channels, EAS alerts", {"nfm", "eas"}},
        {220e6, 225e6, "1.25 m amateur", "NFM", {"nfm"}},
        {380e6, 400e6, "TETRA / public safety", "TETRA (25 kHz), P25", {"tetra", "p25"}},
        {406e6, 420e6, "Federal UHF", "NFM, P25", {"nfm", "p25"}},
        {420e6, 450e6, "70 cm amateur", "NFM, D-STAR, DMR, YSF, M17", {"nfm", "dstar", "dmr", "ysf", "m17"}},
        {433.05e6, 434.79e6, "433 MHz ISM", "garage doors, sensors, weather stations", {"ism"}},
        {450e6, 470e6, "UHF land mobile", "NFM, P25, DMR, NXDN, FRS/GMRS", {"nfm", "p25", "dmr", "nxdn"}},
        {462.5e6, 467.725e6, "FRS / GMRS", "NFM, 12.5/25 kHz", {"nfm"}},
        {470e6, 608e6, "UHF television", "digital TV (wideband)", {}},
        {764e6, 776e6, "700 MHz public safety (downlink)", "P25 Phase 1/2 trunking", {"p25"}},
        {794e6, 806e6, "700 MHz public safety (uplink)", "mobile transmit side", {"p25"}},
        {806e6, 824e6, "800 MHz land mobile (uplink)", "mobile transmit side", {"p25", "dmr", "edacs"}},
        {851e6, 869e6, "800 MHz public safety / LMR (downlink)", "P25 / DMR / EDACS / SmartNet trunking", {"p25", "dmr", "edacs", "nxdn", "smartnet"}},
        {863e6, 870e6, "868 MHz ISM (EU)", "sensors, alarms", {"ism"}},
        {902e6, 928e6, "915 MHz ISM (US)", "sensors, utility meters, LoRa", {"ism"}},
        {929e6, 932e6, "Pagers (929 MHz)", "POCSAG / FLEX", {"pocsag", "flex"}},
        {1030e6, 1030e6 + 2e6, "SSR interrogation", "aircraft radar", {}},
        {1089e6, 1091e6, "ADS-B (1090 MHz)", "Mode S extended squitter, 2 MHz wide", {"adsb"}},
        {1575e6, 1576e6, "GPS L1", "spread spectrum, below noise", {}},
        {1616e6, 1627e6, "Iridium", "satellite burst data", {"iridium"}},
    };
    return t;
}

std::vector<const BandEntry*> bandsFor(double hz) {
    std::vector<const BandEntry*> v;
    for (const auto& b : table()) {
        if (hz >= b.lowHz && hz <= b.highHz) { v.push_back(&b); }
    }
    std::sort(v.begin(), v.end(), [](const BandEntry* a, const BandEntry* b) { return (a->highHz - a->lowHz) < (b->highHz - b->lowHz); });
    return v;
}

const BandEntry* bestBandFor(double hz) {
    auto v = bandsFor(hz);
    return v.empty() ? nullptr : v.front();
}

bool bandSuggests(double hz, const char* protocol) {
    for (const BandEntry* b : bandsFor(hz)) {
        for (const char* p : b->protocols) {
            if (std::strcmp(p, protocol) == 0) { return true; }
        }
    }
    return false;
}

}  // namespace sdrstar
