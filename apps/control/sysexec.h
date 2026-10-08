/* Running the Linux system tools behind the arch311 applets (nmcli, wpctl, hostnamectl, pw-play). */
#ifndef ARCH311_SYSEXEC_H
#define ARCH311_SYSEXEC_H
#include <stddef.h>

/* Runs argv[0] (looked up in PATH) without a shell. `input` (may be NULL) is written to its stdin,
 * stdout goes to out (NUL-terminated, may be NULL), stderr to err. Returns the exit status,
 * 127 if the program cannot be run, -1 on timeout (the child is killed). */
int sys_run(const char *const argv[], const char *input, char *out, size_t outcb, char *err, size_t errcb,
            int timeout_ms);

/* ARCH311_SIMULATE=1: applets use built-in sample data instead of the real system (UI tests). */
int sys_simulated(void);

/* splits one `nmcli -t` line into fields (':' separated, '\:' and '\\' escaped); returns the count */
int sys_split_terse(char *line, char **fields, int max);
#endif
