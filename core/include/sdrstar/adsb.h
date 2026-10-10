// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADS-B (Mode S extended squitter, 1090 MHz): a small self-contained demodulator and message decoder.
//
//   ModeSDemod   complex baseband at 2 MS/s  ->  CRC-checked 112-bit DF17 frames (pulse-position modulation,
//                8 microsecond preamble, 1 bit = 1 microsecond).  Single-bit errors are repaired through the CRC syndrome.
//   AdsbTable    decoded messages -> one record per aircraft: callsign, barometric altitude, position (CPR decoded,
//                global when an even + odd pair is known, local afterwards), ground speed, heading, vertical rate.
//
// Formulas follow the public ICAO Annex 10 / RTCA DO-260 definitions as described in J. Sun, "The 1090 MHz Riddle".
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sdrstar/dsp_lite.h"

namespace sdrstar {

// 24-bit Mode S parity remainder over the first `bits` bits of `msg` (including the transmitted parity field).
// 0 means "valid" for DF17/DF18.
uint32_t modeSCrc24(const uint8_t* msg, int bits);

class ModeSDemod {
public:
    // Called for every accepted frame: 14 bytes (112 bits), CRC already verified (and any single-bit error repaired).
    // `repaired` = accepted only after fixing one bit (less trustworthy: AdsbTable needs the ICAO to be known already).
    using FrameFn = std::function<void(const uint8_t* msg, int bytes, bool repaired)>;
    explicit ModeSDemod(FrameFn fn);

    // Complex baseband at exactly 2 MS/s centred on 1090 MHz.
    void feed(const cf* iq, size_t n);
    void reset();

    long long frames() const { return frames_; }
    long long repaired() const { return repaired_; }   // frames accepted only after fixing one bit
    long long samples() const { return samplesSeen_; }

private:
    void process(bool flush);
    FrameFn fn_;
    std::vector<float> mag_;
    long long frames_ = 0, repaired_ = 0, samplesSeen_ = 0;
};

struct AdsbAircraft {
    uint32_t icao = 0;
    std::string callsign;
    bool hasAlt = false;
    int altFt = 0;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasVel = false;
    double speedKt = 0, headingDeg = 0;
    bool hasVr = false;
    int vrFpm = 0;
    double lastSeenSec = 0;
    long msgCount = 0;
};

class AdsbTable {
public:
    // One verified 14-byte DF17 frame.  `nowSec` is any monotonic clock.  Returns the icao updated (0 = ignored).
    uint32_t update(const uint8_t* msg, double nowSec, bool repaired = false);
    // Most recently heard first.  Aircraft heard fewer than `minMessages` times are left out (one clean frame can in
    // principle be a chance CRC match; a real aircraft transmits several a second).
    std::vector<AdsbAircraft> snapshot(int minMessages = 2) const;
    // Forget aircraft not heard for `maxAgeSec`.
    void prune(double nowSec, double maxAgeSec);
    size_t size() const { return ac_.size(); }
    long long messages() const { return messages_; }

    // "4D2023  AMC421    35000 ft   420 kt  hdg 183  +45 fpm   52.2572 N  3.9194 E   (12 msgs)"
    static std::string describe(const AdsbAircraft& a);

private:
    struct Cpr {
        bool have[2] = {false, false};
        uint32_t lat[2] = {0, 0}, lon[2] = {0, 0};
        double t[2] = {0, 0};
    };
    std::map<uint32_t, AdsbAircraft> ac_;
    std::map<uint32_t, Cpr> cpr_;
    long long messages_ = 0;
};

// Exposed for tests.
int adsbNL(double lat);
bool adsbCprGlobal(uint32_t latE, uint32_t lonE, uint32_t latO, uint32_t lonO, bool oddNewest, double& lat, double& lon);
bool adsbCprLocal(uint32_t latCpr, uint32_t lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon);

}  // namespace sdrstar
