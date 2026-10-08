/* SHELL.DLL: ShellAbout, using dialog 100 and the strings of the user's ripped SHELL.DLL.
 * Checked against real 3.11 (Calculator's About box): 101, 112, 115 and 110; the user name,
 * memory and resources lines show this machine's values. */
#include "w16int.h"
#include "commdlg.h"
#include <pwd.h>
#include <unistd.h>

enum {
    IDD_ICON = 111, IDD_APP = 101, IDD_VERSION = 112, IDD_OTHER = 110, IDD_MODE = 102,
    IDD_MEMLABEL = 103, IDD_MEM = 104, IDD_RESLABEL = 113, IDD_RES = 105,
    IDD_SDLABEL = 106, IDD_SD = 107, IDD_USER = 108, IDD_ORG = 109, IDD_EXTRA = 115,
};
enum { IDS_386 = 213, IDS_SYSRES = 215, IDS_VERSION = 216, IDS_KBFREE = 218, IDS_PCTFREE = 220 };

typedef struct { LPCSTR app, other; HICON icon; } About;

static HINSTANCE shell(void) { return w16_system_module("SHELL.DLL"); }

/* "%s" with thousands separators, like 3.1's "14,320 KB Free" */
static void kb_text(char *out, size_t cb, unsigned long kb)
{
    char d[32], fmt[64];
    int n = snprintf(d, sizeof d, "%lu", kb), o = 0;
    char grouped[48];
    for (int i = 0; i < n; i++) {
        grouped[o++] = d[i];
        if ((n - i - 1) % 3 == 0 && i < n - 1) grouped[o++] = ',';
    }
    grouped[o] = 0;
    if (!LoadString(shell(), IDS_KBFREE, fmt, sizeof fmt)) strcpy(fmt, "%s KB Free");
    snprintf(out, cb, fmt, grouped);
}

static void meminfo(unsigned long *total_kb, unsigned long *avail_kb)
{
    *total_kb = *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    while (f && fgets(l, sizeof l, f)) {
        sscanf(l, "MemTotal: %lu", total_kb);
        sscanf(l, "MemAvailable: %lu", avail_kb);
    }
    if (f) fclose(f);
}

static BOOL AboutDlgProc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_INITDIALOG: {
        About *a = (About *)lp;
        char app[128], tmpl[160], t[300];
        /* "Title#Program" puts Title in the caption and Program in the first line */
        snprintf(app, sizeof app, "%s", a->app ? a->app : "");
        char *hash = strchr(app, '#');
        const char *first = app;
        if (hash) { *hash = 0; first = hash + 1; }
        GetWindowText(dlg, tmpl, sizeof tmpl);
        snprintf(t, sizeof t, tmpl, app);
        SetWindowText(dlg, t);
        GetDlgItemText(dlg, IDD_APP, tmpl, sizeof tmpl);
        snprintf(t, sizeof t, tmpl, first);
        SetDlgItemText(dlg, IDD_APP, t);

        char fmt[64];
        if (LoadString(shell(), IDS_VERSION, fmt, sizeof fmt)) {
            snprintf(t, sizeof t, fmt, "3.11", "");
            SetDlgItemText(dlg, IDD_VERSION, t);
        }
        /* measured on real 3.11 (Calculator's About box): the caller's text goes under the
         * copyright (115), and the box below the first line (110) holds the serial number note,
         * string 517 of USER.EXE, where setup stamps the registration */
        SetDlgItemText(dlg, IDD_EXTRA, a->other ? a->other : "");
        HINSTANCE umod = w16_system_module("USER.EXE");
        if (umod && LoadString(umod, 517, t, sizeof t)) SetDlgItemText(dlg, IDD_OTHER, t);
        SendDlgItemMessage(dlg, IDD_ICON, STM_SETICON, (WPARAM)(a->icon ? a->icon : LoadIcon(NULL, IDI_APPLICATION)), 0);

        /* licensed to: the Linux account's full name; organisation from WIN.INI if set */
        struct passwd *pw = getpwuid(getuid());
        char user[128] = "";
        if (pw) {
            snprintf(user, sizeof user, "%s", pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : pw->pw_name);
            char *comma = strchr(user, ',');
            if (comma) *comma = 0;
            if (!user[0]) snprintf(user, sizeof user, "%s", pw->pw_name); /* GECOS ",,," */
        }
        GetProfileString("arch311", "UserName", user, t, sizeof t);
        SetDlgItemText(dlg, IDD_USER, t);
        GetProfileString("arch311", "Organization", "", t, sizeof t);
        SetDlgItemText(dlg, IDD_ORG, t);

        if (LoadString(shell(), IDS_386, t, sizeof t)) SetDlgItemText(dlg, IDD_MODE, t);
        unsigned long total, avail;
        meminfo(&total, &avail);
        kb_text(t, sizeof t, avail);
        SetDlgItemText(dlg, IDD_MEM, t);
        if (LoadString(shell(), IDS_SYSRES, t, sizeof t)) SetDlgItemText(dlg, IDD_RESLABEL, t);
        if (LoadString(shell(), IDS_PCTFREE, fmt, sizeof fmt)) {
            snprintf(t, sizeof t, fmt, total ? (int)(avail * 100 / total) : 0);
            SetDlgItemText(dlg, IDD_RES, t);
        }
        ShowWindow(GetDlgItem(dlg, IDD_SDLABEL), SW_HIDE);
        ShowWindow(GetDlgItem(dlg, IDD_SD), SW_HIDE);
        return TRUE;
    }
    case WM_COMMAND:
        if (wp == IDOK || wp == IDCANCEL) {
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int ShellAbout(HWND h, LPCSTR app, LPCSTR other, HICON icon)
{
    HINSTANCE m = shell();
    if (!m || !w16_find_res(m, MAKEINTRESOURCE(100), RT_DIALOG)) return FALSE;
    About a = {app, other, icon};
    return DialogBoxParam(m, MAKEINTRESOURCE(100), h, AboutDlgProc, (LPARAM)&a) > 0;
}
