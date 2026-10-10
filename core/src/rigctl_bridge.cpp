// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/rigctl_bridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sdrstar {

namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
}  // namespace

bool RigctlBridge::start(const Handlers& h, int& port, std::string& error) {
    if (running_.load()) {
        error = "rigctl bridge already running";
        return false;
    }
    h_ = h;
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback ONLY
    a.sin_port = htons((uint16_t)port);
    if (::bind(s, (sockaddr*)&a, sizeof a) != 0 || ::listen(s, 4) != 0) {
        error = std::string("bind/listen: ") + std::strerror(errno);
        ::close(s);
        return false;
    }
    socklen_t len = sizeof a;
    ::getsockname(s, (sockaddr*)&a, &len);
    port_ = ntohs(a.sin_port);
    port = port_;
    listenFd_ = s;
    running_ = true;
    thread_ = std::thread([this] { serverLoop(); });
    return true;
}

void RigctlBridge::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) { thread_.join(); }
        return;
    }
    if (thread_.joinable()) { thread_.join(); }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    std::vector<std::thread> ts;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ts.swap(clientThreads_);
    }
    for (auto& t : ts) {
        if (t.joinable()) { t.join(); }
    }
}

void RigctlBridge::serverLoop() {
    while (running_.load()) {
        pollfd pfd{listenFd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, 200);
        if (r <= 0) { continue; }
        sockaddr_in ca{};
        socklen_t cl = sizeof ca;
        int c = ::accept(listenFd_, (sockaddr*)&ca, &cl);
        if (c < 0) { continue; }
        // Defence in depth: only loopback peers are served even if the bind were ever changed.
        if (ntohl(ca.sin_addr.s_addr) != INADDR_LOOPBACK) {
            ::close(c);
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        std::lock_guard<std::mutex> lk(mtx_);
        clientThreads_.emplace_back([this, c] { clientLoop(c); });
    }
}

void RigctlBridge::clientLoop(int fd) {
    clients_++;
    std::string buf;
    char tmp[512];
    bool close = false;
    while (running_.load() && !close) {
        pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 200);
        if (r < 0 && errno != EINTR) { break; }
        if (r <= 0) { continue; }
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { break; }
        buf.append(tmp, (size_t)n);
        if (buf.size() > 8192) { break; }  // garbage / abuse
        size_t pos;
        while (!close && (pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (line.empty()) { continue; }
            std::string reply = handleLine(line, close);
            if (!reply.empty()) {
                if (::send(fd, reply.data(), reply.size(), kSendFlags) < 0) {
                    close = true;
                }
            }
        }
    }
    ::close(fd);
    clients_--;
}

std::string RigctlBridge::handleLine(const std::string& line, bool& closeConn) {
    closeConn = false;
    // hamlib allows an optional leading '+' / ';' extended-response prefix; we do not use it.
    std::string l = line;
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) { i++; }
    if (i >= l.size()) { return ""; }
    char cmd = l[i];
    std::string rest = l.substr(i + 1);
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) { return std::string(); }
        size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    };
    rest = trim(rest);
    if (h_.log) { h_.log("rigctl< " + l); }

    auto reply = [&](const std::string& r) {
        if (h_.log) {
            std::string t = r;
            for (auto& ch : t) { if (ch == '\n') { ch = '|'; } }
            h_.log("rigctl> " + t);
        }
        return r;
    };

    switch (cmd) {
    case 'f': {
        double hz = h_.getFrequency ? h_.getFrequency() : 0.0;
        char b[64];
        std::snprintf(b, sizeof b, "%lld\n", (long long)std::llround(hz));
        return reply(b);
    }
    case 'F': {
        char* end = nullptr;
        double hz = std::strtod(rest.c_str(), &end);
        if (rest.empty() || end == rest.c_str() || !(hz > 0.0) || !std::isfinite(hz) || hz > 1e12) {
            return reply("RPRT -1\n");
        }
        bool ok = h_.setFrequency ? h_.setFrequency(hz) : false;
        return reply(ok ? "RPRT 0\n" : "RPRT -1\n");
    }
    case 'm': {
        std::string mode = h_.getMode ? h_.getMode() : "FM";
        int pb = h_.getPassband ? h_.getPassband() : 12500;
        return reply(mode + "\n" + std::to_string(pb) + "\n");
    }
    case 'M':
        return reply("RPRT 0\n");  // accept (we always run an FM discriminator for digital voice)
    case 'q':
    case 'Q':
        closeConn = true;
        return "";
    default:
        return reply("RPRT -4\n");  // RIG_ENIMPL
    }
}

}  // namespace sdrstar
