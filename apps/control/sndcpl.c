/* SND.CPL: the Sound applet, ported from the 3.11 SND.CPL (seg1 CPlApplet, seg2 dialog 42). The
 * dialog template, icon and strings come from the user's ripped SND.CPL at run time; sounds play
 * through libw16's sndPlaySound (the Linux sound server).
 *
 * Each event in the Events list carries "key=file,description" as its item data (a LocalAlloc'd
 * string in 3.1, a heap string here); OK writes them back to WIN.INI [sounds]. */
#include "cpl.h"
#include <stdlib.h>
#include <string.h>

#define IDD_SOUND 42
#define IDC_PATH 100
#define IDC_EVENTS 101
#define IDC_EVENTS_LABEL 102
#define IDC_FILES 103
#define IDC_FILES_LABEL 104
#define IDC_TEST 106
#define IDC_ENABLE 108
#define IDC_HELP 5121

static HINSTANCE hInst;           /* [0x184] */
static BOOL fBusy;                /* [0x14] the dialog is up */
static BOOL fChanged;             /* [0x24] another program changed [sounds] meanwhile */
static UINT nWaveDevs;            /* [0x26] */
static DWORD dwContext;           /* [0x40] */
static UINT wHelpMessage;         /* [0x182] "ShellHelp" */
static char szSoundOption[32];    /* [0xec]  string 3, "Sound Option" */
static char szCantPlay[234];      /* [0x186] string 4 */
static char szWarnCaption[64];    /* [0x142] string 5 */
static char szWarnText[168];      /* [0x44]  string 6 */
static char szNone[16];           /* [0x30]  string 7, "<none>" */
static char szSound[30];          /* [0x124] string 8, "Sound" */
static char szHelpFile[24];       /* [0x10c] string 9, "control.hlp" */

static const char szSounds[] = "sounds";

/* ds:0016, the applet table: one entry */
static const struct { int idIcon, idName, idInfo; DWORD dwHelpContext; } applet = {100, 1, 2, 5121};

/* ------------------------------------------------------------------ seg2:0000 */
static void CPHelp(HWND hwnd, DWORD ctx) { WinHelp(hwnd, szHelpFile, HELP_CONTEXT, ctx); }

/* ------------------------------------------------------------------ seg2:0036 */
/* ends the first field of "file, description" at a blank, tab or comma; returns the rest, past the
 * separators */
static char *NextField(char *p)
{
    while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
    if (!*p) return p;
    BOOL comma = *p == ',';
    *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!comma && *p == ',') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return p;
}

/* ------------------------------------------------------------------ seg2:00A1 */
/* the parts of event `idx` (-1: the selected one); a "<none>" file is "", an empty description is
 * the key */
static void GetEvent(HWND hDlg, int idx, LPSTR key, LPSTR file, LPSTR desc)
{
    char buf[256];
    HWND h = GetDlgItem(hDlg, IDC_EVENTS);
    if (idx == -1) idx = (int)SendMessage(h, LB_GETCURSEL, 0, 0);
    if (idx == -1) {
        if (key) *key = 0;
        if (file) *file = 0;
        if (desc) *desc = 0;
        return;
    }
    lstrcpy(buf, (LPCSTR)SendMessage(h, LB_GETITEMDATA, idx, 0));
    char *p = strchr(buf, '=');
    *p = 0;
    if (key) lstrcpy(key, buf);
    char *f = p + 1, *rest = NextField(f);
    if (file) {
        if (!lstrcmpi(f, szNone)) *file = 0;
        else lstrcpy(file, f);
    }
    if (desc) lstrcpy(desc, *rest ? rest : buf);
}

/* ------------------------------------------------------------------ seg2:01CD */
/* whether the file can be found (OpenFile's search); `path` becomes its full name */
static BOOL FileExists(LPSTR path)
{
    OFSTRUCT of;
    UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    BOOL ok = OpenFile(path, &of, OF_EXIST | OF_SHARE_DENY_NONE) != HFILE_ERROR;
    if (ok) OemToAnsi(of.szPathName, path);
    SetErrorMode(mode);
    return ok;
}

/* ------------------------------------------------------------------ seg2:021A */
static LPSTR FileNamePart(LPSTR s)
{
    LPSTR p = s + lstrlen(s);
    while (s <= p && *p != '/' && *p != '\\' && *p != ':') p--;
    return p + 1;
}

/* ------------------------------------------------------------------ seg2:0262 */
/* cuts the file name off a path, with its backslash unless that ends a drive root ("C:\") */
static void StripFileName(LPSTR s)
{
    LPSTR p = FileNamePart(s);
    if (p > s + 1 && (p[-1] == '/' || p[-1] == '\\') && p[-2] != ':') p--;
    *p = 0;
}

/* ------------------------------------------------------------------ seg2:02AC */
/* makes `dir` current and lists it in Files (directories and drives, the .wav files, "<none>");
 * the list is kept when it already shows that directory. FALSE (and the old directory back) if it
 * cannot be entered. */
static BOOL ChangeDirAndList(HWND hDlg, LPSTR dir)
{
    char old[128], now[128];
    int n = lstrlen(dir) - 1;
    if (n > 0 && (dir[n] == '/' || dir[n] == '\\') && dir[n - 1] != ':') dir[n] = 0;
    w16_getcwd(old, sizeof old);
    if (w16_chdir(dir)) { w16_chdir(old); return FALSE; }
    w16_getcwd(now, sizeof now);
    HWND hList = GetDlgItem(hDlg, IDC_FILES);
    if (SendMessage(hList, LB_GETCOUNT, 0, 0) && !lstrcmpi(now, old)) return TRUE;
    char spec[8] = "*.*";
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    DlgDirList(hDlg, spec, IDC_FILES, IDC_PATH, DDL_EXCLUSIVE | DDL_DRIVES | DDL_DIRECTORY);
    SendMessage(hList, LB_DIR, 0, (LPARAM) "*.wav");
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)szNone);
    return TRUE;
}

/* ------------------------------------------------------------------ seg2:03A3 */
/* Test needs a wave device and a file (not a directory, not "<none>") selected in Files; without
 * a wave device the lists are disabled too */
static void UpdateButtons(HWND hDlg)
{
    char buf[128];
    HWND hList = GetDlgItem(hDlg, IDC_FILES);
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    GetEvent(hDlg, -1, NULL, buf, NULL);
    BOOL dir = DlgDirSelect(hDlg, buf, IDC_FILES);
    if (!dir) {
        int n = lstrlen(buf) - 1;
        if (n >= 0 && buf[n] == '.') buf[n] = 0;
    }
    BOOL test = nWaveDevs && !dir && sel != -1 && buf[0] && lstrcmp(buf, szNone);
    EnableWindow(GetDlgItem(hDlg, IDC_TEST), test);
    EnableWindow(GetDlgItem(hDlg, IDC_EVENTS), nWaveDevs);
    EnableWindow(hList, nWaveDevs);
    EnableWindow(GetDlgItem(hDlg, IDC_EVENTS_LABEL), nWaveDevs);
    EnableWindow(GetDlgItem(hDlg, IDC_FILES_LABEL), nWaveDevs);
}

/* ------------------------------------------------------------------ seg2:04A0 */
/* selects event `idx` (-1: keeps the selection) and shows its file in Files */
static void SelectEvent(HWND hDlg, int idx)
{
    char buf[128];
    if (idx != -1) SendDlgItemMessage(hDlg, IDC_EVENTS, LB_SETCURSEL, idx, 0);
    GetEvent(hDlg, idx, NULL, buf, NULL);
    if (FileExists(buf)) {
        StripFileName(buf);
        if (ChangeDirAndList(hDlg, buf)) {
            GetEvent(hDlg, idx, NULL, buf, NULL);
            SendDlgItemMessage(hDlg, IDC_FILES, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)FileNamePart(buf));
        } else
            SendDlgItemMessage(hDlg, IDC_FILES, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)szNone);
    } else {
        char dot[2] = ".";
        ChangeDirAndList(hDlg, dot);
        SendDlgItemMessage(hDlg, IDC_FILES, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)szNone);
    }
    UpdateButtons(hDlg);
}

/* ------------------------------------------------------------------ seg2:054A */
static void TestSound(HWND hDlg)
{
    char buf[130];
    if (!nWaveDevs) return;
    DlgDirSelect(hDlg, buf, IDC_FILES);
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    BOOL ok = sndPlaySound(buf, SND_ASYNC | SND_NODEFAULT);
    SetCursor(old);
    if (!ok) MessageBox(hDlg, szCantPlay, szSoundOption, MB_OK);
}

/* ------------------------------------------------------------------ seg2:05AD */
/* adds the event (found by its description) or replaces its data */
static void AddOrUpdateEvent(HWND hDlg, LPCSTR key, LPCSTR file, LPCSTR desc)
{
    HWND h = GetDlgItem(hDlg, IDC_EVENTS);
    int found = (int)SendMessage(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)desc), idx;
    if (found == -1)
        idx = (int)SendMessage(h, LB_ADDSTRING, 0, (LPARAM)desc);
    else {
        free((char *)SendMessage(h, LB_GETITEMDATA, found, 0));
        idx = found;
    }
    LPSTR p = malloc(lstrlen(file) + lstrlen(desc) + lstrlen(key) + 3);
    wsprintf(p, "%s=%s,%s", key, file, desc);
    SendMessage(h, LB_SETITEMDATA, idx, (LPARAM)p);
    if (found == -1) SendMessage(h, LB_SETCURSEL, idx, 0);
}

/* ------------------------------------------------------------------ seg2:0689 */
/* gives event `idx` the file (its full name, or "" when it cannot be found) */
static void SetEventFile(HWND hDlg, int idx, LPSTR file)
{
    char key[128], desc[128];
    if (!FileExists(file)) file = "";
    GetEvent(hDlg, idx, key, NULL, desc);
    if (desc[0]) AddOrUpdateEvent(hDlg, key, file, desc);
}

/* ------------------------------------------------------------------ seg2:06DC */
/* replaces WIN.INI [sounds] with the events and tells every window */
static void SaveEvents(HWND hDlg)
{
    HWND h = GetDlgItem(hDlg, IDC_EVENTS);
    int n = (int)SendMessage(h, LB_GETCOUNT, 0, 0);
    WriteProfileString(szSounds, NULL, NULL);
    for (int i = 0; i < n; i++) {
        LPSTR key = (LPSTR)SendMessage(h, LB_GETITEMDATA, i, 0), p = strchr(key, '=');
        *p = 0;
        WriteProfileString(szSounds, key, p + 1);
    }
    SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM)szSounds);
}

/* ------------------------------------------------------------------ seg2:0764 */
static void OnCommand(HWND hDlg, int id, int code, HWND hwndCtl)
{
    char buf[130];
    (void)hwndCtl;
    switch (id) {
    case IDOK: {
        HWND f = GetFocus();
        /* Enter in Files acts on the selection, as a double click */
        if (f && GetDlgCtrlID(f) == IDC_FILES) {
            PostMessage(hDlg, WM_COMMAND, IDC_FILES, W16_CMD_LPARAM(f, LBN_DBLCLK));
            return;
        }
        if (fChanged) {
            int r = MessageBox(hDlg, szWarnText, szWarnCaption, MB_YESNOCANCEL | MB_ICONEXCLAMATION);
            if (r == IDCANCEL) return;
            if (r == IDNO) {
                PostMessage(hDlg, WM_COMMAND, IDCANCEL, W16_CMD_LPARAM(GetDlgItem(hDlg, IDCANCEL), 0));
                return;
            }
        }
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        sndPlaySound(NULL, 0);
        SaveEvents(hDlg);
        BOOL beep = FALSE;
        SystemParametersInfo(SPI_GETBEEP, 0, &beep, 0);
        if (IsDlgButtonChecked(hDlg, IDC_ENABLE) != (UINT)beep)
            SystemParametersInfo(SPI_SETBEEP, !beep, NULL, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
        SetCursor(old);
        EndDialog(hDlg, 1);
        return;
    }
    case IDCANCEL:
        sndPlaySound(NULL, 0);
        EndDialog(hDlg, 0);
        return;
    case IDC_EVENTS:
        if (code == LBN_SELCHANGE) SelectEvent(hDlg, -1);
        else if (code == LBN_DBLCLK && IsWindowEnabled(GetDlgItem(hDlg, IDC_TEST)))
            PostMessage(hDlg, WM_COMMAND, IDC_TEST, 0);
        return;
    case IDC_FILES:
        if (code == LBN_SELCHANGE) {
            if (!DlgDirSelect(hDlg, buf, IDC_FILES)) SetEventFile(hDlg, -1, buf);
            UpdateButtons(hDlg);
        } else if (code == LBN_DBLCLK) {
            if (DlgDirSelect(hDlg, buf, IDC_FILES)) ChangeDirAndList(hDlg, buf);
            else if (IsWindowEnabled(GetDlgItem(hDlg, IDC_TEST)))
                PostMessage(hDlg, WM_COMMAND, IDC_TEST, 0);
        }
        return;
    case IDC_TEST:
        TestSound(hDlg);
        return;
    case IDC_HELP:
        CPHelp(hDlg, dwContext);
        return;
    }
}

/* ------------------------------------------------------------------ seg2:092D */
/* the events from WIN.INI [sounds]; returns what the dialog proc returns for WM_INITDIALOG: 3.1
 * hands back the EnableWindow result of its last call, which is FALSE, so the dialog manager
 * leaves the focus to activation */
static BOOL InitDialog(HWND hDlg)
{
    char buf[128];
    fChanged = FALSE;
    GetWindowsDirectory(buf, sizeof buf);
    w16_chdir(buf);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, 0x1000);
    if (!h) return FALSE;
    LPSTR keys = GlobalLock(h);
    if (GetProfileString(szSounds, NULL, "", keys, 0x1000))
        for (LPSTR k = keys; *k; k += lstrlen(k) + 1) {
            GetProfileString(szSounds, k, "", buf, sizeof buf);
            char *rest = NextField(buf);
            AddOrUpdateEvent(hDlg, k, buf, *rest ? rest : k);
        }
    GlobalUnlock(h);
    GlobalFree(h);
    BOOL beep = FALSE;
    SystemParametersInfo(SPI_GETBEEP, 0, &beep, 0);
    CheckDlgButton(hDlg, IDC_ENABLE, beep);
    nWaveDevs = waveOutGetNumDevs();
    SelectEvent(hDlg, SendDlgItemMessage(hDlg, IDC_EVENTS, LB_GETCOUNT, 0, 0) ? 0 : -1);
    return FALSE;
}

/* ------------------------------------------------------------------ seg2:0A5C */
static void FreeEvents(HWND hDlg)
{
    HWND h = GetDlgItem(hDlg, IDC_EVENTS);
    for (int i = (int)SendMessage(h, LB_GETCOUNT, 0, 0); i-- > 0;)
        free((char *)SendMessage(h, LB_GETITEMDATA, i, 0));
}

/* ------------------------------------------------------------------ seg2:0AA5 */
static BOOL SoundDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        FreeEvents(hDlg);
        return FALSE;
    case WM_WININICHANGE:
        if (!lstrcmpi((LPCSTR)lParam, szSounds) || !*(LPCSTR)lParam) fChanged = TRUE;
        return FALSE;
    case WM_INITDIALOG:
        return InitDialog(hDlg);
    case WM_COMMAND:
        OnCommand(hDlg, (int)wParam, HIWORD(lParam), W16_CMD_HWND(lParam));
        return TRUE;
    }
    if (msg == wHelpMessage && wHelpMessage) {
        CPHelp(hDlg, dwContext);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg1:00D3 */
static void RunSound(HWND hwnd)
{
    if (fBusy) return;
    fBusy = TRUE;
    dwContext = applet.dwHelpContext;
    DialogBox(hInst, MAKEINTRESOURCE(IDD_SOUND), hwnd, SoundDlgProc);
    fBusy = FALSE;
}

/* ------------------------------------------------------------------ seg1:001C (LibMain) */
static BOOL LoadModule(void)
{
    if (hInst) return TRUE;
    hInst = w16_load_module("SND.CPL");
    if (!hInst) return FALSE;
    LoadString(hInst, 3, szSoundOption, sizeof szSoundOption);
    LoadString(hInst, 4, szCantPlay, sizeof szCantPlay);
    LoadString(hInst, 5, szWarnCaption, sizeof szWarnCaption);
    LoadString(hInst, 6, szWarnText, sizeof szWarnText);
    LoadString(hInst, 7, szNone, sizeof szNone);
    LoadString(hInst, 8, szSound, sizeof szSound);
    LoadString(hInst, 9, szHelpFile, sizeof szHelpFile);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0116 */
LRESULT Sound_CPlApplet(HWND hwndCPl, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    switch (msg) {
    case CPL_INIT:
        if (!LoadModule()) return 0;
        wHelpMessage = RegisterWindowMessage("ShellHelp");
        return 1;
    case CPL_GETCOUNT:
        return 1;
    case CPL_DBLCLK:
        RunSound(hwndCPl);
        return 0;
    case CPL_NEWINQUIRE: {
        NEWCPLINFO *ni = (NEWCPLINFO *)lParam2;
        (void)lParam1;
        ni->hIcon = LoadIcon(hInst, MAKEINTRESOURCE(applet.idIcon));
        if (!LoadString(hInst, applet.idName, ni->szName, sizeof ni->szName)) ni->szName[0] = 0;
        if (!LoadString(hInst, applet.idInfo, ni->szInfo, sizeof ni->szInfo)) ni->szInfo[0] = 0;
        ni->lData = 0;
        ni->dwSize = sizeof *ni;
        ni->dwFlags = 0;
        ni->dwHelpContext = applet.dwHelpContext;
        lstrcpy(ni->szHelpFile, szHelpFile);
        return 1;
    }
    }
    return 0;
}
