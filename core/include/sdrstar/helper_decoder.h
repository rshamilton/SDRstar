// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelperDecoder: runs a text-output decoder (multimon-ng for pagers / APRS / weather alerts, rtl_433 for 433/868/915 MHz
// ISM sensors) on a channel and collects what it prints.  Specs are data, so adding another decoder is a table entry.
//
//   Pcm22050 : channel -> FM discriminator -> 22.05 kHz s16 mono on stdin   (multimon-ng)
//   IqCf32   : channel at `iqRateHz` as complex float32 on stdin            (rtl_433)
#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sdrstar/adsb.h"
#include "sdrstar/bounded_writer.h"
#include "sdrstar/channel_pcm.h"
#include "sdrstar/radio_host.h"
#include "sdrstar/resampler.h"
#include "sdrstar/subprocess.h"

namespace sdrstar {

struct HelperSpec {
    enum Input { Pcm22050, IqCf32 };
    std::string id;         // "pager", "aprs", "eas", "ism"
    std::string title;      // "Pager messages (POCSAG / FLEX)"
    std::string exeName;    // "multimon-ng", "rtl_433"
    Input input = Pcm22050;
    double channelBwHz = 20000;
    double iqRateHz = 250000;           // IqCf32 only
    std::vector<std::string> args;      // "{FREQ}" -> centre frequency in Hz, "{RATE}" -> IQ rate in Hz
    std::string idleHint;               // shown while nothing has been decoded yet
    bool native = false;                // decoded inside SDRstar (no helper program): "adsb"
    std::string countNoun = "received"; // "3 received", "12 aircraft"
    double fixedFreqHz = 0;             // nonzero: always listens exactly here (ADS-B at 1090 MHz), wherever the cursor is
};

const std::vector<HelperSpec>& helperSpecs();
const HelperSpec* findHelperSpec(const std::string& id);

struct HelperLine {
    double tSec = 0;
    std::string text;   // human-readable (rtl_433 JSON is condensed to "Model  id=..  temperature_C=..")
    std::string raw;
    int repeat = 1;     // the same message heard again within a few seconds (sensors send 3 copies)
};

// Condense one rtl_433 JSON line into a readable one; other text is returned unchanged.
std::string summarizeHelperLine(const std::string& line);

class HelperDecoder {
public:
    HelperDecoder() = default;
    ~HelperDecoder() { stop(); }
    HelperDecoder(const HelperDecoder&) = delete;
    HelperDecoder& operator=(const HelperDecoder&) = delete;

    // Private working directory + HOME for the helper (created if missing), so it cannot read or write the user's files
    // or pick up a stray config file (rtl_433 reads rtl_433.conf from its working directory and ~/.config).
    void setWorkDir(const std::string& dir) { workDir_ = dir; }

    // Process only (no radio): feed with feedPcm48k() / feedIq().  Used by tests and by the probe fan-out.
    bool startProcess(const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error);
    // Process + hidden channel on a radio host.
    bool start(RadioHost* host, const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error);
    void stop();
    bool running() const { return native_ ? true : (proc_ && proc_->running()); }

    // Move the running decoder to another ABSOLUTE frequency without losing the messages collected so far (the text
    // decoders follow the user's cursor).  A decoder whose frequency is fixed (ADS-B) ignores the request and returns true.
    // Returns false (error filled in) if the frequency is not inside the receiver window or the decoder could not restart;
    // on a failed move the decoder keeps listening where it was, unless it had to be restarted and that failed.
    bool retarget(double freqHz, std::string& error);

    void feedPcm48k(const int16_t* pcm, size_t n);       // Pcm22050 specs
    void feedIq(const cf* iq, size_t n);                 // IqCf32 specs

    std::vector<HelperLine> lines(size_t maxLines = 300) const;
    std::vector<std::string> rawLines() const;
    long long messageCount() const { return native_ ? (long long)adsbCount_.load() : count_.load(); }
    long long droppedBytes() const { return native_ ? 0 : writer_.dropped(); }
    const HelperSpec& spec() const { return spec_; }
    double freqHz() const { return freqHz_; }
    std::string lastError() const;

private:
    void onStdout(const uint8_t* d, size_t n);
    void onLine(const std::string& line);

    HelperSpec spec_;
    std::string workDir_;
    std::string exePath_;
    double freqHz_ = 0;
    std::unique_ptr<Subprocess> proc_;
    BoundedWriter writer_;
    std::unique_ptr<PolyphaseResampler> resampler_;
    std::unique_ptr<ChannelPcmTap> tap_;                 // PCM specs on a host
    RadioHost* host_ = nullptr;
    int iqChannel_ = -1;                                 // IQ specs on a host
    std::vector<int16_t> rsBuf_;
    FILE* trace_ = nullptr;                              // dev aid: SDRSTAR_HELPER_TRACE=<file> keeps what was fed
    size_t traceBytes_ = 0;

    // native ADS-B decoder
    bool native_ = false;
    std::unique_ptr<ModeSDemod> adsbDemod_;
    AdsbTable adsbTable_;           // guarded by mtx_
    std::atomic<size_t> adsbCount_{0};
    double lastTableRefresh_ = 0;
    void refreshAdsbLines();

    mutable std::mutex mtx_;
    std::vector<HelperLine> lines_;
    std::string partial_;
    std::string lastErr_;
    std::atomic<long long> count_{0};
    double t0_ = 0;
};

}  // namespace sdrstar
