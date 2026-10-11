// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/dsd_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

#include "nlohmann/json.hpp"

namespace sdrstar {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxLogLines = 400;
constexpr size_t kMaxEvents = 200;
constexpr size_t kMaxPcmPending = 96000;  // 1 s at 48 kHz s16

std::string stripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
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

std::string randomHex(size_t bytes) {
    std::string out;
    FILE* f = std::fopen("/dev/urandom", "rb");
    unsigned char b[64] = {0};
    size_t n = f ? std::fread(b, 1, std::min(bytes, sizeof b), f) : 0;
    if (f) { std::fclose(f); }
    if (n < bytes) {
        // extremely unlikely; fall back to time-seeded bytes (token only guards a loopback port)
        std::srand((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = n; i < bytes && i < sizeof b; ++i) { b[i] = (unsigned char)std::rand(); }
    }
    char hex[3];
    for (size_t i = 0; i < bytes && i < sizeof b; ++i) {
        std::snprintf(hex, sizeof hex, "%02x", b[i]);
        out += hex;
    }
    return out;
}

bool mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) {
                if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) { return false; }
            }
        }
        if (i < path.size()) { cur.push_back(path[i]); }
    }
    return true;
}

long long hexOrDec(const std::string& s, int base) { return std::strtoll(s.c_str(), nullptr, base); }

}  // namespace

const std::vector<DsdPreset>& dsdPresets() {
    static const std::vector<DsdPreset> p = {
        {"p25p1", "P25 Phase 1", {"-f1"}, 4800},
        {"p25p2", "P25 Phase 2", {"-f2"}, 6000},
        {"p25", "P25 (Phase 1 + 2, trunk helper)", {"-ft"}, 4800},
        {"dmr", "DMR", {"-fs"}, 4800},
        {"nxdn96", "NXDN 9600 (12.5 kHz)", {"-fn"}, 4800},
        {"nxdn48", "NXDN 4800 (6.25 kHz)", {"-fi"}, 2400},
        {"dpmr", "dPMR", {"-fm"}, 2400},
        {"dstar", "D-STAR", {"-fd"}, 4800},
        {"ysf", "Yaesu System Fusion", {"-fy"}, 4800},
        {"m17", "M17", {"-fz"}, 4800},
        {"provoice", "ProVoice", {"-fp"}, 9600},
        {"edacs", "EDACS", {"-fh"}, 9600},
        {"x2tdma", "X2-TDMA", {"-fx"}, 6000},
        {"auto", "Auto-detect (slow)", {"-fa"}, 0},
    };
    return p;
}

const DsdPreset* findDsdPreset(const std::string& id) {
    for (const auto& p : dsdPresets()) {
        if (id == p.id) { return &p; }
    }
    return nullptr;
}

int dsdOutputChannels(const std::vector<std::string>& modeFlags) {
    for (const auto& f : modeFlags) {
        if (f.size() >= 3 && f[0] == '-' && f[1] == 'f') {
            switch (f[2]) {
            case '1':   // P25 Phase 1
            case 'n':   // NXDN 9600
            case 'i':   // NXDN 4800
            case 'm':   // dPMR
            case 'd':   // D-STAR
            case 'y':   // Yaesu System Fusion
            case 'z':   // M17
            case 'p':   // ProVoice
            case 'h':   // EDACS / ProVoice
                return 1;
            default:    // -ft (trunking), -fs (DMR), -f2 (P25 Phase 2), -fx (X2-TDMA), -fa (auto): two slots
                return 2;
            }
        }
    }
    return 2;   // DSD-neo's own default
}

bool DsdSession::start(const Config& cfg, std::string& error) {
    if (proc_ && proc_->running()) {
        error = "decoder already running";
        return false;
    }
    cfg_ = cfg;
    stopping_ = false;
    audioChannels_ = cfg.audioChannels > 0 ? cfg.audioChannels : dsdOutputChannels(cfg.mode);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_ = DsdState();
        log_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(pcmMtx_);
        pcmPending_.clear();
    }
    audioCarryLen_ = 0;

    std::string work = cfg.workDir;
    if (work.empty()) {
        char tmpl[] = "/tmp/sdrstar-dsd-XXXXXX";
        if (!::mkdtemp(tmpl)) {
            error = "cannot create work dir";
            return false;
        }
        work = tmpl;
    } else if (!mkdirs(work)) {
        error = "cannot create work dir " + work;
        return false;
    }
    tokenDir_ = work;

    // API token (file mode 0600): a random secret so no other local process can drive our decoder.
    token_ = randomHex(16);
    std::string tokenPath = work + "/api.token." + randomHex(4);
    {
        int fd = ::open(tokenPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            error = "cannot write token file";
            return false;
        }
        std::string t = token_ + "\n";
        if (::write(fd, t.data(), t.size()) != (ssize_t)t.size()) {
            ::close(fd);
            error = "cannot write token file";
            return false;
        }
        ::close(fd);
    }
    tokenDir_ = tokenPath;  // remembered so stop() can unlink it

    apiPort_ = pickFreeLoopbackPort();
    if (apiPort_ <= 0) {
        error = "no free loopback port for decoder API";
        return false;
    }

    Subprocess::Options o;
    o.argv.push_back(cfg.exePath);
    o.argv.push_back("--frontend");
    o.argv.push_back("none");
    for (const auto& m : cfg.mode) { o.argv.push_back(m); }
    o.argv.push_back("-i");
    o.argv.push_back("-");
    o.argv.push_back("-s");
    o.argv.push_back(std::to_string(cfg.inputRate));
    o.argv.push_back("-o");
    o.argv.push_back(cfg.wantAudio ? "-" : "null");
    o.argv.push_back("--api");
    o.argv.push_back(std::to_string(apiPort_));
    o.argv.push_back("--api-token-file");
    o.argv.push_back(tokenPath);
    if (cfg.trunking) { o.argv.push_back("-T"); }
    if (cfg.rigctlPort > 0) {
        o.argv.push_back("-U");
        o.argv.push_back(std::to_string(cfg.rigctlPort));
    }
    for (const auto& e : cfg.extra) { o.argv.push_back(e); }
    o.cwd = work;
    o.env["HOME"] = work;
    o.env["XDG_CONFIG_HOME"] = work + "/config";
    o.env["XDG_CACHE_HOME"] = work + "/cache";
    o.env["XDG_DATA_HOME"] = work + "/data";
    o.env["NO_COLOR"] = "1";
    o.captureStdout = true;
    o.captureStderr = true;
    o.niceLevel = cfg.niceLevel;

    proc_ = std::make_unique<Subprocess>();
    proc_->setStdoutCallback([this](const uint8_t* d, size_t n) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            st_.audioBytesOut += (long long)n;
        }
        if (!audioCb_) { return; }
        // Decoded voice is interleaved stereo s16le at 8 kHz; keep whole frames across reads.
        const size_t frameBytes = 2 * (size_t)audioChannels_;
        std::vector<uint8_t> buf;
        buf.reserve(audioCarryLen_ + n);
        buf.insert(buf.end(), audioCarry_, audioCarry_ + audioCarryLen_);
        buf.insert(buf.end(), d, d + n);
        size_t usable = (buf.size() / frameBytes) * frameBytes;
        audioCarryLen_ = buf.size() - usable;
        if (audioCarryLen_ > sizeof audioCarry_) { audioCarryLen_ = 0; }
        std::memcpy(audioCarry_, buf.data() + usable, audioCarryLen_);
        if (usable) { audioCb_((const int16_t*)buf.data(), usable / frameBytes, audioChannels_, 8000); }
    });
    proc_->setStderrLineCallback([this](const std::string& line) { ingestLogLine(line); });
    proc_->setExitCallback([this](int code) {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.processRunning = false;
        if (!stopping_.load() && code != 0) { st_.lastError = "decoder exited with code " + std::to_string(code); }
    });

    if (!proc_->start(o, error)) {
        ::unlink(tokenPath.c_str());
        proc_.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.processRunning = true;
    }
    apiThread_ = std::thread([this] { apiLoop(); });
    return true;
}

void DsdSession::stop() {
    stopping_ = true;
    int fd = apiFd_.exchange(-1);
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); }
    if (proc_) { proc_->stop(1200); }
    if (apiThread_.joinable()) { apiThread_.join(); }
    if (fd >= 0) { ::close(fd); }
    proc_.reset();
    if (!tokenDir_.empty()) {
        ::unlink(tokenDir_.c_str());
        tokenDir_.clear();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    st_.processRunning = false;
    st_.apiConnected = false;
}

void DsdSession::pushPcm(const int16_t* samples, size_t count) {
    if (!proc_ || !proc_->running() || count == 0) { return; }
    std::lock_guard<std::mutex> lk(pcmMtx_);
    const uint8_t* b = (const uint8_t*)samples;
    pcmPending_.insert(pcmPending_.end(), b, b + count * 2);
    long w = proc_->writeStdin(pcmPending_.data(), pcmPending_.size());
    long long dropped = 0;
    if (w > 0) { pcmPending_.erase(pcmPending_.begin(), pcmPending_.begin() + w); }
    if (pcmPending_.size() > kMaxPcmPending) {
        // The decoder is not keeping up: drop the oldest data (whole samples), never block the DSP thread.
        size_t excess = (pcmPending_.size() - kMaxPcmPending) & ~(size_t)1;
        pcmPending_.erase(pcmPending_.begin(), pcmPending_.begin() + excess);
        dropped = (long long)excess;
    }
    std::lock_guard<std::mutex> lk2(mtx_);
    st_.pcmBytesIn += (long long)count * 2;
    st_.pcmBytesDropped += dropped;
}

DsdState DsdSession::state() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_;
}

std::vector<std::string> DsdSession::recentLog(size_t maxLines) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = std::min(maxLines, log_.size());
    return std::vector<std::string>(log_.end() - (long)n, log_.end());
}

bool DsdSession::apiSend(const std::string& s) {
    int fd = apiFd_.load();
    if (fd < 0) { return false; }
    std::string line = s + "\n";
#ifdef MSG_NOSIGNAL
    ssize_t n = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
#else
    ssize_t n = ::send(fd, line.data(), line.size(), 0);
#endif
    return n == (ssize_t)line.size();
}

void DsdSession::apiLoop() {
    int fd = -1;
    // The decoder needs a moment to open its API port.
    for (int attempt = 0; attempt < 100 && !stopping_.load(); ++attempt) {
        if (!proc_ || !proc_->running()) { return; }
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { return; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)apiPort_);
        if (::connect(fd, (sockaddr*)&a, sizeof a) == 0) { break; }
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!stopping_.load()) { st_.lastError = "could not connect to decoder API"; }
        return;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
    apiFd_ = fd;
    apiSend(json({{"id", 1}, {"cmd", "auth"}, {"token", token_}, {"params", {{"token", token_}}}}).dump());
    apiSend(json({{"id", 2}, {"cmd", "subscribe"}, {"topics", json::array({"all"})}, {"params", {{"topics", json::array({"all"})}}}}).dump());
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.apiConnected = true;
    }

    std::string buf;
    char tmp[16384];
    while (!stopping_.load()) {
        pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 200);
        if (r < 0 && errno != EINTR) { break; }
        if (r <= 0) { continue; }
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { break; }
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) { ingestApiLine(line); }
        }
        if (buf.size() > (1u << 20)) { buf.clear(); }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    st_.apiConnected = false;
}

void DsdSession::ingestApiLine(const std::string& line) {
    // The telemetry comes from another program: a field of an unexpected type (a number where a string is expected)
    // makes nlohmann throw, and an exception on a reader thread would terminate the whole app. Whatever was folded in
    // before the bad field stays; the rest of the line is ignored.
    try {
        ingestApiLineChecked(line);
    } catch (const json::exception&) {
    }
}

void DsdSession::ingestApiLineChecked(const std::string& line) {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return; }
    std::string type = j.value("type", "");
    std::lock_guard<std::mutex> lk(mtx_);

    if (type == "system") {
        if (j.contains("protocol") && j["protocol"].is_string()) { st_.protocol = j["protocol"].get<std::string>(); }
        st_.ccFreqHz = j.value("cc_freq_hz", st_.ccFreqHz);
        st_.vcFreqHz = j.value("vc_freq_hz", st_.vcFreqHz);
        st_.trunking = j.value("trunking", st_.trunking);
        if (j.contains("p25_neighbors") && j["p25_neighbors"].is_array()) {
            st_.neighbors.clear();
            for (const auto& n : j["p25_neighbors"]) {
                DsdNeighbor nb;
                nb.freqHz = n.value("freq_hz", 0.0);
                if (n.contains("wacn") && n["wacn"].is_number()) { nb.wacn = n["wacn"].get<long long>(); }
                if (n.contains("sysid") && n["sysid"].is_number()) { nb.sysid = n["sysid"].get<long long>(); }
                nb.rfss = n.value("rfss", -1);
                nb.site = n.value("site", -1);
                nb.currentCc = n.value("current_cc", false);
                st_.neighbors.push_back(nb);
            }
        }
    } else if (type == "status") {
        if (j.contains("protocol") && j["protocol"].is_string()) { st_.protocol = j["protocol"].get<std::string>(); }
        st_.trunkTuned = j.value("trunk_tuned", st_.trunkTuned);
        st_.trunking = j.value("trunking", st_.trunking);
        if (j.contains("message")) {
            const auto& m = j["message"];
            st_.notice = (m.is_object() && m.contains("text") && m["text"].is_string()) ? m["text"].get<std::string>() : "";
        }
    } else if (type == "call") {
        st_.calls.clear();
        if (j.contains("slots") && j["slots"].is_array()) {
            for (const auto& s : j["slots"]) {
                DsdCall c;
                c.slot = s.value("slot", 0);
                c.state = s.value("state", 0);
                c.name = s.value("name", "");
                c.tgText = s.value("tg_text", "");
                c.srcText = s.value("src_text", "");
                c.tgId = s.value("tg_id", 0LL);
                c.srcId = s.value("ota_source_id", 0LL);
                c.enc = s.value("enc", false);
                c.emergency = s.value("emergency", false);
                c.freqHz = s.value("frequency_hz", 0.0);
                c.elapsedMs = s.value("elapsed_ms", 0);
                st_.calls.push_back(c);
            }
        }
    } else if (type == "quality") {
        if (j.contains("cc_fec") && j["cc_fec"].is_object()) {
            const auto& q = j["cc_fec"];
            st_.quality.valid = q.value("valid", false);
            st_.quality.ccOk = q.value("ok", 0LL);
            st_.quality.ccErr = q.value("err", 0LL);
            st_.quality.ccOkPct = q.value("ok_pct", 0.0);
        }
        if (j.contains("voice_fec") && j["voice_fec"].is_object()) {
            const auto& q = j["voice_fec"];
            st_.quality.voiceOk = q.value("ok", 0LL);
            st_.quality.voiceErr = q.value("err", 0LL);
        }
    } else if (type == "metrics") {
        if (j.contains("snr_db") && j["snr_db"].is_number()) { st_.snrDb = j["snr_db"].get<double>(); }
    } else if (type == "event") {
        if (j.contains("row") && j["row"].is_object()) {
            const auto& r = j["row"];
            DsdEvent e;
            e.timeUnix = r.value("event_time", 0.0);
            e.sysid = r.value("sysid_string", "");
            e.text = r.value("event", "");
            e.srcId = r.value("source_id", 0LL);
            e.tgId = r.value("target_id", 0LL);
            e.freqHz = r.value("freq_hz", 0.0);
            e.systype = r.value("systype", 0);
            e.subtype = r.value("subtype", 0);
            st_.events.push_back(e);
            while (st_.events.size() > kMaxEvents) { st_.events.pop_front(); }
        }
    }
}

void DsdSession::ingestLogLine(const std::string& raw) {
    std::string line = stripAnsi(raw);
    // Real log lines are short.  std::regex recurses, so a multi-megabyte line could overflow the stack: cap it.
    if (line.size() > 4096) { line.resize(4096); }

    static const std::regex reSync(R"(Sync:\s*\+?([A-Za-z0-9\-]+))");
    static const std::regex reNac(R"(NAC/CC:\s*([0-9A-Fa-f]+))");
    static const std::regex reWacn(R"(WACN:\s*([0-9A-Fa-f]+))");
    static const std::regex reSys(R"(SYS:\s*([0-9A-Fa-f]+))");
    static const std::regex reRfss(R"(RFSS:\s*([0-9]+))");
    static const std::regex reSite(R"(Site:\s*([0-9]+))");
    static const std::regex reCc(R"(Color Code=([0-9]+))");
    static const std::regex reRan(R"(RAN[=: ]+([0-9]+))");

    // The decoder prints a lot of lines and most of them mention none of these fields.  A regular-expression search is
    // slow (microseconds per line, per field), so each one only runs when the plain text it needs is present, and all of
    // them run BEFORE the lock is taken (the GUI polls state() under the same lock every frame).
    struct Found {
        bool sync = false, nac = false, wacn = false, sys = false, rfss = false, site = false, cc = false, ran = false;
        std::string syncName;
        long long nacV = 0, wacnV = 0, sysV = 0, rfssV = 0, siteV = 0, ccV = 0, ranV = 0;
    } f;
    std::smatch m;
    if (line.find("Sync:") != std::string::npos && std::regex_search(line, m, reSync)) {
        f.sync = true;
        f.syncName = m[1].str();
    }
    if (line.find("NAC/CC:") != std::string::npos && std::regex_search(line, m, reNac)) { f.nac = true; f.nacV = hexOrDec(m[1].str(), 16); }
    if (line.find("WACN:") != std::string::npos && std::regex_search(line, m, reWacn)) { f.wacn = true; f.wacnV = hexOrDec(m[1].str(), 16); }
    if (line.find("SYS:") != std::string::npos && std::regex_search(line, m, reSys)) { f.sys = true; f.sysV = hexOrDec(m[1].str(), 16); }
    if (line.find("RFSS:") != std::string::npos && std::regex_search(line, m, reRfss)) { f.rfss = true; f.rfssV = hexOrDec(m[1].str(), 10); }
    if (line.find("Site:") != std::string::npos && std::regex_search(line, m, reSite)) { f.site = true; f.siteV = hexOrDec(m[1].str(), 10); }
    if (line.find("Color Code=") != std::string::npos && std::regex_search(line, m, reCc)) { f.cc = true; f.ccV = hexOrDec(m[1].str(), 10); }
    if (line.find("RAN") != std::string::npos && std::regex_search(line, m, reRan)) { f.ran = true; f.ranV = hexOrDec(m[1].str(), 10); }
    const bool isError = line.find("ERROR") != std::string::npos || line.find("Error") != std::string::npos;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        log_.push_back(line);
        while (log_.size() > kMaxLogLines) { log_.pop_front(); }
        if (f.sync) {
            st_.syncLines++;
            st_.lastSync = f.syncName;
        }
        if (f.nac) { st_.identity.nac = f.nacV; }
        if (f.wacn) { st_.identity.wacn = f.wacnV; }
        if (f.sys) { st_.identity.sysid = f.sysV; }
        if (f.rfss) { st_.identity.rfss = (int)f.rfssV; }
        if (f.site) { st_.identity.site = (int)f.siteV; }
        if (f.cc) { st_.identity.colorCode = (int)f.ccV; }
        if (f.ran) { st_.identity.ran = (int)f.ranV; }
        if (isError) { st_.lastError = line; }
    }
    if (logCb_) { logCb_(line); }  // outside the lock: UI code may call back into state()
}

}  // namespace sdrstar
