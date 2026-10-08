/* Desktop applet: port of MAIN.CPL seg18, the dialog procedures of dialog 8 "Desktop" (seg18:1419)
 * and dialog 34 "Desktop - Edit Pattern" (seg18:03A0) with their helpers. Pattern, wallpaper,
 * screen saver, Alt+Tab switching, icon spacing and title wrap, sizing grid and border width and the
 * cursor blink rate go through SystemParametersInfo and WIN.INI / CONTROL.INI / SYSTEM.INI exactly
 * as 3.1 writes them; libw16 repaints the desktop when the pattern or the wallpaper changes.
 *
 * Screen savers are 16-bit programs (.SCR). They are listed by the description in their NE header,
 * read as data and never run: Test and Setup ran the saver with " /s" / " /c" (WinExec); arch311
 * has no native screen savers yet, so the two buttons do nothing (TODO: native screen savers). */
#include "maincpl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDD_EDITPATTERN 34
#define IDC_EDITGRID 101        /* Edit Pattern: the 8 x 8 cells */
#define IDC_PATNAME 102         /* pattern name (both dialogs) */
#define IDC_EDITPAT 103
#define IDC_WALLPAPER 105
#define IDC_CENTER 107
#define IDC_TILE 108
#define IDC_ADD 109
#define IDC_CHANGE 110
#define IDC_REMOVE 111
#define IDC_GRANULARITY 113     /* each spin field's cpArrow is its ID + 1 */
#define IDC_SAMPLE 115          /* Edit Pattern: the sample */
#define IDC_SPACING 116
#define IDC_WRAP 118
#define IDC_FASTSWITCH 121
#define IDC_953 953             /* handled by seg18:186F, not in dialog 8 */
#define IDC_BORDER 954
#define IDC_BLINKRATE 958
#define IDC_BLINKSAMPLE 959
#define IDC_SAVER 960
#define IDC_DELAY 961
#define IDC_TEST 963
#define IDC_SETUP 964
#define TIMER_BLINK 1000

/* MAIN.CPL strings */
#define IDS_BLINKERR 5
#define IDS_NONE 206
#define IDS_PATTERNS 207
#define IDS_GRANULARITY 208
#define IDS_WALLPAPER 210
#define IDS_TILE 211
#define IDS_BMP 212
#define IDS_BLINKRATE 213
#define IDS_BORDER 214
#define IDS_NOWALLPAPER 215
#define IDS_SPACING 218
#define IDS_UNLISTED 219
#define IDS_DELAY 220
#define IDS_SCR 221
#define IDS_IW 222
#define IDS_SCRNSAVE 223
#define IDS_REMOVE 239

static HWND hwndSaver;          /* [0xe46] the screen saver list */
static int iCurPattern;         /* [0xfde] Edit Pattern: the pattern loaded (index in its list) */
static BOOL fTracking;          /* [0x11d6] Edit Pattern: the mouse is painting cells */
static WORD wPattern[8];        /* [0x1948] the pattern being edited: one row a word, bit 7 left */
static int nBlinkPos;           /* [0x1d58] 1400 - cursor blink rate in ms */
static RECT rcBlink;            /* [0x1d86] the blinking sample, dialog client coordinates */
static HBRUSH hbrBlinkBk;       /* [0x1e28] */
static BOOL fEraseMode;         /* [0x1ec4] Edit Pattern: the drag clears cells */
static BOOL fBlinkOn;           /* [0x1fc4] */
static BOOL fPatternDirty;      /* [0x1fe4] Edit Pattern: cells changed since loaded or saved */

/* ds:0A90: the spin fields' steps (see ARROWSTEP); the icon spacing minimum becomes SM_CXICON */
static ARROWSTEP asDelay = {{1, -1, 5, -5}, 99, 1, 5, 5, 0};          /* 961, minutes */
static ARROWSTEP asSpacing = {{1, -1, 5, -5}, 512, 1, 100, 100, 0};   /* 116, pixels */
static ARROWSTEP asGranularity = {{1, -1, 5, -5}, 49, 0, 0, 0, 0};    /* 113 */
static ARROWSTEP asBorder = {{1, -1, 5, -5}, 50, 1, 1, 1, 0};         /* 954 */
static ARROWSTEP asBlink = {{-20, 20, -250, 250}, 1200, 200, 0, 0, 0}; /* 958, scroll position */

static const char szDesktop[] = "Desktop";

static void ClientRectOf(HWND hDlg, int id, RECT *rc)
{
    GetWindowRect(GetDlgItem(hDlg, id), rc);
    ScreenToClient(hDlg, (POINT *)&rc->left);
    ScreenToClient(hDlg, (POINT *)&rc->right);
}

/* ------------------------------------------------------------------ seg18:0000
 * the eight numbers of a CONTROL.INI pattern, 16-bit like the original (MAIN.CPL skipped past the
 * end of a string with fewer numbers and trailing non-digits: here the scan stops at the end) */
static void ParsePatternBits(LPCSTR p)
{
    for (int i = 0; i < 8; i++) {
        SHORT n = 0;
        if (*p) {
            while (*p && ((signed char)*p < '0' || (signed char)*p > '9')) p++;
            while ((signed char)*p >= '0' && (signed char)*p <= '9') n = (SHORT)(n * 10 + *p++ - '0');
        }
        wPattern[i] = (WORD)n;
    }
}

/* ------------------------------------------------------------------ seg18:0068
 * a brush of the pattern: set cells in the window text colour, clear ones in the desktop colour */
static HBRUSH CreatePatternBrushFromBits(HWND hwnd)
{
    HBRUSH hbr = NULL;
    HBITMAP hbmMono = CreateBitmap(8, 8, 1, 1, wPattern);
    if (!hbmMono) return NULL;
    HDC hdcScreen = GetDC(hwnd);
    HDC hdcMono = CreateCompatibleDC(hdcScreen);
    if (hdcMono) {
        SelectObject(hdcMono, hbmMono);
        HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, 8, 8);
        if (hbmColor) {
            HDC hdcColor = CreateCompatibleDC(hdcScreen);
            if (hdcColor) {
                SelectObject(hdcColor, hbmColor);
                SetTextColor(hdcColor, GetSysColor(COLOR_BACKGROUND));
                SetBkColor(hdcColor, GetSysColor(COLOR_WINDOWTEXT));
                BitBlt(hdcColor, 0, 0, 8, 8, hdcMono, 0, 0, SRCCOPY);
                hbr = CreatePatternBrush(hbmColor);
                DeleteDC(hdcColor);
            }
            DeleteObject(hbmColor);
        }
        DeleteDC(hdcMono);
    }
    ReleaseDC(hwnd, hdcScreen);
    DeleteObject(hbmMono);
    return hbr;
}

/* ------------------------------------------------------------------ seg18:0156
 * the cells (every one, once the grid meets the rectangle) and the sample, inside their frames */
static void PaintPatternAreas(HWND hDlg, HDC hdc, const RECT *prcPaint)
{
    RECT rc, rcCell;
    GetWindowRect(GetDlgItem(hDlg, IDC_EDITGRID), &rc);
    rc.top++; rc.left++; rc.bottom--; rc.right--;
    ScreenToClient(hDlg, (POINT *)&rc.left);
    ScreenToClient(hDlg, (POINT *)&rc.right);
    if (IntersectRect(&rcCell, prcPaint, &rc)) {
        HBRUSH hbrClear = CreateSolidBrush(GetNearestColor(hdc, GetSysColor(COLOR_BACKGROUND)));
        if (hbrClear) {
            HBRUSH hbrSet = CreateSolidBrush(GetSysColor(COLOR_WINDOWTEXT));
            if (hbrSet) {
                rcCell.right = rc.left;
                for (int i = 0; i < 8; i++) {
                    rcCell.left = rcCell.right;
                    rcCell.right = (SHORT)((i + 1) * (rc.right - rc.left)) / 8 + rc.left;
                    rcCell.bottom = rc.top;
                    for (int j = 0; j < 8; j++) {
                        rcCell.top = rcCell.bottom;
                        rcCell.bottom = (SHORT)((j + 1) * (rc.bottom - rc.top)) / 8 + rc.top;
                        FillRect(hdc, &rcCell, (wPattern[j] & (1 << (7 - i))) ? hbrSet : hbrClear);
                    }
                }
                DeleteObject(hbrSet);
            }
            DeleteObject(hbrClear);
        }
    }
    GetWindowRect(GetDlgItem(hDlg, IDC_SAMPLE), &rc);
    rc.top++; rc.left++; rc.bottom--; rc.right--;
    ScreenToClient(hDlg, (POINT *)&rc.left);
    ScreenToClient(hDlg, (POINT *)&rc.right);
    if (IntersectRect(&rcCell, prcPaint, &rc)) {
        HBRUSH hbr = CreatePatternBrushFromBits(hDlg);
        if (hbr) {
            FillRect(hdc, &rcCell, hbr);
            DeleteObject(hbr);
        }
    }
}

/* ------------------------------------------------------------------ seg18:0306 */
static void RedrawPatternAreas(HWND hDlg)
{
    RECT rc;
    HDC hdc = GetDC(hDlg);
    ClientRectOf(hDlg, IDC_EDITGRID, &rc);
    PaintPatternAreas(hDlg, hdc, &rc);
    ClientRectOf(hDlg, IDC_SAMPLE, &rc);
    PaintPatternAreas(hDlg, hdc, &rc);
    ReleaseDC(hDlg, hdc);
}

/* the Desktop dialog: the parent of the Edit Pattern dialog */
static HWND ParentDlg(HWND hDlg) { return GetParent(hDlg); }

/* ------------------------------------------------------------------ seg18:03A0 */
static BOOL EditPatternDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szName[0x50], szBuf[0x52], szBits[0x52], szSection[10], szNone[0x10];
    HWND hCombo, hParentCombo;
    RECT rc;
    int i;

    switch (msg) {
    case WM_INITDIALOG:
        fTracking = FALSE;
        fPatternDirty = FALSE;
        hCombo = GetDlgItem(hDlg, IDC_PATNAME);
        hParentCombo = GetDlgItem(ParentDlg(hDlg), IDC_PATNAME);
        /* the Desktop list without its first item, "(None)" */
        for (i = (int)SendMessage(hParentCombo, CB_GETCOUNT, 0, 0) - 1; i > 0; i--) {
            SendMessage(hParentCombo, CB_GETLBTEXT, i, (LPARAM)szBuf);
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)szBuf);
        }
        iCurPattern = (int)SendMessage(hParentCombo, CB_GETCURSEL, 0, 0) - 1;
        SendMessage(hCombo, CB_SETCURSEL, iCurPattern, 0);
        if (iCurPattern >= 0) {
            SendMessage(hCombo, CB_GETLBTEXT, iCurPattern, (LPARAM)szName);
            LoadString(hInstMain, IDS_PATTERNS, szSection, sizeof szSection);
            GetPrivateProfileString(szSection, szName, "", szBuf, 0x51, szControlIni);
            ParsePatternBits(szBuf);
        } else {
            memset(wPattern, 0, sizeof wPattern);
        }
        EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), iCurPattern >= 0);
        EnableWindow(GetDlgItem(hDlg, IDC_ADD), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_CHANGE), FALSE);
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hDlg, &ps);
        int old = SetBkMode(ps.hdc, TRANSPARENT);
        PaintPatternAreas(hDlg, ps.hdc, &ps.rcPaint);
        SetBkMode(ps.hdc, old);
        EndPaint(hDlg, &ps);
        return FALSE;
    }

    case WM_MOUSEMOVE:
        if (!fTracking) return FALSE;
        /* fall through */
    case WM_LBUTTONDOWN: {
        POINT pt = {(SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam)};
        ClientRectOf(hDlg, IDC_EDITGRID, &rc);       /* the frame too, unlike the painting */
        if (!PtInRect(&rc, pt)) return FALSE;
        int col = (pt.x - rc.left) * 8 / (rc.right - rc.left);
        int row = (pt.y - rc.top) * 8 / (rc.bottom - rc.top);
        WORD old = wPattern[row];
        if (msg == WM_LBUTTONDOWN) {
            SetCapture(hDlg);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
            fTracking = TRUE;
            fPatternDirty = TRUE;
            fEraseMode = (wPattern[row] & (1 << (7 - col))) != 0;
            if (iCurPattern >= 0 && (int)(SHORT)LOWORD(SendDlgItemMessage(hDlg, IDC_PATNAME, CB_GETCURSEL, 0, 0)) == iCurPattern)
                EnableWindow(GetDlgItem(hDlg, IDC_CHANGE), TRUE);
        }
        if (fEraseMode) wPattern[row] &= ~(1 << (7 - col));
        else wPattern[row] |= 1 << (7 - col);
        if (wPattern[row] != old) RedrawPatternAreas(hDlg);
        return FALSE;
    }

    case WM_LBUTTONUP:
        if (fTracking) {
            ReleaseCapture();
            fTracking = FALSE;
        }
        return FALSE;

    case WM_COMMAND:
        switch (wParam) {
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;

        case IDC_PATNAME: {
            hCombo = GetDlgItem(hDlg, IDC_PATNAME);
            if (HIWORD(lParam) == CBN_SELCHANGE) {
                iCurPattern = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                SendMessage(hCombo, CB_GETLBTEXT, iCurPattern, (LPARAM)szName);
                LoadString(hInstMain, IDS_PATTERNS, szSection, sizeof szSection);
                GetPrivateProfileString(szSection, szName, "", szBuf, 0x50, szControlIni);
                ParsePatternBits(szBuf);
                RedrawPatternAreas(hDlg);
                goto saved;
            }
            if (HIWORD(lParam) != CBN_EDITCHANGE) return TRUE;
            int len = (int)SendMessage(hCombo, WM_GETTEXTLENGTH, 0, 0), found, itemLen;
            if (len == 0) {
                EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_CHANGE), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_ADD), FALSE);
                return TRUE;
            }
            SendMessage(hCombo, WM_GETTEXT, 0x50, (LPARAM)szName);
            found = (int)SendMessage(hCombo, CB_FINDSTRING, (WPARAM)-1, (LPARAM)szName);
            if (found >= 0) itemLen = (int)SendMessage(hCombo, CB_GETLBTEXTLEN, found, 0);
            else itemLen = --found;
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), !fPatternDirty && found == iCurPattern);
            EnableWindow(GetDlgItem(hDlg, IDC_CHANGE), (found != iCurPattern || fPatternDirty) && len == itemLen);
            EnableWindow(GetDlgItem(hDlg, IDC_ADD), len != itemLen);
            return TRUE;
        }

        case IDC_ADD:
        case IDC_CHANGE:
            SetDlgItemText(hDlg, IDCANCEL, szClose);
            hCombo = GetDlgItem(hDlg, IDC_PATNAME);
            SendMessage(hCombo, WM_GETTEXT, 0x50, (LPARAM)szName);
            if (wParam == IDC_ADD) {
                iCurPattern = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)szName);
                SendMessage(hCombo, CB_SETCURSEL, iCurPattern, 0);
            }
            /* wvsprintf over the eight words: 16-bit %d */
            wsprintf(szBits, "%d %d %d %d %d %d %d %d", (SHORT)wPattern[0], (SHORT)wPattern[1],
                     (SHORT)wPattern[2], (SHORT)wPattern[3], (SHORT)wPattern[4], (SHORT)wPattern[5],
                     (SHORT)wPattern[6], (SHORT)wPattern[7]);
            LoadString(hInstMain, IDS_PATTERNS, szSection, sizeof szSection);
            WritePrivateProfileString(szSection, szName, szBits, szControlIni);
        saved:
            fPatternDirty = FALSE;
            EnableWindow(GetDlgItem(hDlg, IDC_ADD), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_CHANGE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), TRUE);
            return TRUE;

        case IDC_REMOVE: {
            hCombo = GetDlgItem(hDlg, IDC_PATNAME);
            int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            if (idx < 0) return TRUE;
            SendMessage(hCombo, WM_GETTEXT, 0x50, (LPARAM)szName);
            LoadString(hInstMain, IDS_PATTERNS, szSection, sizeof szSection);
            if (!ConfirmRemove(hDlg, szName, IDS_REMOVE)) return TRUE;
            SetDlgItemText(hDlg, IDCANCEL, szClose);
            SendMessage(hCombo, CB_DELETESTRING, idx, 0);
            WritePrivateProfileString(szSection, szName, NULL, szControlIni);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
            return TRUE;   /* (iCurPattern is left as it was) */
        }

        case IDOK:
        case IDCANCEL:
            /* the Desktop list again: "(None)" and this dialog's names */
            hCombo = GetDlgItem(hDlg, IDC_PATNAME);
            hParentCombo = GetDlgItem(ParentDlg(hDlg), IDC_PATNAME);
            szName[0] = 0;   /* (MAIN.CPL left it unset when the Desktop list had no selection) */
            iCurPattern = (int)SendMessage(hParentCombo, CB_GETCURSEL, 0, 0);
            SendMessage(hParentCombo, CB_GETLBTEXT, iCurPattern, (LPARAM)szName);
            SendMessage(hParentCombo, CB_RESETCONTENT, 0, 0);
            for (i = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0) - 1; i >= 0; i--) {
                SendMessage(hCombo, CB_GETLBTEXT, i, (LPARAM)szBuf);
                SendMessage(hParentCombo, CB_ADDSTRING, 0, (LPARAM)szBuf);
            }
            LoadString(hInstMain, IDS_NONE, szNone, sizeof szNone);
            SendMessage(hParentCombo, CB_INSERTSTRING, 0, (LPARAM)szNone);
            if (wParam == IDOK) {
                SendMessage(hParentCombo, CB_SETCURSEL, (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0) + 1, 0);
            } else {
                iCurPattern = (int)SendMessage(hParentCombo, CB_FINDSTRING, (WPARAM)-1, (LPARAM)szName);
                SendMessage(hParentCombo, CB_SETCURSEL, iCurPattern > 0 ? iCurPattern : 0, 0);
            }
            EndDialog(hDlg, wParam == IDOK);
            return TRUE;
        }
        return FALSE;

    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}

/* ------------------------------------------------------------------ seg18:0BB2
 * the settings of the Desktop dialog; FALSE (and the dialog stays) when the wallpaper is missing */
static BOOL ApplyDesktopSettings(HWND hDlg)
{
    char szVal[0x9e], szKey[0x9e], szBuf[0x9e], szNone[0x10];
    OFSTRUCT of;                    /* of.szPathName is the wallpaper's full name */
    BOOL fOK;
    int val;

    SystemParametersInfo(SPI_SETFASTTASKSWITCH, IsDlgButtonChecked(hDlg, IDC_FASTSWITCH), NULL, SPIF_UPDATEINIFILE);
    LoadString(hInstMain, IDS_NONE, szNone, sizeof szNone);
    val = (int)GetDlgItemInt(hDlg, IDC_GRANULARITY, &fOK, FALSE);
    SystemParametersInfo(SPI_SETGRIDGRANULARITY, val, NULL, SPIF_UPDATEINIFILE);
    val = (int)GetDlgItemInt(hDlg, IDC_SPACING, &fOK, FALSE);
    SystemParametersInfo(SPI_ICONHORIZONTALSPACING, val, NULL, SPIF_UPDATEINIFILE);
    SystemParametersInfo(SPI_SETICONTITLEWRAP, IsDlgButtonChecked(hDlg, IDC_WRAP), NULL, SPIF_UPDATEINIFILE);

    /* the pattern's numbers from CONTROL.INI [Patterns] go to WIN.INI [Desktop] Pattern= */
    SendMessage(GetDlgItem(hDlg, IDC_PATNAME), WM_GETTEXT, sizeof szBuf, (LPARAM)szBuf);
    LoadString(hInstMain, IDS_PATTERNS, szKey, sizeof szKey);
    GetPrivateProfileString(szKey, szBuf, "", szVal, 0x50, szControlIni);
    szKey[lstrlen(szKey) - 1] = 0;  /* "Patterns" -> "Pattern" */
    WriteProfileString(szDesktop, szKey, szVal);

    szBuf[0] = '0';
    szBuf[1] = 0;
    if (IsDlgButtonChecked(hDlg, IDC_TILE)) szBuf[0] = '1';
    LoadString(hInstMain, IDS_TILE, szKey, 0xf);
    /* byte arithmetic as in the original: changed unless old + '0' is the new digit */
    BOOL fTileChanged = (int)(signed char)LOBYTE(GetProfileInt(szDesktop, szKey, 1)) - (int)(signed char)szBuf[0] != -'0';
    WriteProfileString(szDesktop, szKey, szBuf);

    SendMessage(GetDlgItem(hDlg, IDC_WALLPAPER), WM_GETTEXT, sizeof szBuf, (LPARAM)szBuf);
    lstrcpy(of.szPathName, szNone);
    if (lstrcmp(szBuf, szNone)) {
        if (OpenFileFromWinDir(szBuf, &of, OF_EXIST) == HFILE_ERROR) {
            MyMessageBox(hDlg, IDS_NOWALLPAPER, 1, MB_ICONINFORMATION);
            return FALSE;
        }
        OemToAnsi(of.szPathName, of.szPathName);
    }
    LoadString(hInstMain, IDS_WALLPAPER, szKey, 10);
    GetProfileString(szDesktop, szKey, szNone, szVal, sizeof szVal);
    if (fTileChanged || lstrcmpi(szBuf, szVal)) {
        if (!SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, of.szPathName, 0)) {
            MyMessageBox(hDlg, 0, 1, MB_ICONINFORMATION);   /* text 0: the out-of-memory box */
            return FALSE;
        }
    }
    WriteProfileString(szDesktop, szKey, szBuf);    /* as typed, not the full name */
    BroadcastWinIniChange(6);
    return TRUE;
}

/* ------------------------------------------------------------------ seg18:0E3E */
static void InitDesktopLists(HWND hDlg)
{
    char szKey[0x14], szWall[0x9e], szBuf[0x9e], szNone[0x10];
    int val = 0, i, last, found;

    SystemParametersInfo(SPI_GETGRIDGRANULARITY, 0, &val, 0);
    SetDlgItemInt(hDlg, IDC_GRANULARITY, val, FALSE);

    /* the patterns: the CONTROL.INI [Patterns] keys that have a value. (MAIN.CPL read the key list
     * into 0x200 bytes and its code to grow the buffer never ran, so 3.1 lists only the keys that
     * fit; every key is listed here.) */
    HWND hCtl = GetDlgItem(hDlg, IDC_PATNAME);
    LoadString(hInstMain, IDS_PATTERNS, szKey, 10);
    int cb = 0x200;
    char *keys = calloc(1, cb);
    while (keys && GetPrivateProfileString(szKey, NULL, "", keys, cb, szControlIni) >= cb - 2) {
        char *more = realloc(keys, cb + 0x200);
        if (!more) break;
        keys = more;
        cb += 0x200;
    }
    for (char *p = keys; p && *p; p += lstrlen(p) + 1)
        if (GetPrivateProfileString(szKey, p, "", szBuf, 2, szControlIni) != 0)
            SendMessage(hCtl, CB_ADDSTRING, 0, (LPARAM)p);
    free(keys);

    szKey[lstrlen(szKey) - 1] = 0;   /* "Pattern" */
    LoadString(hInstMain, IDS_NONE, szNone, sizeof szNone);
    GetProfileString(szDesktop, szKey, szNone, szBuf, sizeof szBuf);
    if (!szBuf[0]) lstrcpy(szBuf, szNone);
    szKey[lstrlen(szKey) + 1] = 0;
    szKey[lstrlen(szKey)] = 's';     /* "Patterns" again */
    LPSTR name = FindIniKeyByValue(szControlIni, szKey, szBuf);
    if (name) {
        SendMessage(hCtl, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)name);
        free(name);
    } else {
        LoadString(hInstMain, IDS_UNLISTED, szKey, 10);   /* "Unlisted " (cut to 9) */
        SendMessage(hCtl, WM_SETTEXT, 0, (LPARAM)szKey);
    }

    /* the wallpapers: *.BMP in the system and the Windows directory, each name once */
    LoadString(hInstMain, IDS_WALLPAPER, szKey, 10);
    GetProfileString(szDesktop, szKey, szNone, szWall, sizeof szWall);
    if (!szWall[0]) lstrcpy(szWall, szNone);
    hCtl = GetDlgItem(hDlg, IDC_WALLPAPER);
    LoadString(hInstMain, IDS_BMP, szKey, 10);
    lstrcpy(szBuf, szSysDir);
    lstrcat(szBuf, szKey);
    SendMessage(hCtl, CB_DIR, DDL_READWRITE, (LPARAM)szBuf);
    lstrcpy(szBuf, szWinDir);
    lstrcat(szBuf, szKey);
    SendMessage(hCtl, CB_DIR, DDL_READWRITE, (LPARAM)szBuf);
    SendMessage(hCtl, CB_INSERTSTRING, 0, (LPARAM)szNone);
    last = (int)SendMessage(hCtl, CB_GETCOUNT, 0, 0) - 1;
    for (i = 0; i < last; i++) {
        SendMessage(hCtl, CB_GETLBTEXT, i, (LPARAM)szBuf);
        found = (int)SendMessage(hCtl, CB_FINDSTRING, i, (LPARAM)szBuf);   /* after i, wrapping */
        if (found > i) {
            SendMessage(hCtl, CB_DELETESTRING, found, 0);
            last--;
        }
    }
    SendMessage(hCtl, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)szWall);
    SendMessage(hCtl, WM_SETTEXT, 0, (LPARAM)szWall);

    LoadString(hInstMain, IDS_TILE, szKey, 0xf);
    CheckRadioButton(hDlg, IDC_CENTER, IDC_TILE, GetProfileInt(szDesktop, szKey, 1) ? IDC_TILE : IDC_CENTER);

    SystemParametersInfo(SPI_GETFASTTASKSWITCH, 0, &val, 0);
    CheckDlgButton(hDlg, IDC_FASTSWITCH, val);
}

/* ------------------------------------------------------------------ seg18:11FD */
static void InitCursorBlink(HWND hDlg)
{
    char szKey[0x10], szMsg[0x86];
    HWND hScroll = GetDlgItem(hDlg, IDC_BLINKRATE);
    LoadString(hInstMain, IDS_BLINKRATE, szKey, sizeof szKey);
    nBlinkPos = 1400 - GetProfileInt("windows", szKey, 700);
    SetScrollRange(hScroll, SB_CTL, 200, 1200, FALSE);
    SetScrollPos(hScroll, SB_CTL, nBlinkPos, TRUE);
    ClientRectOf(hDlg, IDC_BLINKSAMPLE, &rcBlink);
    if (!SetTimer(hDlg, TIMER_BLINK, 1400 - nBlinkPos, NULL)) {
        LoadString(hInstMain, IDS_BLINKERR, szMsg, 0x85);
        MessageBox(hDlg, szMsg, szCaption, MB_ICONINFORMATION);
    }
    SetCaretBlinkTime(1400 - nBlinkPos);
    hbrBlinkBk = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    fBlinkOn = FALSE;
}

/* ------------------------------------------------------------------ seg18:12F8
 * the command line of a saver: Idle-Wild modules (.IW) run through SCRNSAVE.SCR; " /s" tests it,
 * " /c" sets it up */
static void BuildSaverCommand(LPSTR dst, LPCSTR path, int id)
{
    const char *dot = strchr(path, '.');   /* the first dot */
    if (dot && !lstrcmpi(dot, ".IW")) {
        LoadString(hInstMain, IDS_SCRNSAVE, dst, 0x9e);   /* "SCRNSAVE.SCR " */
        lstrcat(dst, path);
    } else {
        lstrcpy(dst, path);
    }
    if (id == IDC_TEST) lstrcat(dst, " /s");
    else if (id == IDC_SETUP) lstrcat(dst, " /c");
}

/* ------------------------------------------------------------------ seg18:1387
 * the number in edit `id` must lie between a and b (unsigned, in either order); else the message,
 * the field selected and FALSE */
static BOOL ValidateEditInt(HWND hDlg, int id, UINT a, UINT b, int idsError)
{
    BOOL fOK;
    if (b < a) { UINT t = b; b = a; a = t; }
    UINT val = GetDlgItemInt(hDlg, id, &fOK, FALSE);
    if (fOK && a <= val && val <= b) return TRUE;
    MyMessageBox(hDlg, idsError, 1, MB_ICONINFORMATION, a, b);
    HWND hCtl = GetDlgItem(hDlg, id);
    SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)hCtl, 1);
    SendMessage(hCtl, EM_SETSEL, 0, MAKELPARAM(0, 0x7fff));
    return FALSE;
}

/* ------------------------------------------------------------------ seg18:1D64
 * a list entry for a saver; its item data is the saver's file name (a LocalAlloc'd copy in 3.1, a
 * heap string here) */
static int AddSaverItem(HWND hCombo, LPCSTR display, LPCSTR data)
{
    char *p = strdup(data);
    if (!p) return -1;
    int idx = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)display);
    SendMessage(hCombo, CB_SETITEMDATA, idx, (LPARAM)p);
    return idx;
}

/* ------------------------------------------------------------------ seg18:1DD5
 * the name of a screen saver: the module description in its NE header ("SCRNSAVE : Starfield
 * Simulation" -> "Starfield Simulation"); with fAnyDesc any description will do. The file is read
 * as data, not run. */
static BOOL GetSaverDescription(LPSTR out, LPCSTR file, BOOL fAnyDesc)
{
    OFSTRUCT of;
    BYTE mz[0x40], ne[0x40];
    char desc[0x78];
    BYTE cb = 0;
    BOOL result = FALSE;
    *out = 0;
    HFILE hf = OpenFile(file, &of, OF_READ);
    if (hf <= -1) return FALSE;
    memset(mz, 0, sizeof mz);
    memset(ne, 0, sizeof ne);
    _lread(hf, mz, sizeof mz);
    LONG pos = 0;
    LONG lfanew = (LONG)(mz[0x3c] | mz[0x3d] << 8 | mz[0x3e] << 16 | (DWORD)mz[0x3f] << 24);
    if (mz[0] == 'M' && mz[1] == 'Z' && lfanew) pos = lfanew;
    _llseek(hf, pos, 0);
    _lread(hf, ne, sizeof ne);
    if (ne[0] == 'N' && ne[1] == 'E') {
        /* the non-resident name table: its first entry is the module description */
        _llseek(hf, (LONG)(ne[0x2c] | ne[0x2d] << 8 | ne[0x2e] << 16 | (DWORD)ne[0x2f] << 24), 0);
        _lread(hf, &cb, 1);
        /* (3.1 compared the length as a signed byte, so 0x80 and more read past the buffer) */
        if (cb > 0x77) cb = 0x77;
        memset(desc, 0, sizeof desc);
        _lread(hf, desc, cb);
        desc[cb] = 0;
        if (desc[0]) {
            if (StrNCmpPrefix(desc, "SCRNSAVE", 8) == 0) {
                int idx = StrIndex(desc + 8, ':');
                OemToAnsi(desc + 8 + idx + 1, out);
                idx = StrIndex(out, ':');
                /* a second ':' ends it (with none MAIN.CPL wrote a 0 before the buffer) */
                if (idx > 0) out[idx] = 0;
                TrimSpaces(out);
                result = TRUE;
            } else if (fAnyDesc) {
                OemToAnsi(desc, out);
                result = TRUE;
            }
        }
    }
    _lclose(hf);
    return result;
}

/* ------------------------------------------------------------------ seg18:1F51
 * the savers matching `pattern` in `dir` (DOS find first / next, attributes 0x37) */
static void ScanSaverDir(HWND hCombo, LPSTR dir, LPSTR pattern, BOOL fAnyDesc)
{
    char szOem[0x78], szPath[0x80];
    W16FINDDATA fd;
    AnsiUpper(dir);
    AnsiUpper(pattern);
    lstrcpy(szPath, dir);
    AddBackslash(szPath);
    int base = lstrlen(szPath);
    snprintf(szPath + base, sizeof szPath - base, "%s", pattern);
    AnsiToOem(szPath, szOem);
    for (int more = w16_find_first(szOem, 0x37, &fd) == 0; more; more = w16_find_next(&fd) == 0) {
        if ((size_t)base + strlen(fd.name) >= sizeof szPath) continue;
        OemToAnsi(fd.name, szPath + base);
        if (fd.name[0] == '.') continue;
        if (GetSaverDescription(szOem, szPath, fAnyDesc)) AddSaverItem(hCombo, szOem, szPath);
    }
}

/* ------------------------------------------------------------------ seg18:1B5D
 * the savers (*.SCR in the Windows and system directories, Idle-Wild *.IW beside IWLIB.DLL) and
 * "(None)"; the one SYSTEM.INI [boot] SCRNSAVE.EXE names is selected */
static void FillSaverCombo(HWND hCombo)
{
    char szNone[0x10], szWinDir2[0x78], szSysDir2[0x78], szPattern[10], szDir[0x78];
    OFSTRUCT of;
    LoadString(hInstMain, IDS_NONE, szNone, sizeof szNone);
    GetWindowsDirectory(szWinDir2, sizeof szWinDir2);
    GetSystemDirectory(szSysDir2, sizeof szSysDir2);
    LoadString(hInstMain, IDS_SCR, szPattern, sizeof szPattern);
    ScanSaverDir(hCombo, szWinDir2, szPattern, FALSE);
    ScanSaverDir(hCombo, szSysDir2, szPattern, FALSE);
    LoadString(hInstMain, IDS_IW, szPattern, sizeof szPattern);
    if (OpenFile("IWLIB.DLL", &of, OF_EXIST) != HFILE_ERROR) {
        lstrcpy(szDir, of.szPathName);
        AnsiUpper(szDir);
        char *p = strrchr(szDir, '\\');
        if (!p) {
            p = strrchr(szDir, ':');
            p = p ? p + 1 : szDir;
        }
        *p = 0;
        ScanSaverDir(hCombo, szDir, szPattern, TRUE);
    }

    int sel = AddSaverItem(hCombo, szNone, szNone);
    GetPrivateProfileString("boot", "SCRNSAVE.EXE", szNone, szDir, sizeof szDir, "SYSTEM.INI");
    int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        LPCSTR data = (LPCSTR)SendMessage(hCombo, CB_GETITEMDATA, i, 0);
        if (!data || data == (LPCSTR)(LRESULT)CB_ERR) continue;
        if (StrStrI(szDir, data)) {
            sel = i;
            break;
        }
    }
    SendMessage(hCombo, CB_SETCURSEL, sel, 0);
}

/* ------------------------------------------------------------------ seg18:202A */
static void FreeSaverItemData(HWND hCombo)
{
    UINT count = (UINT)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (UINT i = 0; i < count; i++) {
        LPSTR data = (LPSTR)SendMessage(hCombo, CB_GETITEMDATA, i, 0);
        if (data && data != (LPSTR)(LRESULT)CB_ERR) free(data);
        SendMessage(hCombo, CB_SETITEMDATA, i, 0);
    }
}

/* the selected saver's file name, or NULL */
static LPCSTR SaverData(int sel)
{
    LPCSTR data = (LPCSTR)SendMessage(hwndSaver, CB_GETITEMDATA, sel, 0);
    return data == (LPCSTR)(LRESULT)CB_ERR ? NULL : data;
}

/* ------------------------------------------------------------------ seg18:1419 */
BOOL DesktopDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szBuf[0x9e], szKey[0x28];
    int val = 0, oldVal, sel, id;
    BOOL fOK;
    ARROWSTEP *pd;

    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        DragAcceptFiles(hDlg, TRUE);
        if (!GetSystemMetrics(SM_MOUSEPRESENT)) EnableWindow(GetDlgItem(hDlg, IDC_EDITPAT), FALSE);
        SendDlgItemMessage(hDlg, IDC_DELAY, EM_LIMITTEXT, 2, 0);
        SendDlgItemMessage(hDlg, IDC_GRANULARITY, EM_LIMITTEXT, 2, 0);
        SendDlgItemMessage(hDlg, IDC_BORDER, EM_LIMITTEXT, 2, 0);
        SystemParametersInfo(SPI_GETBORDER, 0, &val, 0);
        SetDlgItemInt(hDlg, IDC_BORDER, val, TRUE);
        InitDesktopLists(hDlg);
        asSpacing.min = GetSystemMetrics(SM_CXICON);
        SystemParametersInfo(SPI_ICONHORIZONTALSPACING, 0, &val, 0);
        SetDlgItemInt(hDlg, IDC_SPACING, val, TRUE);
        SystemParametersInfo(SPI_GETICONTITLEWRAP, 0, &val, 0);
        CheckDlgButton(hDlg, IDC_WRAP, val);
        hwndSaver = GetDlgItem(hDlg, IDC_SAVER);
        FillSaverCombo(hwndSaver);
        SystemParametersInfo(SPI_GETSCREENSAVETIMEOUT, 0, &val, 0);
        val = (SHORT)(val + 59) / 60;
        SetDlgItemInt(hDlg, IDC_DELAY, val, TRUE);
        if (SendMessage(hwndSaver, CB_GETCURSEL, 0, 0) == 0) {   /* "(None)" */
            EnableWindow(GetDlgItem(hDlg, IDC_SETUP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_TEST), FALSE);
        }
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_BORDER + 1));
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_GRANULARITY + 1));
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_SPACING + 1));
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_DELAY + 1));
        InitCursorBlink(hDlg);
        HourGlass(FALSE);
        return TRUE;

    case WM_DESTROY:
        DragAcceptFiles(hDlg, FALSE);
        return TRUE;

    case WM_DROPFILES:
        DragQueryFile((HANDLE)wParam, 0, szBuf, sizeof szBuf);
        SetDlgItemText(hDlg, IDC_WALLPAPER, szBuf);
        DragFinish((HANDLE)wParam);
        return TRUE;

    case WM_HSCROLL:
        /* the blink rate; applied to the carets at once, kept by OK only */
        switch (wParam) {
        case SB_THUMBPOSITION: val = (int)LOWORD(lParam); break;
        case SB_THUMBTRACK:
        case SB_ENDSCROLL: return TRUE;
        default: val = StepField((int)wParam, nBlinkPos, &asBlink); break;
        }
        nBlinkPos = val;
        SetScrollPos(W16_CMD_HWND(HIWORD(lParam)), SB_CTL, val, TRUE);
        KillTimer(hDlg, TIMER_BLINK);
        SetTimer(hDlg, TIMER_BLINK, 1400 - nBlinkPos, NULL);
        SetCaretBlinkTime(1400 - nBlinkPos);
        return TRUE;

    case WM_TIMER:
        if (wParam == TIMER_BLINK) {
            HDC hdc = GetDC(hDlg);
            if (hbrBlinkBk) FillRect(hdc, &rcBlink, hbrBlinkBk);
            fBlinkOn = !fBlinkOn;
            if (fBlinkOn) InvertRect(hdc, &rcBlink);
            ReleaseDC(hDlg, hdc);
            ValidateRect(hDlg, &rcBlink);
        }
        return TRUE;

    case WM_VSCROLL:
        /* from a cpArrow: its ID less one is the field's */
        id = (int)LOWORD(lParam) - 1;
        if (wParam == SB_ENDSCROLL) {
            SendDlgItemMessage(hDlg, id, EM_SETSEL, 0, MAKELPARAM(0, 0x7fff));
            return TRUE;
        }
        switch (id) {
        case IDC_GRANULARITY: pd = &asGranularity; break;
        case IDC_SPACING: pd = &asSpacing; break;
        case IDC_BORDER: pd = &asBorder; break;
        case IDC_DELAY: pd = &asDelay; break;
        default: return FALSE;
        }
        oldVal = val = (SHORT)GetDlgItemInt(hDlg, id, &fOK, FALSE);
        if (!fOK && (pd->min > val || pd->max < val)) val = pd->v4;
        else val = StepField((int)wParam, val, pd);
        if (oldVal != val || !fOK) SetDlgItemInt(hDlg, id, val, FALSE);
        SetFocus(GetDlgItem(hDlg, id));
        return TRUE;

    case WM_COMMAND:
        switch (wParam) {
        case IDC_EDITPAT:
            DoDialogBoxParam(IDD_EDITPATTERN, hDlg, EditPatternDlgProc, 0x1f62, 0);
            return TRUE;

        case IDC_CENTER:
        case IDC_TILE:
            CheckRadioButton(hDlg, IDC_CENTER, IDC_TILE, (int)wParam);
            return TRUE;

        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;

        case IDC_953:
            CheckDlgButton(hDlg, IDC_953, !IsDlgButtonChecked(hDlg, IDC_953));
            return TRUE;

        case IDC_SAVER:
            if (HIWORD(lParam) != CBN_SELCHANGE) return TRUE;
            sel = (int)SendMessage(hwndSaver, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hDlg, IDC_TEST), sel);
            EnableWindow(GetDlgItem(hDlg, IDC_SETUP), sel);
            if (SendDlgItemMessage(hDlg, IDC_SAVER, CB_GETCURSEL, 0, 0) != 0) {
                /* a saver with a delay of 0: two minutes */
                val = (int)GetDlgItemInt(hDlg, IDC_DELAY, &fOK, FALSE);
                if (val == 0 && fOK) SetDlgItemInt(hDlg, IDC_DELAY, 2, FALSE);
            }
            return TRUE;

        case IDC_TEST:
        case IDC_SETUP: {
            sel = (int)SendMessage(hwndSaver, CB_GETCURSEL, 0, 0);
            LPCSTR data = SaverData(sel);
            if (!data || sel == 0) return TRUE;
            BuildSaverCommand(szBuf, data, (int)wParam);
            /* 3.1: WinExec(szBuf, SW_SHOW). Screen savers are 16-bit programs and are not run;
             * TODO: start the native screen saver this names once arch311 has them. */
            return TRUE;
        }

        case IDOK: {
            sel = (int)SendMessage(hwndSaver, CB_GETCURSEL, 0, 0);
            if (sel != 0 && !ValidateEditInt(hDlg, IDC_DELAY, 1, 99, IDS_DELAY)) return TRUE;
            if (!ValidateEditInt(hDlg, IDC_BORDER, 1, 50, IDS_BORDER)) return TRUE;
            if (!ValidateEditInt(hDlg, IDC_GRANULARITY, 0, 49, IDS_GRANULARITY)) return TRUE;
            if (!ValidateEditInt(hDlg, IDC_SPACING, asSpacing.max, asSpacing.min, IDS_SPACING)) return TRUE;
            HourGlass(TRUE);
            if (!ApplyDesktopSettings(hDlg)) return TRUE;   /* (the hourglass stays, as in 3.1) */
            val = (int)GetDlgItemInt(hDlg, IDC_BORDER, &fOK, FALSE);
            if (fOK) SystemParametersInfo(SPI_SETBORDER, val, NULL, SPIF_UPDATEINIFILE);
            IntToStr(1400 - nBlinkPos, szBuf);
            LoadString(hInstMain, IDS_BLINKRATE, szKey, sizeof szKey);
            WriteProfileString("windows", szKey, szBuf);
            LPCSTR data = SaverData(sel);
            if (data) {
                BuildSaverCommand(szBuf, data, 0);
                WritePrivateProfileString("boot", "SCRNSAVE.EXE", szBuf, "SYSTEM.INI");
                SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, sel != 0, NULL, SPIF_UPDATEINIFILE);
            }
            val = (int)GetDlgItemInt(hDlg, IDC_DELAY, &fOK, FALSE);
            val = (SHORT)(60 * val);
            SystemParametersInfo(SPI_SETSCREENSAVETIMEOUT, (UINT)val, NULL, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
            SystemParametersInfo(SPI_SETDESKPATTERN, (UINT)-1, NULL, 0);   /* Pattern= again */
            HourGlass(FALSE);
        }
            /* fall through - OK ends as Cancel does */
        case IDCANCEL:
            if (hbrBlinkBk) DeleteObject(hbrBlinkBk);
            hbrBlinkBk = NULL;
            KillTimer(hDlg, TIMER_BLINK);
            FreeSaverItemData(hwndSaver);
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return TRUE;

    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}
