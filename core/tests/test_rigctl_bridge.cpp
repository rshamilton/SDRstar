// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include "sdrstar/rigctl_bridge.h"
#include "test_framework.h"

using namespace sdrstar;

static std::string rpc(int fd, const std::string& cmd) {
    ::send(fd, cmd.data(), cmd.size(), 0);
    char buf[256];
    ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) { return ""; }
    return std::string(buf, (size_t)n);
}

static int connectTo(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);  // a hung server must fail the test, not hang it
    return fd;
}

TEST(handle_line_semantics) {
    RigctlBridge b;
    std::atomic<double> freq{851012500};
    RigctlBridge::Handlers h;
    h.getFrequency = [&] { return freq.load(); };
    h.setFrequency = [&](double hz) { if (hz > 2e9) { return false; } freq = hz; return true; };
    int port = 0; std::string err;
    CHECK(b.start(h, port, err));
    bool close = false;
    CHECK_EQ(b.handleLine("f", close), std::string("851012500\n"));
    CHECK_EQ(b.handleLine("F 856212500", close), std::string("RPRT 0\n"));
    CHECK_EQ(b.handleLine("f", close), std::string("856212500\n"));
    CHECK_EQ(b.handleLine("F 3000000000", close), std::string("RPRT -1\n"));   // handler refused
    CHECK_EQ(b.handleLine("F abc", close), std::string("RPRT -1\n"));           // malformed
    CHECK_EQ(b.handleLine("F -5", close), std::string("RPRT -1\n"));            // negative
    CHECK_EQ(b.handleLine("F", close), std::string("RPRT -1\n"));               // missing arg
    CHECK_EQ(b.handleLine("m", close), std::string("FM\n12500\n"));
    CHECK_EQ(b.handleLine("M FM 12500", close), std::string("RPRT 0\n"));
    CHECK_EQ(b.handleLine("\\dump_state", close), std::string("RPRT -4\n"));    // unknown -> ENIMPL
    CHECK(!close);
    CHECK_EQ(b.handleLine("q", close), std::string(""));
    CHECK(close);
    b.stop();
}

TEST(tcp_roundtrip_and_loopback_only) {
    RigctlBridge b;
    std::atomic<double> freq{100e6};
    RigctlBridge::Handlers h;
    h.getFrequency = [&] { return freq.load(); };
    h.setFrequency = [&](double hz) { freq = hz; return true; };
    int port = 0; std::string err;
    CHECK(b.start(h, port, err));
    CHECK(port > 0);
    int fd = connectTo(port);
    CHECK(fd >= 0);
    CHECK_EQ(rpc(fd, "f\n"), std::string("100000000\n"));
    CHECK_EQ(rpc(fd, "F 162550000\n"), std::string("RPRT 0\n"));
    CHECK_NEAR(freq.load(), 162550000, 0.5);
    // Two commands in one packet are answered in order (one reply per send).
    ::send(fd, "f\nm\n", 4, 0);
    char buf[128]; std::string got;
    // Replies may arrive coalesced or separately; read until both are in or the 2 s timeout fires.
    while (got.find("FM\n12500\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { break; }
        got.append(buf, n);
    }
    CHECK_CONTAINS(got, "162550000\n");
    CHECK_CONTAINS(got, "FM\n");
    ::close(fd);
    b.stop();
}

TEST_MAIN("rigctl_bridge")
