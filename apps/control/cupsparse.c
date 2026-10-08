/* The CUPS tools' output, parsed (cups.c runs the tools). Plain C without libw16, so that
 * tests/cups_test.c can check it on sample output (make -C apps check). All parsers expect the C
 * locale's English text, which sysexec.c asks for (LC_ALL=C). */
#include "cups.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

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

/* lpstat -v: "device for NAME: URI" (names have no blanks, so the first ": " ends the name) */
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

/* lpstat -d: "system default destination: NAME[/INSTANCE]" or "no system default destination" */
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

/* lpinfo -l -v: "Device: uri = URI" records with indented "class = ", "info = " lines; lpinfo -v:
 * one "CLASS URI" line a device. Entries without a ':' are backends that need an address typed
 * ("network socket"), not devices, and are left out. */
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

/* lpinfo -l -m: "Model:  name = PPD" records with an indented "make-and-model = " line; lpinfo -m:
 * one "PPD MAKE-AND-MODEL" line a driver */
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

/* lpoptions -p NAME: the value of `key` in "name=value name='a value' flag ..." */
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

/* letters, digits, '-' and '.' kept, every other run of characters one '_', at most 127 */
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
