/* NetworkManager (nmcli) back end. UNTESTED against a real NetworkManager: the WSL build host has
 * none; ARCH311_SIMULATE=1 exercises the applet with sample connections and networks.
 * Every value the user typed is passed as its own argv element (sysexec.c runs no shell); the Wi-Fi
 * password goes to nmcli --ask on stdin so it never appears on a command line. */
#include "net.h"
#include "sysexec.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char last_err[512];
const char *net_last_error(void) { return last_err; }

static int nmcli(const char *const argv[], const char *input, char *out, size_t cb, int timeout)
{
    int r = sys_run(argv, input, out, cb, last_err, sizeof last_err, timeout);
    if (r == 127) snprintf(last_err, sizeof last_err, "%s is not installed.", argv[0]);
    else if (r == -1) snprintf(last_err, sizeof last_err, "%s did not answer in time.", argv[0]);
    /* nmcli puts "Error: " in front of its messages */
    else if (!strncmp(last_err, "Error: ", 7)) memmove(last_err, last_err + 7, strlen(last_err + 7) + 1);
    for (size_t L = strlen(last_err); L && (last_err[L - 1] == '\n' || last_err[L - 1] == ' '); L--) last_err[L - 1] = 0;
    return r;
}

/* ------------------------------------------------------------------ simulated system */
static NetConn sim_conns[8] = {
    {"Wired connection 1", "3c1e8a52-0d4b-4d5e-9a61-7f2a1c9b0e11", "802-3-ethernet", "enp3s0", 1},
    {"HomeNetwork", "7d2f4c31-5a6e-4b8f-a2c3-1e9d8f7a6b54", "802-11-wireless", "", 0},
};
static int sim_nconn = 2;
static WifiAp sim_aps[] = {
    {"HomeNetwork", "WPA2", 78, 0}, {"Cafe Guest", "", 52, 0}, {"Neighbor-5G", "WPA2", 31, 0},
};
static NetIp sim_ip[8] = {{1, "", "", "", ""}, {1, "", "", "", ""}};
static char sim_host[64] = "arch311";

static int sim_find(const char *uuid)
{
    for (int i = 0; i < sim_nconn; i++) if (!strcmp(sim_conns[i].uuid, uuid)) return i;
    return -1;
}

/* ------------------------------------------------------------------ queries */
int net_available(void)
{
    if (sys_simulated()) return 1;
    const char *argv[] = {"nmcli", "-t", "-f", "RUNNING", "general", NULL};
    char out[64];
    return nmcli(argv, NULL, out, sizeof out, 5000) == 0 && !strncmp(out, "running", 7);
}

int net_list_connections(NetConn *c, int max)
{
    if (sys_simulated()) {
        int n = sim_nconn < max ? sim_nconn : max;
        memcpy(c, sim_conns, n * sizeof *c);
        return n;
    }
    const char *argv[] = {"nmcli", "-t", "-f", "NAME,UUID,TYPE,DEVICE,ACTIVE", "connection", "show", NULL};
    static char out[16384];
    if (nmcli(argv, NULL, out, sizeof out, 8000) != 0) return 0;
    int n = 0;
    for (char *line = strtok(out, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char *f[5];
        if (sys_split_terse(line, f, 5) < 5) continue;
        if (!strcmp(f[2], "loopback") || !strcmp(f[2], "bridge") || !strcmp(f[2], "tun")) continue;
        snprintf(c[n].name, sizeof c[n].name, "%s", f[0]);
        snprintf(c[n].uuid, sizeof c[n].uuid, "%s", f[1]);
        snprintf(c[n].type, sizeof c[n].type, "%s", f[2]);
        snprintf(c[n].device, sizeof c[n].device, "%s", f[3]);
        c[n].active = !strcmp(f[4], "yes");
        n++;
    }
    return n;
}

int net_list_wifi(WifiAp *a, int max, int rescan)
{
    if (sys_simulated()) {
        int n = 0;
        for (size_t i = 0; i < sizeof sim_aps / sizeof sim_aps[0] && n < max; i++) {
            a[n] = sim_aps[i];
            a[n].in_use = 0;
            for (int k = 0; k < sim_nconn; k++)
                if (sim_conns[k].active && !strcmp(sim_conns[k].name, a[n].ssid)) a[n].in_use = 1;
            n++;
        }
        return n;
    }
    const char *argv[] = {"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                          "--rescan", rescan ? "yes" : "auto", NULL};
    static char out[16384];
    if (nmcli(argv, NULL, out, sizeof out, 20000) != 0) return 0;
    int n = 0;
    for (char *line = strtok(out, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char *f[4];
        if (sys_split_terse(line, f, 4) < 4 || !f[1][0]) continue; /* hidden networks have no SSID */
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(a[k].ssid, f[1])) { /* one entry per SSID, the strongest */
                if (atoi(f[2]) > a[k].signal) a[k].signal = atoi(f[2]);
                if (f[0][0] == '*') a[k].in_use = 1;
                dup = 1;
            }
        if (dup) continue;
        snprintf(a[n].ssid, sizeof a[n].ssid, "%s", f[1]);
        snprintf(a[n].security, sizeof a[n].security, "%s", strcmp(f[3], "--") ? f[3] : "");
        a[n].signal = atoi(f[2]);
        a[n].in_use = f[0][0] == '*';
        n++;
    }
    return n;
}

int net_wifi_ssid(const char *uuid, char *ssid, int cb)
{
    if (sys_simulated()) {
        int i = sim_find(uuid);
        if (i < 0) return 0;
        snprintf(ssid, cb, "%s", sim_conns[i].name);
        return 1;
    }
    const char *argv[] = {"nmcli", "-g", "802-11-wireless.ssid", "connection", "show", "uuid", uuid, NULL};
    char out[128];
    if (nmcli(argv, NULL, out, sizeof out, 5000) != 0) return 0;
    out[strcspn(out, "\n")] = 0;
    snprintf(ssid, cb, "%s", out);
    return ssid[0] != 0;
}

int net_device_address(const char *device, char *addr, int cb)
{
    addr[0] = 0;
    if (!device || !device[0]) return 0;
    if (sys_simulated()) {
        snprintf(addr, cb, "%s", !strcmp(device, "enp3s0") ? "192.168.1.20" : "192.168.1.37");
        return 1;
    }
    const char *argv[] = {"nmcli", "-g", "IP4.ADDRESS", "device", "show", device, NULL};
    char out[256];
    if (nmcli(argv, NULL, out, sizeof out, 5000) != 0) return 0;
    out[strcspn(out, "/|\n")] = 0; /* first address, without the prefix */
    snprintf(addr, cb, "%s", out);
    return addr[0] != 0;
}

/* ------------------------------------------------------------------ actions */
int net_connect(const char *uuid)
{
    if (sys_simulated()) {
        int i = sim_find(uuid);
        if (i < 0) return 0;
        for (int k = 0; k < sim_nconn; k++)
            if (!strcmp(sim_conns[k].type, sim_conns[i].type)) { sim_conns[k].active = 0; sim_conns[k].device[0] = 0; }
        sim_conns[i].active = 1;
        snprintf(sim_conns[i].device, sizeof sim_conns[i].device, "%s", strstr(sim_conns[i].type, "wireless") ? "wlp2s0" : "enp3s0");
        return 1;
    }
    const char *argv[] = {"nmcli", "connection", "up", "uuid", uuid, NULL};
    return nmcli(argv, NULL, NULL, 0, 60000) == 0;
}

int net_disconnect(const char *uuid)
{
    if (sys_simulated()) {
        int i = sim_find(uuid);
        if (i >= 0) { sim_conns[i].active = 0; sim_conns[i].device[0] = 0; }
        return i >= 0;
    }
    const char *argv[] = {"nmcli", "connection", "down", "uuid", uuid, NULL};
    return nmcli(argv, NULL, NULL, 0, 30000) == 0;
}

int net_wifi_connect(const char *ssid, const char *password)
{
    if (sys_simulated()) {
        if (password && strcmp(password, "password")) {
            snprintf(last_err, sizeof last_err, "Secrets were required, but not provided.");
            return 0;
        }
        if (sim_nconn < 8) {
            NetConn *c = &sim_conns[sim_nconn];
            memset(c, 0, sizeof *c);
            snprintf(c->name, sizeof c->name, "%s", ssid);
            snprintf(c->uuid, sizeof c->uuid, "0000-sim-%d", sim_nconn);
            snprintf(c->type, sizeof c->type, "802-11-wireless");
            sim_ip[sim_nconn].dhcp = 1;
            sim_nconn++;
            return net_connect(c->uuid);
        }
        return 0;
    }
    if (password) {
        const char *argv[] = {"nmcli", "--ask", "device", "wifi", "connect", ssid, NULL};
        char in[160];
        snprintf(in, sizeof in, "%s\n", password);
        int r = nmcli(argv, in, NULL, 0, 60000);
        memset(in, 0, sizeof in);
        return r == 0;
    }
    const char *argv[] = {"nmcli", "device", "wifi", "connect", ssid, NULL};
    return nmcli(argv, NULL, NULL, 0, 60000) == 0;
}

/* ------------------------------------------------------------------ IPv4 settings */
int net_get_ipv4(const char *uuid, NetIp *ip)
{
    memset(ip, 0, sizeof *ip);
    if (sys_simulated()) {
        int i = sim_find(uuid);
        if (i < 0) return 0;
        *ip = sim_ip[i];
        return 1;
    }
    const char *argv[] = {"nmcli", "-t", "-f", "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns", "connection", "show",
                          "uuid", uuid, NULL};
    char out[1024];
    if (nmcli(argv, NULL, out, sizeof out, 5000) != 0) return 0;
    ip->dhcp = 1;
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[2];
        if (sys_split_terse(line, f, 2) < 2) continue;
        if (!strcmp(f[0], "ipv4.method")) ip->dhcp = strcmp(f[1], "manual") != 0;
        else if (!strcmp(f[0], "ipv4.addresses") && f[1][0]) {
            char a[64];
            snprintf(a, sizeof a, "%s", f[1]);
            a[strcspn(a, ",")] = 0;
            char *slash = strchr(a, '/');
            int prefix = 24;
            if (slash) { *slash = 0; prefix = atoi(slash + 1); }
            snprintf(ip->address, sizeof ip->address, "%s", a);
            net_prefix_to_mask(prefix, ip->mask, sizeof ip->mask);
        } else if (!strcmp(f[0], "ipv4.gateway"))
            snprintf(ip->gateway, sizeof ip->gateway, "%s", strcmp(f[1], "--") ? f[1] : "");
        else if (!strcmp(f[0], "ipv4.dns")) {
            snprintf(ip->dns, sizeof ip->dns, "%s", f[1]);
            for (char *p = ip->dns; *p; p++) if (*p == ',') *p = ' ';
        }
    }
    return 1;
}

int net_set_ipv4(const char *uuid, const NetIp *ip)
{
    if (sys_simulated()) {
        int i = sim_find(uuid);
        if (i < 0) return 0;
        sim_ip[i] = *ip;
        return 1;
    }
    char addr[40] = "", dns[64] = "";
    if (!ip->dhcp) snprintf(addr, sizeof addr, "%s/%d", ip->address, net_mask_to_prefix(ip->mask));
    snprintf(dns, sizeof dns, "%s", ip->dns);
    for (char *p = dns; *p; p++) if (*p == ' ') *p = ',';
    const char *argv[] = {"nmcli", "connection", "modify", "uuid", uuid,
                          "ipv4.method", ip->dhcp ? "auto" : "manual",
                          "ipv4.addresses", addr,
                          "ipv4.gateway", ip->dhcp ? "" : ip->gateway,
                          "ipv4.dns", dns, NULL};
    if (nmcli(argv, NULL, NULL, 0, 10000) != 0) return 0;
    /* apply now if the connection is in use */
    const char *up[] = {"nmcli", "connection", "up", "uuid", uuid, NULL};
    char act[64];
    const char *q[] = {"nmcli", "-g", "GENERAL.STATE", "connection", "show", "uuid", uuid, NULL};
    if (nmcli(q, NULL, act, sizeof act, 5000) == 0 && strstr(act, "activated")) return nmcli(up, NULL, NULL, 0, 60000) == 0;
    return 1;
}

/* ------------------------------------------------------------------ host name */
int net_get_hostname(char *name, int cb)
{
    if (sys_simulated()) { snprintf(name, cb, "%s", sim_host); return 1; }
    if (gethostname(name, cb)) return 0;
    name[cb - 1] = 0;
    return 1;
}

int net_set_hostname(const char *name)
{
    if (sys_simulated()) { snprintf(sim_host, sizeof sim_host, "%s", name); return 1; }
    const char *argv[] = {"hostnamectl", "set-hostname", name, NULL};
    return nmcli(argv, NULL, NULL, 0, 30000) == 0;
}

/* ------------------------------------------------------------------ address helpers */
int net_valid_ipv4(const char *s)
{
    int parts = 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*s)) { v = v * 10 + (*s++ - '0'); if (++digits > 3) return 0; }
        if (v > 255) return 0;
        parts++;
        if (*s == '.') { s++; if (!*s) return 0; }
        else if (*s) return 0;
    }
    return parts == 4;
}

int net_mask_to_prefix(const char *mask)
{
    if (!net_valid_ipv4(mask)) return -1;
    unsigned a, b, c, d;
    sscanf(mask, "%u.%u.%u.%u", &a, &b, &c, &d);
    unsigned long m = ((unsigned long)a << 24) | (b << 16) | (c << 8) | d;
    int prefix = 0;
    while (prefix < 32 && (m & (0x80000000UL >> prefix))) prefix++;
    unsigned long want = prefix ? (0xFFFFFFFFUL << (32 - prefix)) & 0xFFFFFFFFUL : 0;
    return m == want ? prefix : -1;
}

void net_prefix_to_mask(int prefix, char *mask, int cb)
{
    if (prefix < 0) prefix = 0;
    if (prefix > 32) prefix = 32;
    unsigned long m = prefix ? (0xFFFFFFFFUL << (32 - prefix)) & 0xFFFFFFFFUL : 0;
    snprintf(mask, cb, "%lu.%lu.%lu.%lu", m >> 24, (m >> 16) & 255, (m >> 8) & 255, m & 255);
}
