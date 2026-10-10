// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frequency -> "what is usually here" hints.  Used to sharpen guesses (an isolated 200 kHz-wide lump at
// 98.5 MHz is a broadcast FM station; a continuous 10 kHz carrier at 852 MHz is probably public-safety
// trunking) and to show the user what a frequency is normally used for.
//
// The table is US-centric (the owner's use case) with common worldwide allocations.  It only ever *hints*;
// nothing is decided from the band plan alone.
#pragma once

#include <string>
#include <vector>

namespace sdrstar {

struct BandEntry {
    double lowHz;
    double highHz;
    const char* name;                    // "Broadcast FM"
    const char* typical;                 // "WFM, 200 kHz stations"
    std::vector<const char*> protocols;  // decoder ids that commonly live here: "wfm","nfm","am","p25","dmr","nxdn","pocsag","adsb","ism" ...
};

// All entries that contain `hz`, most specific (narrowest) first.
std::vector<const BandEntry*> bandsFor(double hz);
// Narrowest entry or nullptr.
const BandEntry* bestBandFor(double hz);
// True if any entry for this frequency lists `protocol`.
bool bandSuggests(double hz, const char* protocol);

}  // namespace sdrstar
