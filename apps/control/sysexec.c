/* Process runner for the applets' system back ends: fork/execvp with pipes, never a shell, so names,
 * SSIDs and addresses typed by the user cannot be interpreted as shell syntax, and secrets travel on
 * stdin instead of the command line (where other processes could read them). */
#include "sysexec.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int sys_simulated(void)
{
    const char *s = getenv("ARCH311_SIMULATE");
    return s && *s && *s != '0';
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int sys_run(const char *const argv[], const char *input, char *out, size_t outcb, char *err, size_t errcb,
            int timeout_ms)
{
    int pin[2], pout[2], perr[2];
    if (out && outcb) out[0] = 0;
    if (err && errcb) err[0] = 0;
    if (pipe(pin) || pipe(pout) || pipe(perr)) return 127;
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        dup2(perr[1], 2);
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]);
        /* plain, untranslated output: the parsers expect English nmcli/wpctl text */
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    if (input) {
        size_t n = strlen(input), o = 0;
        signal(SIGPIPE, SIG_IGN);
        while (o < n) {
            ssize_t w = write(pin[1], input + o, n - o);
            if (w <= 0) break;
            o += w;
        }
    }
    close(pin[1]);
    size_t on = 0, en = 0;
    int open_fds = 2, timed_out = 0;
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 30000);
    struct pollfd pf[2] = {{pout[0], POLLIN, 0}, {perr[0], POLLIN, 0}};
    while (open_fds) {
        long left = deadline - now_ms();
        if (left <= 0) { timed_out = 1; break; }
        if (poll(pf, 2, (int)left) < 0 && errno != EINTR) break;
        for (int k = 0; k < 2; k++) {
            if (pf[k].fd < 0 || !(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[4096];
            ssize_t r = read(pf[k].fd, buf, sizeof buf);
            if (r <= 0) { close(pf[k].fd); pf[k].fd = -1; open_fds--; continue; }
            char *dst = k ? err : out;
            size_t cb = k ? errcb : outcb, *len = k ? &en : &on;
            if (dst && cb) {
                size_t c = (size_t)r < cb - 1 - *len ? (size_t)r : cb - 1 - *len;
                memcpy(dst + *len, buf, c);
                *len += c;
                dst[*len] = 0;
            }
        }
    }
    for (int k = 0; k < 2; k++) if (pf[k].fd >= 0) close(pf[k].fd);
    if (timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (timed_out) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
}

int sys_split_terse(char *line, char **fields, int max)
{
    int n = 0;
    char *w = line;
    if (max > 0) fields[n++] = w;
    for (char *r = line; *r; r++) {
        if (*r == '\\' && r[1]) { *w++ = *++r; continue; }
        if (*r == ':' && n < max) { *w++ = 0; fields[n++] = w; continue; }
        *w++ = *r;
    }
    *w = 0;
    return n;
}
