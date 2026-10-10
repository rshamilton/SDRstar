// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdrstar/subprocess.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#ifdef __APPLE__
// `environ` cannot be linked from a dylib/plug-in on macOS (see environ(7)); ask the runtime instead.
#include <crt_externs.h>
#define SDRSTAR_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define SDRSTAR_ENVIRON environ
#endif

namespace sdrstar {

namespace {

void setCloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) { fcntl(fd, F_SETFD, fl | FD_CLOEXEC); }
}

void setNonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) { fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

int pickFreeLoopbackPort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { return -1; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = -1;
    if (::bind(s, (sockaddr*)&a, sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (::getsockname(s, (sockaddr*)&a, &len) == 0) { port = ntohs(a.sin_port); }
    }
    ::close(s);
    return port;
}

Subprocess::~Subprocess() { stop(300); }

bool Subprocess::start(const Options& opt, std::string& error) {
    if (running_.load()) {
        error = "already running";
        return false;
    }
    if (opt.argv.empty() || opt.argv[0].empty()) {
        error = "empty command";
        return false;
    }
    if (access(opt.argv[0].c_str(), X_OK) != 0) {
        error = "cannot execute '" + opt.argv[0] + "': " + std::strerror(errno);
        return false;
    }

    // Everything the child needs is prepared BEFORE fork (only async-signal-safe calls after it).
    std::vector<std::string> envStrings;
    {
        std::map<std::string, std::string> env;
        for (char** e = SDRSTAR_ENVIRON; e && *e; ++e) {
            const char* eq = std::strchr(*e, '=');
            if (eq) { env[std::string(*e, eq - *e)] = eq + 1; }
        }
        for (const auto& kv : opt.env) { env[kv.first] = kv.second; }
        for (const auto& kv : env) { envStrings.push_back(kv.first + "=" + kv.second); }
    }
    std::vector<char*> envp;
    for (auto& s : envStrings) { envp.push_back(const_cast<char*>(s.c_str())); }
    envp.push_back(nullptr);
    std::vector<char*> argv;
    for (auto& s : opt.argv) { argv.push_back(const_cast<char*>(s.c_str())); }
    argv.push_back(nullptr);

    int inSock[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, inSock) != 0) {
        error = std::string("socketpair: ") + std::strerror(errno);
        return false;
    }
    if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        for (int fd : {inSock[0], inSock[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) { ::close(fd); }
        }
        return false;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(inSock[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif

    const char* cwd = opt.cwd.empty() ? nullptr : opt.cwd.c_str();
    const bool capOut = opt.captureStdout, capErr = opt.captureStderr;

    pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        for (int fd : {inSock[0], inSock[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) { ::close(fd); }
        return false;
    }
    if (pid == 0) {
        // ---- child ----
        ::dup2(inSock[1], 0);
        if (capOut) {
            ::dup2(outPipe[1], 1);
        } else {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, 1); }
        }
        if (capErr) {
            ::dup2(errPipe[1], 2);
        } else {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, 2); }
        }
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096) { maxfd = 4096; }
        for (int fd = 3; fd < maxfd; ++fd) { ::close(fd); }
        if (cwd && ::chdir(cwd) != 0) { _exit(126); }
        // Reset signal dispositions the host may have changed.
        ::signal(SIGPIPE, SIG_DFL);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    // ---- parent ----
    ::close(inSock[1]);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    pid_ = pid;
    stdinFd_ = inSock[0];
    stdoutFd_ = outPipe[0];
    stderrFd_ = errPipe[0];
    setCloexec(stdinFd_);
    setCloexec(stdoutFd_);
    setCloexec(stderrFd_);
    setNonblock(stdinFd_);
    exitCode_ = -1;
    running_ = true;

    outThread_ = std::thread([this] { readerLoop(stdoutFd_, true); });
    errThread_ = std::thread([this] { readerLoop(stderrFd_, false); });
    waitThread_ = std::thread([this] { waitLoop(); });
    return true;
}

void Subprocess::readerLoop(int fd, bool isStdout) {
    uint8_t buf[16384];
    std::string partial;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            if (isStdout) {
                if (onStdout_) { onStdout_(buf, (size_t)n); }
            } else {
                partial.append((const char*)buf, (size_t)n);
                size_t pos;
                while ((pos = partial.find('\n')) != std::string::npos) {
                    std::string line = partial.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                    partial.erase(0, pos + 1);
                    if (onStderrLine_) { onStderrLine_(line); }
                }
                if (partial.size() > 65536) { partial.clear(); }  // pathological no-newline spam
            }
            continue;
        }
        if (n < 0 && (errno == EINTR)) { continue; }
        break;  // EOF or error
    }
    if (!isStdout && !partial.empty() && onStderrLine_) { onStderrLine_(partial); }
}

void Subprocess::waitLoop() {
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid_, &status, 0);
    } while (r < 0 && errno == EINTR);
    int code = -1;
    if (r > 0) {
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            code = 128 + WTERMSIG(status);
        }
    }
    exitCode_ = code;
    running_ = false;
    if (onExit_) { onExit_(code); }
}

long Subprocess::writeStdin(const void* data, size_t len) {
    std::lock_guard<std::mutex> lk(stdinMtx_);
    if (stdinFd_ < 0 || !running_.load()) { return -1; }
    ssize_t n = ::send(stdinFd_, data, len, kSendFlags);
    if (n >= 0) { return (long)n; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return 0; }
    return -1;
}

void Subprocess::closeStdin() {
    std::lock_guard<std::mutex> lk(stdinMtx_);
    if (stdinFd_ >= 0) {
        ::shutdown(stdinFd_, SHUT_WR);
        ::close(stdinFd_);
        stdinFd_ = -1;
    }
}

void Subprocess::stop(int graceMs) {
    if (pid_ > 0 && running_.load()) {
        closeStdin();  // polite request first: EOF makes most decoders exit
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs / 2);
        while (running_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (running_.load()) { ::kill(pid_, SIGTERM); }
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs / 2);
        while (running_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (running_.load()) { ::kill(pid_, SIGKILL); }
    }
    if (waitThread_.joinable()) { waitThread_.join(); }
    if (outThread_.joinable()) { outThread_.join(); }
    if (errThread_.joinable()) { errThread_.join(); }
    closeStdin();
    for (int* fd : {&stdoutFd_, &stderrFd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    pid_ = -1;
}

}  // namespace sdrstar
