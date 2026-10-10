// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal hamlib "rigctl" TCP server.
//
// DSD-neo (like DSD-FME) follows trunked systems by *asking the SDR front end to retune* over the rigctl
// protocol.  SDRstar plays the role of that front end: when the decoder says "go to 856.2125 MHz" this
// bridge hands the request to the window planner, which decides whether to just move a software channel
// inside the current capture window, retune the hardware, or refuse and explain why.
//
// Only the handful of commands DSD-neo actually uses are implemented:
//     f            get frequency        -> "<hz>\n"
//     F <hz>       set frequency        -> "RPRT 0\n" | "RPRT -1\n"
//     m            get mode             -> "<mode>\n<passband>\n"
//     M <mode> <pb>  set mode           -> "RPRT 0\n"
//     q / Q        quit
// Every reply is sent with ONE send() call because the DSD-neo client reads each reply with one recv().
//
// The listener binds to 127.0.0.1 only.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdrstar {

class RigctlBridge {
public:
    struct Handlers {
        // Return true to accept the new frequency (RPRT 0), false to refuse (RPRT -1).
        std::function<bool(double hz)> setFrequency;
        std::function<double()> getFrequency;
        std::function<std::string()> getMode = [] { return std::string("FM"); };
        std::function<int()> getPassband = [] { return 12500; };
        std::function<void(const std::string&)> log;  // optional protocol trace
    };

    RigctlBridge() = default;
    ~RigctlBridge() { stop(); }
    RigctlBridge(const RigctlBridge&) = delete;
    RigctlBridge& operator=(const RigctlBridge&) = delete;

    // Starts listening on 127.0.0.1:<port>.  Pass 0 to pick a free port; the chosen port is returned in `port`.
    bool start(const Handlers& h, int& port, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }
    int clientCount() const { return clients_.load(); }

    // Parses and executes ONE command line (no trailing newline), returning the exact bytes to send back.
    // Exposed for unit tests.
    std::string handleLine(const std::string& line, bool& closeConn);

private:
    void serverLoop();
    void clientLoop(int fd);

    Handlers h_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
    std::thread thread_;
    std::mutex mtx_;
    std::vector<std::thread> clientThreads_;
};

}  // namespace sdrstar
