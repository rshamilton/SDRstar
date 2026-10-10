/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ptyrun SECONDS OUTFILE COMMAND [ARGS...]
 *
 * Runs COMMAND with a pseudo-terminal as its stdin/stdout/stderr, copies everything it prints to OUTFILE, and stops
 * it after SECONDS (SIGTERM, then SIGKILL after 3 s).  Why: SDR++ logs with printf() and never flushes; when stdout
 * is a file or a pipe the C library buffers it, so a process that is killed (a GUI/server app has no other way to
 * end) leaves an EMPTY log.  On a pseudo-terminal stdio is line-buffered, so the log is complete up to the kill.
 *
 * Exit status:   0   the program was still running after SECONDS (and was stopped by us)
 *                1xx the program exited by itself with status xx (100 = status 0)
 *                99  the program died from a signal
 *                98  ptyrun itself failed (message on stderr)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Copy whatever is readable on the pty master into the output file. */
static void drain(int master, FILE* out) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0) { return; }
        fwrite(buf, 1, (size_t)n, out);
        fflush(out);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: ptyrun SECONDS OUTFILE COMMAND [ARGS...]\n");
        return 98;
    }
    double limit = atof(argv[1]);
    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        perror("ptyrun: cannot open output file");
        return 98;
    }
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        perror("ptyrun: forkpty");
        return 98;
    }
    if (pid == 0) {
        execvp(argv[3], argv + 3);
        fprintf(stderr, "ptyrun: cannot execute %s: %s\n", argv[3], strerror(errno));
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    double start = now();
    int status = 0;
    int exited = 0;
    int stopped_by_us = 0;
    double kill_at = 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(master, &rd);
        struct timeval tv = {0, 200000};
        select(master + 1, &rd, NULL, NULL, &tv);
        drain(master, out);
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            exited = 1;
            break;
        }
        double t = now() - start;
        if (!stopped_by_us && t >= limit) {
            stopped_by_us = 1;
            kill(pid, SIGTERM);
            kill_at = now() + 3.0;
        } else if (stopped_by_us && now() >= kill_at) {
            kill(pid, SIGKILL);
            kill_at = now() + 3.0;
        }
    }
    drain(master, out);
    fclose(out);
    if (!exited) { return 98; }
    if (stopped_by_us) { return 0; }
    if (WIFEXITED(status)) { return 100 + (WEXITSTATUS(status) > 99 ? 99 : WEXITSTATUS(status)); }
    return 99;
}
