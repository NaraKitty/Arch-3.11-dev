/*
 * Notepad printing - native port of NOTEPAD.EXE 3.10 seg1:0FC6..1E12, 212C, 21B6.
 *
 * Ported from the disassembly of the user's own NOTEPAD.EXE. Each function notes the
 * original segment:offset; the original's globals are noted as [ds:offset].
 * Pages render through libw16's printer DC (GDI Escape interface, see T-PRN-01).
 */
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h"

void GetTimeDate(char *szTime, char *szDate); /* notepad.c, seg4 */

#define IDD_ABORT 12   /* "Now Printing" dialog */
#define ID_ABORTNAME 20
#define IDS_NOTEPAD_ 12 /* "Notepad - " */
#define IDS_UNTITLED 11
#define IDS_LETTERS 42  /* "fFpPtTdDcCrRlL": file, page, time, date, center, right, left */

static HWND hwndMain, hwndText;
static HINSTANCE hInstNP;
static BOOL fAbort;           /* [0x9ee] */
static HWND hAbortDlg;        /* [0xa9a] */
static HMENU hAbortSysMenu;   /* [0x8b2] */
static BOOL fPrintUntitled;   /* [0x16] as seen by the print code */
static char szPrintName[260]; /* [0x7ea] */
static int dyLine;            /* [0x8c2] tmHeight + tmExternalLeading */
static int dxChar;            /* [0x8c0] tmAveCharWidth */
static int tabStop;           /* [0xaac] 8 average characters */
static int dxPaper, dyPaper;  /* [0x8b4], [0x8b6] HORZRES / VERTRES */
static int dyHeaderOffset;    /* [0xbe6] a quarter inch */
static int dyTop, dyBottom, dxLeft, dxRight; /* [0xa9e], [0xaaa], [0x7e4], [0xaa6] */
static int cchLine;           /* [0x7e0] characters per printed line */
static int iPage;             /* [0xa9c] */
static char *pHFBuf;          /* [0x7e6] header/footer line, cchLine + 2 bytes */
static char szHF[2][40];      /* [0x8f8], [0x920] */
static char chDec;            /* [0x86a] */

static char *PFileInPath(char *path)
{
    char *p = path;
    for (char *s = path; *s; s = AnsiNext(s))
        if (*s == ':' || *s == '\\') p = s;
    if (p != path) p++;
    return p;
}

/* ------------------------------------------------------------------ seg1:0FC6: AbortProc */
static BOOL AbortProc(HDC hdc, int code)
{
    MSG msg;
    (void)hdc; (void)code;
    while (!fAbort && PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (!hAbortDlg || !IsDialogMessage(hAbortDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return !fAbort;
}

/* ------------------------------------------------------------------ seg1:174B: DestroyAbortWnd */
static void DestroyAbortWnd(void)
{
    EnableWindow(hwndMain, TRUE);
    DestroyWindow(hAbortDlg);
    hAbortDlg = NULL;
}

/* ------------------------------------------------------------------ seg1:1032: AbortDlgProc */
static BOOL AbortDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        hAbortSysMenu = GetSystemMenu(hDlg, FALSE);
        /* the original only names titled files; an untitled print keeps the template text */
        if (!fPrintUntitled) SetDlgItemText(hDlg, ID_ABORTNAME, PFileInPath(szPrintName));
        SetFocus(hDlg);
        return TRUE;
    case WM_COMMAND:
        fAbort = TRUE;
        DestroyAbortWnd();
        return TRUE;
    case WM_INITMENU:
        EnableMenuItem(hAbortSysMenu, SC_CLOSE, MF_GRAYED);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg1:212C: margin -> device units */
/* "1.5" inches at `pix` pixels/inch; only two decimals count, like the original */
static int MarginToPixels(const char *sz, int pix)
{
    char buf[22];
    int frac = 0;
    lstrcpy(buf, sz);
    char *p = strchr(buf, chDec);
    if (p) {
        *p++ = 0;
        if (p[0] && !p[1]) p[1] = '0';
        if (p[0]) p[2] = 0;
        frac = atoi(p) * pix / 100;
    }
    return (int)(atol(buf) * pix) + frac;
}

/* ------------------------------------------------------------------ seg1:1E12: FormatHeader */
/* Expands &f &p[+n] &t &d &c &r &l && into pHFBuf (cchLine wide, left/centre/right parts). */
static int FormatHeader(const char *src)
{
    char part[3][80];
    int len[3] = {0, 0, 0};
    int cur = 1; /* text before any &l/&c/&r is centred */
    char letters[15], tmp[160];
    if (!LoadString(hInstNP, IDS_LETTERS, letters, sizeof letters)) lstrcpy(letters, "fFpPtTdDcCrRlL");

#define PUT(s) do { const char *s_ = (s); while (*s_ && len[cur] < 79) part[cur][len[cur]++] = *s_++; } while (0)
    while (*src) {
        while (*src && *src != '&') {
            if (len[cur] < 79) part[cur][len[cur]++] = *src;
            src++;
        }
        if (*src != '&') break;
        char c = *++src;
        if (c == letters[0] || c == letters[1]) {
            /* the file name is the window title minus "Notepad - " */
            char pre[40], title[80];
            LoadString(hInstNP, IDS_NOTEPAD_, pre, sizeof pre);
            GetWindowText(hwndMain, title, sizeof title);
            int i = 0;
            while (pre[i] && pre[i] == title[i]) i++;
            PUT(title + i);
        } else if (c == letters[2] || c == letters[3]) {
            int add = 0;
            if (src[1] == '+') {
                src += 2;
                while (*src >= '0' && *src <= '9') add = add * 10 + (*src++ - '0');
                src--;
            }
            wsprintf(tmp, "%d", add + iPage);
            PUT(tmp);
        } else if (c == letters[4] || c == letters[5]) {
            GetTimeDate(tmp, NULL);
            PUT(tmp);
        } else if (c == letters[6] || c == letters[7]) {
            GetTimeDate(NULL, tmp);
            PUT(tmp);
        } else if (c == '&') {
            if (len[cur] < 79) part[cur][len[cur]++] = '&';
        } else if (c == letters[8] || c == letters[9])
            cur = 1;
        else if (c == letters[10] || c == letters[11])
            cur = 2;
        else if (c == letters[12] || c == letters[13])
            cur = 0;
        if (*src) src++;
    }
#undef PUT
    for (int i = 0; i < 3; i++) part[i][len[i]] = 0;

    for (int i = 0; i < cchLine; i++) pHFBuf[i] = ' ';
    pHFBuf[cchLine] = 0;
    for (int i = 0; i < len[0] && i < cchLine; i++) pHFBuf[i] = part[0][i];
    int x = (cchLine - len[1]) / 2;
    for (int i = 0; i < len[1]; i++) if (x + i >= 0 && x + i < cchLine) pHFBuf[x + i] = part[1][i];
    x = cchLine - len[2];
    for (int i = 0; i < len[2]; i++) if (x + i >= 0 && x + i < cchLine) pHFBuf[x + i] = part[2][i];
    return lstrlen(pHFBuf);
}

/* ------------------------------------------------------------------ seg1:10B8: PrintHeaderFooter */
static void PrintHeaderFooter(HDC hdc, int which)
{
    char buf[40];
    lstrcpy(buf, szHF[which]);
    int n = FormatHeader(buf);
    if (!*pHFBuf) return;
    int y = which == 0 ? dyHeaderOffset - dyLine : dyPaper - dyLine - dyHeaderOffset;
    TabbedTextOut(hdc, dxLeft, y, pHFBuf, n, 1, &tabStop, dxLeft);
}

/* ------------------------------------------------------------------ seg1:166F: ExpandLine */
/* Copies one edit line expanding tabs to 8 columns; output comes in chunks of exactly `width`
 * characters so the caller can print it `width` at a time. Returns the expanded length. */
static int ExpandLine(int width, char *dst, int cch, const char *src)
{
    char buf[202];
    int total = 0, n = 0;
    if (width > 200) width = 200;
    for (int i = 0; i < cch; i++, src++) {
        if (n >= width) {
            buf[width] = 0;
            lstrcpy(dst, buf);
            dst += width;
            n = 0;
        }
        if (*src == '\t') {
            int to = ((n + 8) / 8) * 8;
            if (to >= width) to = width;
            for (int k = n; k < to; k++) buf[k] = ' ';
            total += to - n;
            n = to;
        } else {
            buf[n++] = *src;
            total++;
        }
    }
    buf[n] = 0;
    lstrcpy(dst, buf);
    return total;
}

/* ------------------------------------------------------------------ seg1:21B6: GetPrinterDC */
static HDC GetPrinterDC(PRINTDLG *pd, BOOL *fNoPrinter)
{
    *fNoPrinter = FALSE;
    if (!pd->hDevNames) {
        pd->Flags = PD_RETURNDEFAULT | PD_PRINTSETUP;
        PrintDlg(pd);
    }
    if (!pd->hDevNames) { *fNoPrinter = TRUE; return NULL; }
    DEVNAMES *dn = GlobalLock(pd->hDevNames);
    void *dm = pd->hDevMode ? GlobalLock(pd->hDevMode) : NULL;
    HDC hdc = CreateDC((char *)dn + dn->wDriverOffset, (char *)dn + dn->wDeviceOffset, (char *)dn + dn->wOutputOffset, dm);
    GlobalUnlock(pd->hDevNames);
    if (pd->hDevMode) GlobalUnlock(pd->hDevMode);
    return hdc;
}

/* ------------------------------------------------------------------ seg1:1146: NpPrintFile */
int NpPrintFile(HWND hwndNP, HWND hwndEdit, HINSTANCE hInst, const char *title, char header[40], char footer[40],
                const char *margins[4], char chDecimal, PRINTDLG *pd)
{
    char szDoc[300], szUntitledStr[64];
    TEXTMETRIC tm;
    HGLOBAL hTmp = NULL, hHF = NULL;
    int err;

    hwndMain = hwndNP;
    hwndText = hwndEdit;
    hInstNP = hInst;
    chDec = chDecimal;
    lstrcpy(szHF[0], header);
    lstrcpy(szHF[1], footer);
    lstrcpy(szPrintName, title);
    LoadString(hInst, IDS_UNTITLED, szUntitledStr, sizeof szUntitledStr);
    fPrintUntitled = !lstrcmp(title, szUntitledStr);

    SetCursor(LoadCursor(NULL, IDC_WAIT));
    fAbort = FALSE;
    BOOL fNoPrinter;
    HDC hdc = GetPrinterDC(pd, &fNoPrinter);
    if (fNoPrinter) return SP_ERROR;
    if (!hdc) return SP_OUTOFMEMORY;

    SetBkMode(hdc, TRANSPARENT);
    GetTextMetrics(hdc, &tm);
    dyLine = tm.tmHeight + tm.tmExternalLeading;
    dxChar = tm.tmAveCharWidth;
    tabStop = dxChar * 8;
    dxPaper = GetDeviceCaps(hdc, HORZRES);
    dyPaper = GetDeviceCaps(hdc, VERTRES);
    int xPix = GetDeviceCaps(hdc, LOGPIXELSX), yPix = GetDeviceCaps(hdc, LOGPIXELSY);
    dyHeaderOffset = yPix / 4;
    dyTop = MarginToPixels(margins[2], yPix);
    dyBottom = MarginToPixels(margins[3], yPix);
    dxLeft = MarginToPixels(margins[0], xPix);
    dxRight = MarginToPixels(margins[1], xPix);
    int linesPerPage = dyLine ? (dyPaper - dyBottom - dyTop) / dyLine : 0;
    int dxSpace = LOWORD(GetTextExtent(hdc, " ", 1));
    if (!dxSpace) dxSpace = 1;
    int leftChars = dxLeft / dxSpace, rightChars = dxRight / dxSpace;
    cchLine = (dxChar ? dxPaper / dxChar : 0) - rightChars - leftChars;
    if (cchLine < 1 || linesPerPage < 1) { DeleteDC(hdc); return SP_ERROR; }

    Escape(hdc, SETABORTPROC, 0, (LPCSTR)(void *)AbortProc, NULL);
    lstrcpy(szDoc, fPrintUntitled ? szUntitledStr : PFileInPath(szPrintName));
    {
        char pre[40];
        LoadString(hInst, IDS_NOTEPAD_, pre, sizeof pre);
        char tmp[300];
        lstrcpy(tmp, pre);
        lstrcat(tmp, szDoc);
        lstrcpy(szDoc, tmp);
    }
    EnableWindow(hwndNP, FALSE);
    hAbortDlg = CreateDialog(hInst, MAKEINTRESOURCE(IDD_ABORT), hwndNP, AbortDlgProc);
    if (!hAbortDlg) { err = SP_OUTOFMEMORY; goto fail; }

    err = Escape(hdc, STARTDOC, lstrlen(szDoc), szDoc, NULL);
    if (err < 0) goto fail;

    HLOCAL hText = (HLOCAL)SendMessage(hwndEdit, EM_GETHANDLE, 0, 0);
    int iLineStart = 0, y = 0, iLine = 0, iLineOnPage = 0;
    BOOL fMore = FALSE;
    iPage = 1;
    hHF = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cchLine + 2);
    hTmp = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 0x1000);
    if (!hHF || !hTmp) { err = SP_OUTOFMEMORY; goto fail; }
    pHFBuf = GlobalLock(hHF);
    char *pTmp = GlobalLock(hTmp);
    char *pText = LocalLock(hText);

    for (;;) {
        if (iLineOnPage == 0) PrintHeaderFooter(hdc, 0);
        int cch = (int)SendMessage(hwndEdit, EM_LINELENGTH, iLineStart, 0);
        if (cch > 0x1000 / 2) cch = 0x1000 / 2; /* the original's 4 KB buffer; edit lines are shorter */
        cch = ExpandLine(cchLine, pTmp, cch, pText + iLineStart);
        int off = 0;
        if (cch > cchLine) fMore = TRUE;
        do {
            if (cch > cchLine) {
                TabbedTextOut(hdc, dxLeft, dyTop - dyLine + y, pTmp + off, cchLine, 1, &tabStop, dxLeft);
                off += cchLine;
                cch -= cchLine;
            } else {
                TabbedTextOut(hdc, dxLeft, dyTop - dyLine + y, pTmp + off, cch, 1, &tabStop, dxLeft);
                fMore = FALSE;
            }
            y += dyLine;
            if (++iLineOnPage >= linesPerPage) {
                PrintHeaderFooter(hdc, 1);
                iPage++;
                iLineOnPage = 0;
                y = 0;
                err = Escape(hdc, NEWFRAME, 0, NULL, NULL);
                if (err < 0) {
                    LocalUnlock(hText);
                    GlobalUnlock(hTmp);
                    goto fail;
                }
                if (fMore) PrintHeaderFooter(hdc, 0);
            }
        } while (fMore);
        if (fAbort) break;
        iLineStart = (int)SendMessage(hwndEdit, EM_LINEINDEX, ++iLine, 0);
        if (iLineStart == -1) break;
    }
    LocalUnlock(hText);
    GlobalUnlock(hTmp);
    GlobalFree(hTmp);
    hTmp = NULL;
    if (iLineOnPage) PrintHeaderFooter(hdc, 1);
    if (!fAbort) {
        err = Escape(hdc, NEWFRAME, 0, NULL, NULL) < 0 ? 1 : 0;
        if (err) goto fail;
    }
    if (!fAbort) {
        Escape(hdc, ENDDOC, 0, NULL, NULL);
        DestroyAbortWnd();
    }
    DeleteDC(hdc);
    GlobalUnlock(hHF);
    GlobalFree(hHF);
    pHFBuf = NULL;
    return 0;

fail: /* seg1:1349 */
    DeleteDC(hdc);
    if (!fAbort && hAbortDlg) DestroyAbortWnd();
    else EnableWindow(hwndNP, TRUE);
    if (hHF) { GlobalUnlock(hHF); GlobalFree(hHF); pHFBuf = NULL; }
    if (hTmp) GlobalFree(hTmp);
    return err;
}
