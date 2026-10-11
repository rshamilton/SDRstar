// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small POSIX child-process helper used to run external decoders (dsd-neo, multimon-ng, rtl_433 ...).
//
// Design notes (these matter for a GUI app that must never hang or leak processes):
//  * stdin is a UNIX socketpair, not a pipe, so writes can use MSG_NOSIGNAL / SO_NOSIGPIPE and a dead
//    child can never raise SIGPIPE inside the host application.
//  * If the host dies (even by SIGKILL), the kernel closes the socket, the child sees EOF on stdin and exits.
//    (Verified by tests/test_subprocess.cpp.)
//  * Writes are non-blocking and bounded: when the child cannot keep up the oldest data is dropped
//    by the caller (see PcmFeeder) instead of ever blocking the SDR++ DSP thread.
//  * stop() sends SIGTERM, waits briefly, then SIGKILL.  The destructor always reaps the child.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdrstar {

class Subprocess {
public:
    struct Options {
        std::vector<std::string> argv;            // argv[0] is the executable path
        std::map<std::string, std::string> env;   // added to / overriding the inherited environment
        std::string cwd;                          // working directory ("" = inherit)
        bool captureStdout = true;
        bool captureStderr = true;
        bool feedStdin = true;
        int niceLevel = 0;                        // > 0: run the child at a lower CPU priority (0 = same as the host)
    };

    using DataCb = std::function<void(const uint8_t* data, size_t len)>;
    using LineCb = std::function<void(const std::string& line)>;

    Subprocess() = default;
    ~Subprocess();
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // Callbacks must be set before start().  They run on reader threads.
    void setStdoutCallback(DataCb cb) { onStdout_ = std::move(cb); }
    void setStderrLineCallback(LineCb cb) { onStderrLine_ = std::move(cb); }
    void setExitCallback(std::function<void(int exitCode)> cb) { onExit_ = std::move(cb); }

    bool start(const Options& opt, std::string& error);

    // Non-blocking.  Returns the number of bytes accepted (may be 0 when the socket buffer is full,
    // or -1 when the child has gone away).
    long writeStdin(const void* data, size_t len);
    void closeStdin();

    bool running() const { return running_.load(); }
    int pid() const { return pid_; }
    int exitCode() const { return exitCode_.load(); }

    void stop(int graceMs = 800);

private:
    void readerLoop(int fd, bool isStdout);
    void waitLoop();

    int pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
    int stderrFd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> exitCode_{-1};
    std::thread outThread_, errThread_, waitThread_;
    DataCb onStdout_;
    LineCb onStderrLine_;
    std::function<void(int)> onExit_;
    std::mutex stdinMtx_;
};

// Pick a free TCP port on loopback (small race window; callers retry on failure).
int pickFreeLoopbackPort();

}  // namespace sdrstar
