/* pty-answer ANSWER COMMAND [ARGS...]: runs COMMAND on a pseudo-terminal, answers its "[y/N]" prompt with ANSWER,
   copies its output to stdout and exits with its status (124 after killing it at the 60 s timeout). Test helper only. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROMPT "[y/N]"
#define TIMEOUT_S 60

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s ANSWER COMMAND [ARGS...]\n", argv[0]);
        return 2;
    }
    const char *answer = argv[1];
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        perror(argv[2]);
        _exit(127);
    }

    const size_t plen = sizeof PROMPT - 1;
    double deadline = now() + TIMEOUT_S;
    char buf[4096], win[sizeof buf + sizeof PROMPT];
    char tail[sizeof PROMPT] = "";   /* last plen-1 bytes, so a prompt split across reads is still seen */
    size_t ntail = 0;
    int sent = 0;

    while (now() < deadline) {
        struct pollfd pfd = {master, POLLIN, 0};
        int r = poll(&pfd, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0) break;   /* EIO: the command closed the terminal */
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
        if (sent) continue;
        memcpy(win, tail, ntail);
        memcpy(win + ntail, buf, (size_t)n);
        size_t wlen = ntail + (size_t)n;
        if (memmem(win, wlen, PROMPT, plen)) {
            if (write(master, answer, strlen(answer)) < 0 || write(master, "\n", 1) < 0) perror("write");
            sent = 1;
        }
        ntail = wlen < plen - 1 ? wlen : plen - 1;
        memcpy(tail, win + wlen - ntail, ntail);
    }

    /* the command may outlive its terminal: wait for it, but never past the deadline */
    int status = 0, timed_out = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) break;
        if (now() >= deadline) {
            fprintf(stderr, "pty-answer: timed out after %d s, killing the command\n", TIMEOUT_S);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = 1;
            break;
        }
        usleep(10000);
    }
    close(master);
    if (timed_out) return 124;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
