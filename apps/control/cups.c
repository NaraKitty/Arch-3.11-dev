/* CUPS back end of the Printers applet. UNTESTED against a real CUPS server: the WSL build host has
 * none. The output parsers are unit-tested on sample text (tests/cups_test.c), and ARCH311_SIMULATE
 * runs the applet on simulated lpstat/lpinfo output (see cups.h).
 * Every name, URI and option is passed as its own argv element (sysexec.c runs no shell). lpadmin
 * and lpinfo -v need CUPS administration rights (the SystemGroup of cups-files.conf, usually sys or
 * wheel); without them they fail with "Forbidden", which the applet shows. */
#include "cups.h"
#include "sysexec.h"
#include "w16.h"
#include "commdlg.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_err[512];
const char *cups_last_error(void) { return last_err; }

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int run(const char *const argv[], char *out, size_t cb, int timeout)
{
    int r = sys_run(argv, NULL, out, cb, last_err, sizeof last_err, timeout);
    if (r == 127) snprintf(last_err, sizeof last_err, "%s is not installed.", argv[0]);
    else if (r == -1) snprintf(last_err, sizeof last_err, "%s did not answer in time.", argv[0]);
    trim_end(last_err);
    if (r && !last_err[0]) snprintf(last_err, sizeof last_err, "%s failed.", argv[0]);
    return r;
}

/* ------------------------------------------------------------------ parsers */
/* the lines of a text, in place */
static char *next_line(char **p)
{
    char *s = *p;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = 0; *p = nl + 1; }
    else *p = s + strlen(s);
    trim_end(s);
    return s;
}

/* "device for NAME: URI" (names have no blanks, so the first ": " ends the name) */
int cups_parse_lpstat_v(char *text, CupsQueue *q, int max)
{
    int n = 0;
    for (char *p = text, *l; (l = next_line(&p)) && n < max;) {
        if (strncmp(l, "device for ", 11)) continue;
        char *name = l + 11, *c = strstr(name, ": ");
        if (!c || c == name) continue;
        *c = 0;
        snprintf(q[n].name, sizeof q[n].name, "%s", name);
        snprintf(q[n].uri, sizeof q[n].uri, "%s", c + 2);
        n++;
    }
    return n;
}

/* "system default destination: NAME[/INSTANCE]" or "no system default destination" */
int cups_parse_lpstat_d(const char *text, char *name, int cb)
{
    static const char tag[] = "system default destination: ";
    const char *s = strstr(text, tag);
    name[0] = 0;
    if (!s || (s != text && s[-1] != '\n')) return 0;
    s += sizeof tag - 1;
    size_t n = strcspn(s, "/\r\n \t");
    snprintf(name, cb, "%.*s", (int)n, s);
    return name[0] != 0;
}

/* "Device: uri = URI" records with indented "class = ", "info = " lines (lpinfo -l -v), or one
 * "CLASS URI" line a device (lpinfo -v). Entries without a ':' are backends that need an address
 * typed ("network socket"), not devices, and are left out. */
static void add_device(CupsDevice *d, int max, int *n, const CupsDevice *t)
{
    if (t->uri[0] && strchr(t->uri, ':') && *n < max) d[(*n)++] = *t;
}

int cups_parse_lpinfo_v(char *text, CupsDevice *d, int max)
{
    int n = 0, have = 0;
    CupsDevice t;
    for (char *p = text, *l; (l = next_line(&p));) {
        if (!strncmp(l, "Device:", 7)) {
            if (have) add_device(d, max, &n, &t);
            memset(&t, 0, sizeof t);
            have = 1;
            char *u = strstr(l, "uri = ");
            if (u) snprintf(t.uri, sizeof t.uri, "%s", u + 6);
        } else if (l[0] == ' ' || l[0] == '\t') {
            if (!have) continue;
            while (*l == ' ' || *l == '\t') l++;
            if (!strncmp(l, "class = ", 8)) snprintf(t.cls, sizeof t.cls, "%s", l + 8);
            else if (!strncmp(l, "info = ", 7)) snprintf(t.info, sizeof t.info, "%s", l + 7);
        } else if (l[0]) {
            if (have) add_device(d, max, &n, &t);
            have = 0;
            char *sp = strchr(l, ' ');
            if (!sp) continue;
            CupsDevice s;
            memset(&s, 0, sizeof s);
            snprintf(s.cls, sizeof s.cls, "%.*s", (int)(sp - l), l);
            snprintf(s.uri, sizeof s.uri, "%s", sp + 1);
            add_device(d, max, &n, &s);
        }
    }
    if (have) add_device(d, max, &n, &t);
    return n;
}

/* "Model:  name = PPD" records with an indented "make-and-model = " line (lpinfo -l -m), or one
 * "PPD MAKE-AND-MODEL" line a driver (lpinfo -m) */
int cups_parse_lpinfo_m(char *text, CupsModel *m, int max)
{
    int n = 0, have = 0;
    CupsModel t;
    for (char *p = text, *l; (l = next_line(&p));) {
        if (!strncmp(l, "Model:", 6)) {
            if (have && t.ppd[0] && n < max) m[n++] = t;
            memset(&t, 0, sizeof t);
            have = 1;
            char *s = strstr(l, "name = ");
            if (s) snprintf(t.ppd, sizeof t.ppd, "%s", s + 7);
        } else if (l[0] == ' ' || l[0] == '\t') {
            if (!have) continue;
            while (*l == ' ' || *l == '\t') l++;
            if (!strncmp(l, "make-and-model = ", 17)) snprintf(t.model, sizeof t.model, "%s", l + 17);
        } else if (l[0]) {
            if (have && t.ppd[0] && n < max) m[n++] = t;
            have = 0;
            char *sp = strchr(l, ' ');
            if (!sp || n >= max) continue;
            memset(&m[n], 0, sizeof m[n]);
            snprintf(m[n].ppd, sizeof m[n].ppd, "%.*s", (int)(sp - l), l);
            snprintf(m[n].model, sizeof m[n].model, "%s", sp + 1);
            n++;
        }
    }
    if (have && t.ppd[0] && n < max) m[n++] = t;
    for (int i = 0; i < n; i++)
        if (!m[i].model[0]) snprintf(m[i].model, sizeof m[i].model, "%s", m[i].ppd);
    return n;
}

/* the value of `key` in lpoptions' "name=value name='a value' flag ..." line */
int cups_parse_option(const char *text, const char *key, char *val, int cb)
{
    size_t kl = strlen(key);
    const char *s = text;
    val[0] = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (!*s) break;
        const char *name = s;
        while (*s && *s != '=' && *s != ' ' && *s != '\t' && *s != '\n') s++;
        size_t nl = (size_t)(s - name);
        char buf[512];
        size_t o = 0;
        if (*s == '=') {
            s++;
            char q = (*s == '\'' || *s == '"') ? *s++ : 0;
            while (*s && (q ? *s != q : (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r'))) {
                if (*s == '\\' && s[1]) s++;
                if (o < sizeof buf - 1) buf[o++] = *s;
                s++;
            }
            if (q && *s == q) s++;
        }
        buf[o] = 0;
        if (nl == kl && !strncmp(name, key, kl)) {
            snprintf(val, cb, "%s", buf);
            return 1;
        }
    }
    return 0;
}

void cups_queue_name(const char *model, char *name, int cb)
{
    int o = 0;
    for (const char *s = model; *s && o < cb - 1 && o < 127; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '.') name[o++] = (char)c;
        else if (o && name[o - 1] != '_') name[o++] = '_';
    }
    while (o && name[o - 1] == '_') o--;
    name[o] = 0;
    if (!o) snprintf(name, cb, "Printer");
}

/* ------------------------------------------------------------------ simulated CUPS */
typedef struct {
    char name[128], uri[512], ppd[256], media[32], orient[8];
} SimQueue;
static SimQueue sim_q[32];
static int sim_nq = -1;
static char sim_def[128];

static void sim_init(void)
{
    if (sim_nq >= 0) return;
    sim_nq = 0;
    const char *s = getenv("ARCH311_SIM_PRINTERS");
    if (!s) s = "*HP_LaserJet_4=parallel:/dev/lp0;Text_Only=file:///dev/null";
    while (*s && sim_nq < 32) {
        size_t n = strcspn(s, ";");
        char item[700];
        snprintf(item, sizeof item, "%.*s", (int)n, s);
        s += n;
        if (*s) s++;
        int def = item[0] == '*';
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        SimQueue *q = &sim_q[sim_nq++];
        memset(q, 0, sizeof *q);
        snprintf(q->name, sizeof q->name, "%s", item + def);
        snprintf(q->uri, sizeof q->uri, "%s", eq + 1);
        snprintf(q->media, sizeof q->media, "Letter");
        snprintf(q->orient, sizeof q->orient, "3");
        if (def) snprintf(sim_def, sizeof sim_def, "%s", q->name);
    }
}

static SimQueue *sim_find(const char *name)
{
    sim_init();
    for (int i = 0; i < sim_nq; i++)
        if (!strcmp(sim_q[i].name, name)) return &sim_q[i];
    return NULL;
}

/* a change is refused when ARCH311_SIM_ERROR is set, as lpadmin refuses users without the rights */
static int sim_refused(void)
{
    const char *e = getenv("ARCH311_SIM_ERROR");
    if (!e || !*e) return 0;
    snprintf(last_err, sizeof last_err, "%s", e);
    return 1;
}

/* the reference PC's devices as lpinfo -l -v prints them: LPT1, COM1, COM2 and the network backends */
static const char sim_devices[] =
    "Device: uri = socket\n        class = network\n        info = AppSocket/HP JetDirect\n"
    "        make-and-model = Unknown\n        device-id = \n        location = \n"
    "Device: uri = parallel:/dev/lp0\n        class = direct\n        info = LPT #1\n"
    "        make-and-model = Unknown\n        device-id = \n        location = \n"
    "Device: uri = serial:/dev/ttyS0?baud=115200\n        class = serial\n        info = Serial Port #1\n"
    "        make-and-model = Unknown\n        device-id = \n        location = \n"
    "Device: uri = serial:/dev/ttyS1?baud=115200\n        class = serial\n        info = Serial Port #2\n"
    "        make-and-model = Unknown\n        device-id = \n        location = \n"
    "Device: uri = ipp\n        class = network\n        info = Internet Printing Protocol (ipp)\n"
    "        make-and-model = Unknown\n        device-id = \n        location = \n";

/* without a CONTROL.INF: a few of CUPS's own drivers */
static const char sim_models_builtin[] =
    "Model:  name = drv:///sample.drv/generic.ppd\n        natural_language = en\n"
    "        make-and-model = Generic PostScript Printer\n        device-id = \n"
    "Model:  name = drv:///sample.drv/generpcl.ppd\n        natural_language = en\n"
    "        make-and-model = Generic PCL Laser Printer\n        device-id = \n"
    "Model:  name = everywhere\n        natural_language = en\n        make-and-model = IPP Everywhere\n"
    "        device-id = \n";

static void append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t c = (*cap + n + 1) * 2;
        char *b = realloc(*buf, c);
        if (!b) return;
        *buf = b;
        *cap = c;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

/* lpinfo -l -m as the reference PC would answer it: one driver for each printer name of CONTROL.INF
 * [io.device] (read as seg23:0245 EnumInfSection reads a section: lines up to an unquoted ';',
 * blank and comment lines skipped, the next '[' ends it; the name is the first quoted field) */
static char *sim_models(void)
{
    OFSTRUCT of;
    HFILE f = OpenFile("CONTROL.INF", &of, OF_READ);
    char *inf = NULL;
    if (f != HFILE_ERROR) {
        LONG size = _llseek(f, 0, 2);
        _llseek(f, 0, 0);
        inf = size > 0 ? malloc((size_t)size + 1) : NULL;
        if (inf) inf[_lread(f, inf, (UINT)size)] = 0;
        _lclose(f);
    }
    char *s = inf ? strstr(inf, "[io.device") : NULL;
    if (!s) {
        free(inf);
        return strdup(sim_models_builtin);
    }
    s = strchr(s, '\n');
    char *out = NULL;
    size_t len = 0, cap = 0;
    append(&out, &len, &cap, "");
    for (int n = 0; s && *s;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (*s == '[' || !*s) break;
        char *e = s;
        for (int q = 0; *e && *e != '\r' && *e != '\n' && (q || *e != ';'); e++)
            if (*e == '"') q = !q;
        if (*s != ';') {
            char line[512], *q1, *q2;
            snprintf(line, sizeof line, "%.*s", (int)(e - s), s);
            if ((q1 = strchr(line, '"')) && (q2 = strchr(q1 + 1, '"'))) {
                char rec[700];
                *q2 = 0;
                snprintf(rec, sizeof rec, "Model:  name = arch311-sim:%d\n        natural_language = en\n"
                                          "        make-and-model = %.159s\n        device-id = \n", n++, q1 + 1);
                append(&out, &len, &cap, rec);
            }
        }
        s = strchr(e, '\n');
    }
    free(inf);
    return out;
}

/* the queries' output in simulation: what the CUPS tools would print */
static int sim_query(const char *const argv[], char *out, size_t cb)
{
    sim_init();
    last_err[0] = 0;
    out[0] = 0;
    if (!strcmp(argv[0], "lpstat") && !strcmp(argv[1], "-v")) {
        if (!sim_nq) { snprintf(last_err, sizeof last_err, "lpstat: No destinations added."); return 1; }
        size_t o = 0;
        for (int i = 0; i < sim_nq && o < cb; i++)
            o += snprintf(out + o, cb - o, "device for %s: %s\n", sim_q[i].name, sim_q[i].uri);
        return 0;
    }
    if (!strcmp(argv[0], "lpstat") && !strcmp(argv[1], "-d")) {
        if (sim_def[0]) snprintf(out, cb, "system default destination: %s\n", sim_def);
        else snprintf(out, cb, "no system default destination\n");
        return 0;
    }
    if (!strcmp(argv[0], "lpinfo") && !strcmp(argv[2], "-v")) {
        snprintf(out, cb, "%s", sim_devices);
        return 0;
    }
    if (!strcmp(argv[0], "lpoptions") && !strcmp(argv[1], "-p")) {
        SimQueue *q = sim_find(argv[2]);
        if (!q) { snprintf(last_err, sizeof last_err, "lpoptions: Unknown printer or class."); return 1; }
        snprintf(out, cb, "copies=1 device-uri=%s media=%s orientation-requested=%s printer-info=%s", q->uri,
                 q->media, q->orient, q->name);
        return 0;
    }
    snprintf(last_err, sizeof last_err, "%s: not simulated.", argv[0]);
    return 1;
}

static int query(const char *const argv[], char *out, size_t cb, int timeout)
{
    return sys_simulated() ? sim_query(argv, out, cb) : run(argv, out, cb, timeout);
}

/* ------------------------------------------------------------------ queries */
int cups_list_queues(CupsQueue *q, int max)
{
    const char *argv[] = {"lpstat", "-v", NULL};
    static char out[65536];
    if (query(argv, out, sizeof out, 15000)) {
        if (strstr(last_err, "No destinations added")) return 0; /* lpstat's answer when there are none */
        return -1;
    }
    return cups_parse_lpstat_v(out, q, max);
}

int cups_get_default(char *name, int cb)
{
    const char *argv[] = {"lpstat", "-d", NULL};
    char out[512];
    name[0] = 0;
    if (query(argv, out, sizeof out, 15000)) return -1;
    return cups_parse_lpstat_d(out, name, cb);
}

int cups_list_devices(CupsDevice *d, int max)
{
    const char *argv[] = {"lpinfo", "-l", "-v", NULL};
    static char out[262144];
    if (query(argv, out, sizeof out, 60000)) return -1;
    return cups_parse_lpinfo_v(out, d, max);
}

int cups_list_models(CupsModel **m)
{
    char *out;
    size_t cb = 8u << 20;
    *m = NULL;
    if (sys_simulated()) {
        sim_init();
        out = sim_models();
    } else {
        const char *argv[] = {"lpinfo", "-l", "-m", NULL};
        out = malloc(cb);
        if (!out) return -1;
        if (run(argv, out, cb, 120000)) { free(out); return -1; }
    }
    if (!out) return -1;
    int max = 1;
    for (const char *s = out; (s = strstr(s, "\n")); s++) max++;
    *m = calloc((size_t)max, sizeof **m);
    int n = *m ? cups_parse_lpinfo_m(out, *m, max) : -1;
    free(out);
    return n;
}

int cups_get_option(const char *name, const char *key, char *val, int cb)
{
    const char *argv[] = {"lpoptions", "-p", name, NULL};
    static char out[16384];
    val[0] = 0;
    if (query(argv, out, sizeof out, 15000)) return 0;
    return cups_parse_option(out, key, val, cb);
}

int cups_network_available(void)
{
    if (sys_simulated()) {
        const char *s = getenv("ARCH311_SIM_NETWORK");
        return s && *s && *s != '0';
    }
    return 1; /* CUPS reaches network printers (ipp, lpd, socket, smb backends) */
}

/* ------------------------------------------------------------------ changes */
int cups_add_queue(const char *name, const char *ppd, int ppd_is_file, const char *uri)
{
    if (sys_simulated()) {
        sim_init();
        if (sim_refused()) return 0;
        if (sim_find(name)) { snprintf(last_err, sizeof last_err, "lpadmin: Printer \"%s\" already exists.", name); return 0; }
        if (sim_nq >= 32) { snprintf(last_err, sizeof last_err, "lpadmin: Too many printers."); return 0; }
        SimQueue *q = &sim_q[sim_nq++];
        memset(q, 0, sizeof *q);
        snprintf(q->name, sizeof q->name, "%s", name);
        snprintf(q->uri, sizeof q->uri, "%s", uri);
        snprintf(q->ppd, sizeof q->ppd, "%s", ppd ? ppd : "");
        snprintf(q->media, sizeof q->media, "Letter");
        snprintf(q->orient, sizeof q->orient, "3");
        return 1;
    }
    /* -E after -p enables the queue (before it, -E would ask for encryption); without a driver the
     * queue is a raw one (network connections) */
    const char *argv[12];
    int k = 0;
    argv[k++] = "lpadmin";
    argv[k++] = "-p";
    argv[k++] = name;
    if (ppd && *ppd) {
        argv[k++] = ppd_is_file ? "-P" : "-m";
        argv[k++] = ppd;
    }
    argv[k++] = "-v";
    argv[k++] = uri;
    argv[k++] = "-E";
    argv[k] = NULL;
    return run(argv, NULL, 0, 60000) == 0;
}

int cups_set_ppd(const char *name, const char *ppd_file)
{
    if (sys_simulated()) {
        SimQueue *q = sim_find(name);
        if (sim_refused()) return 0;
        if (q) snprintf(q->ppd, sizeof q->ppd, "%s", ppd_file);
        return q != NULL;
    }
    const char *argv[] = {"lpadmin", "-p", name, "-P", ppd_file, NULL};
    return run(argv, NULL, 0, 60000) == 0;
}

int cups_set_device(const char *name, const char *uri)
{
    if (sys_simulated()) {
        SimQueue *q = sim_find(name);
        if (sim_refused()) return 0;
        if (!q) { snprintf(last_err, sizeof last_err, "lpadmin: The printer or class does not exist."); return 0; }
        snprintf(q->uri, sizeof q->uri, "%s", uri);
        return 1;
    }
    const char *argv[] = {"lpadmin", "-p", name, "-v", uri, NULL};
    return run(argv, NULL, 0, 60000) == 0;
}

int cups_delete_queue(const char *name)
{
    if (sys_simulated()) {
        SimQueue *q = sim_find(name);
        if (sim_refused()) return 0;
        if (!q) { snprintf(last_err, sizeof last_err, "lpadmin: The printer or class does not exist."); return 0; }
        if (!strcmp(sim_def, name)) sim_def[0] = 0;
        memmove(q, q + 1, (size_t)(sim_q + sim_nq - q - 1) * sizeof *q);
        sim_nq--;
        return 1;
    }
    const char *argv[] = {"lpadmin", "-x", name, NULL};
    return run(argv, NULL, 0, 60000) == 0;
}

int cups_set_default(const char *name)
{
    if (sys_simulated()) {
        if (sim_refused()) return 0;
        if (!sim_find(name)) { snprintf(last_err, sizeof last_err, "lpoptions: Unknown printer or class."); return 0; }
        snprintf(sim_def, sizeof sim_def, "%s", name);
        return 1;
    }
    const char *argv[] = {"lpoptions", "-d", name, NULL};
    char out[1024];
    return run(argv, out, sizeof out, 15000) == 0;
}

int cups_set_options(const char *name, const char *const *opts, int n)
{
    if (sys_simulated()) {
        SimQueue *q = sim_find(name);
        if (sim_refused()) return 0;
        if (!q) { snprintf(last_err, sizeof last_err, "lpoptions: Unknown printer or class."); return 0; }
        for (int i = 0; i < n; i++) {
            if (!strncmp(opts[i], "media=", 6)) snprintf(q->media, sizeof q->media, "%s", opts[i] + 6);
            else if (!strncmp(opts[i], "orientation-requested=", 22)) snprintf(q->orient, sizeof q->orient, "%s", opts[i] + 22);
        }
        return 1;
    }
    const char *argv[40];
    int k = 0;
    argv[k++] = "lpoptions";
    argv[k++] = "-p";
    argv[k++] = name;
    for (int i = 0; i < n && k < 37; i++) {
        argv[k++] = "-o";
        argv[k++] = opts[i];
    }
    argv[k] = NULL;
    char out[16384];
    return run(argv, out, sizeof out, 15000) == 0;
}
