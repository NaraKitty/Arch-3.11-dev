/* SHELL.DLL: ShellAbout (seg9:0004) and its dialog procedure AboutDlgProc (seg9:013A), on dialog 100
 * and the strings of the user's ripped SHELL.DLL and USER.EXE. Verified against real 3.11 with
 * WINMINE's About box (shots/winmine-e); what differs on arch311 on purpose: the licensed user is the
 * Linux account's name (WIN.INI [arch311] UserName / Organization override it) where 3.1 has the
 * name Setup wrote into USER.EXE, and Memory / System Resources describe the Linux machine. */
#include "w16int.h"
#include "commdlg.h"
#include <pwd.h>
#include <unistd.h>

enum {
    IDD_APP = 101, IDD_MODE = 102, IDD_MEMLABEL = 103, IDD_MEM = 104, IDD_RES = 105, IDD_SDLABEL = 106,
    IDD_SD = 107, IDD_USER = 108, IDD_ORG = 109, IDD_SERIAL = 110, IDD_ICON = 111, IDD_VERSION = 112,
    IDD_RESLABEL = 113, IDD_OTHER = 115,
};
enum { IDS_386 = 213, IDS_SYSRES = 215, IDS_VERSION = 216, IDS_DEBUG = 217, IDS_KBFREE = 218, IDS_PCTFREE = 220 };
/* USER.EXE strings: Setup's user and organisation, the version, the serial number note */
enum { IDS_USER_NAME = 514, IDS_USER_ORG = 515, IDS_USER_VERSION = 516, IDS_USER_SERIAL = 517 };

typedef struct { HICON icon; LPCSTR app, other; } About; /* the block ShellAbout hands over */

static HINSTANCE shell(void) { return w16_system_module("SHELL.DLL"); }

/* seg9:0050: "%ld" with WIN.INI [intl] sThousand (default ",") every three digits */
static void thousands(char *out, size_t cb, unsigned long v)
{
    char sep[8], d[32];
    GetProfileString("intl", "sThousand", ",", sep, sizeof sep);
    int n = snprintf(d, sizeof d, "%lu", v), o = 0;
    for (int i = 0; i < n && o + 2 < (int)cb; i++) {
        out[o++] = d[i];
        if ((n - i - 1) % 3 == 0 && i < n - 1 && sep[0]) out[o++] = sep[0];
    }
    out[o] = 0;
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
        HINSTANCE user = w16_system_module("USER.EXE");
        char app[128], tmpl[160], t[300], fmt[64], ver[16];
        /* "Title#Program": Title is the whole caption, Program goes on the first line */
        snprintf(app, sizeof app, "%s", a->app ? a->app : "");
        char *hash = strchr(app, '#');
        const char *first = app;
        if (hash) {
            *hash = 0;
            SetWindowText(dlg, app);
            first = hash + 1;
        } else {
            GetWindowText(dlg, tmpl, 64);
            wsprintf(t, tmpl, app);
            SetWindowText(dlg, t);
        }
        GetDlgItemText(dlg, IDD_APP, tmpl, 64);
        wsprintf(t, tmpl, first);
        SetDlgItemText(dlg, IDD_APP, t);
        SetDlgItemText(dlg, IDD_OTHER, a->other ? a->other : "");
        SendDlgItemMessage(dlg, IDD_ICON, STM_SETICON, (WPARAM)a->icon, 0);
        if (!a->icon) ShowWindow(GetDlgItem(dlg, IDD_ICON), SW_HIDE);
        /* the serial number note and "Version 3.11 " (with "(Debug)" on a debug system) */
        if (!user || !LoadString(user, IDS_USER_VERSION, ver, 16)) snprintf(ver, sizeof ver, "3.11");
        if (user && LoadString(user, IDS_USER_SERIAL, t, 200)) SetDlgItemText(dlg, IDD_SERIAL, t);
        if (LoadString(shell(), IDS_VERSION, fmt, sizeof fmt)) {
            wsprintf(t, fmt, ver, "");
            SetDlgItemText(dlg, IDD_VERSION, t);
        }
        if (LoadString(shell(), IDS_386, t, 64)) SetDlgItemText(dlg, IDD_MODE, t);
        /* memory: "%s KB Free"; no EMS, so the SMARTDrive label goes */
        unsigned long total, avail;
        meminfo(&total, &avail);
        char kb[48];
        thousands(kb, sizeof kb, avail);
        if (!LoadString(shell(), IDS_KBFREE, fmt, 16)) strcpy(fmt, "%s KB Free");
        wsprintf(t, fmt, kb);
        SetDlgItemText(dlg, IDD_MEM, t);
        ShowWindow(GetDlgItem(dlg, IDD_SDLABEL), SW_HIDE);
        /* protected mode: "System Resources:" in place of "Expanded Memory", then "%d%% Free" */
        if (LoadString(shell(), IDS_SYSRES, t, 64)) SetDlgItemText(dlg, IDD_RESLABEL, t);
        if (LoadString(shell(), IDS_PCTFREE, fmt, 64)) {
            wsprintf(t, fmt, total ? (int)(avail * 100 / total) : 0);
            SetDlgItemText(dlg, IDD_RES, t);
        }
        /* licensed to: the Linux account's full name; organisation from WIN.INI if set */
        struct passwd *pw = getpwuid(getuid());
        char uname[128] = "";
        if (pw) {
            snprintf(uname, sizeof uname, "%s", pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : pw->pw_name);
            char *comma = strchr(uname, ',');
            if (comma) *comma = 0;
            if (!uname[0]) snprintf(uname, sizeof uname, "%s", pw->pw_name); /* GECOS ",,," */
        }
        GetProfileString("arch311", "UserName", uname, t, sizeof t);
        SetDlgItemText(dlg, IDD_USER, t);
        GetProfileString("arch311", "Organization", "", t, sizeof t);
        SetDlgItemText(dlg, IDD_ORG, t);
        return TRUE;
    }
    case WM_PAINT: {
        /* without an icon the Windows logo (bitmap 130, 64 x 64) is drawn at (10, 10) */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(dlg, &ps);
        if (!SendDlgItemMessage(dlg, IDD_ICON, STM_GETICON, 0, 0)) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bm = mem ? LoadBitmap(shell(), MAKEINTRESOURCE(130)) : NULL;
            if (bm) {
                HGDIOBJ old = SelectObject(mem, bm);
                BitBlt(dc, 10, 10, 64, 64, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bm);
            }
            if (mem) DeleteDC(mem);
        }
        EndPaint(dlg, &ps);
        return TRUE;
    }
    case WM_COMMAND: /* any command closes it */
        EndDialog(dlg, TRUE);
        return TRUE;
    }
    return FALSE;
}

int ShellAbout(HWND h, LPCSTR app, LPCSTR other, HICON icon)
{
    HINSTANCE m = shell();
    if (!m || !w16_find_res(m, MAKEINTRESOURCE(100), RT_DIALOG)) return FALSE;
    About a = {icon, app, other};
    return DialogBoxParam(m, MAKEINTRESOURCE(100), h, AboutDlgProc, (LPARAM)&a) > 0;
}
