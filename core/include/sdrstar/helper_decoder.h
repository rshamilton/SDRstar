// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelperDecoder: runs a text-output decoder (multimon-ng for pagers / APRS / weather alerts, rtl_433 for 433/868/915 MHz
// ISM sensors) on a channel and collects what it prints.  Specs are data, so adding another decoder is a table entry.
//
//   Pcm22050 : channel -> FM discriminator -> 22.05 kHz s16 mono on stdin   (multimon-ng)
//   IqCf32   : channel at `iqRateHz` as complex float32 on stdin            (rtl_433)
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
};

const std::vector<HelperSpec>& helperSpecs();
const HelperSpec* findHelperSpec(const std::string& id);

struct HelperLine {
    double tSec = 0;
    std::string text;   // human-readable (rtl_433 JSON is condensed to "Model  id=..  temperature_C=..")
    std::string raw;
};

// Condense one rtl_433 JSON line into a readable one; other text is returned unchanged.
std::string summarizeHelperLine(const std::string& line);

class HelperDecoder {
public:
    HelperDecoder() = default;
    ~HelperDecoder() { stop(); }
    HelperDecoder(const HelperDecoder&) = delete;
    HelperDecoder& operator=(const HelperDecoder&) = delete;

    // Process only (no radio): feed with feedPcm48k() / feedIq().  Used by tests and by the probe fan-out.
    bool startProcess(const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error);
    // Process + hidden channel on a radio host.
    bool start(RadioHost* host, const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error);
    void stop();
    bool running() const { return proc_ && proc_->running(); }

    void feedPcm48k(const int16_t* pcm, size_t n);       // Pcm22050 specs
    void feedIq(const cf* iq, size_t n);                 // IqCf32 specs

    std::vector<HelperLine> lines(size_t maxLines = 300) const;
    std::vector<std::string> rawLines() const;
    long long messageCount() const { return count_.load(); }
    long long droppedBytes() const { return writer_.dropped(); }
    const HelperSpec& spec() const { return spec_; }
    double freqHz() const { return freqHz_; }
    std::string lastError() const;

private:
    void onStdout(const uint8_t* d, size_t n);
    void onLine(const std::string& line);

    HelperSpec spec_;
    double freqHz_ = 0;
    std::unique_ptr<Subprocess> proc_;
    BoundedWriter writer_;
    std::unique_ptr<PolyphaseResampler> resampler_;
    std::unique_ptr<ChannelPcmTap> tap_;                 // PCM specs on a host
    RadioHost* host_ = nullptr;
    int iqChannel_ = -1;                                 // IQ specs on a host
    std::vector<int16_t> rsBuf_;

    mutable std::mutex mtx_;
    std::vector<HelperLine> lines_;
    std::string partial_;
    std::string lastErr_;
    std::atomic<long long> count_{0};
    double t0_ = 0;
};

}  // namespace sdrstar
