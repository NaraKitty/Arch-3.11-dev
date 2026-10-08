/*
 * SysEdit - native 64-bit port of the Windows 3.11 System Configuration Editor (SYSEDIT.EXE 3.10),
 * an MDI program: SYSTEM.INI, WIN.INI, CONFIG.SYS and AUTOEXEC.BAT each in an edit window.
 *
 * Ported function by function from the disassembly of the user's own SYSEDIT.EXE
 * (tools/rip/arch311rip/disasm.py); each function notes its segment:offset. Menus, dialogs, strings,
 * accelerators and icons are loaded at run time from the user's ripped SYSEDIT.EXE; nothing
 * Microsoft-made is compiled into this file.
 *
 * Differences from the original, all deliberate:
 *  - the files are read and written through libw16's drive mapping: C:\ is the user's C: drive, the
 *    Windows directory's WIN.INI and SYSTEM.INI are arch311's settings files;
 *  - a child keeps its edit window in a pointer-sized window long (offset 0), so its three flag words
 *    sit at 8, 10 and 12 and the class has 16 extra bytes (3.1: 0, 2, 4, 6 of 8);
 *  - one instance at a time is not enforced (libw16 runs each program in its own process): the
 *    hPrevInstance branch of WinMain is not reachable;
 *  - Print Setup asks the printer driver (ExtDeviceMode), which cannot run here: the command does
 *    nothing visible (and is grayed anyway, as 3.1 grays it without the driver entry point).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h" /* ShellAbout */

const char *w16_app_module = "SYSEDIT.EXE";

/* menu commands (menu 1) */
enum {
    IDM_FILEOPEN = 1002, IDM_FILESAVE = 1003, IDM_FILESAVEAS = 1004, IDM_FILEPRINT = 1005, IDM_FILEEXIT = 1006,
    IDM_FILEABOUT = 1007, IDM_FILESETUP = 1008,
    IDM_EDITUNDO = 2001, IDM_EDITCUT = 2002, IDM_EDITCOPY = 2003, IDM_EDITPASTE = 2004, IDM_EDITCLEAR = 2005,
    IDM_EDITSELECT = 2006, IDM_EDITWRAP = 2008,
    IDM_SEARCHFIND = 3001, IDM_SEARCHNEXT = 3002, IDM_SEARCHPREV = 3003,
    IDM_WINDOWTILE = 4001, IDM_WINDOWCASCADE = 4002, IDM_WINDOWCLOSEALL = 4003, IDM_WINDOWICONS = 4004,
    IDM_WINDOWCHILD = 0x1004, /* idFirstChild */
};
#define ID_EDIT 0xCAC

/* string resources */
enum {
    IDS_CANTOPEN = 1, IDS_CANTREAD = 2, IDS_CANTCREATE = 3, IDS_CANTWRITE = 4, IDS_READONLY = 5,
    IDS_ADDEXT = 7, IDS_CLOSESAVE = 8, IDS_CANTFIND = 9, IDS_BAKEXT = 11, IDS_CANTBACKUP = 12,
    IDS_UNTITLED = 17, IDS_APPNAME = 18, IDS_ABOUTNAME = 19, IDS_PRINTJOB = 24, IDS_PRINTERROR = 25,
    IDS_PRINTDEVICE = 26, IDS_PRINTPORT = 27,
};

/* the child window's longs and words (see the note at the top) */
#define GWL_HWNDEDIT 0
#define GWW_CHANGED 8
#define GWW_WORD4 10
#define GWW_UNTITLED 12

/* ------------------------------------------------------------------ globals (DGROUP) */
static HWND hwndFrame;          /* [0x20] */
static HWND hwndMDIClient;      /* [0x22] */
static HWND hwndActive;         /* [0x24] */
static HWND hwndActiveEdit;     /* [0x26] */
static DWORD styleDefault;      /* [0x28] the children's style */
static BOOL fCase;              /* [0x94] */
static char szSearch[160];      /* [0x96] */
static BOOL fReverse;           /* [0x136] */
static int iPrinter;            /* [0x148] 0 none, 1 a DC, 2 the driver has ExtDeviceMode */
static HLOCAL hDevMode;         /* [0x14a] */
static HINSTANCE hInst;         /* [0x2e0] */
static char szPrinter[0xa0];    /* [0x2e2] "device,driver,port" split in place */
static char *szDriver, *szPort; /* [0x388], [0x384] */
static BOOL fAbort;             /* [0x382] */
static char *szPrintFile;       /* [0x386] the file name inside the print title */
static HWND hDlgPrint;          /* [0x42a] */
static HACCEL hAccel;           /* [0x4ac] */
static char szTemp[0xa0];       /* [0x38a] */

static HWND AddFile(char *pName);
static BOOL SaveFile(HWND hwnd);
static BOOL SaveAsDlg(HWND hwnd);
static void SysFindText(const char *psz);
static HDC GetPrinterDC(void);
static void PrintFile(HWND hwnd);
static void PrinterSetup(HWND hwnd);
static void FindDlg(void);
static void OpenFileCmd(void);

/* ------------------------------------------------------------------ seg1:079C AlertBox */
static int AlertBox(HWND hwnd, UINT flags, UINT id, const char *arg)
{
    char fmt[0x80];
    (void)hwnd;
    LoadString(hInst, id, fmt, sizeof fmt);
    wsprintf(szTemp, fmt, arg);
    LoadString(hInst, IDS_APPNAME, fmt, sizeof fmt);
    return MessageBox(hwndFrame, szTemp, fmt, flags);
}

/* seg1:08CC GetFileAttr (INT 21h 4300h): the attributes, or 0x80 in the high byte with the DOS error */
static int GetFileAttr(const char *name)
{
    int a = w16_dos_getattr(name);
    return a >= 0 ? a : 0x8000 | (-a & 0xFF);
}

/* ------------------------------------------------------------------ seg1:083C QueryCloseChild */
static BOOL QueryCloseChild(HWND hwnd)
{
    char buf[0x40];
    if (!GetWindowWord(hwnd, GWW_CHANGED)) return TRUE;
    GetWindowText(hwnd, buf, sizeof buf);
    switch (AlertBox(hwnd, MB_YESNOCANCEL | MB_ICONQUESTION, IDS_CLOSESAVE, buf)) {
    case IDYES:
        SaveFile(hwnd);
        return TRUE;
    case IDNO:
        return TRUE;
    }
    return FALSE;
}

/* seg1:07FB QueryCloseAll: every child (icon titles skipped) asked with WM_QUERYENDSESSION, which a
 * child answers non-zero when the user cancelled (see MPMDIChildWndProc) */
static BOOL QueryCloseAll(void)
{
    for (HWND hwnd = GetWindow(hwndMDIClient, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (GetWindow(hwnd, GW_OWNER)) continue;
        if (SendMessage(hwnd, WM_QUERYENDSESSION, 0, 0)) return FALSE;
    }
    return TRUE;
}

/* seg1:0591 CloseAllChildren */
static void CloseAllChildren(void)
{
    ShowWindow(hwndMDIClient, SW_HIDE);
    HWND hwnd;
    while ((hwnd = GetWindow(hwndMDIClient, GW_CHILD)) != NULL) {
        while (hwnd && GetWindow(hwnd, GW_OWNER)) hwnd = GetWindow(hwnd, GW_HWNDNEXT);
        if (!hwnd) break;
        SendMessage(hwndMDIClient, WM_MDIDESTROY, (WPARAM)hwnd, 0);
    }
}

/* ------------------------------------------------------------------ seg1:03BD InitMenu */
static void InitMenu(HMENU hmenu)
{
    UINT f;
    if (hwndActiveEdit) {
        f = SendMessage(hwndActiveEdit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(hmenu, IDM_EDITUNDO, f);
        LRESULT sel = SendMessage(hwndActiveEdit, EM_GETSEL, 0, 0);
        UINT fsel = LOWORD(sel) == HIWORD(sel) ? MF_GRAYED : MF_ENABLED;
        EnableMenuItem(hmenu, IDM_EDITCUT, fsel);
        EnableMenuItem(hmenu, IDM_EDITCOPY, fsel);
        EnableMenuItem(hmenu, IDM_EDITCLEAR, fsel);
        f = MF_GRAYED;
        if (OpenClipboard(hwndFrame)) {
            UINT fmt = 0;
            while ((fmt = EnumClipboardFormats(fmt)) != 0)
                if (fmt == CF_TEXT) { f = MF_ENABLED; break; }
            CloseClipboard();
        }
        EnableMenuItem(hmenu, IDM_EDITPASTE, f);
        f = SendMessage(hwndActive, WM_COMMAND, IDM_EDITWRAP, 0) ? MF_CHECKED : MF_UNCHECKED;
        CheckMenuItem(hmenu, IDM_EDITWRAP, f);
        f = szSearch[0] ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(hmenu, IDM_SEARCHNEXT, f);
        EnableMenuItem(hmenu, IDM_SEARCHPREV, f);
        EnableMenuItem(hmenu, IDM_FILEPRINT, iPrinter < 1 ? MF_GRAYED : MF_ENABLED);
        f = MF_ENABLED;
        EnableMenuItem(hmenu, IDM_EDITSELECT, f);
        EnableMenuItem(hmenu, IDM_EDITWRAP, MF_ENABLED);
        EnableMenuItem(hmenu, IDM_SEARCHFIND, MF_ENABLED);
    } else {
        f = MF_GRAYED;
        for (UINT id = IDM_EDITUNDO; id <= 2009; id++) EnableMenuItem(hmenu, id, f);
        CheckMenuItem(hmenu, IDM_EDITWRAP, MF_UNCHECKED);
        for (UINT id = IDM_SEARCHFIND; id <= IDM_SEARCHPREV; id++) EnableMenuItem(hmenu, id, f);
        EnableMenuItem(hmenu, IDM_FILEPRINT, f);
    }
    EnableMenuItem(hmenu, IDM_FILESAVE, f);
    EnableMenuItem(hmenu, IDM_FILESAVEAS, f);
    EnableMenuItem(hmenu, IDM_WINDOWTILE, f);
    EnableMenuItem(hmenu, IDM_WINDOWCASCADE, f);
    EnableMenuItem(hmenu, IDM_WINDOWICONS, f);
    EnableMenuItem(hmenu, IDM_WINDOWCLOSEALL, f);
    if (iPrinter < 2) f = MF_GRAYED;
    EnableMenuItem(hmenu, IDM_FILESETUP, f);
}

/* ------------------------------------------------------------------ seg1:05E7 CommandHandler */
static void CommandHandler(HWND hwnd, UINT id)
{
    switch (id) {
    case IDM_FILEOPEN:
        OpenFileCmd();
        break;
    case IDM_FILESAVEAS:
        if (!SaveAsDlg(hwndActive)) break;
        /* fall through */
    case IDM_FILESAVE:
        SendMessage(hwndActive, WM_COMMAND, IDM_FILESAVE, 0);
        break;
    case IDM_FILEPRINT:
        PrintFile(hwndActive);
        break;
    case IDM_FILEEXIT:
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    case IDM_FILESETUP:
        PrinterSetup(hwnd);
        break;
    case IDM_FILEABOUT:
        LoadString(hInst, IDS_ABOUTNAME, szTemp, sizeof szTemp);
        ShellAbout(hwnd, szTemp, "", LoadIcon(hInst, MAKEINTRESOURCE(1)));
        /* SYSEDIT falls through from About into Copy here (seg1:06E3): the active edit's selection
         * goes to the clipboard after the About box closes */
        /* fall through */
    case IDM_EDITCOPY:
        SendMessage(hwndActiveEdit, WM_COPY, 0, 0);
        break;
    case IDM_EDITUNDO:
        SendMessage(hwndActiveEdit, EM_UNDO, 0, 0);
        break;
    case IDM_EDITCUT:
        SendMessage(hwndActiveEdit, WM_CUT, 0, 0);
        break;
    case IDM_EDITPASTE:
        SendMessage(hwndActiveEdit, WM_PASTE, 0, 0);
        break;
    case IDM_EDITCLEAR:
        SendMessage(hwndActiveEdit, EM_REPLACESEL, 0, (LPARAM)"");
        break;
    case IDM_EDITSELECT:
        SendMessage(hwndActiveEdit, EM_SETSEL, 0, MAKELPARAM(0, 0xE000));
        break;
    case IDM_SEARCHFIND:
        FindDlg();
        break;
    case IDM_SEARCHNEXT:
        fReverse = FALSE;
        SysFindText(szSearch);
        break;
    case IDM_SEARCHPREV:
        fReverse = TRUE;
        SysFindText(szSearch);
        break;
    case IDM_WINDOWTILE:
        SendMessage(hwndMDIClient, WM_MDITILE, 0, 0);
        break;
    case IDM_WINDOWCASCADE:
        SendMessage(hwndMDIClient, WM_MDICASCADE, 0, 0);
        break;
    case IDM_WINDOWICONS:
        SendMessage(hwndMDIClient, WM_MDIICONARRANGE, 0, 0);
        break;
    case IDM_WINDOWCLOSEALL:
        if (QueryCloseAll()) {
            CloseAllChildren();
            ShowWindow(hwndMDIClient, SW_SHOW);
        }
        break;
    default:
        DefFrameProc(hwnd, hwndMDIClient, WM_COMMAND, id, 0);
        break;
    }
}

/* ------------------------------------------------------------------ seg1:0135 MPFrameWndProc */
static LRESULT MPFrameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CLIENTCREATESTRUCT ccs;
        ccs.hWindowMenu = GetSubMenu(GetMenu(hwnd), 3);
        ccs.idFirstChild = IDM_WINDOWCHILD;
        hwndMDIClient = CreateWindow("mdiclient", NULL, WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd,
                                     (HMENU)(uintptr_t)ID_EDIT, hInst, &ccs);
        ShowWindow(hwndMDIClient, SW_SHOW);
    }
        /* fall through - the printer is looked at as for WM_WININICHANGE */
    case WM_WININICHANGE:
    case 0x001B: { /* WM_DEVMODECHANGE */
        HDC hdc = GetPrinterDC();
        if (hdc) DeleteDC(hdc);
        return 0;
    }
    case WM_INITMENU:
        InitMenu((HMENU)wParam);
        return 0;
    case WM_COMMAND:
        CommandHandler(hwnd, (UINT)wParam);
        return 0;
    case WM_CLOSE:
        if (QueryCloseAll()) DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        return QueryCloseAll();
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefFrameProc(hwnd, hwndMDIClient, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:0229 MPMDIChildWndProc */
static LRESULT MPMDIChildWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HWND hwndEdit = CreateWindow("edit", NULL,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                         ES_AUTOHSCROLL,
                                     0, 0, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, (HMENU)(uintptr_t)ID_EDIT, hInst, NULL);
        w16_SetWindowPtr(hwnd, GWL_HWNDEDIT, (intptr_t)hwndEdit);
        SetWindowWord(hwnd, GWW_CHANGED, 0);
        SetWindowWord(hwnd, GWW_WORD4, 0);
        SetWindowWord(hwnd, GWW_UNTITLED, 1);
        SetFocus(hwndEdit);
        return 0;
    }
    case WM_MDIACTIVATE:
        if (wParam) {
            hwndActive = hwnd;
            hwndActiveEdit = (HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT);
        } else
            hwndActive = hwndActiveEdit = NULL;
        return 0;
    case WM_QUERYENDSESSION:
        /* (inverted, as SYSEDIT has it: QueryCloseAll counts a non-zero answer as "cancelled") */
        return !QueryCloseChild(hwnd);
    case WM_CLOSE:
        if (!QueryCloseChild(hwnd)) return 0;
        break;
    case WM_SIZE: {
        HWND hwndEdit = (HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT);
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(hwndEdit, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        break;
    }
    case WM_SETFOCUS:
        SetFocus((HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT));
        return 0;
    case WM_COMMAND:
        switch (wParam) {
        case IDM_FILESAVE:
            if (GetWindowWord(hwnd, GWW_UNTITLED) && !SaveAsDlg(hwnd)) return 0;
            SaveFile(hwnd);
            SetWindowWord(hwnd, GWW_CHANGED, 0);
            return 0;
        case ID_EDIT:
            switch (HIWORD(lParam)) {
            case EN_CHANGE:
                SetWindowWord(hwnd, GWW_CHANGED, 1);
                break;
            case EN_ERRSPACE:
                MessageBeep(0);
                break;
            }
            return 0;
        }
        break;
    }
    return DefMDIChildProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ files (seg3) */
/* seg3:009F LoadFile: the file read into the edit's own buffer (EM_GETHANDLE grown, EM_SETHANDLE) */
static BOOL LoadFile(HWND hwnd, const char *pName)
{
    OFSTRUCT of;
    HWND hwndEdit = (HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT);
    SetWindowWord(hwnd, GWW_UNTITLED, 0);
    HFILE fh = OpenFile(pName, &of, OF_READ);
    if (fh < 0) {
        AlertBox(hwnd, MB_ICONHAND, IDS_CANTOPEN, pName);
        return FALSE;
    }
    LONG len = _llseek(fh, 0, 2);
    _llseek(fh, 0, 0);
    HLOCAL hText = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    if (!LocalReAlloc(hText, (UINT)len + 1, LHND)) {
        _lclose(fh);
        AlertBox(hwnd, MB_ICONHAND, IDS_CANTOPEN, pName);
        return FALSE;
    }
    char *p = LocalLock(hText);
    if (_lread(fh, p, (UINT)len) != (UINT)len) AlertBox(hwnd, MB_ICONHAND, IDS_CANTREAD, pName);
    p[len] = 0;
    LocalUnlock(hText);
    SendMessage(hwndEdit, EM_SETHANDLE, (WPARAM)hText, 0);
    _lclose(fh);
    return TRUE;
}

/* seg3:0004 AddFile: a child for the file (upper case title) or "(Untitled)", then the file loaded */
static HWND AddFile(char *pName)
{
    char sz[0xa0];
    MDICREATESTRUCT mcs;
    if (!pName) {
        LoadString(hInst, IDS_UNTITLED, sz, sizeof sz);
        mcs.szTitle = sz;
    } else {
        AnsiUpper(pName);
        mcs.szTitle = pName;
    }
    mcs.szClass = "mpchild";
    mcs.hOwner = hInst;
    mcs.x = mcs.cx = mcs.y = mcs.cy = CW_USEDEFAULT;
    mcs.style = styleDefault;
    mcs.lParam = 0;
    HWND hwnd = (HWND)SendMessage(hwndMDIClient, WM_MDICREATE, 0, (LPARAM)&mcs);
    LoadFile(hwnd, pName);
    return hwnd;
}

/* seg3:01E6 SaveFile: written to a temporary file on the file's drive, the old file renamed to
 * NAME.SYD (an old backup deleted first), the temporary file renamed to the file; on a failed rename
 * the backup goes back. A name without an extension gets .TXT */
static BOOL SaveFile(HWND hwnd)
{
    char szFile[0x44], szBak[0x44], szTmp[0x44];
    OFSTRUCT of;
    int err = 0;
    HWND hwndEdit = (HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT);
    GetWindowText(hwnd, szFile, 0x40);
    if (GetFileAttr(szFile) & 1) {
        AlertBox(hwnd, MB_ICONHAND, IDS_READONLY, szFile);
        return FALSE;
    }
    /* the backup: the extension (or nothing) replaced with ".SYD" */
    lstrcpy(szBak, szFile);
    char *p = szBak + lstrlen(szBak);
    while (p > szBak && *p != '.' && *p != '\\') p--;
    if (p == szBak || *p != '.') p = szBak + lstrlen(szBak);
    *p++ = '.';
    LoadString(hInst, IDS_BAKEXT, p, (int)(szBak + sizeof szBak - p));
    /* a name without an extension gets ".TXT" */
    int ext = 0;
    for (p = szFile; *p; p++) {
        if (*p == '.') ext = 1;
        else if (*p == '\\' || *p == ':') ext = 0;
    }
    if (!ext) LoadString(hInst, IDS_ADDEXT, p, (int)(szFile + sizeof szFile - p));
    GetTempFileName((BYTE)(szFile[0] | TF_FORCEDRIVE), "syd", 0, szTmp);
    HFILE fh = OpenFile(szTmp, &of, OF_CREATE | OF_WRITE);
    if (fh < 0) {
        AlertBox(hwnd, MB_ICONHAND, IDS_CANTCREATE, szFile);
        return FALSE;
    }
    UINT len = GetWindowTextLength(hwndEdit);
    HLOCAL hText = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    char *t = LocalLock(hText);
    if (_lwrite(fh, t, len) != len) {
        AlertBox(hwnd, MB_ICONHAND, IDS_CANTWRITE, szFile);
        err = 0x8000;
    }
    LocalUnlock(hText);
    SendMessage(hwndEdit, EM_SETHANDLE, (WPARAM)hText, 0);
    _lclose(fh);
    if (!err && OpenFile(szFile, &of, OF_EXIST) != HFILE_ERROR) {
        OpenFile(szBak, &of, OF_DELETE);
        if (OpenFile(szBak, &of, OF_EXIST) != HFILE_ERROR) {
            AlertBox(hwnd, MB_ICONHAND, IDS_CANTBACKUP, szBak);
            err = 0x8000;
        } else
            err = w16_dos_rename(szFile, szBak);
    }
    if (!err) {
        err = w16_dos_rename(szTmp, szFile);
        if (err && !w16_dos_rename(szBak, szFile)) {
            AlertBox(hwnd, MB_ICONHAND, IDS_CANTWRITE, szFile);
            err = 0x8000;
        }
    }
    if (err && err != 0x8000) AlertBox(hwnd, MB_ICONHAND, IDS_CANTWRITE, szFile);
    OpenFile(szTmp, &of, OF_DELETE);
    return !err;
}

/* seg3:04EC: "Save %s &As" filled in with the file name */
static void SetSaveTitle(HWND hDlg, const char *name)
{
    char fmt[0x20], buf[0xa0];
    GetDlgItemText(hDlg, 501, fmt, sizeof fmt);
    wsprintf(buf, fmt, name);
    SetDlgItemText(hDlg, 501, buf);
}

/* seg3:0539 SaveAsDlgProc (dialog 500; SysEdit's menu has no Save As: its files always have names) */
static BOOL SaveAsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[0x48], dir[0x80];
    switch (msg) {
    case WM_INITDIALOG: {
        HWND child = (HWND)lParam;
        SetProp(hDlg, "FILENAME", (HANDLE)child);
        GetWindowText(child, buf, 0x40);
        SetSaveTitle(hDlg, buf);
        AnsiUpper(buf);
        int ext = 0;
        char *p;
        for (p = buf; *p; p++) {
            if (*p == '.') ext = 1;
            else if (*p == '\\') ext = 0;
        }
        if (!ext) LoadString(hInst, IDS_ADDEXT, p, (int)(buf + sizeof buf - p));
        SetDlgItemText(hDlg, 502, buf);
        SendDlgItemMessage(hDlg, 502, EM_SETSEL, 0, MAKELPARAM(0, 100));
        lstrcpy(dir, "*.*");
        DlgDirList(hDlg, dir, 204, 203, 0xC010);
        if (!buf[0]) EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);
        return TRUE;
    }
    case WM_COMMAND:
        switch (wParam) {
        case IDOK: {
            HWND child = (HWND)GetProp(hDlg, "FILENAME");
            GetDlgItemText(hDlg, 502, buf, 0x40);
            AnsiUpper(buf);
            SetWindowText(child, buf);
            EndDialog(hDlg, 0);
            break;
        }
        case IDCANCEL:
            EndDialog(hDlg, 1);
            break;
        case 204:
            if (HIWORD(lParam) == LBN_DBLCLK) {
                DlgDirSelect(hDlg, dir, 204);
                lstrcat(dir, "*.*");
                DlgDirList(hDlg, dir, 204, 203, 0xC010);
            }
            break;
        case 502:
            if (HIWORD(lParam) == EN_CHANGE)
                EnableWindow(GetDlgItem(hDlg, IDOK), (BOOL)SendDlgItemMessage(hDlg, 502, WM_GETTEXTLENGTH, 0, 0));
            break;
        }
        return FALSE;
    }
    return FALSE;
}

/* seg3:070D SaveAsDlg: TRUE when a name was given (the child is no longer untitled) */
static BOOL SaveAsDlg(HWND hwnd)
{
    int r = DialogBoxParam(hInst, MAKEINTRESOURCE(500), hwnd, SaveAsDlgProc, (LPARAM)hwnd);
    if (r == 0) SetWindowWord(hwnd, GWW_UNTITLED, 0);
    return r == 0;
}

/* seg3:0776: a name with wildcards */
static BOOL HasWild(const char *p)
{
    for (; *p; p++)
        if (*p == '?' || *p == '*') return TRUE;
    return FALSE;
}

/* seg3:079F: OK in the Open dialog: a wildcard lists, else the dialog ends */
static void OpenOk(HWND hDlg)
{
    char *name = (char *)GetProp(hDlg, "FILENAME");
    GetDlgItemText(hDlg, 201, name, 0x40);
    if (HasWild(name)) {
        DlgDirList(hDlg, name, 204, 203, 0xC010);
        char *p = name, *last = name;
        for (; *p; p++)
            if (*p == '\\' || *p == ':') last = p + 1;
        DlgDirList(hDlg, last, 202, 203, 0);
        SetDlgItemText(hDlg, 201, last);
    } else {
        RemoveProp(hDlg, "FILENAME");
        EndDialog(hDlg, 0);
    }
}

/* seg3:0836 FileOpenDlgProc (dialog 200; not reachable from SysEdit's menu) */
static BOOL FileOpenDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hDlg, 201, "*.TXT");
        SetProp(hDlg, "FILENAME", (HANDLE)lParam);
        SendDlgItemMessage(hDlg, 201, EM_LIMITTEXT, 0x40, 0);
        OpenOk(hDlg);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            OpenOk(hDlg);
            break;
        case IDCANCEL: {
            char *name = (char *)GetProp(hDlg, "FILENAME");
            name[0] = 0;
            EndDialog(hDlg, 0);
            break;
        }
        case 201:
            EnableWindow(GetDlgItem(hDlg, IDOK), GetWindowTextLength(W16_CMD_HWND(lParam)));
            break;
        case 202:
            if (HIWORD(lParam) == LBN_DBLCLK) OpenOk(hDlg);
            else if (HIWORD(lParam) == LBN_SELCHANGE) {
                char *name = (char *)GetProp(hDlg, "FILENAME");
                DlgDirSelect(hDlg, name, 202);
                SetDlgItemText(hDlg, 201, name);
            }
            break;
        case 204:
            if (HIWORD(lParam) == LBN_SELCHANGE) {
                /* the directory chosen, followed by the file part of what the name field holds */
                char *name = (char *)GetProp(hDlg, "FILENAME"), *add, *w, *r;
                DlgDirSelect(hDlg, name, 204);
                add = name + lstrlen(name);
                GetDlgItemText(hDlg, 201, add, 0x40);
                for (r = w = add; *r; r++) {
                    if (*r == '\\' || *r == ':') { w = add; continue; }
                    *w++ = *r;
                }
                *w = 0;
                SetDlgItemText(hDlg, 201, name);
            } else if (HIWORD(lParam) == LBN_DBLCLK)
                OpenOk(hDlg);
            break;
        }
        return TRUE;
    }
    return FALSE;
}

/* seg3:01B6 OpenFileCmd: dialog 200 for a name, then a child for it */
static void OpenFileCmd(void)
{
    char sz[0x82];
    DialogBoxParam(hInst, MAKEINTRESOURCE(200), hwndFrame, FileOpenDlgProc, (LPARAM)sz);
    if (sz[0]) AddFile(sz);
}

/* ------------------------------------------------------------------ searching (seg4) */
/* seg4:0582 / 05FC: the first n characters compared (lstrcmp / lstrcmpi) */
static int CompareN(int n, const char *text, const char *pat, BOOL ci)
{
    char a[0xa0], b[0xa0];
    int ln = 0;
    while (ln < n && text[ln]) ln++;
    if (n >= (int)sizeof a) n = sizeof a - 1;
    if (ln >= (int)sizeof a) ln = sizeof a - 1;
    memcpy(a, text, ln);
    a[ln] = 0;
    snprintf(b, sizeof b, "%.*s", n, pat);
    return ci ? lstrcmpi(a, b) : lstrcmp(a, b);
}

/* seg4:0316 / 0343: one character against another, case-insensitively with lstrcmpi */
static BOOL SameChar(char a, char b, BOOL ci)
{
    if (!ci) return a == b;
    char x[2] = {a, 0}, y[2] = {b, 0};
    return lstrcmpi(x, y) == 0;
}

/* seg4:0676 / 0786: the first match at or after text */
static const char *FwdScan(const char *text, const char *pat, BOOL ci)
{
    int len = lstrlen(pat);
    for (; *text; text++)
        if (SameChar(*text, *pat, ci) && !CompareN(len, text, pat, ci)) return text;
    return NULL;
}

/* seg4:06DC / 07EC: the last match starting in [start, end) */
static const char *RevScan(const char *start, const char *end, const char *pat, BOOL ci)
{
    int len = lstrlen(pat);
    if (!end) end = start + lstrlen(start);
    if (!*pat) return NULL;
    while (end > start) {
        end--;
        if (SameChar(*end, *pat, ci) && !CompareN(len, end, pat, ci)) return end;
    }
    return NULL;
}

/* seg4:0004 FindText (named SysFindText here: commdlg.h has FindText): from the selection's end forwards, or backwards from its start line by line;
 * the match selected, or "Cannot find" */
static void SysFindText(const char *psz)
{
    if (!*psz || !hwndActiveEdit) return;
    LRESULT sel = SendMessage(hwndActiveEdit, EM_GETSEL, 0, 0);
    int start = LOWORD(sel), end = HIWORD(sel);
    HLOCAL hText = (HLOCAL)SendMessage(hwndActiveEdit, EM_GETHANDLE, 0, 0);
    const char *text = LocalLock(hText), *found = NULL;
    if (fReverse) {
        int line = (int)SendMessage(hwndActiveEdit, EM_LINEFROMCHAR, start, 0);
        int lineStart = (int)SendMessage(hwndActiveEdit, EM_LINEINDEX, line, 0), limit = start;
        for (; line >= 0; line--) {
            found = RevScan(text + lineStart, text + limit, psz, !fCase);
            if (found) break;
            limit = lineStart;
            if (line - 1 >= 0) lineStart = (int)SendMessage(hwndActiveEdit, EM_LINEINDEX, line - 1, 0);
        }
    } else
        found = FwdScan(text + end, psz, !fCase);
    LocalUnlock(hText);
    if (!found) {
        AlertBox(hwndFrame, MB_ICONEXCLAMATION, IDS_CANTFIND, szSearch);
        PostMessage(hwndFrame, WM_SETFOCUS, 0, 0);
        return;
    }
    int at = (int)(found - text);
    SendMessage(hwndActiveEdit, EM_SETSEL, 0, MAKELPARAM(at, at + lstrlen(psz)));
}

/* seg4:0183 FindDlgProc (dialog 400) */
static BOOL FindDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, 403, fCase);
        SetDlgItemText(hDlg, 401, szSearch);
        if (!lstrlen(szSearch)) {
            EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);
            EnableWindow(GetDlgItem(hDlg, 402), FALSE);
        }
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
        case 402:
            fReverse = wParam == 402;
            fCase = IsDlgButtonChecked(hDlg, 403);
            GetDlgItemText(hDlg, 401, szSearch, sizeof szSearch);
            SysFindText(szSearch);
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        case 401:
            if (HIWORD(lParam) == EN_CHANGE) {
                BOOL f = SendDlgItemMessage(hDlg, 401, WM_GETTEXTLENGTH, 0, 0) != 0;
                EnableWindow(GetDlgItem(hDlg, IDOK), f);
                EnableWindow(GetDlgItem(hDlg, 402), f);
            }
            break;
        case 403:
            CheckDlgButton(hDlg, 403, !IsDlgButtonChecked(hDlg, 403));
            break;
        }
        return TRUE;
    }
    return FALSE;
}

/* seg4:02D0 */
static void FindDlg(void) { DialogBox(hInst, MAKEINTRESOURCE(400), hwndFrame, FindDlgProc); }

/* ------------------------------------------------------------------ printing (seg5) */
/* seg5:0004 GetPrinterDC: WIN.INI [windows] device= "name,driver,port" */
static HDC GetPrinterDC(void)
{
    iPrinter = 0;
    GetProfileString("windows", "device", "", szPrinter, sizeof szPrinter);
    szDriver = szPrinter;
    while (*szDriver && *szDriver != ',') szDriver++;
    if (*szDriver) *szDriver++ = 0;
    szPort = szDriver;
    while (*szPort && *szPort != ',') szPort++;
    if (*szPort) *szPort++ = 0;
    if (!szPrinter[0] || !*szDriver || !*szPort) {
        szPrinter[0] = 0;
        return NULL;
    }
    const void *dm = NULL;
    if (hDevMode) {
        const char *d = LocalLock(hDevMode);
        if (lstrcmp(szPrinter, d)) {
            LocalUnlock(hDevMode);
            LocalFree(hDevMode);
            hDevMode = NULL;
        } else
            dm = d;
    }
    HDC hdc = CreateDC(szDriver, szPrinter, szPort, dm);
    if (hDevMode) LocalUnlock(hDevMode);
    if (!hdc) return NULL;
    iPrinter = 1;
    /* iPrinter 2 needs the driver module's EXTDEVICEMODE entry point: printer drivers are 16-bit
     * code that arch311 does not run */
    return hdc;
}

/* seg5:0149 AbortProc */
static BOOL AbortProc(HDC hdc, int code)
{
    MSG msg;
    (void)hdc;
    (void)code;
    while (!fAbort && PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (!hDlgPrint || !IsDialogMessage(hDlgPrint, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return !fAbort;
}

/* seg5:01B1 */
static void InitPrintDlg(HWND hDlg)
{
    char fmt[0x32], buf[0xc8];
    SetDlgItemText(hDlg, 603, szPrintFile);
    LoadString(hInst, IDS_PRINTDEVICE, fmt, sizeof fmt);
    wsprintf(buf, fmt, szPrinter);
    SetDlgItemText(hDlg, 601, buf);
    LoadString(hInst, IDS_PRINTPORT, fmt, sizeof fmt);
    wsprintf(buf, fmt, szPort);
    SetDlgItemText(hDlg, 602, buf);
}

/* seg5:0245 PrintDlgProc (dialog 600) */
static BOOL PrintDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        InitPrintDlg(hDlg);
        return TRUE;
    case WM_COMMAND:
        fAbort = TRUE;
        return TRUE;
    }
    return FALSE;
}

#define SETABORTPROC 9
#define STARTDOC 10
#define NEWFRAME 1
#define ENDDOC 11
#define ABORTDOC 2

/* seg5:027A PrintFile: the lines of the edit, TextOut one under the other, a new page when the next
 * would pass VERTRES. UNTESTED: libw16's printer DCs (no printer on the reference machine) */
static void PrintFile(HWND hwnd)
{
    BOOL fError = TRUE;
    char szTitle[0x40];
    HWND hwndEdit = (HWND)w16_GetWindowPtr(hwnd, GWL_HWNDEDIT);
    int n = LoadString(hInst, IDS_PRINTJOB, szTitle, sizeof szTitle);
    szPrintFile = szTitle + n;
    n += GetWindowText(hwnd, szPrintFile, (int)sizeof szTitle - n);
    szTitle[0x3f] = 0;
    HDC hdc = GetPrinterDC();
    if (hdc) {
        EnableWindow(hwndFrame, FALSE);
        fAbort = FALSE;
        hDlgPrint = CreateDialog(hInst, MAKEINTRESOURCE(600), hwnd, PrintDlgProc);
        if (hDlgPrint) {
            ShowWindow(hDlgPrint, SW_SHOW);
            UpdateWindow(hDlgPrint);
            if (Escape(hdc, SETABORTPROC, 0, (LPCSTR)(void *)AbortProc, NULL) >= 0) {
                int r = Escape(hdc, STARTDOC, n, szTitle, NULL);
                if (r < 0) {
                    if (r == -3) hdc = NULL; /* SP_USERABORT: the DC is not deleted (as SysEdit) */
                } else {
                    int dy = HIWORD(GetTextExtent(hdc, "CC", 2)), page = GetDeviceCaps(hdc, VERTRES), y = 0;
                    int lines = (int)SendMessage(hwndEdit, EM_GETLINECOUNT, 0, 0);
                    HLOCAL hText = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
                    BOOL bad = FALSE;
                    for (int line = 0; line < lines; line++) {
                        if (y + dy > page) {
                            if (Escape(hdc, NEWFRAME, 0, NULL, NULL) < 0 || fAbort) { bad = TRUE; break; }
                            y = 0;
                        }
                        int idx = (int)SendMessage(hwndEdit, EM_LINEINDEX, line, 0);
                        int len = (int)SendMessage(hwndEdit, EM_LINELENGTH, idx, 0);
                        TextOut(hdc, 0, y, (char *)LocalLock(hText) + idx, len);
                        LocalUnlock(hText);
                        if (fAbort) { bad = TRUE; break; }
                        y += dy;
                    }
                    if (!bad && Escape(hdc, NEWFRAME, 0, NULL, NULL) >= 0 && Escape(hdc, ENDDOC, 0, NULL, NULL) >= 0)
                        fError = FALSE;
                    else
                        Escape(hdc, ABORTDOC, 0, NULL, NULL);
                }
            }
        }
        EnableWindow(hwndFrame, TRUE);
        DestroyWindow(hDlgPrint);
        hDlgPrint = NULL;
        if (hdc) DeleteDC(hdc);
    }
    if (fError && !fAbort) AlertBox(hwnd, MB_ICONEXCLAMATION, IDS_PRINTERROR, szPrintFile);
}

/* seg5:0544 PrinterSetup: the driver's ExtDeviceMode dialog - 16-bit driver code, not run here
 * (TODO: a native printer setup for CUPS printers); the menu item is grayed (iPrinter < 2) */
static void PrinterSetup(HWND hwnd) { (void)hwnd; }

/* ------------------------------------------------------------------ start (seg2) */
/* seg2:0004 InitApplication */
static BOOL InitApplication(void)
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = MPFrameWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_APPWORKSPACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCE(1);
    wc.lpszClassName = "mpframe";
    if (!RegisterClass(&wc)) return FALSE;
    wc.lpfnWndProc = MPMDIChildWndProc;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(2));
    wc.lpszMenuName = NULL;
    wc.cbWndExtra = 16; /* 8 in 3.1: the edit's handle is pointer-sized here */
    wc.lpszClassName = "mpchild";
    return RegisterClass(&wc) != 0;
}

/* seg2:00BB InitInstance: the frame, then the four files */
static BOOL InitInstance(LPSTR lpCmdLine, int nCmdShow)
{
    char sz[0x80];
    LoadString(hInst, IDS_APPNAME, sz, 0x50);
    hwndFrame = CreateWindow("mpframe", sz, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
                             NULL, NULL, hInst, NULL);
    if (!hwndFrame || !hwndMDIClient) return FALSE;
    hAccel = LoadAccelerators(hInst, MAKEINTRESOURCE(1));
    if (!hAccel) return FALSE;
    HDC hdc = GetPrinterDC();
    if (hdc) DeleteDC(hdc);
    ShowWindow(hwndFrame, nCmdShow);
    UpdateWindow(hwndFrame);
    if (lpCmdLine && !*lpCmdLine) lpCmdLine = NULL; /* (SYSEDIT ignores its command line) */
    styleDefault = 0;
    GetWindowsDirectory(sz, sizeof sz);
    if (sz[lstrlen(sz) - 1] != '\\') lstrcat(sz, "\\");
    lstrcat(sz, "system.ini");
    AddFile(sz);
    GetWindowsDirectory(sz, sizeof sz);
    if (sz[lstrlen(sz) - 1] != '\\') lstrcat(sz, "\\");
    lstrcat(sz, "win.ini");
    AddFile(sz);
    lstrcpy(sz, "c:\\config.sys");
    AddFile(sz);
    lstrcpy(sz, "c:\\autoexec.bat");
    AddFile(sz);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0010 WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    hInst = hInstance;
    (void)hPrevInstance; /* a second SysEdit would only bring the first one up (seg1:0072) */
    if (!InitApplication()) return 0;
    /* (RegisterPenApp: no pen extensions - GetSystemMetrics(SM_PENWINDOWS) is 0) */
    if (!InitInstance(lpCmdLine, nCmdShow)) return 0;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateMDISysAccel(hwndMDIClient, &msg) && !TranslateAccelerator(hwndFrame, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return 0;
}
