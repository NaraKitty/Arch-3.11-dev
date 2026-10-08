/*
 * Control Panel - native 64-bit port of the Windows 3.11 CONTROL.EXE (15,872 bytes).
 *
 * Ported function by function from the disassembly of the user's own CONTROL.EXE; each function
 * notes the original seg1:offset and globals are noted as [ds:offset]. The menu, accelerators,
 * icon and strings are loaded at run time from the user's ripped CONTROL.EXE.
 *
 * Deliberate differences:
 *  - applet "libraries" are native modules (cplreg.c) instead of LoadLibrary'd 16-bit DLLs; the
 *    CPlApplet protocol between host and applet is unchanged;
 *  - list box item data holds a pointer to the applet entry instead of MAKELONG(module, index);
 *  - CONTROL.INI is kept in the arch311 configuration folder (like WIN.INI), not C:\WINDOWS;
 *  - no WH_MSGFILTER hook / RegisterPenApp / previous-instance hand-over (Linux runs each
 *    program in its own process); F1 help still works through the accelerator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h"
#include "cpl.h"

const char *w16_app_module = "CONTROL.EXE";

#define IDM_SEARCH 33
#define IDM_CONTENTS 40
#define IDM_HELPHELP 41
#define IDM_ABOUT 50
#define IDM_EXIT 51
#define IDM_HELPSEL 52
#define IDM_APPLET 100   /* 0x64 + n: the Settings menu entries */
#define ID_LB 0x14
#define ID_STATUS 0x15
#define WM_OPENAPPLET 0x7E8 /* posted with an applet name (command line) */

#define LBS_OWNERDRAWFIXED_ 0x0010
#define LBS_MULTICOLUMN_ 0x0200
#define LBS_WANTKEYBOARDINPUT_ 0x0400

static const char szIni[] = "control.ini";   /* [0xec] */
static const char szMMCPL[] = "MMCPL";
static const char szNumApps[] = "NumApps";
static const char szDontLoad[] = "don't load";
static const char szHelpFile[] = "control.hlp";
static const char *const szPosKeys[4] = {"X", "Y", "W", "H"}; /* [0x74..0x7a] */

typedef struct CplModule CplModule;
typedef struct {
    HICON hIcon;      /* +0 */
    char *name;       /* +2  without '&' */
    char *info;       /* +4  status line text */
    int width;        /* +6  title width in the icon title font */
    LPARAM lData;     /* +8 */
    DWORD helpContext;/* +0xc */
    char *helpFile;   /* +0x10 */
    CplModule *mod;
    int index;        /* applet number inside its module */
} CplItem;

struct CplModule {
    const CplModuleDef *def;
    APPLET_PROC proc;
    CplItem *items;
    int count;
};

static HINSTANCE hInst;             /* [0x1ee] */
static HWND hwndMain;               /* [0x6e] */
static HWND hwndLB, hwndStatus;     /* [0x27e], [0x284] */
static int nItems;                  /* [0x70] entries in the list and the Settings menu */
static int cxColumn;                /* [0x72] widest title */
static int cyIcon, cxIcon;          /* [0xe8], [0x1f8] */
static int cyTitle;                 /* [0xea] icon title font height */
static int dxIcon, dyIcon;          /* [0x27a], [0x27c] icon offset inside an entry */
static HFONT hfontTitle;            /* [0x1ec] */
static HBRUSH hbrBack;              /* [0x282] COLOR_APPWORKSPACE */
static CplModule *modules[64];      /* [0x1fa] */
static int fInApplet;               /* [0x9e] */
static char szCaption[64], szLoading[32], szNoMem[256], szNoCpl[256]; /* [0xe0], [0xe2], [0xe4], [0xe6] */
static UINT menuSelId, menuSelFlags;/* [0x84], [0x86] */
/* list box item data is an index into this table (DRAWITEMSTRUCT.itemData is a 32-bit DWORD) */
static CplItem *g_items[256];
static int g_nitems;

/* ------------------------------------------------------------------ helpers (seg1:0028, 0072, 03F1) */
static int TitleWidth(HDC hdc, const char *s)
{
    char buf[128], *o = buf;
    for (; *s && o < buf + sizeof buf - 1; s++) {
        if (*s == '&') s++;
        if (!*s) break;
        *o++ = *s;
    }
    *o = 0;
    return LOWORD(GetTextExtent(hdc, buf, lstrlen(buf)));
}

/* gives a name its '&' mnemonic if it has none */
static void AddMnemonic(char *s, size_t cb)
{
    for (const char *p = s; *p; p++)
        if (*p == '&') {
            if (p[1] != '&') return;
            p++;
        }
    char tmp[128];
    snprintf(tmp, sizeof tmp, "&%s", s);
    snprintf(s, cb, "%s", tmp);
}

/* case-insensitive compare ignoring '&' in the pattern */
static BOOL NameMatch(const char *pat, const char *name)
{
    char a[2] = {0, 0}, b[2] = {0, 0};
    while (*name) {
        if (*pat == '&') { pat++; continue; }
        a[0] = *name++; b[0] = *pat++;
        AnsiUpper(a); AnsiUpper(b);
        if (a[0] != b[0]) return FALSE;
    }
    return *pat == 0;
}

static CplItem *ItemAt(int i)
{
    LRESULT d = SendMessage(hwndLB, LB_GETITEMDATA, i, 0);
    return (d < 0 || d >= g_nitems) ? NULL : g_items[d];
}

/* ------------------------------------------------------------------ seg1:00D4: InitItem */
static void InitItem(CplModule *m, int idx, CPLINFO *ci, NEWCPLINFO *ni)
{
    CplItem *it = &m->items[idx];
    char buf[128];
    HINSTANCE res = m->def->resfile ? w16_load_module(m->def->resfile) : NULL;
    it->mod = m;
    it->index = idx;
    if (ni) {
        it->lData = ni->lData;
        it->helpContext = ni->dwHelpContext;
        it->helpFile = ni->szHelpFile[0] ? strdup(ni->szHelpFile) : NULL;
        it->hIcon = ni->hIcon;
        lstrcpy(buf, ni->szInfo);
    } else {
        it->lData = ci->lData;
        it->helpContext = 0;
        it->helpFile = NULL;
        it->hIcon = LoadIcon(res, MAKEINTRESOURCE(ci->idIcon));
        if (!LoadString(res, ci->idInfo, buf, 0x7f)) buf[0] = 0;
    }
    it->info = strdup(buf);
    if (ni) lstrcpy(buf, ni->szName);
    else if (!LoadString(res, ci->idName, buf, 0x7b)) buf[0] = 0;
    it->name = malloc(strlen(buf) + 1);
    char *o = it->name;
    for (const char *s = buf; *s; s++) if (*s != '&') *o++ = *s;
    *o = 0;

    char skip[10];
    GetPrivateProfileString(szDontLoad, it->name, "", skip, sizeof skip, szIni);
    if (skip[0] || !hwndMain) return;

    HDC hdc = GetDC(NULL);
    HGDIOBJ old = SelectObject(hdc, hfontTitle);
    it->width = TitleWidth(hdc, buf);
    if (it->width > cxColumn) cxColumn = it->width;
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    AddMnemonic(buf, sizeof buf);
    lstrcat(buf, "...");
    HMENU hSettings = GetSubMenu(GetMenu(hwndMain), 0);
    /* a new menu column every 16 applets */
    UINT fl = MF_BYPOSITION | MF_STRING | ((nItems && !(nItems & 15)) ? MF_MENUBARBREAK : 0);
    InsertMenu(hSettings, nItems, fl, IDM_APPLET + nItems, buf);
    nItems++;
    if (g_nitems < 256) {
        g_items[g_nitems] = it;
        SendMessage(hwndLB, LB_ADDSTRING, 0, g_nitems++);
    }
}

/* ------------------------------------------------------------------ seg1:034F: InquireApplets */
static void InquireApplets(CplModule *m, int n)
{
    for (int i = 0; i < n; i++) {
        NEWCPLINFO ni;
        memset(&ni, 0, sizeof ni);
        m->proc(hwndMain, CPL_NEWINQUIRE, i, (LPARAM)&ni);
        if (ni.dwSize == sizeof ni) {
            InitItem(m, i, NULL, &ni);
        } else {
            CPLINFO ci;
            memset(&ci, 0, sizeof ci);
            m->proc(hwndMain, CPL_INQUIRE, i, (LPARAM)&ci);
            InitItem(m, i, &ci, NULL);
        }
    }
}

/* ------------------------------------------------------------------ seg1:04FF: LoadModule */
static BOOL LoadModule(int slot, const CplModuleDef *def)
{
    for (int i = 0; i < slot; i++) /* seg1:04CA: already loaded */
        if (modules[i] && modules[i]->def == def) return FALSE;
    CplModule *m = calloc(1, sizeof *m);
    m->def = def;
    m->proc = def->proc;
    if (hwndMain) {
        char t[96];
        lstrcpy(t, szLoading);
        lstrcat(t, def->file);
        SetWindowText(hwndStatus, t);
        UpdateWindow(hwndMain);
    }
    if (!m->proc(hwndMain, CPL_INIT, 0, 0)) { free(m); return FALSE; }
    m->count = (int)m->proc(hwndMain, CPL_GETCOUNT, 0, 0);
    m->items = calloc(m->count > 0 ? m->count : 1, sizeof *m->items);
    modules[slot] = m;
    InquireApplets(m, m->count);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:06F8: LoadAllModules */
static int LoadAllModules(void)
{
    int n = 0;
    for (const CplModuleDef *d = cpl_modules; d->file && n < 63; d++)
        if (LoadModule(n, d)) n++;
    modules[n] = NULL;
    return n;
}

/* ------------------------------------------------------------------ seg1:088F: LoadStrings */
static BOOL LoadStrings(void)
{
    char *dst[4] = {szCaption, szLoading, szNoMem, szNoCpl};
    int cb[4] = {sizeof szCaption, sizeof szLoading, sizeof szNoMem, sizeof szNoCpl};
    for (int i = 0; i < 4; i++)
        if (!LoadString(hInst, 0x400 + i, dst[i], cb[i])) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:090A: CreateChildren */
static BOOL CreateChildren(void)
{
    LOGFONT lf;
    TEXTMETRIC tm;
    SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof lf, &lf, 0);
    hfontTitle = CreateFontIndirect(&lf);
    HDC hdc = GetDC(NULL);
    HGDIOBJ old = hfontTitle ? SelectObject(hdc, hfontTitle) : NULL;
    GetTextMetrics(hdc, &tm);
    if (old) SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    cyTitle = tm.tmHeight + tm.tmExternalLeading;
    hbrBack = CreateSolidBrush(GetSysColor(COLOR_APPWORKSPACE));
    hwndLB = CreateWindow("lb", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | LBS_OWNERDRAWFIXED_ |
                          LBS_NOINTEGRALHEIGHT | LBS_MULTICOLUMN_ | LBS_WANTKEYBOARDINPUT_,
                          0, 0, 0, 0, hwndMain, (HMENU)ID_LB, hInst, NULL);
    if (!hwndLB) return FALSE;
    hwndStatus = CreateWindow("Text", szLoading, WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwndMain,
                              (HMENU)ID_STATUS, hInst, NULL);
    return hwndStatus != NULL;
}

/* ------------------------------------------------------------------ seg1:09FC: LoadApplets */
static BOOL LoadApplets(void)
{
    HCURSOR hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    SendMessage(hwndLB, WM_SETREDRAW, FALSE, 0);
    int ok = LoadAllModules();
    if (ok) {
        SendMessage(hwndLB, LB_SETCOLUMNWIDTH, cxColumn, 0);
        dxIcon = (cxColumn - cxIcon) / 2;
        dyIcon = cyTitle / 2;
        SendMessage(hwndLB, LB_SETCURSEL, 0, 0);
    }
    if (!IsIconic(hwndMain)) {
        int count = (int)SendMessage(hwndLB, LB_GETCOUNT, 0, 0);
        if (GetPrivateProfileInt(szMMCPL, szNumApps, 0, szIni) != count) {
            char t[16];
            wsprintf(t, "%d", count);
            WritePrivateProfileString(szMMCPL, szNumApps, t, szIni);
            RECT rc, wr;
            GetClientRect(hwndLB, &rc);
            int perRow = cxColumn ? rc.right / cxColumn : 1;
            if (!perRow) perRow = 1;
            int rows = (count + perRow - 1) / perRow;
            int need = rows * (cyTitle / 2 + cyIcon + cyTitle);
            if (need > rc.bottom) {
                /* grow to six columns (or the screen) and to all rows */
                GetWindowRect(hwndMain, &wr);
                int w = cxColumn * 6 - wr.left - rc.right + wr.right;
                int sw = GetSystemMetrics(SM_CXSCREEN);
                if (w > sw) w = sw;
                if (sw - w < wr.left) wr.left = sw - w;
                perRow = cxColumn ? w / cxColumn : 1;
                if (!perRow) perRow = 1;
                rows = (count + perRow - 1) / perRow;
                int h = rows * (cyTitle / 2 + cyIcon + cyTitle) - wr.top - rc.bottom + wr.bottom;
                int sh = GetSystemMetrics(SM_CYSCREEN);
                if (h > sh) h = sh;
                if (sh - h < wr.top) wr.top = sh - h;
                SetWindowPos(hwndMain, NULL, wr.left, wr.top, w, h, SWP_NOZORDER);
            }
        }
    }
    SendMessage(hwndLB, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwndMain, NULL, TRUE);
    SetCursor(hcur);
    return ok;
}

/* ------------------------------------------------------------------ seg1:0C02: RunApplet */
static CplItem *RunApplet(int i, UINT msg)
{
    if (fInApplet) return NULL;
    fInApplet = 1;
    CplItem *it = ItemAt(i);
    HCURSOR hcur = NULL;
    if (msg == CPL_DBLCLK) hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (it) it->mod->proc(hwndMain, msg, it->index, it->lData);
    if (msg == CPL_DBLCLK) SetCursor(hcur);
    fInApplet = 0;
    return it;
}

/* ------------------------------------------------------------------ seg1:0DC1 / 0E5A: window position */
static void ReadWindowPos(int pos[4])
{
    int def[4] = {(SHORT)CW_USEDEFAULT, 0, 0x1ae, 0xf0};
    for (int i = 0; i < 4; i++) pos[i] = GetPrivateProfileInt(szMMCPL, szPosKeys[i], def[i], szIni);
    /* the 16-bit original stored 0x8000; as a signed int that reads back as -32768 */
    if (pos[0] == (SHORT)CW_USEDEFAULT) pos[0] = CW_USEDEFAULT;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    while (pos[2] >= sw) pos[2] /= 2;
    while (pos[3] >= sh) pos[3] /= 2;
}

static void SaveWindowPos(HWND hwnd)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    GetWindowPlacement(hwnd, &wp);
    RECT *r = &wp.rcNormalPosition;
    int v[4] = {r->left, r->top, r->right - r->left, r->bottom - r->top};
    for (int i = 0; i < 4; i++) {
        char t[16];
        wsprintf(t, "%d", v[i]);
        WritePrivateProfileString(szMMCPL, szPosKeys[i], t, szIni);
    }
}

/* ------------------------------------------------------------------ seg1:0ED4: FreeModules */
static void FreeModules(void)
{
    for (int i = 0; modules[i]; i++) {
        CplModule *m = modules[i];
        m->proc(hwndMain, CPL_EXIT, 0, 0);
        free(m->items);
        free(m);
        modules[i] = NULL;
    }
    if (hfontTitle) DeleteObject(hfontTitle);
    if (hbrBack) DeleteObject(hbrBack);
}

/* ------------------------------------------------------------------ seg1:0F47, 1E71: help */
static void HelpFailed(HWND hwnd) { MessageBox(hwnd, szNoMem, szCaption, MB_ICONHAND | MB_SYSTEMMODAL); }

static void AppletHelp(HWND hwnd, const char *file, DWORD ctx)
{
    if (!WinHelp(hwnd, file ? file : szHelpFile, ctx ? HELP_CONTEXT : HELP_INDEX, ctx)) HelpFailed(hwnd);
}

/* ------------------------------------------------------------------ seg1:0F65: FindMnemonic */
static int FindMnemonic(int ch, int start)
{
    for (int i = start + 1;; i++) {
        if (i == nItems) {
            if (!start) return -1;
            i = 0;
        }
        if (i == start) return -1;
        CplItem *it = ItemAt(i);
        if (it && it->name[0] && (it->name[0] | 0x20) == ch) return i;
    }
}

/* ------------------------------------------------------------------ seg1:1009: CplWndProc */
static LRESULT CplWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        hwndMain = hwnd;
        return CreateChildren() ? 0 : -1;
    case WM_DESTROY:
        WinHelp(hwnd, szHelpFile, HELP_QUIT, 0);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
    case WM_ENDSESSION:
        SaveWindowPos(hwnd);
        break;
    case WM_SETFOCUS:
        SetFocus(hwndLB);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
            int w = LOWORD(lParam), h = HIWORD(lParam);
            RECT rs;
            GetClientRect(hwndStatus, &rs);
            MoveWindow(hwndLB, -cxb, -cyb, w + 2 * cxb, h - rs.bottom + cyb, TRUE);
            MoveWindow(hwndStatus, -cxb, h - rs.bottom - cyb, w + 2 * cxb, rs.bottom + 2 * cyb, TRUE);
        }
        return 0;
    case WM_SYSCOLORCHANGE:
        if (hbrBack) DeleteObject(hbrBack);
        hbrBack = CreateSolidBrush(GetSysColor(COLOR_APPWORKSPACE));
        break;
    case WM_CTLCOLOR:
        if (HIWORD(lParam) == CTLCOLOR_LISTBOX) {
            /* titles in black on light backgrounds, white on dark ones */
            COLORREF c = GetSysColor(COLOR_APPWORKSPACE);
            int sum = GetRValue(c) + GetGValue(c) + GetBValue(c);
            SetBkColor((HDC)wParam, c);
            SetTextColor((HDC)wParam, sum > 0x17f ? RGB(0, 0, 0) : RGB(255, 255, 255));
            UnrealizeObject(hbrBack);
            return (LRESULT)hbrBack;
        }
        break;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lParam;
        mi->itemHeight = cyTitle / 2 + cyIcon + cyTitle;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lParam;
        CplItem *it = ((int)di->itemID >= 0 && di->itemData < (DWORD)g_nitems) ? g_items[di->itemData] : NULL;
        if (!it) return TRUE;
        HDC hdc = di->hDC;
        COLORREF oldBk = 0, oldText = 0;
        int oldMode = 0;
        RECT r;
        if (di->itemAction & ODA_DRAWENTIRE)
            DrawIcon(hdc, di->rcItem.left + dxIcon, di->rcItem.top + dyIcon, it->hIcon);
        if (di->itemState & ODS_SELECTED) {
            /* the selected title is drawn opaque in the caption colours */
            oldBk = SetBkColor(hdc, GetSysColor(COLOR_ACTIVECAPTION));
            oldText = SetTextColor(hdc, GetSysColor(COLOR_CAPTIONTEXT));
        } else {
            CopyRect(&r, &di->rcItem);
            r.top += cyIcon + dyIcon;
            HGDIOBJ ob = SelectObject(hdc, hbrBack);
            FillRect(hdc, &r, hbrBack);
            SelectObject(hdc, ob);
            oldMode = SetBkMode(hdc, TRANSPARENT);
        }
        HGDIOBJ of = SelectObject(hdc, hfontTitle);
        CopyRect(&r, &di->rcItem);
        r.top += cyIcon + dyIcon;
        DrawText(hdc, it->name, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, of);
        if (di->itemState & ODS_SELECTED) {
            SetBkColor(hdc, oldBk);
            SetTextColor(hdc, oldText);
            SetWindowText(hwndStatus, it->info);
        } else
            SetBkMode(hdc, oldMode);
        return TRUE;
    }
    case WM_DELETEITEM: {
        DELETEITEMSTRUCT *d = (DELETEITEMSTRUCT *)lParam;
        CplItem *it = RunApplet(d->itemID, CPL_STOP);
        if (it) {
            free(it->name);
            free(it->info);
            free(it->helpFile);
            it->name = it->info = it->helpFile = NULL;
        }
        if (--nItems == 0) FreeModules();
        return TRUE;
    }
    case WM_CHARTOITEM:
        return FindMnemonic(LOWORD(wParam) | 0x20, HIWORD(lParam));
    case WM_MENUSELECT:
        if (HIWORD(lParam)) { menuSelId = (UINT)wParam; menuSelFlags = LOWORD(lParam); }
        break;
    case WM_COMMAND:
        if (W16_CMD_HWND(lParam) && wParam == ID_LB) {
            int code = HIWORD(lParam);
            if (code == LBN_DBLCLK || code == LBN_SELCHANGE) {
                int cur = (int)SendMessage(hwndLB, LB_GETCURSEL, 0, 0);
                if (cur >= 0) RunApplet(cur, code == LBN_DBLCLK ? CPL_DBLCLK : CPL_SELECT);
            }
            break;
        }
        if (wParam >= IDM_APPLET && wParam < (WPARAM)(IDM_APPLET + nItems)) {
            int i = (int)wParam - IDM_APPLET;
            SendMessage(hwndLB, LB_SETCURSEL, i, 0);
            RunApplet(i, CPL_DBLCLK);
            return 1;
        }
        switch (wParam) {
        case IDM_HELPSEL: {
            int cur = (int)SendMessage(hwndLB, LB_GETCURSEL, 0, 0);
            CplItem *it = cur >= 0 ? ItemAt(cur) : NULL;
            if (it) AppletHelp(hwnd, it->helpFile, it->helpContext);
            break;
        }
        case IDM_EXIT:
            PostMessage(hwndMain, WM_CLOSE, 0, 0);
            break;
        case IDM_HELPHELP:
            if (!WinHelp(hwnd, NULL, HELP_HELPONHELP, 0)) HelpFailed(hwnd);
            break;
        case IDM_CONTENTS:
            if (!WinHelp(hwnd, szHelpFile, HELP_INDEX, 0)) HelpFailed(hwnd);
            break;
        case IDM_SEARCH:
            if (!WinHelp(hwnd, szHelpFile, HELP_PARTIALKEY, (DWORD)(uintptr_t)"")) HelpFailed(hwnd);
            break;
        case IDM_ABOUT: {
            char t[40];
            LoadString(hInst, 0x410, t, sizeof t);
            ShellAbout(hwnd, t, "", LoadIcon(hInst, MAKEINTRESOURCE(1001)));
            break;
        }
        }
        break;
    case WM_OPENAPPLET: {
        /* seg1:10A7: "control NAME" opens that applet and closes the Control Panel after it */
        int ran = 0;
        const char *name = (const char *)lParam;
        for (int i = 0; name && i < nItems; i++) {
            CplItem *it = ItemAt(i);
            if (!it || !NameMatch(name, it->name)) continue;
            if (wParam) EnableWindow((HWND)wParam, FALSE);
            SendMessage(hwndLB, LB_SETCURSEL, i, 0);
            ran = RunApplet(i, CPL_DBLCLK) != NULL;
            if (wParam) EnableWindow((HWND)wParam, TRUE);
            break;
        }
        if ((HWND)wParam == hwnd && ran) PostMessage(hwnd, WM_CLOSE, 0, 0);
        return ran;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:1790: TextWndProc (status line) */
static HFONT hfontStatus; /* [0xb2] */

static LRESULT TextWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HDC hdc = GetDC(hwnd);
        TEXTMETRIC tm;
        hfontStatus = CreateFont(GetDeviceCaps(hdc, LOGPIXELSY) * 10 / -72, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                 0, 0, 0, VARIABLE_PITCH | FF_SWISS, "MS Sans Serif");
        HGDIOBJ old = hfontStatus ? SelectObject(hdc, hfontStatus) : NULL;
        GetTextMetrics(hdc, &tm);
        SetWindowPos(hwnd, NULL, 0, 0, 0, GetSystemMetrics(SM_CYBORDER) * 6 + tm.tmHeight + 2, SWP_NOMOVE | SWP_NOZORDER);
        if (old) SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        break;
    }
    case WM_DESTROY:
        if (hfontStatus) DeleteObject(hfontStatus);
        break;
    case WM_SETTEXT: {
        char cur[128];
        GetWindowText(hwnd, cur, sizeof cur);
        if (lstrcmp(cur, (LPCSTR)lParam)) {
            DefWindowProc(hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        char text[128];
        HDC hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &r);
        int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
        r.left += cxb * 9;
        r.right -= cxb * 9;
        r.top += cyb * 3;
        r.bottom -= cyb * 3;
        /* recessed box: highlight right and bottom, shadow left and top, face inside */
        HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_BTNHIGHLIGHT));
        HGDIOBJ ob = SelectObject(hdc, br);
        PatBlt(hdc, r.left - cxb, r.bottom, r.right - r.left + 2 * cxb, cyb, PATCOPY);
        PatBlt(hdc, r.right, r.top - cyb, cxb, r.bottom - r.top + 2 * cyb, PATCOPY);
        SelectObject(hdc, ob);
        DeleteObject(br);
        br = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
        ob = SelectObject(hdc, br);
        PatBlt(hdc, r.left - cxb, r.top - cyb, r.right - r.left + cxb, cyb, PATCOPY);
        PatBlt(hdc, r.left - cxb, r.top - cyb, cxb, r.bottom - r.top + cyb, PATCOPY);
        SelectObject(hdc, ob);
        DeleteObject(br);
        br = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        ob = SelectObject(hdc, br);
        PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, PATCOPY);
        SelectObject(hdc, ob);
        DeleteObject(br);
        int n = GetWindowText(hwnd, text, sizeof text);
        int mode = SetBkMode(hdc, TRANSPARENT);
        COLORREF oc = SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        HGDIOBJ of = hfontStatus ? SelectObject(hdc, hfontStatus) : NULL;
        ExtTextOut(hdc, r.left + 2 * cxb, r.top, ETO_CLIPPED, &r, text, n, NULL);
        if (of) SelectObject(hdc, of);
        SetBkMode(hdc, mode);
        SetTextColor(hdc, oc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:203A..2956: the "lb" icon list */
typedef struct {
    HWND hwnd, parent;  /* +0, +2 */
    int id;             /* +4 */
    int count;          /* +6 */
    int perRow;         /* +8 */
    int rows;           /* +0xa */
    int cx, cy;         /* +0xc, +0xe entry size */
    int top, maxTop;    /* +0x10, +0x12 first visible row */
    int flags;          /* +0x14 bit0 focus, bit1 mouse tracking */
    int cur;            /* +0x16 */
    LPARAM *data;       /* +0x18 item data */
    int cap;
} Lb;

static Lb *lbd(HWND h) { return (Lb *)GetProp(h, "lb"); }

static LRESULT LbSendParent(Lb *l, UINT msg, WPARAM wp, LPARAM lp) { return l->parent ? SendMessage(l->parent, msg, wp, lp) : 0; }
static void LbNotify(Lb *l, int code) { LbSendParent(l, WM_COMMAND, l->id, W16_CMD_LPARAM(l->hwnd, code)); }

static void LbItemRect(Lb *l, int i, RECT *r)
{
    if (!l->perRow || i < 0 || i >= l->count) return;
    r->left = (i % l->perRow) * l->cx;
    r->right = r->left + l->cx;
    r->top = (i / l->perRow) * l->cy - l->cy * l->top;
    r->bottom = r->top + l->cy;
}

static int LbHitTest(Lb *l, int x, int y)
{
    y += l->cy * l->top;
    if (!l->cx || !l->cy || l->perRow * l->cx < x) return -1;
    int i = x / l->cx + (y / l->cy) * l->perRow;
    return (i < 0 || i >= l->count) ? -1 : i;
}

static void LbDrawItem(Lb *l, HDC hdc, int i, int action)
{
    if (i < 0 || i >= l->count) return;
    int own = !hdc;
    if (own) {
        hdc = GetDC(l->hwnd);
        LbSendParent(l, WM_CTLCOLOR, (WPARAM)hdc, MAKELPARAM(0, CTLCOLOR_LISTBOX));
    }
    RECT r = {0, 0, 0, 0};
    LbItemRect(l, i, &r);
    if (RectVisible(hdc, &r)) {
        DRAWITEMSTRUCT di;
        memset(&di, 0, sizeof di);
        di.CtlType = ODT_LISTBOX;
        di.CtlID = l->id;
        di.itemID = i;
        di.itemAction = action;
        di.itemState = (l->cur == i ? ODS_SELECTED : 0) | ((l->flags & 1) ? ODS_FOCUS : 0);
        di.hwndItem = l->hwnd;
        di.hDC = hdc;
        di.rcItem = r;
        di.itemData = (DWORD)l->data[i];
        LbSendParent(l, WM_DRAWITEM, 0, (LPARAM)&di);
    }
    if (own) ReleaseDC(l->hwnd, hdc);
}

static void LbDeleteItem(Lb *l, int i)
{
    if (i < 0 || i >= l->count) return;
    DELETEITEMSTRUCT d = {ODT_LISTBOX, l->id, i, l->hwnd, (DWORD)l->data[i]};
    LbSendParent(l, WM_DELETEITEM, 0, (LPARAM)&d);
    l->count--;
    memmove(&l->data[i], &l->data[i + 1], (l->count - i) * sizeof *l->data);
}

/* client area as if there were no vertical scroll bar */
static void LbClientRect(HWND h, RECT *r)
{
    GetClientRect(h, r);
    if (GetWindowLong(h, GWL_STYLE) & WS_VSCROLL)
        r->right += GetSystemMetrics(SM_CXVSCROLL) - GetSystemMetrics(SM_CXBORDER);
}

static void LbCalcLayout(Lb *l, const RECT *r)
{
    l->perRow = l->cx ? r->right / l->cx : 1;
    if (!l->perRow) l->perRow = 1;
    l->rows = (l->count + l->perRow - 1) / l->perRow;
    l->maxTop = l->rows - (l->cy ? r->bottom / l->cy : 0);
    if (l->maxTop < 0) l->maxTop = 0;
}

static void LbUpdateScroll(Lb *l)
{
    if (!l->cx) return;
    RECT r;
    LbClientRect(l->hwnd, &r);
    LbCalcLayout(l, &r);
    if (l->maxTop) {
        r.right -= GetSystemMetrics(SM_CXVSCROLL) - GetSystemMetrics(SM_CXBORDER);
        LbCalcLayout(l, &r);
    }
    SetScrollRange(l->hwnd, SB_VERT, 0, l->maxTop, TRUE);
    if (l->top > l->maxTop) {
        l->top = l->maxTop;
        InvalidateRect(l->hwnd, NULL, TRUE);
    }
}

static void LbPaint(Lb *l, HDC hdc)
{
    RECT r;
    GetClientRect(l->hwnd, &r);
    HBRUSH br = (HBRUSH)LbSendParent(l, WM_CTLCOLOR, (WPARAM)hdc, MAKELPARAM(0, CTLCOLOR_LISTBOX));
    FillRect(hdc, &r, br);
    for (int i = 0; i < l->count; i++) LbDrawItem(l, hdc, i, ODA_DRAWENTIRE);
}

static void LbEnsureVisible(Lb *l, int i)
{
    RECT r;
    GetClientRect(l->hwnd, &r);
    int row = i / l->perRow, vis = l->cy ? r.bottom / l->cy : 0;
    if (row < l->top) SendMessage(l->hwnd, WM_VSCROLL, SB_THUMBPOSITION, MAKELPARAM(row, 0));
    else if (l->top + vis <= row) SendMessage(l->hwnd, WM_VSCROLL, SB_THUMBPOSITION, MAKELPARAM(row - vis + 1, 0));
}

static LRESULT LbWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Lb *l = lbd(h);
    switch (msg) {
    case WM_CREATE: { /* seg1:249E */
        l = calloc(1, sizeof *l);
        SetProp(h, "lb", (HANDLE)l);
        l->hwnd = h;
        l->parent = GetParent(h);
        l->id = GetDlgCtrlID(h);
        l->cur = -1;
        MEASUREITEMSTRUCT mi = {ODT_LISTBOX, (UINT)l->id, (UINT)-1, 1, 1, 0};
        LbSendParent(l, WM_MEASUREITEM, 0, (LPARAM)&mi);
        l->cx = mi.itemWidth;
        l->cy = mi.itemHeight;
        SetScrollRange(h, SB_HORZ, 0, 0, FALSE);
        SetScrollRange(h, SB_VERT, 0, 0, FALSE);
        return 0;
    }
    case WM_DESTROY:
        if (l) {
            SendMessage(h, LB_RESETCONTENT, 0, 0);
            RemoveProp(h, "lb");
            free(l->data);
            free(l);
        }
        return 0;
    case WM_SIZE:
        if (l) LbUpdateScroll(l);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (l) {
            if (msg == WM_SETFOCUS) l->flags |= 1;
            else l->flags &= ~1;
            LbDrawItem(l, NULL, l->cur, ODA_FOCUS);
        }
        return 0;
    case WM_ERASEBKGND:
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        if (l) LbPaint(l, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN: {
        int d = 0;
        switch (wp) {
        case VK_RETURN: LbNotify(l, LBN_DBLCLK); break;
        case VK_LEFT: d = -1; break;
        case VK_RIGHT: d = 1; break;
        case VK_UP: d = -l->perRow; break;
        case VK_DOWN: d = l->perRow; break;
        case VK_HOME: d = -l->cur; break;
        case VK_END: d = l->count - l->cur - 1; break;
        }
        if (d) {
            SendMessage(h, LB_SETCURSEL, l->cur + d, 0);
            LbEnsureVisible(l, l->cur);
        }
        return 0;
    }
    case WM_CHAR:
        if (GetWindowLong(h, GWL_STYLE) & LBS_WANTKEYBOARDINPUT_) {
            int i = (int)LbSendParent(l, WM_CHARTOITEM, wp, MAKELPARAM(0, l->cur));
            if (i >= 0) {
                SendMessage(h, LB_SETCURSEL, i, 0);
                LbEnsureVisible(l, l->cur);
            }
        }
        return 0;
    case WM_VSCROLL: {
        int d = 0;
        switch (wp) {
        case SB_LINEUP: case SB_PAGEUP: d = -1; break;
        case SB_LINEDOWN: case SB_PAGEDOWN: d = 1; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: d = (SHORT)LOWORD(lp) - l->top; break;
        }
        int nt = l->top + d;
        if (nt < 0) nt = 0;
        if (nt > l->maxTop) nt = l->maxTop;
        d = nt - l->top;
        if (d) {
            l->top = nt;
            ScrollWindow(h, 0, -d * l->cy, NULL, NULL);
            SetScrollPos(h, SB_VERT, l->top, TRUE);
            UpdateWindow(h);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (l->flags & 2) return 0;
        l->flags |= 2;
        SetFocus(h);
        SetCapture(h);
        /* fall through */
    case WM_MOUSEMOVE:
        if (l->flags & 2) {
            int i = LbHitTest(l, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
            if (i >= 0) SendMessage(h, LB_SETCURSEL, i, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        if (l->flags & 2) {
            ReleaseCapture();
            l->flags &= ~2;
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (LbHitTest(l, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp)) >= 0) LbNotify(l, LBN_DBLCLK);
        return 0;
    case LB_ADDSTRING: {
        if (l->count == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 16;
            l->data = realloc(l->data, l->cap * sizeof *l->data);
        }
        int i = l->count++;
        l->data[i] = lp;
        LbUpdateScroll(l);
        LbDrawItem(l, NULL, i, ODA_DRAWENTIRE);
        return i;
    }
    case LB_RESETCONTENT:
        for (int i = l->count - 1; i >= 0; i--) LbDeleteItem(l, i);
        return 0;
    case LB_SETCURSEL: {
        int i = (int)(SHORT)wp;
        if (i < 0 || i >= l->count) return LB_ERR;
        if (i == l->cur) return i;
        int old = l->cur;
        l->cur = i;
        LbDrawItem(l, NULL, old, ODA_SELECT);
        LbDrawItem(l, NULL, l->cur, ODA_SELECT);
        LbNotify(l, LBN_SELCHANGE);
        return i;
    }
    case LB_GETCURSEL: return l->cur;
    case LB_GETCOUNT: return l->count;
    case LB_SETCOLUMNWIDTH:
        /* every item moves: repaint all (items past the area a resize exposes were left blank) */
        l->cx = (int)wp;
        LbUpdateScroll(l);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case LB_GETITEMDATA: return ((int)wp >= 0 && (int)wp < l->count) ? l->data[wp] : LB_ERR;
    case LB_SETITEMDATA:
        if ((int)wp >= 0 && (int)wp < l->count) l->data[wp] = lp;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ seg1:1BAF: WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    MSG msg;
    (void)hPrev;
    hInst = hInstance;
    if (!LoadStrings()) return 0;
    while (lpCmdLine && *lpCmdLine == ' ') lpCmdLine++;

    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = LbWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "lb";
    RegisterClass(&wc);

    memset(&wc, 0, sizeof wc);
    wc.style = CS_VREDRAW | CS_HREDRAW;
    wc.lpfnWndProc = CplWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1001));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_APPWORKSPACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCE(1001);
    wc.lpszClassName = "CtlPanelClass";
    if (!RegisterClass(&wc)) return 0;
    wc.style = CS_VREDRAW | CS_HREDRAW;
    wc.lpfnWndProc = TextWndProc;
    wc.hIcon = NULL;
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "Text";
    if (!RegisterClass(&wc)) return 0;

    HACCEL hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(1001));
    int pos[4];
    ReadWindowPos(pos);
    cyIcon = GetSystemMetrics(SM_CYICON);
    cxIcon = GetSystemMetrics(SM_CXICON);
    hwndMain = NULL;
    HWND h = CreateWindow("CtlPanelClass", szCaption, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
                          pos[0], pos[1], pos[2], pos[3], NULL, NULL, hInstance, NULL);
    if (!h) return 0;
    ShowWindow(h, nCmdShow);
    if (!LoadApplets()) {
        MessageBox(h, szNoCpl, szCaption, MB_ICONHAND | MB_SYSTEMMODAL);
        DestroyWindow(h);
        return 1;
    }
    if (lpCmdLine && *lpCmdLine) PostMessage(h, WM_OPENAPPLET, (WPARAM)h, (LPARAM)lpCmdLine);
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (TranslateAccelerator(h, hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
