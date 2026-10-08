/*
 * Notepad - native 64-bit port of the Windows 3.11 Notepad (NOTEPAD.EXE 3.10, 32,736 bytes).
 *
 * Ported function by function from the disassembly of the user's own NOTEPAD.EXE
 * (tools/rip/arch311rip/disasm.py). Each function notes the original segment:offset.
 * Menus, dialogs, strings, accelerators and the icon are loaded at run time from the
 * user's ripped NOTEPAD.EXE; nothing Microsoft-made is compiled into this file.
 *
 * Differences from the original, all deliberate:
 *  - files are read/written on the Linux file system through libw16's drive mapping;
 *  - the 64 KB edit limit is kept for files (3.1 behaviour), see LoadFile;
 *  - printing goes through libw16's printer DC (CUPS) instead of a Win16 printer driver.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "w16.h"
#include "commdlg.h"

const char *w16_app_module = "NOTEPAD.EXE";

/* menu / control ids (from the menu and dialog templates) */
#define M_SAVE 1
#define M_SAVEAS 2
#define M_FIND 3
#define M_HELP 5
#define M_SELECTALL 7
#define M_FINDNEXT 8
#define M_NEW 9
#define M_OPEN 10
#define M_ABOUT 11
#define M_DATETIME 12
#define M_PRINT 14
#define ID_EDIT 15
#define M_UNDO 25
#define M_WW 27
#define M_EXIT 28
#define M_SETUP 31
#define M_PAGESETUP 32
#define M_USEHELP 40
#define M_SEARCHHELP 41
#define M_CUT WM_CUT     /* 768: the edit commands are the edit messages */
#define M_COPY WM_COPY
#define M_PASTE WM_PASTE
#define M_CLEAR WM_CLEAR

/* string resource ids (seg2:002C loads 33 of them into a local block) */
enum {
    IDS_CANTOPEN = 1, IDS_CANTFIND = 3, IDS_ALREADYEXISTS = 5, IDS_MODIFIED = 10, IDS_UNTITLED = 11,
    IDS_NOTEPAD_ = 12, IDS_CANTFINDSTR = 16, IDS_NOMEM = 17, IDS_FTOOBIG = 18, IDS_NOTEPAD = 19,
    IDS_CLIPTOOLONG = 20, IDS_DISKFULL = 21, IDS_INVALIDFILE = 22, IDS_EMPTYFILE = 23, IDS_NOTEXT = 24,
    IDS_CANTPRINT = 25, IDS_BADNAME = 26, IDS_BADNAME2 = 27, IDS_PRINTDISKFULL = 28, IDS_PRINTNOMEM = 29,
    IDS_CREATEERR = 30, IDS_NOWW = 31, IDS_MERGE = 32, IDS_FILTERSPEC = 33, IDS_HELPFILE = 34,
    IDS_BADMARGINS = 35, IDS_HEADER = 36, IDS_FOOTER = 37, IDS_LEFT = 38, IDS_RIGHT = 39, IDS_TOP = 40,
    IDS_BOTTOM = 41, IDS_LETTERS = 42, IDS_CANTOPENPRINT = 43, IDS_TEXTFILES = 44, IDS_ALLFILES = 45,
    IDS_OPENCAPTION = 46, IDS_SAVECAPTION = 47, IDS_CANTQUIT = 48, IDS_LOADDRVFAIL = 50,
};

/* ------------------------------------------------------------------ globals (DGROUP) */
static HINSTANCE hInstanceNP;
static HWND hwndNP;           /* [0x10] */
static HWND hwndEdit;         /* [0x12] */
static HWND hDlgFind;         /* [0x14] */
static BOOL fUntitled = TRUE; /* [0x16] */
static int fInSaveAsDlg;      /* [0x34] */
static int fEditErr;          /* [0x1a]  1 = loading, 2 = EN_ERRSPACE seen while loading */
static BOOL fWrap;            /* [0x1e] */
static BOOL fSetupMode;       /* [0x1c]  NOTEPAD /.SETUP */
static DWORD dwSavedSel;      /* [0x36] selection kept across WM_ACTIVATEAPP */
static HCURSOR hWaitCursor;   /* [0xb14] */
static HACCEL hAccel;         /* [0x8bc] */
static HFONT hFont;           /* [0x8b8] SYSTEM_FIXED_FONT */
static HLOCAL hEdit;          /* [0xaae] edit control buffer */
static HMENU hSysMenuSetup;   /* [0xaa4] */
static UINT wFRMsg, wHlpMsg;  /* [0x7e2], [0x8ba] */
static BOOL fReverse;         /* [0x434] */
static BOOL fCase;            /* [0x432] */
static char szFileName[260];  /* [0x7ea] */
static char szSearch[160];    /* [0xab0] */
static char szFilter[200];    /* [0xad4] */
static char szCustFilter[80]; /* [0x872] */
static char szOpenFile[260];  /* [0xc0] */
static char szSaveTmp[260];   /* [0x140] */
static char szFileTitle[64];  /* [0x9f4] */
static OFSTRUCT of;           /* [0xb5e] */
static HFILE fp = HFILE_ERROR;/* [0x9f0] */
static OPENFILENAME OFN;      /* [0xb16] */
static FINDREPLACE FR;        /* [0xa74] */
static PRINTDLG PD;           /* [0x8c4] */
static char szPageSetup[6][40]; /* [0x8f8]: header, footer, left, right, top, bottom */
static char chDecimal = '.';  /* [0x86a] */
static int iMeasure;          /* [0x390] */

/* the 33 strings loaded at start-up (pointers at [0x3a..0x7a]) */
static char szCantOpen[300], szCantFind[300], szAlreadyExists[300], szModified[300], szUntitled[64],
    szNoMem[300], szCantFindStr[300], szNotepad_[64], szFTooBig[300], szNotepad[64], szClipTooLong[300],
    szDiskFull[300], szInvalidFile[300], szEmptyFile[300], szNoText[300], szCantPrint[300], szBadName[300],
    szBadName2[300], szPrintDiskFull[300], szPrintNoMem[300], szCreateErr[300], szNoWW[300], szMerge[8],
    szFilterSpec[32], szHelpFile[64], szBadMargins[300], szCantOpenPrint[300], szTextFiles[64],
    szAllFiles[64], szOpenCaption[64], szSaveCaption[64], szCantQuit[300], szLoadDrvFail[300];

/* intl date/time (seg2:06B9) */
static int iDate, iTime, iTLZero, fDateFlags;
static char chDateSep = '/', chTimeSep = ':';
static char sz1159[8] = "AM", sz2359[8] = "PM";

static BOOL CheckSave(BOOL fSysModal);
static BOOL LoadFile(char *name);
static BOOL SaveFile(HWND hwnd, char *name, BOOL fSaveAs);
static void New(BOOL fCheck);
static void Search(char *sz);
static int NpPrint(void);

/* ------------------------------------------------------------------ seg1:0EEB / 0F8B: AlertBox */
/* replace the "%%" marker in the template with the file name */
static void MergeStrings(const char *tmpl, const char *sub, char *out)
{
    while (*tmpl) {
        if (tmpl[0] == szMerge[0] && tmpl[1] == szMerge[1]) {
            if (sub) while (*sub) *out++ = *sub++;
            tmpl += 2;
            while ((*out++ = *tmpl++)) ;
            return;
        }
        *out++ = *tmpl++;
    }
    *out = 0;
}

static int AlertBox(HWND hwnd, const char *caption, const char *text, const char *name, UINT style)
{
    char buf[600];
    MergeStrings(text, name, buf);
    return MessageBox(hwnd, buf, caption, style);
}

/* ------------------------------------------------------------------ seg1:0D1D, 0CB7: title */
static char *PFileInPath(char *path)
{
    char *p = path;
    for (char *s = path; *s; s = AnsiNext(s))
        if (*s == ':' || *s == '\\') p = s;
    if (p != path) p++;
    return p;
}

static void SetTitle(char *name)
{
    char buf[300];
    if (!fUntitled) AnsiUpper(name);
    lstrcpy(buf, szNotepad_);
    lstrcat(buf, PFileInPath(name));
    SetWindowText(hwndNP, buf);
}

/* ------------------------------------------------------------------ seg3:05A1: AddExt */
static void AddExt(char *name)
{
    char *p = name + lstrlen(name);
    while (p > name) {
        p = AnsiPrev(name, p);
        if (*p == '.' || *p == '\\' || *p == ':') break;
    }
    if (*p != '.') lstrcat(name, ".TXT");
}

/* ------------------------------------------------------------------ seg2:06B9: intl settings */
static const char *dateFormats[8][3] = {
    {"M/d/yy", "d/M/yy", "yy/M/d"}, {"M/dd/yy", "dd/M/yy", "yy/M/dd"},
    {"MM/d/yy", "d/MM/yy", "yy/MM/d"}, {"MM/dd/yy", "dd/MM/yy", "yy/MM/dd"},
    {"M/d/yyyy", "d/M/yyyy", "yyyy/M/d"}, {"M/dd/yyyy", "dd/M/yyyy", "yyyy/M/dd"},
    {"MM/d/yyyy", "d/MM/yyyy", "yyyy/MM/d"}, {"MM/dd/yyyy", "dd/MM/yyyy", "yyyy/MM/dd"}};
static const int dateFlags[8] = {0, 2, 1, 3, 4, 6, 5, 7};

static void IniInit(void)
{
    char buf[16], fmt[16];
    iDate = GetProfileInt("intl", "iDate", 0);
    GetProfileString("intl", "sDate", "/", buf, 2);
    chDateSep = buf[0];
    GetProfileString("intl", "sShortDate", "M/d/yy", fmt, 11);
    if (chDateSep != '/')
        for (char *p = fmt; *p; p++)
            if (*p != 'M' && *p != 'd' && *p != 'y') *p = '/';
    fDateFlags = 0;
    for (int i = 0; i < 8; i++)
        if (iDate >= 0 && iDate < 3 && !lstrcmpi(dateFormats[i][iDate], fmt)) { fDateFlags = dateFlags[i]; break; }
    GetProfileString("intl", "s1159", "AM", sz1159, 6);
    GetProfileString("intl", "s2359", "PM", sz2359, 6);
    iTime = GetProfileInt("intl", "iTime", 0);
    GetProfileString("intl", "sTime", ":", buf, 2);
    chTimeSep = buf[0];
    iTLZero = GetProfileInt("intl", "iTLzero", 0);
    char old = chDecimal;
    GetProfileString("intl", "sDecimal", ".", buf, 2);
    chDecimal = buf[0];
    /* the margin strings follow the decimal separator */
    for (int i = 2; i < 6; i++)
        for (char *p = szPageSetup[i]; *p; p++)
            if (*p == old) *p = chDecimal;
}

/* ------------------------------------------------------------------ seg4: time/date */
void GetTimeDate(char *szTime, char *szDate) /* also used by print.c */
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (szTime) {
        int hour = tm->tm_hour;
        BOOL pm = hour >= 12;
        if (iTime == 0) {
            if (hour == 0) hour = 12;
            else if (hour > 12) hour -= 12;
        }
        char h[8];
        wsprintf(h, iTLZero ? "%02d" : "%d", hour);
        wsprintf(szTime, "%s%c%02d%s", h, chTimeSep, tm->tm_min, pm ? sz2359 : sz1159);
    }
    if (szDate) {
        char m[8], d[8], y[8];
        wsprintf(m, (fDateFlags & 1) ? "%02d" : "%d", tm->tm_mon + 1);
        wsprintf(d, (fDateFlags & 2) ? "%02d" : "%d", tm->tm_mday);
        wsprintf(y, "%04d", tm->tm_year + 1900);
        if (!(fDateFlags & 4)) { y[0] = y[2]; y[1] = y[3]; y[2] = 0; }
        if (iDate == 0) wsprintf(szDate, "%s%c%s%c%s", m, chDateSep, d, chDateSep, y);
        else if (iDate == 1) wsprintf(szDate, "%s%c%s%c%s", d, chDateSep, m, chDateSep, y);
        else wsprintf(szDate, "%s%c%s%c%s", y, chDateSep, m, chDateSep, d);
    }
}

static void InsertDateTime(BOOL fCrlf)
{
    char szTime[32], szDate[32], buf[80];
    GetTimeDate(szTime, szDate);
    wsprintf(buf, fCrlf ? "\r\n%s  %s\r\n" : "%s  %s", szTime, szDate);
    SendMessage(hwndEdit, EM_REPLACESEL, 0, (LPARAM)buf);
}

/* ------------------------------------------------------------------ seg5:0004: Search */
static const char *FwdScan(const char *p, const char *s, BOOL fCaseSensitive)
{
    int n = lstrlen(s);
    for (; *p; p = AnsiNext(p))
        if (fCaseSensitive ? !strncmp(p, s, n) : !strncasecmp(p, s, n)) return p;
    return NULL;
}

static const char *RevScan(const char *start, const char *pEnd, const char *s, BOOL fCaseSensitive)
{
    int n = lstrlen(s);
    for (const char *p = pEnd; p >= start; p--) {
        if (p != pEnd || 1) {
            if ((fCaseSensitive ? !strncmp(p, s, n) : !strncasecmp(p, s, n)) && p + n <= pEnd + n && p < pEnd) return p;
        }
        if (p == start) break;
    }
    return NULL;
}

static void Search(char *sz)
{
    if (!*sz) return;
    HCURSOR hOld = SetCursor(hWaitCursor);
    DWORD sel = (DWORD)SendMessage(hwndEdit, EM_GETSEL, 0, 0);
    int selStart = LOWORD(sel), selEnd = HIWORD(sel);
    HLOCAL h = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    char *pText = LocalLock(h);
    const char *found = NULL;
    if (fReverse) {
        int line = (int)SendMessage(hwndEdit, EM_LINEFROMCHAR, selStart, 0);
        int lineStart = (int)SendMessage(hwndEdit, EM_LINEINDEX, line, 0);
        int end = selStart;
        for (; line >= 0 && !found; line--) {
            found = RevScan(pText + lineStart, pText + end, sz, fCase);
            if (found) break;
            end = lineStart;
            if (line > 0) lineStart = (int)SendMessage(hwndEdit, EM_LINEINDEX, line - 1, 0);
        }
    } else
        found = FwdScan(pText + selEnd, sz, fCase);
    int pos = found ? (int)(found - pText) : 0;
    LocalUnlock(h);
    SetCursor(hOld);
    if (!found)
        AlertBox(hDlgFind ? hDlgFind : hwndNP, szNotepad, szCantFindStr, szSearch, MB_ICONASTERISK);
    else
        SendMessage(hwndEdit, EM_SETSEL, 0, MAKELPARAM(pos, pos + lstrlen(sz)));
}

/* ------------------------------------------------------------------ seg3:0517: New */
static void New(BOOL fCheck)
{
    if (fCheck && !CheckSave(FALSE)) return;
    SendMessage(hwndEdit, WM_SETTEXT, 0, (LPARAM)"");
    fUntitled = TRUE;
    lstrcpy(szFileName, szUntitled);
    SetTitle(szFileName);
    SendMessage(hwndEdit, EM_SETSEL, 0, 0);
    hEdit = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    LocalReAlloc(hEdit, 1, LHND);
    SendMessage(hwndEdit, EM_SETHANDLE, (WPARAM)hEdit, 0);
    szSearch[0] = 0;
}

/* ------------------------------------------------------------------ seg3:0004: empty-file check */
static int CheckEmpty(HWND hwnd, char *name, BOOL fSaveAs)
{
    int r = 0;
    if (SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0) == 0) {
        if (fSaveAs)
            r = AlertBox(hwnd, szNotepad, szNoText, NULL, MB_ICONEXCLAMATION);
        else {
            r = AlertBox(hwndNP, szNotepad, szEmptyFile, name, MB_ICONEXCLAMATION | MB_OKCANCEL);
            if (r == IDOK) {
                OpenFile(of.szPathName, &of, OF_DELETE);
                New(FALSE);
            }
        }
    }
    return r;
}

/* ------------------------------------------------------------------ seg3:0080: SaveFile */
static BOOL SaveFile(HWND hwnd, char *name, BOOL fSaveAs)
{
    BOOL fCreated = FALSE;
    SetCursor(hWaitCursor);
    if (!fSaveAs)
        fp = OpenFile(name, &of, OF_REOPEN | OF_PROMPT | OF_CANCEL | OF_SHARE_DENY_WRITE | OF_READWRITE);
    else {
        fp = _lopen(name, OF_READWRITE);
        if (fp == HFILE_ERROR) {
            fp = _lcreat(name, 0);
            fCreated = fp != HFILE_ERROR;
        }
    }
    if (fp == HFILE_ERROR) {
        if (fSaveAs) AlertBox(hwnd, szNotepad, szCreateErr, name, MB_ICONEXCLAMATION);
        return FALSE;
    }
    BOOL fSoftBreaks = (BOOL)SendMessage(hwndEdit, EM_FMTLINES, FALSE, 0);
    char *p = LocalLock(hEdit);
    UINT len = (UINT)SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0);
    if (_lwrite(fp, p, len) != len) {
        LocalUnlock(hEdit);
        _lclose(fp);
        if (fCreated) OpenFile(name, &of, OF_DELETE);
        AlertBox(hwnd, szNotepad, szDiskFull, name, MB_ICONEXCLAMATION);
        return FALSE;
    }
    _lwrite(fp, p, 0); /* truncate */
    SendMessage(hwndEdit, EM_SETMODIFY, FALSE, 0);
    SetTitle(name);
    fUntitled = FALSE;
    _lclose(fp);
    if (fSoftBreaks) SendMessage(hwndEdit, EM_FMTLINES, TRUE, 0);
    if (fSaveAs) _lclose(OpenFile(name, &of, OF_READ));
    LocalUnlock(hEdit);
    return TRUE;
}

/* ------------------------------------------------------------------ seg3:0224: LoadFile */
static BOOL LoadFile(char *name)
{
    char szSave[160];
    if (fp == HFILE_ERROR) {
        AlertBox(hwndNP, szNotepad, szCantOpen, name, MB_ICONEXCLAMATION);
        return FALSE;
    }
    SetCursor(hWaitCursor);
    LONG size = _llseek(fp, 0, 2);
    _llseek(fp, 0, 0);
    HLOCAL hNew = NULL;
    if (size <= 0xFFFF) hNew = LocalReAlloc(hEdit, size + 1, LHND);
    if (!hNew) {
        lstrcpy(szSave, name);
        if (size <= 0xFFFF) {
            SendMessage(hwndEdit, WM_SETTEXT, 0, (LPARAM)"");
            New(FALSE);
            hNew = LocalReAlloc(hEdit, size + 1, LHND);
            if (!hNew) _lclose(fp);
        } else {
            _lclose(fp);
            New(FALSE);
        }
        if (!hNew) {
            AlertBox(hwndNP, szNotepad, szFTooBig, szSave, MB_ICONEXCLAMATION);
            return FALSE;
        }
    }
    hEdit = hNew;
    SendMessage(hwndEdit, EM_SETSEL, 0, 0);
    char *p = LocalLock(hEdit);
    _lread(fp, p, (UINT)size);
    _lclose(fp);
    for (LONG i = 0; i < size; i++)
        if (!p[i]) p[i] = ' ';
    p[size] = 0;
    BOOL fLog = p[0] == '.' && p[1] == 'L' && p[2] == 'O' && p[3] == 'G';
    LocalUnlock(hEdit);
    lstrcpy(szFileName, name);
    SetTitle(name);
    fUntitled = FALSE;
    SendMessage(hwndEdit, WM_SETREDRAW, FALSE, 0);
    fEditErr = 1;
    SendMessage(hwndEdit, EM_SETHANDLE, (WPARAM)hEdit, 0);
    if (fEditErr == 2) {
        fEditErr = 0;
        AlertBox(hwndNP, szNotepad, szFTooBig, name, MB_ICONEXCLAMATION);
        SendMessage(hwndEdit, WM_SETREDRAW, TRUE, 0);
        New(FALSE);
        return FALSE;
    }
    fEditErr = 0;
    PostMessage(hwndEdit, EM_LIMITTEXT, 0xFFFF, 0);
    if (fLog) {
        SendMessage(hwndEdit, EM_SETSEL, 0, MAKELPARAM(size, size));
        InsertDateTime(TRUE);
    }
    SetScrollPos(hwndNP, SB_VERT, (int)SendMessage(hwndEdit, WM_VSCROLL, EM_GETTHUMB, 0), TRUE);
    SendMessage(hwndEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwndEdit, NULL, TRUE);
    UpdateWindow(hwndEdit);
    return TRUE;
}

/* ------------------------------------------------------------------ common dialogs setup */
static void SetupOFN(char *file, const char *title, DWORD flags)
{
    OFN.lpstrFile = file;
    OFN.lpstrFileTitle = szFileTitle;
    OFN.lpstrTitle = title;
    OFN.Flags = flags;
    OFN.lpstrFilter = szFilter;
    OFN.lpstrCustomFilter = szCustFilter;
    OFN.lpstrDefExt = szFilterSpec + 3; /* "TXT" */
}

/* the Save As flow shared by File>Save As and CheckSave; returns TRUE when saved */
static BOOL DoSaveAs(HWND hwnd, char *buf)
{
    BOOL ok = FALSE;
    SetupOFN(buf, szSaveCaption, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN);
    fInSaveAsDlg = TRUE;
    if (GetSaveFileName(&OFN)) {
        if (SaveFile(hwnd, buf, TRUE)) {
            lstrcpy(szFileName, buf);
            ok = TRUE;
        }
    } else if (CommDlgExtendedError())
        AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONHAND | MB_SYSTEMMODAL);
    else
        ok = -1;
    fInSaveAsDlg = FALSE;
    return ok;
}

/* ------------------------------------------------------------------ seg1:06C8: CheckSave */
static BOOL CheckSave(BOOL fSysModal)
{
    int r = 1;
    if (fUntitled && !SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0)) return TRUE;
    if (!SendMessage(hwndEdit, EM_GETMODIFY, 0, 0)) return TRUE;
    char *name = fUntitled ? szUntitled : (szFileName[2] == '\\' && szFileName[3] == '\\' ? szFileName + 2 : szFileName);
    r = AlertBox(hwndNP, szNotepad, szModified, name, MB_YESNOCANCEL | MB_ICONEXCLAMATION | (fSysModal ? MB_SYSTEMMODAL : 0));
    if (r == IDYES) {
        if (!fUntitled) {
            int e = CheckEmpty(hwndNP, szFileName, FALSE);
            if (e) r = e == IDCANCEL ? IDCANCEL : IDYES;
            else if (SaveFile(hwndNP, szFileName, FALSE)) return TRUE;
            else {
                lstrcpy(szSaveTmp, szFileName);
                int ok = DoSaveAs(hwndNP, szSaveTmp);
                if (ok == -1) r = IDCANCEL;
            }
        } else {
            lstrcpy(szSaveTmp, szFilterSpec + 1); /* "*.TXT" */
            int ok = DoSaveAs(hwndNP, szSaveTmp);
            if (ok == -1) r = IDCANCEL;
        }
    }
    return r != IDCANCEL;
}

/* ------------------------------------------------------------------ seg1:0D7F: menu init */
static void NpResetMenu(void)
{
    HMENU hMenu = GetMenu(hwndNP);
    DWORD sel = (DWORD)SendMessage(hwndEdit, EM_GETSEL, 0, 0);
    UINT mf = LOWORD(sel) == HIWORD(sel) ? MF_GRAYED : MF_ENABLED;
    HMENU hEditMenu = GetSubMenu(hMenu, 1);
    EnableMenuItem(hEditMenu, M_CUT, mf);
    EnableMenuItem(hEditMenu, M_COPY, mf);
    EnableMenuItem(hEditMenu, M_CLEAR, mf);
    EnableMenuItem(hEditMenu, M_UNDO, SendMessage(hwndEdit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(hEditMenu, M_WW, fWrap ? MF_CHECKED : MF_UNCHECKED);
    char dev[32];
    BOOL havePrinter = PD.hDevNames || GetProfileString("windows", "device", "", dev, 20);
    EnableMenuItem(hMenu, M_PRINT, havePrinter ? MF_ENABLED : MF_GRAYED);
    mf = MF_GRAYED;
    if (OpenClipboard(hwndNP)) {
        UINT fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)))
            if (fmt == CF_TEXT) { mf = MF_ENABLED; break; }
        CloseClipboard();
    }
    EnableMenuItem(hEditMenu, M_PASTE, mf);
}

/* ------------------------------------------------------------------ seg1:1770: Page Setup */
static BOOL ParseHundredths(const char *s, LONG *out)
{
    /* "1.75" -> 175 (decimal separator from WIN.INI) */
    char buf[40];
    lstrcpy(buf, s);
    char *p = buf;
    while (*p && *p != chDecimal) p++;
    lstrcat(buf, "00");
    if (*p == chDecimal) { p[3] = 0; memmove(p, p + 1, strlen(p + 1) + 1); }
    else p[2] = 0;
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    BOOL neg = *p == '-';
    if (neg) p++;
    LONG v = 0;
    *out = -1;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return FALSE;
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return TRUE;
}

static void FormatHundredths(char *out, LONG v, BOOL fExact)
{
    if (fExact) wsprintf(out, "%d%c%02d", (int)(v / 100), chDecimal, (int)(v % 100));
    else wsprintf(out, "%d", (int)((v + 50) / 100));
}

static BOOL PageSetupDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        iMeasure = GetProfileInt("intl", "iMeasure", 1);
        for (int i = 30; i <= 31; i++) {
            SendDlgItemMessage(hDlg, i, EM_LIMITTEXT, 39, 0);
            SetDlgItemText(hDlg, i, szPageSetup[i - 30]);
        }
        for (int i = 32; i <= 35; i++) {
            SendDlgItemMessage(hDlg, i, EM_LIMITTEXT, 39, 0);
            if (iMeasure) SetDlgItemText(hDlg, i, szPageSetup[i - 30]);
            else {
                LONG v;
                ParseHundredths(szPageSetup[i - 30], &v);
                char b[40];
                FormatHundredths(b, (v * 254 + 50) / 100, TRUE);
                SetDlgItemText(hDlg, i, b);
            }
        }
        SendDlgItemMessage(hDlg, 30, EM_SETSEL, 0, MAKELPARAM(0, 39));
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDOK) {
            /* validate: digits and the decimal separator only, and the margins must fit the page */
            for (int i = 32; i <= 35; i++) {
                char b[40];
                GetDlgItemText(hDlg, i, b, 40);
                for (char *p = b; *p; p = AnsiNext(p))
                    if (!(*p >= '0' && *p <= '9') && *p != chDecimal) {
                        AlertBox(hDlg, szNotepad, szBadMargins, NULL, MB_ICONEXCLAMATION);
                        SetFocus(GetDlgItem(hDlg, i));
                        return FALSE;
                    }
            }
            /* page size in hundredths of an inch from the printer (Letter if none) */
            HDC hdcPrn = CreateIC("PRINTER", NULL, NULL, NULL);
            int pw = 850, ph = 1100;
            if (hdcPrn) {
                int hres = GetDeviceCaps(hdcPrn, HORZRES), vres = GetDeviceCaps(hdcPrn, VERTRES);
                int lx = GetDeviceCaps(hdcPrn, LOGPIXELSX), ly = GetDeviceCaps(hdcPrn, LOGPIXELSY);
                if (lx > 0 && ly > 0 && hres > 1 && vres > 1) { pw = hres * 100 / lx; ph = vres * 100 / ly; }
                DeleteDC(hdcPrn);
            }
            LONG m[4];
            for (int i = 0; i < 4; i++) {
                char b[40];
                GetDlgItemText(hDlg, 32 + i, b, 40);
                ParseHundredths(b, &m[i]);
                if (!iMeasure) m[i] = (m[i] * 100 + 127) / 254;
            }
            int bad = 0;
            if (m[0] >= pw) bad = 32;
            else if (m[1] >= pw) bad = 33;
            else if (m[2] >= ph) bad = 34;
            else if (m[3] >= ph) bad = 35;
            if (bad || pw - m[1] <= m[0] || ph - m[3] <= m[2]) {
                AlertBox(hDlg, szNotepad, szBadMargins, NULL, MB_ICONEXCLAMATION);
                if (bad) SetFocus(GetDlgItem(hDlg, bad));
                return TRUE;
            }
            GetDlgItemText(hDlg, 30, szPageSetup[0], 40);
            GetDlgItemText(hDlg, 31, szPageSetup[1], 40);
            for (int i = 32; i <= 35; i++) {
                GetDlgItemText(hDlg, i, szPageSetup[i - 30], 40);
                if (!iMeasure) FormatHundredths(szPageSetup[i - 30], m[i - 32], TRUE);
            }
            EndDialog(hDlg, 0);
            return TRUE;
        }
        if (wParam == IDCANCEL) { EndDialog(hDlg, 0); return TRUE; }
        return FALSE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg5:0209: word wrap */
static BOOL NpReCreate(DWORD style)
{
    BOOL fWrapOn = !(style & ES_AUTOHSCROLL);
    if (!fWrapOn) SendMessage(hwndEdit, EM_FMTLINES, FALSE, 0);
    int len = (int)SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0);
    HLOCAL hNew = len < 0xFFFF ? LocalAlloc(LHND, len + 1) : NULL;
    if (!hNew) {
        if (!fWrapOn) SendMessage(hwndEdit, EM_FMTLINES, TRUE, 0);
        return FALSE;
    }
    SetScrollRange(hwndNP, SB_HORZ, 0, fWrapOn ? 0 : 100, TRUE);
    RECT rc;
    GetClientRect(hwndNP, &rc);
    HWND hNewEdit = CreateWindow("Edit", "", style, 8, 2, rc.right - 15, rc.bottom - 4, hwndNP,
                                 (HMENU)ID_EDIT, hInstanceNP, NULL);
    if (!hNewEdit) { LocalFree(hNew); return FALSE; }
    SendMessage(hNewEdit, WM_SETFONT, (WPARAM)hFont, 0);
    char *p = LocalLock(hNew);
    SendMessage(hwndEdit, WM_GETTEXT, len + 1, (LPARAM)p);
    LocalUnlock(hNew);
    DestroyWindow(hwndEdit);
    hwndEdit = hNewEdit;
    hEdit = hNew;
    SendMessage(hwndEdit, EM_SETHANDLE, (WPARAM)hEdit, 0);
    PostMessage(hwndEdit, EM_LIMITTEXT, 0xFFFF, 0);
    ShowWindow(hwndNP, SW_SHOW);
    SetTitle(fUntitled ? szUntitled : szFileName);
    if (len) SendMessage(hwndEdit, EM_SETMODIFY, TRUE, 0);
    SetFocus(hwndEdit);
    SetScrollPos(hwndNP, SB_VERT, 0, TRUE);
    if (!fWrapOn) SetScrollPos(hwndNP, SB_HORZ, 0, TRUE);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:006C: NpCommand */
static BOOL NpCommand(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    switch (wParam) {
    case M_EXIT:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    case M_NEW:
        New(TRUE);
        break;
    case M_OPEN:
        if (!CheckSave(FALSE)) break;
        lstrcpy(szOpenFile, szFilterSpec + 1); /* "*.TXT" */
        SetupOFN(szOpenFile, szOpenCaption, OFN_HIDEREADONLY | OFN_FILEMUSTEXIST);
        if (GetOpenFileName(&OFN)) {
            fp = OpenFile(szOpenFile, &of, OF_READ);
            if (!LoadFile(szOpenFile)) fp = OpenFile(szFileName, &of, OF_READ);
        } else if (CommDlgExtendedError())
            AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONHAND | MB_SYSTEMMODAL);
        break;
    case M_SAVE:
        if (!fUntitled) {
            if (CheckEmpty(hwnd, szFileName, FALSE) || SaveFile(hwnd, szFileName, FALSE)) break;
        }
        /* fall through: untitled or the save failed */
    case M_SAVEAS:
        if (CheckEmpty(hwnd, szFileName, TRUE)) break;
        lstrcpy(szOpenFile, fUntitled ? szFilterSpec + 1 : szFileName);
        DoSaveAs(hwnd, szOpenFile);
        break;
    case M_SELECTALL: {
        int len = (int)SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0);
        SendMessage(hwndEdit, EM_SETSEL, 1, MAKELPARAM(0, len));
        break;
    }
    case M_FINDNEXT:
        if (szSearch[0]) { Search(szSearch); break; }
        /* fall through */
    case M_FIND:
        if (hDlgFind) SetFocus(hDlgFind);
        else {
            FR.lpstrFindWhat = szSearch;
            FR.wFindWhatLen = sizeof szSearch;
            hDlgFind = FindText(&FR);
        }
        break;
    case M_ABOUT:
        ShellAbout(hwndNP, szNotepad, "", LoadIcon(hInstanceNP, MAKEINTRESOURCE(1)));
        break;
    case M_USEHELP:
        if (!WinHelp(hwndNP, NULL, HELP_HELPONHELP, 0))
            AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONHAND | MB_SYSTEMMODAL);
        break;
    case M_HELP:
        if (!WinHelp(hwndNP, szHelpFile, HELP_INDEX, 0))
            AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONHAND | MB_SYSTEMMODAL);
        break;
    case M_SEARCHHELP:
        if (!WinHelp(hwndNP, szHelpFile, HELP_PARTIALKEY, (DWORD)0))
            AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONHAND | MB_SYSTEMMODAL);
        break;
    case M_CUT:
    case M_COPY:
    case M_CLEAR: {
        DWORD sel = (DWORD)SendMessage(hwndEdit, EM_GETSEL, 0, 0);
        if (LOWORD(sel) == HIWORD(sel)) break;
    }
        /* fall through */
    case M_PASTE: {
        HWND f = GetFocus();
        if (f == hwndEdit || f == hwndNP) SendMessage(hwndEdit, (UINT)wParam, 0, 0);
        break;
    }
    case M_DATETIME:
        InsertDateTime(FALSE);
        break;
    case M_UNDO:
        SendMessage(hwndEdit, EM_UNDO, 0, 0);
        break;
    case M_WW:
        if (NpReCreate(WS_CHILD | WS_VISIBLE | ES_NOHIDESEL | ES_AUTOVSCROLL | ES_MULTILINE | (fWrap ? ES_AUTOHSCROLL : 0)))
            fWrap = !fWrap;
        else
            AlertBox(hwndNP, szNotepad, szNoWW, NULL, MB_ICONEXCLAMATION);
        break;
    case ID_EDIT:
        if (W16_CMD_HWND(lParam) != hwndEdit) break;
        switch (HIWORD(lParam)) {
        case EN_CHANGE:
            if (!SendMessage(hwndEdit, WM_GETTEXTLENGTH, 0, 0)) {
                SetScrollPos(hwndNP, SB_VERT, 0, TRUE);
                SetScrollPos(hwndNP, SB_HORZ, 0, TRUE);
            }
            break;
        case EN_HSCROLL:
            SetScrollPos(hwndNP, SB_HORZ, (int)SendMessage(hwndEdit, WM_HSCROLL, EM_GETTHUMB, 0), TRUE);
            break;
        case EN_VSCROLL:
            SetScrollPos(hwndNP, SB_VERT, (int)SendMessage(hwndEdit, WM_VSCROLL, EM_GETTHUMB, 0), TRUE);
            break;
        }
        break;
    case M_PRINT: {
        int r = NpPrint();
        if (r >= 0 || r == SP_USERABORT) break;
        const char *msg = r == SP_OUTOFDISK ? szPrintDiskFull : r == SP_OUTOFMEMORY ? szPrintNoMem : szCantPrint;
        AlertBox(hwndNP, szNotepad, msg, fUntitled ? szUntitled : szFileName, MB_ICONEXCLAMATION);
        break;
    }
    case M_PAGESETUP:
        DialogBox(hInstanceNP, MAKEINTRESOURCE(14), hwndNP, PageSetupDlgProc);
        break;
    case M_SETUP:
        for (;;) {
            PD.Flags = PD_PRINTSETUP;
            PrintDlg(&PD);
            DWORD err = CommDlgExtendedError();
            if (err == PDERR_PRINTERNOTFOUND || err == PDERR_DNDMMISMATCH) {
                if (PD.hDevMode) GlobalFree(PD.hDevMode);
                if (PD.hDevNames) GlobalFree(PD.hDevNames);
                PD.hDevMode = PD.hDevNames = NULL;
                continue;
            }
            if (err == CDERR_DIALOGFAILURE || err == CDERR_INITIALIZATION || err == CDERR_LOADSTRFAILURE ||
                err == CDERR_LOADRESFAILURE || err == PDERR_LOADDRVFAILURE || err == PDERR_GETDEVMODEFAIL)
                AlertBox(hwndNP, szNotepad, err == PDERR_LOADDRVFAILURE ? szLoadDrvFail : szNoMem, NULL, 0);
            break;
        }
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:088A: NpWndProc */
static LRESULT NpWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_SIZE:
        if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
            InvalidateRect(hwndEdit, NULL, TRUE);
            MoveWindow(hwndEdit, 8, 2, LOWORD(lParam) - 15, HIWORD(lParam) - 4, TRUE);
        }
        break;
    case WM_ACTIVATE:
        if (wParam != WA_ACTIVE && wParam != WA_CLICKACTIVE) break;
        /* fall through */
    case WM_SETFOCUS:
        if (!IsIconic(hwndNP)) SetFocus(hwndEdit);
        break;
    case WM_KILLFOCUS:
        SendMessage(hwndEdit, msg, wParam, lParam);
        break;
    case WM_CLOSE:
        if (CheckSave(FALSE)) {
            if (!WinHelp(hwndNP, szHelpFile, HELP_QUIT, 0)) { /* no help running: fine */ }
            DestroyWindow(hwndNP);
            DeleteObject(hFont);
        }
        break;
    case WM_QUERYENDSESSION:
        if (fInSaveAsDlg) {
            MessageBeep(0);
            MessageBeep(0);
            MessageBox(hwndNP, szCantQuit, szNotepad, MB_SYSTEMMODAL);
            return 0;
        }
        return CheckSave(TRUE);
    case WM_ACTIVATEAPP:
        if (wParam) {
            if (dwSavedSel) SendMessage(hwndEdit, EM_SETSEL, 1, dwSavedSel);
        } else {
            DWORD sel = (DWORD)SendMessage(hwndEdit, EM_GETSEL, 0, 0);
            if (LOWORD(sel) == HIWORD(sel)) dwSavedSel = 0;
            else {
                dwSavedSel = sel;
                SendMessage(hwndEdit, EM_SETSEL, 1, MAKELPARAM(HIWORD(sel), HIWORD(sel)));
            }
        }
        break;
    case WM_COMMAND:
        if (W16_CMD_HWND(lParam) == hwndEdit && (HIWORD(lParam) == EN_ERRSPACE || HIWORD(lParam) == EN_MAXTEXT)) {
            if (fEditErr == 1) fEditErr = 2;
            else AlertBox(hwndNP, szNotepad, szNoMem, NULL, MB_ICONEXCLAMATION);
            break;
        }
        if (!NpCommand(hwnd, wParam, lParam)) IniInit();
        break;
    case WM_WININICHANGE:
        IniInit();
        break;
    case WM_SYSCOMMAND:
        if (fSetupMode && (wParam == SC_MINIMIZE || wParam == SC_NEXTWINDOW || wParam == SC_PREVWINDOW)) break;
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_HSCROLL:
    case WM_VSCROLL:
        return SendMessage(hwndEdit, msg, wParam, lParam);
    case WM_INITMENU:
        NpResetMenu();
        break;
    case WM_INITMENUPOPUP:
        if (fSetupMode && HIWORD(lParam)) EnableMenuItem(hSysMenuSetup, SC_MINIMIZE, MF_GRAYED | MF_DISABLED);
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        /* clicks in the margin around the edit control go to the edit control */
        RECT rc;
        GetClientRect(hwndEdit, &rc);
        int x = (SHORT)LOWORD(lParam), y = (SHORT)HIWORD(lParam);
        x = x <= 8 ? 0 : (x - 8 >= rc.right ? rc.right - 1 : x - 8);
        y = y <= 2 ? 0 : (y - 2 >= rc.bottom ? rc.bottom - 1 : y - 2);
        PostMessage(hwndEdit, msg, wParam, MAKELPARAM(x, y));
        break;
    }
    case WM_DROPFILES: {
        HANDLE hDrop = (HANDLE)wParam;
        if (DragQueryFile(hDrop, 0xFFFF, NULL, 0)) {
            DragQueryFile(hDrop, 0, szOpenFile, 128);
            SetActiveWindow(hwnd);
            if (CheckSave(FALSE)) {
                fp = OpenFile(szOpenFile, &of, OF_READ);
                if (!LoadFile(szOpenFile)) fp = OpenFile(szFileName, &of, OF_READ);
            }
        }
        DragFinish(hDrop);
        break;
    }
    default:
        if (msg == wFRMsg) {
            FINDREPLACE *fr = (FINDREPLACE *)lParam;
            fReverse = !(fr->Flags & FR_DOWN);
            fCase = (fr->Flags & FR_MATCHCASE) != 0;
            if (fr->Flags & FR_FINDNEXT) Search(szSearch);
            else if (fr->Flags & FR_DIALOGTERM) hDlgFind = NULL;
            break;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* ------------------------------------------------------------------ seg2:002C / 0626 / 02A7: init */
static BOOL NpInitStrings(HINSTANCE h)
{
    struct { char *buf; int id; int cb; } s[] = {
        {szCantOpen, IDS_CANTOPEN, 300}, {szCantFind, IDS_CANTFIND, 300}, {szAlreadyExists, IDS_ALREADYEXISTS, 300},
        {szModified, IDS_MODIFIED, 300}, {szUntitled, IDS_UNTITLED, 64}, {szNoMem, IDS_NOMEM, 300},
        {szCantFindStr, IDS_CANTFINDSTR, 300}, {szNotepad_, IDS_NOTEPAD_, 64}, {szFTooBig, IDS_FTOOBIG, 300},
        {szNotepad, IDS_NOTEPAD, 64}, {szClipTooLong, IDS_CLIPTOOLONG, 300}, {szDiskFull, IDS_DISKFULL, 300},
        {szInvalidFile, IDS_INVALIDFILE, 300}, {szEmptyFile, IDS_EMPTYFILE, 300}, {szNoText, IDS_NOTEXT, 300},
        {szCantPrint, IDS_CANTPRINT, 300}, {szBadName, IDS_BADNAME, 300}, {szBadName2, IDS_BADNAME2, 300},
        {szPrintDiskFull, IDS_PRINTDISKFULL, 300}, {szPrintNoMem, IDS_PRINTNOMEM, 300},
        {szCreateErr, IDS_CREATEERR, 300}, {szNoWW, IDS_NOWW, 300}, {szMerge, IDS_MERGE, 8},
        {szFilterSpec, IDS_FILTERSPEC, 32}, {szHelpFile, IDS_HELPFILE, 64}, {szBadMargins, IDS_BADMARGINS, 300},
        {szCantOpenPrint, IDS_CANTOPENPRINT, 300}, {szTextFiles, IDS_TEXTFILES, 64}, {szAllFiles, IDS_ALLFILES, 64},
        {szOpenCaption, IDS_OPENCAPTION, 64}, {szSaveCaption, IDS_SAVECAPTION, 64}, {szCantQuit, IDS_CANTQUIT, 300},
        {szLoadDrvFail, IDS_LOADDRVFAIL, 300}};
    for (size_t i = 0; i < sizeof s / sizeof *s; i++)
        if (!LoadString(h, s[i].id, s[i].buf, s[i].cb)) return FALSE;
    for (int i = 0; i < 6; i++) LoadString(h, IDS_HEADER + i, szPageSetup[i], 40);
    return TRUE;
}

static BOOL NpRegister(HINSTANCE h)
{
    WNDCLASS wc = {0};
    wc.style = CS_BYTEALIGNCLIENT;
    wc.lpfnWndProc = NpWndProc;
    wc.hInstance = h;
    wc.hIcon = LoadIcon(h, MAKEINTRESOURCE(1));
    wc.hCursor = LoadCursor(NULL, GetSystemMetrics(SM_PENWINDOWS) ? IDC_ARROW : IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCE(1);
    wc.lpszClassName = "Notepad";
    return RegisterClass(&wc) != 0;
}

/* NOTEPAD /.SETUP <file>: used by Setup to show README files */
static BOOL ProcessSetupCmd(char *cmd)
{
    if (strncmp(cmd, "/.SETUP", 7)) return FALSE;
    fSetupMode = TRUE;
    hSysMenuSetup = GetSystemMenu(hwndNP, FALSE);
    hAccel = LoadAccelerators(hInstanceNP, "SlipUpAcc");
    cmd += 7;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd) {
        lstrcpy(szFileName, cmd);
        AddExt(szFileName);
        fp = OpenFile(szFileName, &of, OF_READ);
        if (fp == HFILE_ERROR && AlertBox(hwndNP, szNotepad, szCantFind, szFileName, MB_ICONEXCLAMATION | MB_YESNO) == IDYES)
            fp = OpenFile(szFileName, &of, OF_CREATE);
        if (fp != HFILE_ERROR) LoadFile(szFileName);
    }
    return TRUE;
}

/* NOTEPAD /P <file>: print and exit */
static BOOL ProcessPrintCmd(char *cmd, int nCmdShow)
{
    if (cmd[0] != '/' || cmd[1] != 'P') return FALSE;
    cmd += 2;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) return FALSE;
    ShowWindow(hwndNP, nCmdShow);
    lstrcpy(szFileName, cmd);
    AddExt(szFileName);
    fp = OpenFile(szFileName, &of, OF_READ);
    if (fp == HFILE_ERROR) {
        AlertBox(hwndNP, szNotepad, szCantOpenPrint, szFileName, MB_ICONEXCLAMATION);
        return TRUE;
    }
    LoadFile(szFileName);
    int r = NpPrint();
    if (r < 0 && r != SP_USERABORT) {
        const char *msg = r == SP_OUTOFDISK ? szPrintDiskFull : r == SP_OUTOFMEMORY ? szPrintNoMem : szCantPrint;
        AlertBox(hwndNP, szNotepad, msg, fUntitled ? szUntitled : szFileName, MB_ICONEXCLAMATION);
    }
    return TRUE;
}

static BOOL NpInit(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    if (!NpInitStrings(hInstance)) return FALSE;
    hWaitCursor = LoadCursor(NULL, IDC_WAIT);
    hAccel = LoadAccelerators(hInstance, "MainAcc");
    if (!hWaitCursor || !hAccel) return FALSE;
    if (!hPrev && !NpRegister(hInstance)) return FALSE;
    hInstanceNP = hInstance;
    chDecimal = '.';
    IniInit();
    hwndNP = CreateWindow("Notepad", "", WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, CW_USEDEFAULT, 0,
                          CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
    if (!hwndNP) return FALSE;
    DragAcceptFiles(hwndNP, TRUE);
    PD.lStructSize = sizeof PD;
    PD.hwndOwner = hwndNP;
    hwndEdit = CreateWindow("Edit", "", WS_CHILD | WS_VISIBLE | ES_NOHIDESEL | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_MULTILINE,
                            0, 0, 600, 400, hwndNP, (HMENU)ID_EDIT, hInstance, NULL);
    if (!hwndEdit) return FALSE;
    hFont = GetStockObject(SYSTEM_FIXED_FONT);
    SendMessage(hwndEdit, WM_SETFONT, (WPARAM)hFont, 0);
    IniInit();
    szSearch[0] = 0;
    hEdit = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    PostMessage(hwndEdit, EM_LIMITTEXT, 0xFFFF, 0);
    SetTitle(szUntitled);
    lstrcpy(szFileName, szUntitled);
    AnsiUpper(lpCmdLine);
    if (!ProcessSetupCmd(lpCmdLine)) {
        if (ProcessPrintCmd(lpCmdLine, nCmdShow)) {
            PostMessage(hwndNP, WM_CLOSE, 0, 0);
            return TRUE;
        }
        if (*lpCmdLine) {
            lstrcpy(szFileName, lpCmdLine);
            AddExt(szFileName);
            fp = OpenFile(szFileName, &of, OF_READ);
            if (fp == HFILE_ERROR &&
                AlertBox(hwndNP, szNotepad, szCantFind, szFileName, MB_ICONEXCLAMATION | MB_YESNO) == IDYES)
                fp = OpenFile(szFileName, &of, OF_CREATE);
            if (fp != HFILE_ERROR) LoadFile(szFileName);
        }
    }
    if (fSetupMode) w16_SetWindowPtr(hwndNP, GWL_STYLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_VSCROLL | WS_HSCROLL);
    ShowWindow(hwndNP, nCmdShow);
    /* filter: "Text Files (*.TXT)\0*.TXT\0All Files (*.*)\0*.*\0\0" */
    char *p = szFilter;
    lstrcpy(p, szTextFiles); p += lstrlen(p) + 1;
    lstrcpy(p, szFilterSpec + 1); p += lstrlen(p) + 1;
    lstrcpy(p, szAllFiles); p += lstrlen(p) + 1;
    lstrcpy(p, "*.*"); p += lstrlen(p) + 1;
    *p = 0;
    szCustFilter[0] = 0;
    lstrcpy(szCustFilter + 1, szFilterSpec + 1);
    memset(&OFN, 0, sizeof OFN);
    OFN.lStructSize = sizeof OFN;
    OFN.hwndOwner = hwndNP;
    OFN.nMaxCustFilter = sizeof szCustFilter;
    OFN.nFilterIndex = 1;
    OFN.nMaxFile = 128;
    OFN.nMaxFileTitle = sizeof szFileTitle;
    memset(&FR, 0, sizeof FR);
    FR.lStructSize = sizeof FR;
    FR.hwndOwner = hwndNP;
    FR.Flags = FR_HIDEWHOLEWORD | FR_DOWN;
    wFRMsg = RegisterWindowMessage(FINDMSGSTRING);
    wHlpMsg = RegisterWindowMessage(HELPMSGSTRING);
    if (!wFRMsg || !wHlpMsg) return FALSE;
    SendMessage(hwndEdit, EM_SETSEL, 0, SendMessage(hwndEdit, EM_GETSEL, 0, 0));
    return TRUE;
}

/* ------------------------------------------------------------------ printing (seg1:1146) */
/* Printing: print.c (seg1:1146 NpPrintFile). */
static int NpPrint(void)
{
    extern int NpPrintFile(HWND hwndNP, HWND hwndEdit, HINSTANCE hInst, const char *title,
                           char header[40], char footer[40], const char *margins[4], char chDecimal,
                           PRINTDLG *pd);
    const char *m[4] = {szPageSetup[2], szPageSetup[3], szPageSetup[4], szPageSetup[5]};
    return NpPrintFile(hwndNP, hwndEdit, hInstanceNP, fUntitled ? szUntitled : szFileName,
                       szPageSetup[0], szPageSetup[1], m, chDecimal, &PD);
}

/* ------------------------------------------------------------------ seg1:0BFC: WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    if (!NpInit(hInstance, hPrev, lpCmdLine, nCmdShow)) return 0;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (hDlgFind && IsDialogMessage(hDlgFind, &msg)) continue;
        if (TranslateAccelerator(hwndNP, hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (PD.hDevMode) GlobalFree(PD.hDevMode);
    if (PD.hDevNames) GlobalFree(PD.hDevNames);
    return (int)msg.wParam;
}
