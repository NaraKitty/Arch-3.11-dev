/* mbox: one USER MessageBox, for libw16's regression tests against the message boxes real 3.11 shows.
 *   mbox FLAGS CAPTION TEXT [ARG]
 * FLAGS are MB_ flags in C notation (0x34 = MB_YESNO | MB_ICONEXCLAMATION). CAPTION and TEXT are
 * either MODULE:ID - a string resource of a module among the user's ripped files (MAIN.CPL:236), read
 * at run time so that no Windows text is kept in the repository - or the text itself. TEXT is a
 * wsprintf format for ARG, the rest of the command line (one %s). The box has no owner: its place on
 * the screen does not depend on one. The answer is written to ini/MBOX.INI ([mbox] result=N) in the
 * current directory, where tools/regress.sh's "# ini:" lines read it. */
#include <w16.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *w16_app_module = "USER.EXE";

/* the next blank-separated word of *p */
static void next_word(const char **p, char *out, int cb)
{
    const char *s = *p;
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ' ') {
        if (n < cb - 1) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    *p = s;
}

static void load_text(const char *spec, char *out, int cb)
{
    const char *colon = strrchr(spec, ':');
    if (colon && colon > spec && colon[1] >= '0' && colon[1] <= '9') {
        char mod[64];
        snprintf(mod, sizeof mod, "%.*s", (int)(colon - spec), spec);
        HINSTANCE m = w16_load_module(mod);
        if (m && LoadString(m, atoi(colon + 1), out, cb)) return;
        fprintf(stderr, "mbox: no string %s\n", spec);
    }
    snprintf(out, cb, "%s", spec);
}

int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    char flags[32], capspec[128], textspec[128], caption[256], fmt[1024], text[2048];
    const char *p = lpCmdLine;
    next_word(&p, flags, sizeof flags);
    next_word(&p, capspec, sizeof capspec);
    next_word(&p, textspec, sizeof textspec);
    while (*p == ' ') p++;
    load_text(capspec, caption, sizeof caption);
    load_text(textspec, fmt, sizeof fmt);
    wsprintf(text, fmt, p);
    int r = MessageBox(NULL, text, caption, (UINT)strtoul(flags, NULL, 0));
    mkdir("ini", 0777);
    FILE *f = fopen("ini/MBOX.INI", "wb");
    if (f) {
        fprintf(f, "[mbox]\r\nresult=%d\r\n", r);
        fclose(f);
    }
    return 0;
}
