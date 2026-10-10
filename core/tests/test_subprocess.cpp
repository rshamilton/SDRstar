// SPDX-License-Identifier: GPL-3.0-or-later
//
// The process-safety promises of Subprocess / BoundedWriter, each tested for real:
//   * data goes in and out, and a child that ends is noticed;
//   * stop() really ends a child that ignores polite requests (SIGTERM);
//   * a child that never reads cannot block the (real-time) caller, memory use stays bounded;
//   * if the host application dies suddenly (SIGKILL), its children do not live on.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "sdrstar/bounded_writer.h"
#include "sdrstar/subprocess.h"
#include "test_framework.h"

using namespace sdrstar;
using Clock = std::chrono::steady_clock;

static double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// "gone" = no such process, or (Linux) a zombie that nobody has reaped yet
static bool processGone(int pid) {
    if (::kill(pid, 0) != 0) { return true; }
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (f && std::getline(f, line)) {
        size_t rp = line.rfind(')');
        if (rp != std::string::npos && rp + 2 < line.size() && line[rp + 2] == 'Z') { return true; }
    }
    return false;
}

TEST(data_round_trip_and_exit_are_observed) {
    Subprocess p;
    std::string out;
    std::atomic<int> exitCode{-99};
    p.setStdoutCallback([&](const uint8_t* d, size_t n) { out.append((const char*)d, n); });
    p.setExitCallback([&](int c) { exitCode = c; });
    Subprocess::Options o;
    o.argv = {"/bin/cat"};
    std::string err;
    CHECK(p.start(o, err));
    const char* msg = "hello decoder\n";
    CHECK_EQ(p.writeStdin(msg, std::strlen(msg)), (long)std::strlen(msg));
    p.closeStdin();   // EOF makes cat finish
    auto t0 = Clock::now();
    while (p.running() && since(t0) < 5.0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(!p.running());
    p.stop();
    CHECK_EQ(out, std::string("hello decoder\n"));
    CHECK_EQ(exitCode.load(), 0);
}

TEST(missing_or_unrunnable_programs_fail_cleanly) {
    Subprocess p;
    Subprocess::Options o;
    o.argv = {"/nonexistent/decoder"};
    std::string err;
    CHECK(!p.start(o, err));
    CHECK_CONTAINS(err, "cannot execute");
    Subprocess q;
    o.argv = {};
    CHECK(!q.start(o, err));
}

TEST(stop_kills_a_child_that_ignores_polite_requests) {
    Subprocess p;
    Subprocess::Options o;
    o.argv = {"/bin/sh", "-c", "trap '' TERM HUP INT; while :; do sleep 1; done"};
    std::string err;
    CHECK(p.start(o, err));
    const int pid = p.pid();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(p.running());
    auto t0 = Clock::now();
    p.stop(400);   // closes stdin, SIGTERM (ignored), then SIGKILL
    CHECK(since(t0) < 2.0);
    CHECK(!p.running());
    CHECK(processGone(pid));
    p.stop();   // idempotent
}

TEST(a_child_that_never_reads_cannot_block_the_caller_or_grow_memory) {
    Subprocess p;
    Subprocess::Options o;
    o.argv = {"/bin/sleep", "30"};
    std::string err;
    CHECK(p.start(o, err));
    BoundedWriter w(&p, 256 * 1024, 2);
    std::vector<uint8_t> chunk(1 << 20, 0x55);
    auto t0 = Clock::now();
    for (int i = 0; i < 64; ++i) { w.write(chunk.data(), chunk.size()); }   // 64 MB at a child that never reads
    const double took = since(t0);
    std::printf("    64 MB offered in %.3f s, dropped %lld bytes\n", took, w.dropped());
    CHECK(took < 3.0);
    CHECK(w.dropped() > 60LL * 1024 * 1024);   // all but the socket buffer and the queue was discarded, oldest first
    p.stop(300);
}

// The host application dying suddenly must not leave decoders running.
TEST(children_do_not_outlive_a_killed_host) {
    int pidPipe[2];
    CHECK_EQ(::pipe(pidPipe), 0);
    pid_t host = ::fork();
    CHECK(host >= 0);
    if (host == 0) {
        // --- the "application" ---
        ::close(pidPipe[0]);
        Subprocess* p = new Subprocess();   // never destroyed: the process is killed instead
        Subprocess::Options o;
        o.argv = {"/bin/cat"};
        std::string err;
        if (!p->start(o, err)) { _exit(3); }
        int cpid = p->pid();
        ssize_t w = ::write(pidPipe[1], &cpid, sizeof cpid);
        (void)w;
        for (;;) { ::sleep(1); }
    }
    ::close(pidPipe[1]);
    int childPid = -1;
    CHECK_EQ(::read(pidPipe[0], &childPid, sizeof childPid), (ssize_t)sizeof childPid);
    ::close(pidPipe[0]);
    CHECK(childPid > 1);
    CHECK(!processGone(childPid));   // alive while its host lives
    ::kill(host, SIGKILL);           // sudden death: no destructors, no stop()
    int st = 0;
    ::waitpid(host, &st, 0);
    auto t0 = Clock::now();
    while (!processGone(childPid) && since(t0) < 5.0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::printf("    decoder gone %.2f s after its host was killed\n", since(t0));
    CHECK(processGone(childPid));
    if (!processGone(childPid)) { ::kill(childPid, SIGKILL); }
}

TEST_MAIN("subprocess")
