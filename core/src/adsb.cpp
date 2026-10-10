// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/adsb.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sdrstar {

// ------------------------------------------------------------------------------------------------------------
// CRC-24 (generator 0xFFF409)
// ------------------------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kPoly = 0xFFF409;

struct CrcTables {
    uint32_t byteTab[256];
    // syndrome of a single flipped bit at position p (0 = first transmitted bit) of a 112-bit frame
    uint32_t bitSyndrome[112];
    CrcTables() {
        for (int b = 0; b < 256; ++b) {
            uint32_t c = (uint32_t)b << 16;
            for (int k = 0; k < 8; ++k) { c = (c & 0x800000) ? ((c << 1) ^ kPoly) : (c << 1); }
            byteTab[b] = c & 0xFFFFFF;
        }
        for (int p = 0; p < 112; ++p) {
            uint8_t m[14] = {0};
            m[p / 8] = (uint8_t)(0x80 >> (p % 8));
            bitSyndrome[p] = crcRaw(m, 112);
        }
    }
    uint32_t crcRaw(const uint8_t* msg, int bits) const {
        uint32_t crc = 0;
        const int bytes = bits / 8;
        for (int i = 0; i < bytes; ++i) { crc = ((crc << 8) ^ byteTab[((crc >> 16) ^ msg[i]) & 0xFF]) & 0xFFFFFF; }
        return crc;
    }
};

const CrcTables& tables() {
    static const CrcTables t;
    return t;
}

}  // namespace

uint32_t modeSCrc24(const uint8_t* msg, int bits) { return tables().crcRaw(msg, bits); }

// ------------------------------------------------------------------------------------------------------------
// Demodulator
// ------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kPreambleSamples = 16;                    // 8 microseconds at 2 MS/s
constexpr int kDataSamples = 112 * 2;                   // 112 bits, 2 samples each
constexpr int kFrameSamples = kPreambleSamples + kDataSamples;
}  // namespace

ModeSDemod::ModeSDemod(FrameFn fn) : fn_(std::move(fn)) {}

void ModeSDemod::reset() {
    mag_.clear();
    frames_ = repaired_ = samplesSeen_ = 0;
}

void ModeSDemod::feed(const cf* iq, size_t n) {
    mag_.reserve(mag_.size() + n);
    for (size_t i = 0; i < n; ++i) { mag_.push_back(std::sqrt(iq[i].real() * iq[i].real() + iq[i].imag() * iq[i].imag())); }
    samplesSeen_ += (long long)n;
    process(false);
}

void ModeSDemod::process(bool) {
    const float* m = mag_.data();
    const long total = (long)mag_.size();
    long j = 0;
    const long last = total - (kFrameSamples + 2);
    for (; j < last; ++j) {
        // --- preamble: pulses at 0, 1.0, 3.5, 4.5 microseconds = samples 0, 2, 7, 9; quiet elsewhere.  The test is by
        // CONTRAST (pulse samples against the quiet samples between them) rather than by exact edges, so it still fires
        // when the channel filter has smeared the 0.5 us pulses; the 24-bit CRC is what validates a frame. ---
        const float* p = m + j;
        const float hiMean = (p[0] + p[2] + p[7] + p[9]) * 0.25f;
        if (hiMean <= 0.0f) { continue; }
        const float loMean = (p[1] + p[3] + p[4] + p[5] + p[6] + p[8]) * (1.0f / 6.0f);
        if (hiMean < 1.6f * loMean) { continue; }
        if (p[0] < 0.6f * hiMean || p[2] < 0.6f * hiMean || p[7] < 0.6f * hiMean || p[9] < 0.6f * hiMean) { continue; }
        if (std::max(p[4], std::max(p[5], p[6])) > 0.75f * hiMean) { continue; }
        if ((p[11] + p[12] + p[13] + p[14]) * 0.25f > 0.75f * hiMean) { continue; }
        if (p[1] > p[0] || p[1] > p[2]) { continue; }   // a dip between the first two pulses

        // --- data: bit = first half of the microsecond stronger than the second half ---
        uint8_t msg[14] = {0};
        const float* d = p + kPreambleSamples;
        for (int i = 0; i < 112; ++i) {
            if (d[2 * i] > d[2 * i + 1]) { msg[i / 8] |= (uint8_t)(0x80 >> (i % 8)); }
        }
        // The DF field (first 5 bits) must be 17, or one bit away from 17 (a bit error we may be able to repair).
        const int dfx = (msg[0] >> 3) ^ 17;
        if (dfx != 0 && (dfx & (dfx - 1)) != 0) { continue; }
        uint32_t syn = tables().crcRaw(msg, 112);
        bool fixed = false;
        if (syn != 0) {
            // single-bit repair through the syndrome table
            int pos = -1;
            for (int b = 0; b < 112; ++b) {
                if (tables().bitSyndrome[b] == syn) { pos = b; break; }
            }
            if (pos < 0) { continue; }
            msg[pos / 8] ^= (uint8_t)(0x80 >> (pos % 8));
            fixed = true;
        }
        const int df2 = msg[0] >> 3;
        if (df2 != 17) { continue; }   // ADS-B only (TIS-B / DF18 is not decoded)
        ++frames_;
        if (fixed) { ++repaired_; }
        if (fn_) { fn_(msg, 14, fixed); }
        j += kFrameSamples - 1;   // frames cannot overlap
    }
    // keep the unprocessed tail (a frame may straddle the chunk boundary)
    if (j > 0) {
        const long keep = std::max(0L, total - j);
        std::memmove(mag_.data(), mag_.data() + (total - keep), (size_t)keep * sizeof(float));
        mag_.resize((size_t)keep);
    }
}

// ------------------------------------------------------------------------------------------------------------
// CPR
// ------------------------------------------------------------------------------------------------------------
int adsbNL(double lat) {
    lat = std::fabs(lat);
    if (lat < 1e-9) { return 59; }
    if (std::fabs(lat - 87.0) < 1e-9) { return 2; }
    if (lat > 87.0) { return 1; }
    const double nz = 15.0;
    const double a = 1.0 - std::cos(kPi / (2.0 * nz));
    const double b = std::cos(kPi / 180.0 * lat);
    return (int)std::floor(2.0 * kPi / std::acos(1.0 - a / (b * b)));
}

namespace {
double modPos(double x, double y) { return x - y * std::floor(x / y); }
}  // namespace

bool adsbCprGlobal(uint32_t latE, uint32_t lonE, uint32_t latO, uint32_t lonO, bool oddNewest, double& lat, double& lon) {
    const double lat0 = latE / 131072.0, lat1 = latO / 131072.0;
    const double lon0 = lonE / 131072.0, lon1 = lonO / 131072.0;
    const double dLat0 = 360.0 / 60.0, dLat1 = 360.0 / 59.0;
    const double j = std::floor(59.0 * lat0 - 60.0 * lat1 + 0.5);
    double rlat0 = dLat0 * (modPos(j, 60.0) + lat0);
    double rlat1 = dLat1 * (modPos(j, 59.0) + lat1);
    if (rlat0 >= 270.0) { rlat0 -= 360.0; }
    if (rlat1 >= 270.0) { rlat1 -= 360.0; }
    if (rlat0 < -90.0 || rlat0 > 90.0 || rlat1 < -90.0 || rlat1 > 90.0) { return false; }
    if (adsbNL(rlat0) != adsbNL(rlat1)) { return false; }   // the two frames straddle a latitude zone boundary
    const double rlat = oddNewest ? rlat1 : rlat0;
    const int nl = adsbNL(rlat);
    const int ni = std::max(nl - (oddNewest ? 1 : 0), 1);
    const double dLon = 360.0 / ni;
    const double m = std::floor(lon0 * (nl - 1) - lon1 * nl + 0.5);
    double rlon = dLon * (modPos(m, (double)ni) + (oddNewest ? lon1 : lon0));
    if (rlon >= 180.0) { rlon -= 360.0; }
    lat = rlat;
    lon = rlon;
    return true;
}

bool adsbCprLocal(uint32_t latCprI, uint32_t lonCprI, bool odd, double refLat, double refLon, double& lat, double& lon) {
    const double latCpr = latCprI / 131072.0, lonCpr = lonCprI / 131072.0;
    const double dLat = 360.0 / (odd ? 59.0 : 60.0);
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + modPos(refLat, dLat) / dLat - latCpr);
    const double rlat = dLat * (j + latCpr);
    if (rlat < -90.0 || rlat > 90.0) { return false; }
    const int ni = std::max(adsbNL(rlat) - (odd ? 1 : 0), 1);
    const double dLon = 360.0 / ni;
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + modPos(refLon, dLon) / dLon - lonCpr);
    double rlon = dLon * (m + lonCpr);
    if (rlon >= 180.0) { rlon -= 360.0; }
    lat = rlat;
    lon = rlon;
    return true;
}

// ------------------------------------------------------------------------------------------------------------
// Messages
// ------------------------------------------------------------------------------------------------------------
uint32_t AdsbTable::update(const uint8_t* msg, double nowSec, bool repaired) {
    if ((msg[0] >> 3) != 17) { return 0; }
    const uint32_t icao = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | msg[3];
    if (repaired && ac_.find(icao) == ac_.end()) { return 0; }   // a repaired frame may only update an aircraft we already know
    const int tc = msg[4] >> 3;
    AdsbAircraft& a = ac_[icao];
    a.icao = icao;
    a.lastSeenSec = nowSec;
    a.msgCount++;
    messages_++;

    if (tc >= 1 && tc <= 4) {
        static const char* charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
        const int c[8] = {msg[5] >> 2,
                          ((msg[5] & 3) << 4) | (msg[6] >> 4),
                          ((msg[6] & 15) << 2) | (msg[7] >> 6),
                          msg[7] & 63,
                          msg[8] >> 2,
                          ((msg[8] & 3) << 4) | (msg[9] >> 4),
                          ((msg[9] & 15) << 2) | (msg[10] >> 6),
                          msg[10] & 63};
        std::string cs;
        for (int i = 0; i < 8; ++i) { cs += charset[c[i]]; }
        while (!cs.empty() && cs.back() == ' ') { cs.pop_back(); }
        a.callsign = cs;
    } else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        const int ac12 = (msg[5] << 4) | (msg[6] >> 4);
        if (tc <= 18 && (ac12 & 0x10)) {   // barometric altitude with the 25 ft resolution bit
            const int n = ((ac12 & 0xFE0) >> 1) | (ac12 & 0x0F);
            a.altFt = n * 25 - 1000;
            a.hasAlt = true;
        }
        const bool odd = (msg[6] >> 2) & 1;
        const uint32_t latCpr = ((uint32_t)(msg[6] & 3) << 15) | ((uint32_t)msg[7] << 7) | (msg[8] >> 1);
        const uint32_t lonCpr = ((uint32_t)(msg[8] & 1) << 16) | ((uint32_t)msg[9] << 8) | msg[10];
        Cpr& c = cpr_[icao];
        const int f = odd ? 1 : 0;
        c.have[f] = true;
        c.lat[f] = latCpr;
        c.lon[f] = lonCpr;
        c.t[f] = nowSec;
        double lat = 0, lon = 0;
        bool ok = false;
        if (c.have[0] && c.have[1] && std::fabs(c.t[0] - c.t[1]) <= 10.0) {
            ok = adsbCprGlobal(c.lat[0], c.lon[0], c.lat[1], c.lon[1], c.t[1] > c.t[0], lat, lon);
        }
        if (!ok && a.hasPos) { ok = adsbCprLocal(latCpr, lonCpr, odd, a.lat, a.lon, lat, lon); }
        if (ok) {
            a.lat = lat;
            a.lon = lon;
            a.hasPos = true;
        }
    } else if (tc == 19) {
        const int st = msg[4] & 7;   // sub-type: the low 3 bits of the first ME byte
        if (st == 1 || st == 2) {
            const int ewSign = (msg[5] & 0x04) >> 2;
            const int ew = ((msg[5] & 0x03) << 8) | msg[6];
            const int nsSign = (msg[7] & 0x80) >> 7;
            const int ns = ((msg[7] & 0x7F) << 3) | (msg[8] >> 5);
            if (ew != 0 && ns != 0) {
                const double mult = st == 2 ? 4.0 : 1.0;
                const double vwe = (ewSign ? -1.0 : 1.0) * (ew - 1) * mult;
                const double vsn = (nsSign ? -1.0 : 1.0) * (ns - 1) * mult;
                a.speedKt = std::sqrt(vwe * vwe + vsn * vsn);
                double h = std::atan2(vwe, vsn) * 180.0 / kPi;
                if (h < 0) { h += 360.0; }
                a.headingDeg = h;
                a.hasVel = true;
            }
        } else if (st == 3 || st == 4) {
            const bool headingValid = msg[5] & 0x04;
            const int hdg = ((msg[5] & 0x03) << 8) | msg[6];
            const int as = ((msg[7] & 0x7F) << 3) | (msg[8] >> 5);
            if (headingValid && as != 0) {
                a.headingDeg = hdg * 360.0 / 1024.0;
                a.speedKt = (as - 1) * (st == 4 ? 4.0 : 1.0);
                a.hasVel = true;
            }
        }
        const int vr = ((msg[8] & 0x07) << 6) | (msg[9] >> 2);
        if (vr != 0) {
            a.vrFpm = (vr - 1) * 64 * ((msg[8] & 0x08) ? -1 : 1);
            a.hasVr = true;
        }
    }
    return icao;
}

std::vector<AdsbAircraft> AdsbTable::snapshot(int minMessages) const {
    std::vector<AdsbAircraft> v;
    v.reserve(ac_.size());
    for (const auto& kv : ac_) {
        if (kv.second.msgCount >= minMessages) { v.push_back(kv.second); }
    }
    std::sort(v.begin(), v.end(), [](const AdsbAircraft& x, const AdsbAircraft& y) { return x.lastSeenSec > y.lastSeenSec; });
    return v;
}

void AdsbTable::prune(double nowSec, double maxAgeSec) {
    for (auto it = ac_.begin(); it != ac_.end();) {
        if (nowSec - it->second.lastSeenSec > maxAgeSec) {
            cpr_.erase(it->first);
            it = ac_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string AdsbTable::describe(const AdsbAircraft& a) {
    char buf[256];
    std::string s;
    std::snprintf(buf, sizeof buf, "%06X  %-8s", a.icao, a.callsign.empty() ? "" : a.callsign.c_str());
    s = buf;
    if (a.hasAlt) {
        std::snprintf(buf, sizeof buf, "  %d ft", a.altFt);
        s += buf;
    }
    if (a.hasVel) {
        std::snprintf(buf, sizeof buf, "  %.0f kt  hdg %.0f", a.speedKt, a.headingDeg);
        s += buf;
    }
    if (a.hasVr) {
        std::snprintf(buf, sizeof buf, "  %+d fpm", a.vrFpm);
        s += buf;
    }
    if (a.hasPos) {
        std::snprintf(buf, sizeof buf, "  %.4f%c %.4f%c", std::fabs(a.lat), a.lat >= 0 ? 'N' : 'S', std::fabs(a.lon), a.lon >= 0 ? 'E' : 'W');
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "  (%ld msgs)", a.msgCount);
    s += buf;
    return s;
}

}  // namespace sdrstar
