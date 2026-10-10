// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/helper_decoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "nlohmann/json.hpp"

namespace sdrstar {

namespace {
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr size_t kMaxLines = 600;

std::string replaceAll(std::string s, const std::string& a, const std::string& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) {
        s.replace(p, a.size(), b);
        p += b.size();
    }
    return s;
}
}  // namespace

std::string summarizeHelperLineChecked(const std::string& line);

// nlohmann::json sorts keys; rtl_433 lists the most useful fields first (id, channel, temperature ...), so recover the
// order the sensor decoder printed them in by scanning the top level of the (already validated) JSON text.
static std::vector<std::string> topLevelKeysInOrder(const std::string& s) {
    std::vector<std::string> keys;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            std::string str;
            size_t j = i + 1;
            for (; j < s.size() && s[j] != '"'; ++j) {
                if (s[j] == '\\' && j + 1 < s.size()) { ++j; }
                str += s[j];
            }
            size_t k = j + 1;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) { ++k; }
            if (depth == 1 && k < s.size() && s[k] == ':') { keys.push_back(str); }
            i = j;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return keys;
}

const std::vector<HelperSpec>& helperSpecs() {
    static const std::vector<HelperSpec> v = [] {
        std::vector<HelperSpec> s;
        HelperSpec p;
        p.id = "pager";
        p.title = "Pager messages (POCSAG / FLEX)";
        p.exeName = "multimon-ng";
        p.input = HelperSpec::Pcm22050;
        p.channelBwHz = 20000;
        p.args = {"-t", "raw", "-q", "-a", "POCSAG512", "-a", "POCSAG1200", "-a", "POCSAG2400", "-a", "FLEX", "-a", "FLEX_NEXT", "-"};
        p.idleHint = "Waiting for a pager message...";
        s.push_back(p);
        HelperSpec a = p;
        a.id = "aprs";
        a.title = "APRS packets (AFSK 1200)";
        a.channelBwHz = 15000;
        a.args = {"-t", "raw", "-q", "-A", "-a", "AFSK1200", "-"};
        a.idleHint = "Waiting for an APRS packet...";
        s.push_back(a);
        HelperSpec e = p;
        e.id = "eas";
        e.title = "Weather / emergency alerts (EAS-SAME)";
        e.channelBwHz = 15000;
        e.args = {"-t", "raw", "-q", "-a", "EAS", "-"};
        e.idleHint = "Waiting for an alert (they are rare)...";
        s.push_back(e);
        HelperSpec i;
        i.id = "ism";
        i.title = "ISM sensors (433 / 868 / 915 MHz)";
        i.exeName = "rtl_433";
        i.input = HelperSpec::IqCf32;
        i.channelBwHz = 220000;
        i.iqRateHz = 250000;
        i.args = {"-r", "cf32:-", "-s", "{RATE}", "-f", "{FREQ}", "-F", "json"};
        i.idleHint = "Waiting for a sensor transmission (many only send every 30-60 s)...";
        s.push_back(i);
        HelperSpec ad;
        ad.id = "adsb";
        ad.title = "Aircraft (ADS-B, 1090 MHz)";
        ad.exeName = "";
        ad.native = true;
        ad.input = HelperSpec::IqCf32;
        ad.channelBwHz = 1.9e6;   // the 0.5 us PPM pulses need about this much; narrower and bits become undecidable
        ad.iqRateHz = 2.0e6;
        ad.fixedFreqHz = 1090e6;
        ad.countNoun = "aircraft";
        ad.idleHint = "Waiting for aircraft (needs an antenna with a view of the sky; range is line of sight)...";
        s.push_back(ad);
        return s;
    }();
    return v;
}

const HelperSpec* findHelperSpec(const std::string& id) {
    for (const auto& s : helperSpecs()) {
        if (s.id == id) { return &s; }
    }
    return nullptr;
}

std::string summarizeHelperLine(const std::string& line) {
    // The line comes from another program: any unexpected JSON shape must degrade to "show the raw line", never throw.
    try {
        return summarizeHelperLineChecked(line);
    } catch (const nlohmann::json::exception&) {
        return line;
    }
}

// A sensor line is a few hundred bytes of shallow JSON.  Refuse to parse anything else: the JSON library's serializer
// recurses, so a hostile line with tens of thousands of nesting levels could overflow the stack.
static bool plausibleSensorJson(const std::string& s) {
    if (s.size() > 32768) { return false; }
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (c == '\\') { ++i; }
            else if (c == '"') { inString = false; }
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 16) { return false; }
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

std::string summarizeHelperLineChecked(const std::string& line) {
    if (line.empty() || line[0] != '{') { return line; }
    if (!plausibleSensorJson(line)) { return line; }
    nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return line; }
    std::string out = j.value("model", std::string("device"));
    for (const std::string& k : topLevelKeysInOrder(line)) {
        if (k == "model" || k == "time" || k == "mic" || k == "protocol" || !j.contains(k)) { continue; }
        out += "  " + k + "=";
        const auto& v = j[k];
        if (v.is_string()) {
            out += v.get<std::string>();
        } else if (v.is_number_float()) {
            char b[32];
            std::snprintf(b, sizeof b, "%.3f", v.get<double>());
            std::string s = b;
            while (s.size() > 1 && s.back() == '0') { s.pop_back(); }
            if (!s.empty() && s.back() == '.') { s.pop_back(); }
            out += s;
        } else {
            out += v.dump();
        }
    }
    return out;
}

bool HelperDecoder::startProcess(const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error) {
    stop();
    spec_ = spec;
    freqHz_ = spec.fixedFreqHz > 0 ? spec.fixedFreqHz : freqHz;
    native_ = spec.native;
    if (native_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            lines_.clear();
            adsbTable_ = AdsbTable();
        }
        adsbCount_ = 0;
        count_ = 0;
        lastTableRefresh_ = 0;
        t0_ = nowSec();
        adsbDemod_ = std::make_unique<ModeSDemod>([this](const uint8_t* m, int, bool repaired) {
            std::lock_guard<std::mutex> lk(mtx_);
            adsbTable_.update(m, nowSec() - t0_, repaired);
        });
        return true;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        lines_.clear();
        partial_.clear();
        lastErr_.clear();
    }
    count_ = 0;
    t0_ = nowSec();
    Subprocess::Options o;
    o.argv.push_back(exePath);
    char buf[64];
    for (const auto& a : spec.args) {
        std::string x = a;
        std::snprintf(buf, sizeof buf, "%.0f", spec.iqRateHz);
        x = replaceAll(x, "{RATE}", buf);
        std::snprintf(buf, sizeof buf, "%.0f", freqHz);
        x = replaceAll(x, "{FREQ}", buf);
        o.argv.push_back(x);
    }
    o.captureStdout = true;
    o.captureStderr = true;
    if (!workDir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(workDir_ + "/config", ec);
        std::filesystem::create_directories(workDir_ + "/cache", ec);
        std::filesystem::create_directories(workDir_ + "/data", ec);
        o.cwd = workDir_;
        o.env["HOME"] = workDir_;
        o.env["XDG_CONFIG_HOME"] = workDir_ + "/config";
        o.env["XDG_CACHE_HOME"] = workDir_ + "/cache";
        o.env["XDG_DATA_HOME"] = workDir_ + "/data";
        o.env["NO_COLOR"] = "1";
    }
    proc_ = std::make_unique<Subprocess>();
    proc_->setStdoutCallback([this](const uint8_t* d, size_t n) { onStdout(d, n); });
    proc_->setStderrLineCallback([this](const std::string& l) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!l.empty()) { lastErr_ = l; }
    });
    if (!proc_->start(o, error)) {
        proc_.reset();
        return false;
    }
    writer_.setProcess(proc_.get());
    // PCM: 0.4 s of 22 kHz s16 may queue; IQ: 0.5 s of complex float at the channel rate.  Beyond that the oldest samples go.
    if (spec.input == HelperSpec::IqCf32) {
        writer_.configure((size_t)(spec.iqRateHz * 8 * 0.5), 8);
    } else {
        writer_.configure(262144, 2);
    }
    if (const char* tp = std::getenv("SDRSTAR_HELPER_TRACE")) {
        trace_ = std::fopen(tp, "wb");
        traceBytes_ = 0;
    }
    resampler_.reset();
    if (spec.input == HelperSpec::Pcm22050) { resampler_ = std::make_unique<PolyphaseResampler>(48000, 22050); }
    return true;
}

bool HelperDecoder::start(RadioHost* host, const HelperSpec& spec, const std::string& exePath, double freqHz, std::string& error) {
    if (!startProcess(spec, exePath, freqHz, error)) { return false; }
    freqHz = freqHz_;   // a decoder with a fixed frequency (ADS-B) ignores where the cursor was
    host_ = host;
    if (spec.input == HelperSpec::Pcm22050) {
        tap_ = std::make_unique<ChannelPcmTap>();
        if (!tap_->open(host, freqHz, spec.channelBwHz, error)) {
            tap_.reset();
            stop();
            return false;
        }
        tap_->addSink([this](const int16_t* p, size_t n) { feedPcm48k(p, n); });
    } else {
        iqChannel_ = host->openChannel(freqHz, spec.channelBwHz, [this](const cf* x, size_t n) { feedIq(x, n); }, error, spec.iqRateHz);
        if (iqChannel_ < 0) {
            stop();
            return false;
        }
    }
    return true;
}

void HelperDecoder::stop() {
    if (tap_) {
        tap_->clearSinks();
        tap_.reset();
    }
    if (iqChannel_ >= 0 && host_) {
        host_->closeChannel(iqChannel_);
        iqChannel_ = -1;
    }
    if (proc_) {
        proc_->stop(800);
        proc_.reset();
    }
    writer_.setProcess(nullptr);
    adsbDemod_.reset();
    native_ = false;
    if (trace_) {
        std::fclose(trace_);
        trace_ = nullptr;
    }
}

void HelperDecoder::feedPcm48k(const int16_t* pcm, size_t n) {
    if (!proc_ || !resampler_) { return; }
    rsBuf_.clear();
    resampler_->process(pcm, n, rsBuf_);
    if (trace_ && traceBytes_ < (64u << 20)) {
        std::fwrite(rsBuf_.data(), 2, rsBuf_.size(), trace_);
        traceBytes_ += rsBuf_.size() * 2;
    }
    writer_.write(rsBuf_.data(), rsBuf_.size() * 2);
}

void HelperDecoder::refreshAdsbLines() {
    std::lock_guard<std::mutex> lk(mtx_);
    const double now = nowSec() - t0_;
    adsbTable_.prune(now, 90.0);
    std::vector<HelperLine> v;
    for (const auto& a : adsbTable_.snapshot()) {
        HelperLine l;
        l.tSec = a.lastSeenSec;
        l.text = AdsbTable::describe(a);
        l.raw = l.text;
        v.push_back(l);
        if (v.size() >= 40) { break; }
    }
    lines_ = v;
    adsbCount_ = v.size();
}

void HelperDecoder::feedIq(const cf* iq, size_t n) {
    if (native_) {
        if (!adsbDemod_) { return; }
        adsbDemod_->feed(iq, n);
        const double now = nowSec();
        if (now - lastTableRefresh_ > 0.5) {
            lastTableRefresh_ = now;
            refreshAdsbLines();
        }
        return;
    }
    if (!proc_) { return; }
    if (trace_ && traceBytes_ < (64u << 20)) {
        std::fwrite(iq, sizeof(cf), n, trace_);
        traceBytes_ += n * sizeof(cf);
    }
    writer_.write(iq, n * sizeof(cf));
}

void HelperDecoder::onStdout(const uint8_t* d, size_t n) {
    std::string chunk((const char*)d, n);
    std::vector<std::string> done;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        partial_ += chunk;
        size_t pos;
        while ((pos = partial_.find('\n')) != std::string::npos) {
            std::string l = partial_.substr(0, pos);
            partial_.erase(0, pos + 1);
            if (!l.empty() && l.back() == '\r') { l.pop_back(); }
            if (!l.empty()) { done.push_back(l); }
        }
        if (partial_.size() > 65536) { partial_.clear(); }
    }
    for (const auto& l : done) { onLine(l); }
}

void HelperDecoder::onLine(const std::string& line) {
    HelperLine hl;
    hl.tSec = nowSec() - t0_;
    hl.raw = line;
    hl.text = summarizeHelperLine(line);
    std::lock_guard<std::mutex> lk(mtx_);
    if (!lines_.empty() && lines_.back().text == hl.text && hl.tSec - lines_.back().tSec < 6.0) {
        lines_.back().repeat++;   // a sensor's repeated copies (or a pager re-send) are one message heard several times
        lines_.back().tSec = hl.tSec;
        return;
    }
    lines_.push_back(hl);
    while (lines_.size() > kMaxLines) { lines_.erase(lines_.begin()); }
    count_++;
}

std::vector<HelperLine> HelperDecoder::lines(size_t maxLines) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = std::min(maxLines, lines_.size());
    return std::vector<HelperLine>(lines_.end() - (long)n, lines_.end());
}

std::vector<std::string> HelperDecoder::rawLines() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> v;
    for (const auto& l : lines_) { v.push_back(l.raw); }
    return v;
}

std::string HelperDecoder::lastError() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lastErr_;
}

}  // namespace sdrstar
