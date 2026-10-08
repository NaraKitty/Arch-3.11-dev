/* Color applet: port of MAIN.CPL seg6 (dialog 100 "Color" and the Save Scheme dialog 27) and seg7
 * (dialog 26, the modeless Custom Color Selector), with the helpers it uses from seg1 (191B frame),
 * seg4 (00E5 grid subclass, 0168/019E string helpers) and seg23 (07E1 INI section into a combo box).
 * seg3:0756 runs it: CreateDialog 100, then the private message loop seg3:06AE.
 *
 * The colours being edited live in g_rgbElem (element order = scheme order = WIN.INI order); nothing
 * is applied before OK, which calls SetSysColors and writes WIN.INI [colors] and CONTROL.INI
 * [current] (and [Custom Colors] once a custom colour was added). "Windows Default" and the 48 basic
 * colours come from the display driver's OEMBIN resources #1 and #2 (the user's ripped VGA.DRV),
 * read at run time. Original bugs that only corrupt memory are not reproduced (see the notes).
 *
 * Verified against real 3.11 on VGA (tests/color*.w16 in 16 colours, tools/regress.sh): the dialog
 * as opened, all 22 schemes in the sample, the palette (element combo, basic grid focus and
 * selection), the Custom Color Selector (typed hue/sat/lum, Add Color, Close), Save Scheme, Remove
 * Scheme with its confirmation, OK (the repaint in the new colours, WIN.INI [colors], CONTROL.INI
 * [current] and [Custom Colors]). UNTESTED (the rig types keys only): every mouse path - clicks in
 * the sample, on colour boxes (also Ctrl/Shift with the selector open), the crosshair and the
 * luminosity arrow, the cpArrow spin buttons; the selector's own arrow keys; Color|Solid (Alt+O);
 * Help; Save Scheme over an existing name; true-colour drawing. */
#include "maincpl.h"
#include <stdlib.h>
#include <string.h>

int AdjustArrowWidth(HWND h);   /* arrow.c, seg2:0000 */

/* ------------------------------------------------------------------ globals (MAIN.CPL ds) */
static HWND g_hDlg;                 /* [0x46] Color dialog; 0 ends the message loop */
static HWND g_hCust;                /* [0x48] Custom Color Selector, 0 = not open */
static HBITMAP g_hbmUp, g_hbmDn;    /* [0x9e], [0xa0] OBM_UPARROW, OBM_DNARROW (freed at CPL exit) */
static HRGN g_hrgnDesktop;          /* [0x482] sample minus the two sample windows */
static int g_iBtnCycle;             /* [0x490] 0,1,2,7: sample button hits (kept for the module's life) */
static int g_iHiCycle;              /* [0x492] 0/1: sample popup menu hits */
static BYTE g_bFlags;               /* [0xe78] bit 0: a custom colour was added */
static char g_szIni[260];           /* [0xe7c] "<windows dir>\control.ini" */
static HWND g_hwndArrow[6];         /* [0xe7e] cpArrow 720..725 */
static HWND g_hwndElemCombo;        /* [0xe96] combo 716 */
static int g_cyBox, g_cxBox;        /* [0xf3e], [0x12cc] colour box pitch */
static HDC g_hdcMem;                /* [0x1002] MAIN.CPL's memory DC (seg3:013A) */
static int g_cxVScroll, g_cyVScroll;/* [0x1012], [0x101e] */
static COLORREF g_rgbCust;          /* [0x1048] Custom Color Selector colour */
static COLORREF g_rgbOrig[21];      /* [0x1060] system colours at init; [2] is read */
static char g_szScheme[192];        /* [0x10b4] scheme name / "Name=values" scratch */
static int g_cxSize;                /* [0x1174] SM_CXSIZE */
static int g_cxBorder, g_cyBorder;  /* [0x1178], [0x118a] */
static int g_cyIcon;                /* [0x1fc6] */
static int g_nBasic;                /* [0x11a2] basic colours from the driver (<= 48) */
static WNDPROC g_lpfnOldStatic;     /* [0x11d2] */
static int g_sysTmHeight, g_sysExtLead; /* [0x18ae], [0x11e0] system font (seg3:013A) */
static char g_szMenuSample[40];     /* [0x127c] string 95 "File  Edit" */
static char g_szWindowText[40];     /* [0x12a4] string 96 */
static char g_szInactive[40];       /* [0x13da] string 94 */
static char g_szActive[40];         /* [0x1e00] string 93 */
static char g_szDisabled[40];       /* [0x1f8a] string 97 */
static char g_szHighlighted[40];    /* [0x17b8] string 98 */
static RECT g_rcSample;             /* [0x12d6] control 717, client coordinates */
static RECT g_rcDlgFull;            /* [0x12de] the whole dialog (screen) */
static RECT g_rcSampleScr;          /* [0x171e] */
static COLORREF g_rgbBox[64];       /* [0x1404] 0-47 basic, 48-63 custom */
static RECT g_rcBox[64];            /* [0x1506] */
static BOOL g_fPalette;             /* [0x1706] */
static HWND g_hwndSchemeCombo;      /* [0x1726] combo 715 */
static COLORREF g_rgbElem[21];      /* [0x1764] colours being edited */
static COLORREF g_rgbCur;           /* [0x183e] */
static int g_iElem;                 /* [0x1844] current element */
static int g_iFocusBasic;           /* [0x18ac] */
static int g_cyItem;                /* [0x1936] owner-draw item height (WM_SETFONT) */
static HWND g_hwndBasic;            /* [0x1a24] static 32 */
static HWND g_hwndCustom;           /* [0x1d6c] static 80 */
static int g_iFocusCustom;          /* [0x1da2] */
static int g_iSel;                  /* [0x1da4] */
static BOOL g_fTracking;            /* [0x1fdc] mouse captured (both dialogs) */

/* the sample screen (named by their ds offsets; roles from LayoutSample's comments) */
static RECT rcE6E, rcE8C, rcF36, rcFE2, rcFFA, rc1016, rc1022, rc102C, rc1058, rc1182, rc118E,
    rc119A, rc11A6, rc11C2, rc11CA, rc11D8, rc1274, rc12E8, rc139E, rc13B4, rc170E, rc1716,
    rc1732, rc173A, rc1746, rc1752, rc175C, rc1938, rc1940, rc1958, rc19E8, rc1A26, rc1D5C,
    rc1D64, rc1D70, rc1D9A, rc1DB4, rc1DD2, rc1DE0, rc1DEA, rc1DF8, rc1FB2, rc1FBC, rc1FCC;
static POINT ptActive, ptInactive, ptMenu;  /* [0x1196], [0x170a], [0x1fe0] */

/* the Custom Color Selector */
static HWND g_hwndLumBar;           /* [0x18a2] control 702 */
static RECT g_rcSpec, g_rcSpecScr;  /* [0x1728], [0x189a] spectrum (client, screen) */
static RECT g_rcLumBar, g_rcLumScr; /* [0x1d50], [0x1daa] */
static RECT g_rcLumArrow;           /* [0x19e0] */
static RECT rc103E, rc1396, rc11BA; /* "Color" half, "Solid" half, whole sample */
static int g_cyLumBar;              /* [0x1394] */
static int g_cxSpec, g_cySpec;      /* [0x175a], [0x1df4] */
static HBITMAP g_hbmSpectrum;       /* [0x13b0] */
static int g_H, g_S, g_L;           /* [0x1a20], [0x1db2], [0x1dc0] 0..239, 0..240, 0..240 */
static int g_Hcalc, g_Scalc, g_Lcalc; /* [0x1dbc], [0x1ec2], [0x1dda] RGBtoHLS outputs */
static int g_xCross, g_yCross;      /* [0x1d98], [0x13a6] */
static int g_yLum;                  /* [0x1de8] */

#define IDC_SAVE 711
#define IDC_REMOVE 712
#define IDC_PALETTE 714
#define IDC_SCHEMES 715
#define IDC_ELEMENT 716
#define IDC_SAMPLE 717
#define IDC_DEFINE 719
#define IDC_BASIC 32
#define IDC_CUSTOM 80
#define WM_CUSTADD 0x800        /* "Add Color": lParam = COLORREF */
#define WM_GRIDFOCUS 0x801      /* a grid got (0x801) / lost (0x802) the focus */

/* ds:03CE: element at element-combo position i */
static const int g03CE[21] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 19, 9, 10, 11, 12, 13, 14, 15, 20, 16, 17, 18};
/* ds:03F8: element -> COLOR_ index (also the index array of SetSysColors) */
static const int g03F8[21] = {1, 12, 5, 8, 4, 7, 2, 3, 9, 10, 11, 6, 0, 15, 16, 18, 17, 13, 14, 19, 20};
/* ds:0422: element -> WIN.INI [colors] key */
static const char *const g0422[21] = {
    "Background", "AppWorkspace", "Window", "WindowText", "Menu", "MenuText", "ActiveTitle",
    "InactiveTitle", "TitleText", "ActiveBorder", "InactiveBorder", "WindowFrame", "Scrollbar",
    "ButtonFace", "ButtonShadow", "ButtonText", "GrayText", "Hilight", "HilightText",
    "InactiveTitleText", "ButtonHilight"};

static const char szColorSchemes[] = "color schemes";   /* ds:044C */
static const char szCurrent[] = "current";              /* ds:045A */
static const char szCustomColors[] = "Custom Colors";   /* ds:0462 */
static const char szOEMBIN[] = "OEMBIN";                /* ds:0472 */

static BOOL CALLBACK ColorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static BOOL CALLBACK CustColorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK GridSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void PaintElement(HDC hdc, int elem);
static void CustSetColor(COLORREF rgb);
static void SetHLSEdits(int id);
static void SetRGBEdits(int id);

/* a DWORD of a resource (little endian, any alignment) */
static COLORREF ResDword(const BYTE *p) { return p[0] | (p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }

/* ds:02CE: MAIN.CPL's own 48 basic colours, for a display driver without OEMBIN #2 - read from the
 * user's ripped MAIN.CPL (its automatic data segment), white if it is not there. UNTESTED: VGA.DRV
 * has OEMBIN #2, so the rig never shows these. */
static COLORREF DefaultBasicColor(int i)
{
    unsigned len;
    const BYTE *ds = w16_module_data(hInstMain, 0, &len);
    if (!ds || len < 0x2CE + 48 * 4) return 0x00FFFFFF;
    return ResDword(ds + 0x2CE + 4 * i);
}

/* ------------------------------------------------------------------ seg1:191B
 * a frame of border width out of four PATCOPY strips */
static void DrawFrame(HDC hdc, const RECT *lprc, HBRUSH hbr)
{
    int cx = lprc->right - lprc->left, cy = lprc->bottom - lprc->top;
    HGDIOBJ hOld = SelectObject(hdc, hbr);
    PatBlt(hdc, lprc->left, lprc->top, cx, g_cyBorder, PATCOPY);
    PatBlt(hdc, lprc->left, lprc->bottom - g_cyBorder, cx, g_cyBorder, PATCOPY);
    PatBlt(hdc, lprc->left, lprc->top, g_cxBorder, cy, PATCOPY);
    PatBlt(hdc, lprc->right - g_cxBorder, lprc->top, g_cxBorder, cy, PATCOPY);
    if (hOld) SelectObject(hdc, hOld);
}

/* ------------------------------------------------------------------ seg6:0000
 * a Yes/No question from a MAIN.CPL string with one %s */
static BOOL ConfirmMsg(HWND hwnd, LPCSTR lpszArg, int ids)
{
    char szFmt[256], szMsg[512];
    LoadString(hInstMain, ids, szFmt, sizeof szFmt);
    wsprintf(szMsg, szFmt, lpszArg);
    return MessageBox(hwnd, szMsg, szCaption, MB_YESNO | MB_ICONEXCLAMATION) == IDYES;
}

/* ------------------------------------------------------------------ seg6:005B
 * hex digits -> DWORD; stops at the first other character. Upper-cases the buffer as it goes. */
static COLORREF ParseHexColor(char *p)
{
    DWORD val = 0;
    while (*p) {
        BYTE digit = 0xF0;
        if ((BYTE)*p >= '0') {
            if ((BYTE)*p <= '9') digit = (BYTE)(*p - '0');
            else {
                *p &= 0xDF;
                if ((BYTE)*p >= 'A' && (BYTE)*p <= 'F') digit = (BYTE)(*p - 0x37);
            }
        }
        if (digit & 0xF0) break;
        val = (val << 4) | digit;
        p++;
    }
    return val;
}

/* ------------------------------------------------------------------ seg6:00CA
 * the 1-pixel frame just outside a colour box: black when selected (flags bit 0), else the
 * window colour */
static void FrameColorBox(HDC hdc, int idx, WORD flags)
{
    RECT rc;
    CopyRect(&rc, &g_rcBox[idx]);
    rc.left--; rc.top--; rc.right++; rc.bottom++;
    HBRUSH hbr = CreateSolidBrush((flags & 1) ? RGB(0, 0, 0) : GetSysColor(COLOR_WINDOW));
    if (hbr) {
        FrameRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }
}

/* ------------------------------------------------------------------ seg6:012A */
static void SetSelectedBox(int idx)
{
    HDC hdc = GetDC(g_hDlg);
    FrameColorBox(hdc, g_iSel, 0);
    FrameColorBox(hdc, idx, 1);
    ReleaseDC(g_hDlg, hdc);
    g_rgbCur = g_rgbBox[idx];
}

/* ------------------------------------------------------------------ seg6:0174 */
static void SetFocusBox(int idx)
{
    RECT rc;
    HDC hdc = GetDC(g_hDlg);
    int *pFocus = idx >= 48 ? &g_iFocusCustom : &g_iFocusBasic;
    CopyRect(&rc, &g_rcBox[*pFocus]);
    InflateRect(&rc, 3, 3);
    DrawFocusRect(hdc, &rc);
    *pFocus = idx;
    CopyRect(&rc, &g_rcBox[idx]);
    InflateRect(&rc, 3, 3);
    DrawFocusRect(hdc, &rc);
    ReleaseDC(g_hDlg, hdc);
}

/* ------------------------------------------------------------------ seg6:0209 */
static void SelectColorIfInPalette(COLORREF rgb)
{
    int i;
    for (i = 0; i < 64; i++)
        if (g_rgbBox[i] == rgb) break;
    if (i < 64) {
        SetSelectedBox(i);
        g_iSel = i;
    }
}

/* ------------------------------------------------------------------ seg6:024B */
static void CloseComboDropdowns(void)
{
    HWND h = GetFocus();
    if (h == g_hwndElemCombo || h == g_hwndSchemeCombo) SendMessage(h, CB_SHOWDROPDOWN, FALSE, 0);
}

/* ------------------------------------------------------------------ seg6:026C
 * does "Name=v0,...,v20" hold the colours being edited? Cuts the text at '=' (and upper-cases it). */
static BOOL SchemeMatchesCurrent(HDC hdc, char *p)
{
    if (*p == 0) return FALSE;
    do p++; while (*p && *p != '=');   /* (the original scans on to the '=' unconditionally) */
    if (*p == 0) return FALSE;
    *p++ = 0;
    for (int i = 0; i < 21; i++) {
        COLORREF c = ParseHexColor(p);
        switch (i) {                    /* jump table seg6:02BD */
        case 2: case 3: case 4: case 5: case 8: case 11: case 13:
        case 15: case 16: case 17: case 18: case 19:
            c = GetNearestColor(hdc, c);
        }
        if (c != g_rgbElem[i]) return FALSE;
        while (*p) { if (*p++ == ',') break; }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ seg6:0400
 * writes the selected combo item back with all 21 current values (INI and combo) */
static void RewriteCurrentScheme(void)
{
    char buf[256];
    char *p;
    int n = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
    if (n != 0) {
        SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, n, (LPARAM)g_szScheme);
        p = g_szScheme;
        do p++; while (*p && *p != '=');
    } else {
        /* the original writes through an uninitialised pointer here (index 0 is normally the
         * driver's "Windows Default", which has all 21 values); cut at the name instead */
        p = strchr(g_szScheme, '=');
        if (!p) p = g_szScheme + strlen(g_szScheme);
    }
    *p = 0;
    lstrcpy(buf, g_szScheme);
    lstrcat(buf, "=");
    int iFound = (int)SendMessage(g_hwndSchemeCombo, CB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
    if (iFound != -1) SendMessage(g_hwndSchemeCombo, CB_DELETESTRING, iFound, 0);
    p = buf;
    for (n = 0; n < 21; n++) {
        wsprintf(p, "%lX,", g_rgbElem[n]);
        p += lstrlen(p);
    }
    buf[lstrlen(buf) - 1] = 0;
    WritePrivateProfileString(szColorSchemes, g_szScheme, buf, g_szIni);
    lstrcat(g_szScheme, "=");
    lstrcat(g_szScheme, buf);
    n = (int)SendMessage(g_hwndSchemeCombo, iFound == -1 ? CB_ADDSTRING : CB_INSERTSTRING, iFound, (LPARAM)g_szScheme);
    SendMessage(g_hwndSchemeCombo, CB_SETCURSEL, n, 0);
}

/* ------------------------------------------------------------------ seg6:0339
 * "Name=v0,v1,..." -> g_rgbElem, then the sample repaints */
static BOOL ApplySchemeString(char *p)
{
    int i;
    while (*p) { if (*p++ == '=') break; }
    for (i = 0; *p != 0 && i < 21; i++) {
        g_rgbElem[i] = ParseHexColor(p);
        while (*p) { if (*p++ == ',') break; }
    }
    if (i != 21) {
        /* a short (3.0) scheme: the original loops j from i to 20 but fills only element i */
        for (int j = i; j < 21; j++) g_rgbElem[i] = GetSysColor(g03F8[i]);
        RewriteCurrentScheme();
    }
    if (g_fPalette) {
        g_rgbCur = g_rgbElem[g_iElem];
        SelectColorIfInPalette(g_rgbCur);
    }
    InvalidateRect(g_hDlg, &g_rcSample, FALSE);
    return TRUE;
}

/* ------------------------------------------------------------------ seg4:0168 */
static char *FindSubstrOrEnd(char *s, const char *sub)
{
    char *q = strstr(s, sub);
    return q ? q : s + lstrlen(s);
}

/* ------------------------------------------------------------------ seg4:019E: spaces only */
static void TrimBlanks(char *s)
{
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    p = s + lstrlen(s);
    if (p != s) {
        p--;
        while (p >= s && *p == ' ') p--;
        *++p = 0;
    }
}

/* ------------------------------------------------------------------ seg6:0563, dialog 27 */
static BOOL CALLBACK SaveSchemeDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szBuf[256], szTmp[256];
    int idErr;
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        SetDlgItemText(hDlg, IDC_SAVE, g_szScheme);
        SendDlgItemMessage(hDlg, IDC_SAVE, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        SendDlgItemMessage(hDlg, IDC_SAVE, EM_LIMITTEXT, 32, 0);
        EnableWindow(GetDlgItem(hDlg, IDOK), g_szScheme[0] != 0);
        HourGlass(FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            GetDlgItemText(hDlg, IDC_SAVE, szBuf, 0xBE);
            TrimBlanks(szBuf);
            if (*FindSubstrOrEnd(szBuf, "=") != 0) idErr = 0x21;
            else if (*FindSubstrOrEnd(szBuf, "[") != 0 || *FindSubstrOrEnd(szBuf, "]") != 0) idErr = 0x22;
            else if (szBuf[0] == 0) idErr = 0x23;
            else {
                LoadString(hInstMain, 99, szTmp, 0xBE);
                idErr = lstrcmpi(szBuf, szTmp) == 0 ? 0x20 : 0;
            }
            if (idErr) {
                /* strings 104 Windows Default, 105 '=', 106 brackets, 107 blank */
                if (!LoadString(hInstMain, idErr + 0x48, szBuf, 0xBE)) OutOfMemory(hDlg);
                else MessageBox(hDlg, szBuf, szCaption, MB_ICONINFORMATION);
                break;
            }
            lstrcpy(g_szScheme, szBuf);
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, (int)wParam);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_SAVE:
            if (HIWORD(lParam) == EN_CHANGE)
                EnableWindow(GetDlgItem(hDlg, IDOK), GetDlgItemText(hDlg, IDC_SAVE, szBuf, 2));
            break;
        }
        break;
    }
    if (msg == wHelpMessage) {
        CPHelp(hDlg);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg6:0763
 * owner-draw text length: the scheme name (cuts the item text at '=') */
static int SchemeNameLen(char *lpsz)
{
    char *p = lpsz;
    while (*p != '=' && *p != 0) p++;
    *p = 0;
    return (int)(p - lpsz);
}

/* ------------------------------------------------------------------ seg6:0796
 * WM_DRAWITEM for controls 0x20..0x5F: dormant (USER 3.1 statics never send WM_DRAWITEM) */
static BOOL DrawColorBoxItem(LPDRAWITEMSTRUCT lpdis)
{
    RECT rc;
    GetClientRect(lpdis->hwndItem, &rc);
    HBRUSH hbr = CreateSolidBrush(g_rgbBox[(lpdis->CtlID - 0x20) & 63]);
    if (hbr) {
        HGDIOBJ hOld = SelectObject(lpdis->hDC, hbr);
        Rectangle(lpdis->hDC, rc.left, rc.top, rc.right, rc.bottom);
        if (hOld) SelectObject(lpdis->hDC, hOld);
        DeleteObject(hbr);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ seg6:0817 (the scheme combo) */
static BOOL DrawComboItem(LPDRAWITEMSTRUCT lpdis, int (*lpfnLen)(char *))
{
    char sz[256];
    COLORREF oldBk = 0, oldText = 0;
    if (!(lpdis->itemAction & 7) || lpdis->itemID == (UINT)-1) return FALSE;
    sz[0] = 0;
    SendMessage(lpdis->hwndItem, CB_GETLBTEXT, lpdis->itemID, (LPARAM)sz);
    int len = lpfnLen(sz);
    if (lpdis->itemAction & (ODA_DRAWENTIRE | ODA_SELECT)) {
        if (lpdis->itemState & ODS_SELECTED) {
            oldBk = SetBkColor(lpdis->hDC, GetSysColor(COLOR_HIGHLIGHT));
            oldText = SetTextColor(lpdis->hDC, GetSysColor(COLOR_HIGHLIGHTTEXT));
        }
        ExtTextOut(lpdis->hDC, lpdis->rcItem.left, lpdis->rcItem.top, ETO_OPAQUE | ETO_CLIPPED, &lpdis->rcItem, sz, len, NULL);
        if (lpdis->itemState & ODS_SELECTED) {
            SetBkColor(lpdis->hDC, oldBk);
            SetTextColor(lpdis->hDC, oldText);
        }
        if (lpdis->itemState & ODS_FOCUS) lpdis->itemAction |= ODA_FOCUS;
    }
    if (lpdis->itemAction & ODA_FOCUS) {
        InflateRect(&lpdis->rcItem, 1, 1);
        DrawFocusRect(lpdis->hDC, &lpdis->rcItem);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ seg6:0931
 * arrows, Home and End in the colour grids; TRUE if the focus box moves */
static BOOL GridKeyNav(WPARAM vk, int *pIdx)
{
    int i = GetWindowWord(GetFocus(), GWW_ID);
    if (i == IDC_BASIC) i = g_iFocusBasic;
    else if (i == IDC_CUSTOM) i = g_iFocusCustom;
    else return FALSE;
    switch (vk) {                       /* jump table seg6:0A05 */
    case VK_END: i = g_iFocusBasic == i ? g_nBasic - 1 : 63; break;
    case VK_HOME: i = g_iFocusBasic == i ? 0 : 48; break;
    case VK_LEFT: if (i % 8 != 0) i--; break;
    case VK_UP:
        if (i >= 56) i -= 8;
        else if (i >= 48) ;
        else if (i >= 8) i -= 8;
        break;
    case VK_RIGHT: i++; if (i % 8 == 0) i--; break;
    case VK_DOWN:
        if (i < 40) i += 8;
        else if (i >= 48 && i < 56) i += 8;
        break;
    }
    if (i >= g_nBasic && i < 48) i = g_iFocusBasic;
    *pIdx = i;
    return g_iFocusBasic != i && g_iFocusCustom != i;
}

/* ------------------------------------------------------------------ seg6:0A45
 * which element of the sample screen is at pt */
static int HitTestSample(POINT pt)
{
    int e;
    if (PtInRect(&rc11D8, pt) || PtInRect(&rc1DEA, pt)) {
        if (PtInRect(&rc13B4, pt)) {
            if (PtInRect(&rc102C, pt)) return 3;
            if (PtInRect(&rc1940, pt)) {
                e = g_iBtnCycle + 13;
                g_iBtnCycle++;
                if (g_iBtnCycle == 3) g_iBtnCycle = 7;
                else if (g_iBtnCycle == 8) g_iBtnCycle = 0;
                return e;
            }
            return 2;
        }
        if (PtInRect(&rc1FCC, pt)) {
            if (PtInRect(&rc1D9A, pt)) return 16;
            e = g_iHiCycle + 17;
            g_iHiCycle = (g_iHiCycle + 1) % 2;
            return e;
        }
        return 1;
    }
    if (PtInRect(&rc1022, pt) || PtInRect(&rc1752, pt)) return PtInRect(&rc1732, pt) ? 5 : 4;
    if (PtInRect(&rcFE2, pt)) return 8;
    if (PtInRect(&rc173A, pt)) return 19;
    if (PtInRect(&rc170E, pt) || PtInRect(&rc1716, pt)) return 6;
    if (PtInRect(&rc1DE0, pt) || PtInRect(&rc118E, pt) || PtInRect(&rc173A, pt)) return 7;
    if (PtInRect(&rc1182, pt) || PtInRect(&rc175C, pt)) return g_iElem;
    if (PtInRect(&rc1058, pt)) return 12;
    if (PtInRect(&rc139E, pt) || PtInRect(&rc1D64, pt) || PtInRect(&rc1DF8, pt) || PtInRect(&rc11CA, pt)) return 9;
    if (PtInRect(&rc1FBC, pt) || PtInRect(&rc1FB2, pt) || PtInRect(&rc11C2, pt) || PtInRect(&rcF36, pt)) return 10;
    if (g_hrgnDesktop && PtInRegion(g_hrgnDesktop, pt.x, pt.y)) return 0;
    return 11;
}

/* ------------------------------------------------------------------ seg6:0D3F */
static void PaintColorBox(HDC hdc, int idx)
{
    if (idx < 48 && g_nBasic <= idx) return;
    HBRUSH hbr = CreateSolidBrush(g_rgbBox[idx]);
    if (!hbr) return;
    HGDIOBJ hOld = SelectObject(hdc, hbr);
    Rectangle(hdc, g_rcBox[idx].left, g_rcBox[idx].top, g_rcBox[idx].right, g_rcBox[idx].bottom);
    if (g_iSel == idx) FrameColorBox(hdc, g_iSel, 1);
    if (hOld) SelectObject(hdc, hOld);
    DeleteObject(hbr);
}

/* ------------------------------------------------------------------ seg6:1CB9
 * the geometry of the sample screen (from the system metrics and the system font) and the
 * desktop region = sample - (active window + inactive window). Assignments in the original order. */
static BOOL LayoutSample(HWND hDlg)
{
    TEXTMETRIC tm;
    RECT *RS = &g_rcSample;
    int cxB = g_cxBorder, cyB = g_cyBorder, cxS = g_cxSize;
    HDC hdc = GetDC(hDlg);
    GetTextMetrics(hdc, &tm);
    int cyBar = 2 * cyB + tm.tmHeight;
    DWORD exA = GetTextExtent(hdc, g_szActive, lstrlen(g_szActive));
    DWORD exI = GetTextExtent(hdc, g_szInactive, lstrlen(g_szInactive));
    DWORD exM = GetTextExtent(hdc, g_szMenuSample, lstrlen(g_szMenuSample));
    (void)GetTextExtent(hdc, g_szWindowText, lstrlen(g_szWindowText));
    ReleaseDC(hDlg, hdc);
    HWND hSample = GetDlgItem(hDlg, IDC_SAMPLE);
    if (!hSample) return FALSE;     /* (no guard in the original; see the decode's open question 8) */
    GetWindowRect(hSample, RS);
    CopyRect(&g_rcSampleScr, RS);
    ScreenToClient(hDlg, (LPPOINT)&RS->left);
    ScreenToClient(hDlg, (LPPOINT)&RS->right);

    int hx = cxS / 2, hy = cyBar / 2;
    rc1D70.left = rc119A.left = RS->left + hx;
    rc1DD2.top = rc119A.top = RS->top + hy;
    rc11CA.right = rc1A26.right = rc1D5C.right = RS->right - hx;
    rc1FB2.top = rc11C2.top = rc1FBC.top = rc1DD2.top + 1;
    rc1FBC.bottom = rc1274.top = rc1FB2.top + 5 * cyB;
    rc1DD2.bottom = rc118E.top = rc173A.top = rc1DE0.top = rc1FBC.bottom + 1;
    rc1746.top = rc1D70.top = rc1A26.top = rc1938.top = rc118E.bottom = rc1958.top = rc173A.bottom =
        rc1DE0.bottom = rc1DD2.bottom + cyBar;
    rc1752.top = rc1746.top + cyB;
    rc1D64.top = rc1DF8.top = rc139E.top = rc1746.top + 1;
    rc1752.bottom = rc1752.top + cyBar;
    rc1958.bottom = rc1DEA.top = rc1752.bottom + cyB;
    rc1016.top = rc139E.bottom = 5 * cyB + rc1D64.top;
    rc1938.bottom = rc1716.top = rcFE2.top = rc170E.top = rc1016.top + 1;
    rcFFA.top = rc1716.bottom = rcFE2.bottom = rc170E.bottom = rc1938.bottom + cyBar;
    rc1022.top = rc1732.top = rcFFA.top + cyB;
    rc1D5C.top = rc12E8.top = rcFFA.top;
    rc1022.bottom = rc1732.bottom = rc1182.top = rc1022.top + cyBar;
    rc11D8.top = rcFFA.bottom = rc1022.bottom + cyB;
    rc1DB4.top = rc11D8.top - cyB;
    rc1182.bottom = rc1058.top = rc1022.bottom + g_cyVScroll;
    rc1A26.bottom = rc11A6.bottom = RS->bottom - (g_cyIcon >> 2) + 1;
    rc1DF8.bottom = rc11CA.bottom = rc1D64.bottom = rc1A26.bottom - 1;
    rc1016.bottom = rc11CA.top = rc175C.bottom = rc1DB4.bottom = rc1DF8.bottom - 5 * cyB;
    rc11D8.bottom = rc11A6.top = rc1016.bottom - 1;
    rc119A.bottom = rc1A26.bottom - hy;
    rcF36.bottom = rc11C2.bottom = rc1FB2.bottom = rc119A.bottom - 1;
    rc1274.bottom = rcF36.top = rcF36.bottom - 5 * cyB;
    rc1DEA.bottom = rc1274.bottom - 1;
    rc1746.bottom = rc1D70.bottom = rc1DEA.bottom - cyBar;
    rc1D5C.bottom = rc12E8.bottom = rc11D8.bottom - cyBar;
    rc175C.top = rc1058.bottom = rc1016.bottom - g_cyVScroll;
    rc1FBC.left = rcF36.left = rc1FB2.left = rc1D70.left + 1;
    rc1958.left = rc1274.left = rc1FB2.right = 5 * cxB + rc1D70.left;
    rc1DEA.left = rc1752.left = rc1DE0.left = rc1D70.right = rc1958.left + 1;
    rc1DD2.left = cxS + rc1958.left;
    rc12E8.left = rc1A26.left = rc1752.right = rc1DEA.right = cxS / 4 + rc1958.left;
    rc139E.left = rc11CA.left = rc1D64.left = rc1958.right = rc12E8.left + 1;
    rcFFA.left = rc1016.left = rc1D64.right = rc12E8.left + 5 * cxB;
    rc11D8.left = rc1732.left = rc1022.left = rc170E.left = rc12E8.right = rcFFA.left + 1;
    rc1732.right = rc11D8.left + LOWORD(exM);
    rc11A6.left = rc1938.left = rcFFA.left + cxS;
    rc139E.right = rc1DF8.right = rc1A26.right - 1;
    rc1DB4.right = rcFFA.right = rc1016.right = rc1DF8.left = rc175C.right = rc1182.right = rc139E.right - 5 * cxB;
    rc1058.right = rc1D5C.left = rc1022.right = rc1716.right = rc1DB4.right - 1;
    rc175C.left = rc1182.left = rc11D8.right = rc1DB4.left = rc1DB4.right - g_cxVScroll;
    rc1058.left = rc175C.left + 1;
    rc119A.right = rc1746.right = rc1A26.right - hx;
    rc11A6.right = rc1938.right = rc1058.right - cxS;
    rcF36.right = rc11C2.right = rc1FBC.right = rc119A.right - 1;
    rc1274.right = rc11C2.left = rcF36.right - 5 * cxB;
    rc1DD2.right = rc1274.right - cxS;
    rc118E.right = rc1746.left = rc1274.right - 1;
    rcFE2.left = rc170E.right = ((rc1058.right + rc11D8.left) >> 1) - ((int)((unsigned)LOWORD(exA) >> 1)) - 2 * cxB;
    rcFE2.right = rc1716.left = rcFE2.left + 4 * cxB + LOWORD(exA);
    rc173A.left = rc1DE0.right = ((rc118E.right + rc1DEA.left) >> 1) - ((int)((unsigned)LOWORD(exI) >> 1)) - 2 * cxB;
    rc173A.right = rc118E.left = 4 * cxB + rc173A.left + LOWORD(exI);

    /* vertical text position in a caption (the active caption's metrics) and in the menu bar */
    int t = g_sysExtLead;
    int d = (rcFE2.bottom - g_sysExtLead) - g_sysTmHeight - rcFE2.top - 1;
    if (d > 0) t = (d >> 1) + g_sysExtLead;
    ptActive.x = 2 * cxB + rcFE2.left;
    ptActive.y = rcFE2.top + t;
    ptInactive.x = 2 * cxB + rc173A.left;
    ptInactive.y = rc173A.top + t;
    ptMenu.x = 4 * cxB + rc1022.left;
    t = g_sysExtLead;
    d = (rc1022.bottom - g_sysExtLead) - g_sysTmHeight - rc1022.top - 1;
    if (d > 0) t = (d >> 1) + g_sysExtLead;
    ptMenu.y = rc1022.top + t;

    /* the document window, the popup menu, their parts */
    CopyRect(&rc13B4, &rc11D8);
    rc13B4.left += (rc11D8.right - rc11D8.left) / 2;
    InflateRect(&rc13B4, -((rc13B4.right - rc13B4.left) >> 4), -((rc13B4.bottom - rc13B4.top) >> 4));
    CopyRect(&rc1FCC, &rc11D8);
    rc1FCC.right += (rc11D8.left - rc11D8.right) / 2;
    rc1FCC.left--;
    rc1FCC.right -= (rc1FCC.right - rc1FCC.left) >> 4;
    rc1FCC.top--;
    rc1FCC.bottom = ((g_sysExtLead + g_sysTmHeight + 1) << 1) + rc1FCC.top;
    CopyRect(&rc19E8, &rc1FCC);
    rc19E8.top++; rc19E8.left++; rc19E8.bottom--; rc19E8.right--;
    CopyRect(&rc1D9A, &rc1FCC);
    rc1D9A.bottom -= g_sysExtLead + g_sysTmHeight + 1;
    rc1D9A.left++; rc1D9A.top++;
    CopyRect(&rcE8C, &rc1FCC);
    rcE8C.top += g_sysExtLead + g_sysTmHeight + 1;
    rcE8C.left++; rcE8C.right--; rcE8C.bottom--;
    CopyRect(&rcE6E, &rc13B4);
    rc13B4.left++; rc13B4.top++; rc13B4.right--; rc13B4.bottom--;
    hdc = GetDC(hDlg);
    CopyRect(&rc102C, &rc13B4);
    DrawText(hdc, g_szWindowText, lstrlen(g_szWindowText), &rc102C, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    ReleaseDC(hDlg, hdc);
    CopyRect(&rc1940, &rc13B4);
    int dx = (rc1940.left - rc1940.right) >> 3;
    rc1940.top += (rc1940.bottom - rc1940.top) / 2;
    int dy = (rc1940.top - rc1940.bottom) >> 3;
    InflateRect(&rc1940, dx, dy);

    /* the desktop region */
    HRGN hA = CreateRectRgnIndirect(&rc1A26);
    HRGN hB = CreateRectRgnIndirect(&rc119A);
    HRGN hC = CreateRectRgnIndirect(&rc119A);
    g_hrgnDesktop = CreateRectRgnIndirect(&rc119A);
    if (!hA || !hB || !hC || !g_hrgnDesktop) {
        if (hA) DeleteObject(hA);
        if (hB) DeleteObject(hB);
        if (hC) DeleteObject(hC);
        if (g_hrgnDesktop) DeleteObject(g_hrgnDesktop);
        g_hrgnDesktop = 0;          /* (the original leaves the deleted handle here) */
        return FALSE;
    }
    BOOL fErr = CombineRgn(hC, hA, hB, RGN_OR) == ERROR;
    DeleteObject(hA);
    DeleteObject(hB);
    hA = CreateRectRgnIndirect(RS);
    if (!hA) {
        DeleteObject(g_hrgnDesktop);
        g_hrgnDesktop = 0;
        DeleteObject(hC);
        return FALSE;
    }
    fErr |= CombineRgn(g_hrgnDesktop, hA, hC, RGN_DIFF) == ERROR;
    DeleteObject(hA);
    DeleteObject(hC);
    if (fErr) {
        DeleteObject(g_hrgnDesktop);
        g_hrgnDesktop = 0;
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ seg6:23D3 "Color Palette >>"
 * widens the dialog to its template size and builds the 48 basic and 16 custom colour boxes */
static void OpenPalette(void)
{
    RECT rc;
    char szKey[8], szVal[256];
    const BYTE *lp = NULL;
    int i;
    EnableWindow(g_hwndElemCombo, TRUE);
    EnableWindow(GetDlgItem(g_hDlg, IDC_SAVE), TRUE);
    EnableWindow(GetDlgItem(g_hDlg, IDC_DEFINE), TRUE);
    EnableWindow(g_hwndBasic, TRUE);
    g_lpfnOldStatic = (WNDPROC)w16_GetWindowPtr(g_hwndBasic, GWL_WNDPROC);
    w16_SetWindowPtr(g_hwndBasic, GWL_WNDPROC, (intptr_t)GridSubclassProc);
    EnableWindow(g_hwndCustom, TRUE);
    w16_SetWindowPtr(g_hwndCustom, GWL_WNDPROC, (intptr_t)GridSubclassProc);
    g_iElem = 0;
    g_rgbCur = g_rgbElem[0];
    SendMessage(g_hwndElemCombo, CB_SETCURSEL, 0, 0);

    GetWindowRect(g_hwndBasic, &rc);
    ScreenToClient(g_hDlg, (LPPOINT)&rc.left);
    ScreenToClient(g_hDlg, (LPPOINT)&rc.right);
    rc.right -= 3; rc.left += 3;
    g_cxBox = (rc.right - rc.left) / 8;
    rc.bottom -= 3; rc.top += 3;
    g_cyBox = (rc.bottom - rc.top) / 6;

    /* the display driver's basic colours: OEMBIN #2 = WORD count + count COLORREFs. DISPLAY's
     * GetProcAddress(450) (GetDriverResourceID) is not looked up: VGA.DRV does not export it, and
     * another driver's would be 16-bit code (not run) */
    g_nBasic = 0;
    HINSTANCE hDisp = GetModuleHandle("DISPLAY");
    HANDLE hFind = hDisp ? FindResource(hDisp, MAKEINTRESOURCE(2), szOEMBIN) : NULL;
    HGLOBAL hRes = hFind ? LoadResource(hDisp, hFind) : NULL;
    if (hRes && (lp = LockResource(hRes)) != NULL) {
        g_nBasic = (SHORT)(lp[0] | (lp[1] << 8));
        lp += 2;
        if (g_nBasic > 48) g_nBasic = 48;
        if (g_nBasic < 0) g_nBasic = 0;
        if ((DWORD)(2 + 4 * g_nBasic) > SizeofResource(hDisp, hFind)) g_nBasic = 0;
    }
    for (i = 0; i < 48; i++) {
        g_rcBox[i].left = (i % 8) * g_cxBox + rc.left;
        g_rcBox[i].right = g_rcBox[i].left + g_cxBox - 5;
        g_rcBox[i].top = (i / 8) * g_cyBox + rc.top;
        g_rcBox[i].bottom = g_rcBox[i].top + g_cyBox - 5;
        if (i < g_nBasic) g_rgbBox[i] = ResDword(lp + 4 * i);
        else if (g_nBasic != 0) g_rgbBox[i] = 0x00FFFFFF;
        else g_rgbBox[i] = DefaultBasicColor(i);
    }
    if (g_nBasic == 0) g_nBasic = 48;
    if (hRes) FreeResource(hRes);

    GetWindowRect(g_hwndCustom, &rc);
    ScreenToClient(g_hDlg, (LPPOINT)&rc.left);
    ScreenToClient(g_hDlg, (LPPOINT)&rc.right);
    rc.left += 3; rc.top += 3; rc.right -= 3; rc.bottom -= 3;
    lstrcpy(szKey, "ColorA");
    for (i = 48; i < 64; i++) {
        int j = i - 48;
        g_rcBox[i].left = (j % 8) * g_cxBox + rc.left;
        g_rcBox[i].right = g_rcBox[i].left + g_cxBox - 5;
        g_rcBox[i].top = (j / 8) * g_cyBox + rc.top;
        g_rcBox[i].bottom = g_rcBox[i].top + g_cyBox - 5;
        szKey[5] = (char)(i + 0x11);
        GetPrivateProfileString(szCustomColors, szKey, "FFFFFF", szVal, 0xBE, g_szIni);
        g_rgbBox[i] = ParseHexColor(szVal);
    }
    g_iFocusBasic = g_iSel = 0;
    g_iFocusCustom = 48;
    SelectColorIfInPalette(g_rgbCur);
    if (g_iSel < 48) g_iFocusBasic = g_iSel;
    else g_iFocusCustom = g_iSel;
    SetFocus(g_hwndElemCombo);
    GetWindowRect(g_hDlg, &rc);
    SetWindowPos(g_hDlg, NULL, 0, 0, g_rcDlgFull.right - g_rcDlgFull.left, g_rcDlgFull.bottom - g_rcDlgFull.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

/* ------------------------------------------------------------------ seg23:07E1
 * adds "key=value" for every key of a CONTROL.INI section that has a value; returns the last
 * add's result, or the key list length (0 for a missing or empty section) */
static int AddIniSectionToList(HWND hwnd, LPCSTR lpszSection, UINT wMsg)
{
    int cb = 0x1000, res;
    char *keys = NULL, *pKeys;
    char szLine[512];
    for (;;) {
        char *k = realloc(keys, cb);
        if (!k) { free(keys); return -3; }
        keys = k;
        res = GetPrivateProfileString(lpszSection, NULL, "", keys, cb, g_szIni);
        if (res < cb - 2) break;        /* (the original retries while the list fills the buffer) */
        cb += 0x1000;
    }
    for (pKeys = keys; *pKeys; pKeys += strlen(pKeys) + 1) {
        int n = lstrlen(pKeys);
        if (n > 250) continue;
        GetPrivateProfileString(lpszSection, pKeys, "", szLine + n + 1, (int)sizeof szLine - n - 1, g_szIni);
        if (szLine[n + 1] != 0) {
            lstrcpy(szLine, pKeys);
            szLine[n] = '=';
            res = (int)SendMessage(hwnd, wMsg, 0, (LPARAM)szLine);
            if (res < 0) break;
        }
    }
    free(keys);
    return res;
}

/* ------------------------------------------------------------------ seg6:27A7 (WM_INITDIALOG) */
static BOOL InitColorDlg(HWND hDlg)
{
    char szName[256], szDef[256], szWD[256];
    RECT rcCombo;
    BOOL fFail = FALSE;
    int i, n;
    g_hDlg = hDlg;
    if (!g_hbmUp) {
        g_hbmUp = LoadBitmap(NULL, MAKEINTRESOURCE(OBM_UPARROW));
        g_hbmDn = LoadBitmap(NULL, MAKEINTRESOURCE(OBM_DNARROW));
    }
    g_hwndSchemeCombo = GetDlgItem(hDlg, IDC_SCHEMES);
    g_hwndElemCombo = GetDlgItem(hDlg, IDC_ELEMENT);
    EnableWindow(g_hwndElemCombo, FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_SAVE), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DEFINE), FALSE);
    g_hwndBasic = GetDlgItem(g_hDlg, IDC_BASIC);
    EnableWindow(g_hwndBasic, FALSE);
    g_hwndCustom = GetDlgItem(g_hDlg, IDC_CUSTOM);
    EnableWindow(g_hwndCustom, FALSE);
    GetWindowRect(g_hwndElemCombo, &rcCombo);
    GetWindowRect(hDlg, &g_rcDlgFull);
    /* the left column only, until "Color Palette >>" */
    MoveWindow(hDlg, g_rcDlgFull.left, g_rcDlgFull.top, rcCombo.left - g_rcDlgFull.left - 1,
               g_rcDlgFull.bottom - g_rcDlgFull.top, TRUE);

    if (AddIniSectionToList(g_hwndSchemeCombo, szColorSchemes, CB_ADDSTRING) < 0) {
        /* (only on an allocation or combo error) */
        LoadString(hInstMain, 99, szName, 0x9E);
        LoadString(hInstMain, 102, szDef, 0xBE);
        WritePrivateProfileString(szColorSchemes, szName, szDef, g_szIni);
        lstrcat(szName, "=");
        lstrcat(szName, szDef);
        SendMessage(g_hwndSchemeCombo, CB_ADDSTRING, 0, (LPARAM)szName);
    }

    /* "Windows Default" from the display driver's OEMBIN #1 (COLORREFs of COLOR_ 0..18 at 0x12),
     * always first; elements 19 and 20 are fixed */
    HINSTANCE hDisp = GetModuleHandle("DISPLAY");
    HANDLE hRsrc = hDisp ? FindResource(hDisp, MAKEINTRESOURCE(1), szOEMBIN) : NULL;
    if (hRsrc && SizeofResource(hDisp, hRsrc) >= 0x12 + 19 * 4) {
        HGLOBAL hMem = LoadResource(hDisp, hRsrc);
        if (hMem) {
            const BYTE *lpRes = LockResource(hMem);
            if (lpRes) {
                const BYTE *pc = lpRes + 0x12;
                LoadString(hInstMain, 99, szWD, 0xBE);
                lstrcat(szWD, "=");
                char *p = szWD + lstrlen(szWD);
                for (i = 0; i < 19; i++) {
                    wsprintf(p, "%lX,", ResDword(pc + 4 * g03F8[i]));
                    p += lstrlen(p);
                }
                lstrcpy(p, "0,ffffff");
                SendMessage(g_hwndSchemeCombo, CB_INSERTSTRING, 0, (LPARAM)szWD);
                GlobalUnlock(hMem);
            }
            FreeResource(hMem);
        }
    }

    /* the scheme named in [current] */
    GetPrivateProfileString(szCurrent, szColorSchemes, "", szName, 0xBE, g_szIni);
    if (szName[0]) {
        n = lstrlen(szName);
        GetPrivateProfileString(szColorSchemes, szName, "", szName + n + 1, (int)sizeof szName - n - 1, g_szIni);
        szName[n] = '=';
        if (SendMessage(g_hwndSchemeCombo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)szName) == CB_ERR)
            SendMessage(g_hwndSchemeCombo, CB_SETCURSEL, 0, 0);
    }
    n = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), n != -1 && n != 0);

    /* the element combo and the current system colours */
    for (i = 0; i < 21; i++) {
        g_iElem = g03CE[i];
        g_rgbOrig[g_iElem] = GetSysColor(g03F8[g_iElem]);
        g_rgbElem[g_iElem] = g_rgbOrig[g_iElem];
        if (!fFail && LoadString(hInstMain, 72 + g_iElem, szName, 0x9E))
            fFail = SendMessage(g_hwndElemCombo, CB_ADDSTRING, 0, (LPARAM)szName) == CB_ERR;
        else
            fFail = TRUE;
    }
    fFail = fFail || !LoadString(hInstMain, 93, g_szActive, sizeof g_szActive);
    fFail = fFail || !LoadString(hInstMain, 94, g_szInactive, sizeof g_szInactive);
    fFail = fFail || !LoadString(hInstMain, 95, g_szMenuSample, sizeof g_szMenuSample);
    fFail = fFail || !LoadString(hInstMain, 96, g_szWindowText, sizeof g_szWindowText);
    fFail = fFail || !LoadString(hInstMain, 97, g_szDisabled, sizeof g_szDisabled);
    fFail = fFail || !LoadString(hInstMain, 98, g_szHighlighted, sizeof g_szHighlighted);
    (void)fFail;

    g_iElem = 0;
    g_rgbCur = g_rgbElem[0];
    g_cyBorder = GetSystemMetrics(SM_CYBORDER);
    (void)GetSystemMetrics(SM_CYCAPTION);
    (void)GetSystemMetrics(SM_CYMENU);
    g_cyIcon = GetSystemMetrics(SM_CYICON);
    g_cyVScroll = GetSystemMetrics(SM_CYVSCROLL);
    (void)GetSystemMetrics(SM_CYVTHUMB);
    g_cxVScroll = GetSystemMetrics(SM_CXVSCROLL);
    g_cxBorder = GetSystemMetrics(SM_CXBORDER);
    g_cxSize = GetSystemMetrics(SM_CXSIZE);
    g_bFlags = 0;
    if (!LayoutSample(hDlg)) return FALSE;
    ShowWindow(hDlg, SW_SHOW);
    return TRUE;
}

/* ------------------------------------------------------------------ seg6:2D83 */
static void DrawSampleArrow(HDC hdc, BOOL fUp)
{
    const RECT *prc = fUp ? &rc1182 : &rc175C;
    HGDIOBJ hOld = SelectObject(g_hdcMem, fUp ? g_hbmUp : g_hbmDn);
    if (hOld) {
        BitBlt(hdc, prc->left, prc->top, g_cxVScroll, g_cyVScroll, g_hdcMem, 0, 0, SRCCOPY);
        SelectObject(g_hdcMem, hOld);
    }
}

/* the shared tail seg6:2FCC: the clip back to the whole client area */
static void RestoreClip(HDC hdc)
{
    RECT rcCl;
    GetClientRect(g_hDlg, &rcCl);
    HRGN hrgn = CreateRectRgnIndirect(&rcCl);
    if (!hrgn) return;
    SelectClipRgn(hdc, hrgn);
    DeleteObject(hrgn);
}

#define NC(e) GetNearestColor(hdc, g_rgbElem[e])

/* ------------------------------------------------------------------ seg6:2DEC
 * draws one element of the sample screen, -1 = all of it (jump table seg6:3A58) */
static void PaintElement(HDC hdc, int elem)
{
    HBRUSH hbr;
    COLORREF oldText, oldBk;
    int oldMode;
    switch (elem) {
    case -1:
        PaintElement(hdc, 0); PaintElement(hdc, 9); PaintElement(hdc, 6); PaintElement(hdc, 7);
        PaintElement(hdc, 4); PaintElement(hdc, 2); PaintElement(hdc, 12); PaintElement(hdc, 1);
        PaintElement(hdc, 11); PaintElement(hdc, 10);
        return;

    case 0:                             /* Desktop */
        if (!g_hrgnDesktop) return;
        hbr = CreateSolidBrush(g_rgbElem[0]);
        if (!hbr) return;
        FillRgn(hdc, g_hrgnDesktop, hbr);
        DeleteObject(hbr);
        return;

    case 9:                             /* Active Border */
        hbr = CreateSolidBrush(g_rgbElem[9]);
        if (hbr) {
            FillRect(hdc, &rc139E, hbr); FillRect(hdc, &rc11CA, hbr);
            FillRect(hdc, &rc1D64, hbr); FillRect(hdc, &rc1DF8, hbr);
            DeleteObject(hbr);
        }
        hbr = CreateSolidBrush(NC(11));
        if (!hbr) return;
        DrawFrame(hdc, &rc1A26, hbr); DrawFrame(hdc, &rc1016, hbr); DrawFrame(hdc, &rc12E8, hbr);
        DrawFrame(hdc, &rc1D5C, hbr); DrawFrame(hdc, &rc1938, hbr); DrawFrame(hdc, &rc11A6, hbr);
        DeleteObject(hbr);
        return;

    case 10:                            /* Inactive Border */
        ExcludeClipRect(hdc, rc1A26.left, rc1A26.top, rc1A26.right, rc1A26.bottom);
        hbr = CreateSolidBrush(g_rgbElem[10]);
        if (hbr) {
            FillRect(hdc, &rc1FBC, hbr); FillRect(hdc, &rc1FB2, hbr);
            FillRect(hdc, &rc11C2, hbr); FillRect(hdc, &rcF36, hbr);
            DeleteObject(hbr);
        }
        hbr = CreateSolidBrush(NC(11));
        if (hbr) {
            DrawFrame(hdc, &rc119A, hbr); DrawFrame(hdc, &rc1274, hbr);
            DrawFrame(hdc, &rc1746, hbr); DrawFrame(hdc, &rc1D70, hbr);
            DeleteObject(hbr);
        }
        RestoreClip(hdc);
        return;

    case 11:                            /* Window Frame */
        DrawSampleArrow(hdc, TRUE);
        DrawSampleArrow(hdc, FALSE);
        hbr = CreateSolidBrush(NC(11));
        if (!hbr) { RestoreClip(hdc); return; }
        DrawFrame(hdc, &rc1A26, hbr); DrawFrame(hdc, &rc1016, hbr); DrawFrame(hdc, &rc12E8, hbr);
        DrawFrame(hdc, &rc1D5C, hbr); DrawFrame(hdc, &rc1938, hbr); DrawFrame(hdc, &rc11A6, hbr);
        DrawFrame(hdc, &rcFFA, hbr); DrawFrame(hdc, &rc1958, hbr); DrawFrame(hdc, &rcE6E, hbr);
        DrawFrame(hdc, &rc1DB4, hbr);
        rc1058.left--; rc1058.right++;
        DrawFrame(hdc, &rc1058, hbr);
        rc1058.left++; rc1058.right--;
        DrawFrame(hdc, &rc1FCC, hbr);
        ExcludeClipRect(hdc, rc1A26.left, rc1A26.top, rc1A26.right, rc1A26.bottom);
        DrawFrame(hdc, &rc119A, hbr); DrawFrame(hdc, &rc1274, hbr); DrawFrame(hdc, &rc1746, hbr);
        DrawFrame(hdc, &rc1D70, hbr); DrawFrame(hdc, &rc1DD2, hbr);
        DeleteObject(hbr);
        RestoreClip(hdc);
        return;

    case 6:                             /* Active Title Bar */
        hbr = CreateSolidBrush(g_rgbElem[6]);
        if (hbr) {
            FillRect(hdc, &rc170E, hbr); FillRect(hdc, &rcFE2, hbr); FillRect(hdc, &rc1716, hbr);
            DeleteObject(hbr);
        }
        PaintElement(hdc, 8);
        return;

    case 7:                             /* Inactive Title Bar */
        hbr = CreateSolidBrush(g_rgbElem[7]);
        if (hbr) {
            FillRect(hdc, &rc1DE0, hbr); FillRect(hdc, &rc173A, hbr); FillRect(hdc, &rc118E, hbr);
            DeleteObject(hbr);
        }
        PaintElement(hdc, 19);
        return;

    case 8:                             /* Active Title Bar Text (redraws the arrows too) */
        oldText = SetTextColor(hdc, NC(8));
        oldMode = SetBkMode(hdc, TRANSPARENT);
        TextOut(hdc, ptActive.x, ptActive.y, g_szActive, lstrlen(g_szActive));
        DrawSampleArrow(hdc, TRUE);
        DrawSampleArrow(hdc, FALSE);
        SetBkMode(hdc, oldMode);
        SetTextColor(hdc, oldText);
        return;

    case 19:                            /* Inactive Title Bar Text */
        oldText = SetTextColor(hdc, NC(19));
        oldMode = SetBkMode(hdc, TRANSPARENT);
        TextOut(hdc, ptInactive.x, ptInactive.y, g_szInactive, lstrlen(g_szInactive));
        SetBkMode(hdc, oldMode);
        SetTextColor(hdc, oldText);
        return;

    case 4:                             /* Menu Bar */
        hbr = CreateSolidBrush(NC(4));
        if (hbr) { FillRect(hdc, &rc1022, hbr); FillRect(hdc, &rc1752, hbr); DeleteObject(hbr); }
        PaintElement(hdc, 5);
        PaintElement(hdc, 16);
        hbr = CreateSolidBrush(NC(11));
        if (!hbr) return;
        DrawFrame(hdc, &rc1958, hbr);
        DeleteObject(hbr);
        return;

    case 5:                             /* Menu Text */
        oldText = SetTextColor(hdc, NC(5));
        oldBk = SetBkColor(hdc, NC(4));
        oldMode = SetBkMode(hdc, OPAQUE);
        TextOut(hdc, ptMenu.x, ptMenu.y, g_szMenuSample, lstrlen(g_szMenuSample));
        SetBkMode(hdc, oldMode);
        SetBkColor(hdc, oldBk);
        SetTextColor(hdc, oldText);
        return;

    case 1:                             /* Application Workspace */
        ExcludeClipRect(hdc, rcE6E.left, rcE6E.top, rcE6E.right, rcE6E.bottom);
        ExcludeClipRect(hdc, rc1FCC.left, rc1FCC.top, rc1FCC.right, rc1FCC.bottom);
        hbr = CreateSolidBrush(g_rgbElem[1]);
        if (!hbr) { RestoreClip(hdc); return; }
        FillRect(hdc, &rc11D8, hbr);
        FillRect(hdc, &rc1DEA, hbr);
        DeleteObject(hbr);
        RestoreClip(hdc);
        return;

    case 2:                             /* Window Background */
        hbr = CreateSolidBrush(NC(2));
        if (hbr) { FillRect(hdc, &rc13B4, hbr); DeleteObject(hbr); }
        PaintElement(hdc, 3);
        PaintElement(hdc, 13);
        return;

    case 16: case 17: case 18:          /* the popup menu: Disabled Text, Highlight, Highlighted Text */
        hbr = CreateSolidBrush(NC(11));
        if (hbr) { DrawFrame(hdc, &rc1FCC, hbr); DeleteObject(hbr); }
        hbr = CreateSolidBrush(NC(4));
        if (hbr) { FillRect(hdc, &rc19E8, hbr); DeleteObject(hbr); }
        oldBk = SetBkColor(hdc, NC(4));
        oldText = SetTextColor(hdc, NC(16));
        DrawText(hdc, g_szDisabled, -1, &rc1D9A, DT_SINGLELINE);
        hbr = CreateSolidBrush(NC(17));
        if (hbr) { FillRect(hdc, &rcE8C, hbr); DeleteObject(hbr); }
        SetBkColor(hdc, NC(17));
        SetTextColor(hdc, NC(18));
        DrawText(hdc, g_szHighlighted, -1, &rcE8C, DT_SINGLELINE);
        SetBkColor(hdc, oldBk);
        SetTextColor(hdc, oldText);
        return;

    case 3:                             /* Window Text */
        oldBk = SetBkColor(hdc, NC(2));
        oldText = SetTextColor(hdc, NC(3));
        oldMode = SetBkMode(hdc, TRANSPARENT);
        DrawText(hdc, g_szWindowText, -1, &rc13B4, DT_CENTER | DT_WORDBREAK);
        SetBkMode(hdc, oldMode);
        SetTextColor(hdc, oldText);
        SetBkColor(hdc, oldBk);
        return;

    case 12:                            /* Scroll Bars */
        hbr = CreateSolidBrush(g_rgbElem[12]);
        if (hbr) { FillRect(hdc, &rc1058, hbr); DeleteObject(hbr); }
        rc1058.left--; rc1058.right++;
        hbr = CreateSolidBrush(NC(11));
        if (hbr) { DrawFrame(hdc, &rc1058, hbr); DeleteObject(hbr); }
        rc1058.left++; rc1058.right--;
        return;

    case 13: case 14: case 15: case 20: {  /* the sample "OK" button */
        RECT rc;
        HGDIOBJ hOldBr = NULL, hOldPen = NULL;
        HPEN hPen;
        char szOK[10];
        CopyRect(&rc, &rc1940);
        InflateRect(&rc, -g_cxBorder, -g_cyBorder);
        oldText = SetTextColor(hdc, NC(15));
        oldBk = SetBkColor(hdc, NC(13));
        hbr = CreateSolidBrush(NC(2));          /* the window colour shows in the corners */
        if (hbr) hOldBr = SelectObject(hdc, hbr);
        hPen = CreatePen(PS_SOLID, 1, NC(11));
        if (hPen) hOldPen = SelectObject(hdc, hPen);
        Rectangle(hdc, rc1940.left, rc1940.top, rc1940.right, rc1940.bottom);
        PatBlt(hdc, rc1940.left, rc1940.top, g_cxBorder, g_cyBorder, PATCOPY);
        PatBlt(hdc, rc1940.right - g_cxBorder, rc1940.top, g_cxBorder, g_cyBorder, PATCOPY);
        PatBlt(hdc, rc1940.left, rc1940.bottom - g_cyBorder, g_cxBorder, g_cyBorder, PATCOPY);
        PatBlt(hdc, rc1940.right - g_cxBorder, rc1940.bottom - g_cyBorder, g_cxBorder, g_cyBorder, PATCOPY);
        if (hbr) { if (hOldBr) SelectObject(hdc, hOldBr); DeleteObject(hbr); }

        hbr = CreateSolidBrush(g_rgbElem[20]);  /* Button Highlight */
        if (hbr) hOldBr = SelectObject(hdc, hbr);
        PatBlt(hdc, rc.left, rc.top, 2 * g_cxBorder, rc.bottom - rc.top, PATCOPY);
        PatBlt(hdc, rc.left, rc.top, rc.right - rc.left, 2 * g_cyBorder, PATCOPY);
        if (hbr) { if (hOldBr) SelectObject(hdc, hOldBr); DeleteObject(hbr); }

        hbr = CreateSolidBrush(g_rgbElem[14]);  /* Button Shadow */
        if (hbr) hOldBr = SelectObject(hdc, hbr);
        rc.bottom -= g_cyBorder;
        rc.right -= g_cxBorder;
        for (int i = 0; i <= 2; i++) {
            PatBlt(hdc, rc.left, rc.bottom, g_cxBorder - rc.left + rc.right, g_cyBorder, PATCOPY);
            PatBlt(hdc, rc.right, rc.top, g_cxBorder, rc.bottom - rc.top, PATCOPY);
            if (i == 0) InflateRect(&rc, -g_cxBorder, -g_cyBorder);
        }
        if (hbr) { if (hOldBr) SelectObject(hdc, hOldBr); DeleteObject(hbr); }

        hbr = CreateSolidBrush(NC(13));         /* Button Face */
        if (hbr) hOldBr = SelectObject(hdc, hbr);
        rc.left += g_cxBorder;
        rc.top += g_cyBorder;
        PatBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, PATCOPY);
        int len = LoadString(hInstMain, 108, szOK, sizeof szOK);
        DrawText(hdc, szOK, len, &rc1940, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hbr) { if (hOldBr) SelectObject(hdc, hOldBr); DeleteObject(hbr); }
        if (hPen) { if (hOldPen) SelectObject(hdc, hOldPen); DeleteObject(hPen); }
        SetTextColor(hdc, oldText);
        SetBkColor(hdc, oldBk);
        return;
    }
    }
}

/* ------------------------------------------------------------------ seg6:3A88 (WM_PAINT) */
static void PaintColorDlg(HDC hdc, const RECT *lprcPaint)
{
    RECT rc;
    if (IntersectRect(&rc, lprcPaint, &g_rcSample)) PaintElement(hdc, -1);
    if (g_fPalette) {
        for (int i = 0; i < 64; i++) PaintColorBox(hdc, i);
        if (GetFocus() == g_hwndBasic || GetFocus() == g_hwndCustom) {
            PaintColorBox(hdc, g_iFocusBasic);
            PaintColorBox(hdc, g_iFocusCustom);
        }
    }
}

/* ------------------------------------------------------------------ seg6:3AF8: WIN.INI [colors] */
static void SaveColorsToWinIni(void)
{
    char szNum[8], szVal[16];
    BOOL fOk = TRUE;
    for (int i = 0; fOk && i < 21; i++) {
        wsprintf(szNum, "%d", GetRValue(g_rgbElem[i]));
        lstrcpy(szVal, szNum);
        lstrcat(szVal, " ");
        wsprintf(szNum, "%d", GetGValue(g_rgbElem[i]));
        lstrcat(szVal, szNum);
        lstrcat(szVal, " ");
        wsprintf(szNum, "%d", GetBValue(g_rgbElem[i]));
        lstrcat(szVal, szNum);
        fOk = WriteProfileString("colors", g0422[i], szVal) != 0;
    }
    BroadcastWinIniChange(1);
    if (!fOk) MyMessageBox(g_hDlg, 0x41, 1, MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ seg4:00E5
 * the colour grids (statics 32 and 80): focus changes to the dialog as 0x801/0x802, arrows and
 * characters to the dialog */
static LRESULT CALLBACK GridSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        return SendMessage(GetParent(hwnd), msg + 0x7FA, wParam, (LPARAM)hwnd);
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
    case WM_CHAR:
        return SendMessage(GetParent(hwnd), msg, wParam, lParam);
    }
    return CallWindowProc(g_lpfnOldStatic, hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg6:0DC8, dialog 100 */
static BOOL CALLBACK ColorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt = {(SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam)};
    BOOL fApply = FALSE;
    HDC hdc;
    RECT rc;
    int e, i, idx;
    HWND hwndHit;
    char szItem[256], szMsg[256], szFmt[256];

    switch (msg) {
    case WM_DESTROY:
    case WM_RBUTTONDOWN:
        goto tail;

    case WM_MOVE:
        if (g_hrgnDesktop) DeleteObject(g_hrgnDesktop);
        g_hrgnDesktop = 0;
        LayoutSample(hDlg);
        goto tail;

    case WM_INITDIALOG: {
        HourGlass(TRUE);
        BOOL r = InitColorDlg(hDlg);
        g_fPalette = FALSE;
        HourGlass(FALSE);
        if (r) goto tail;
        return FALSE;
    }

    case WM_GRIDFOCUS:                  /* a grid got the focus */
        if ((HWND)lParam == g_hwndBasic) idx = g_iFocusBasic;
        else if ((HWND)lParam == g_hwndCustom) idx = g_iFocusCustom;
        else return FALSE;
        hdc = GetDC(g_hDlg);
        CopyRect(&rc, &g_rcBox[idx]);
        InflateRect(&rc, 3, 3);
        DrawFocusRect(hdc, &rc);
        FrameColorBox(hdc, idx, g_iSel == idx ? 3 : 2);
        ReleaseDC(g_hDlg, hdc);
        goto tail;

    case WM_GRIDFOCUS + 1:              /* a grid lost the focus */
        if (!g_hDlg) goto tail;
        if ((HWND)lParam == g_hwndBasic) idx = g_iFocusBasic;
        else if ((HWND)lParam == g_hwndCustom) idx = g_iFocusCustom;
        else return FALSE;
        hdc = GetDC(g_hDlg);
        CopyRect(&rc, &g_rcBox[idx]);
        InflateRect(&rc, 3, 3);
        DrawFocusRect(hdc, &rc);
        FrameColorBox(hdc, idx, g_iSel == idx ? 1 : 0);
        ReleaseDC(g_hDlg, hdc);
        goto tail;

    case WM_MOUSEMOVE:
        if (!g_fTracking) goto tail;
        /* fall through */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (PtInRect(&g_rcSample, pt)) {
            if (!g_fPalette) return FALSE;
            e = HitTestSample(pt);
            if (e != g_iElem) {
                g_iElem = e;
                for (e = 0; e < 20; e++)
                    if (g03CE[e] == g_iElem) break;
                SendMessage(g_hwndElemCombo, CB_SETCURSEL, e, 0);
            }
            if (msg == WM_LBUTTONDOWN) CloseComboDropdowns();
            SetCapture(hDlg);
            ClipCursor(&g_rcSampleScr);
            g_fTracking = TRUE;
            goto tail;
        }
        /* seg6:104F: the colour grids */
        if ((WORD)pt.x < (WORD)g_rcBox[0].left) return TRUE;
        hwndHit = ChildWindowFromPoint(hDlg, pt);
        if (hwndHit == g_hwndBasic) {
            rc.left = g_rcBox[0].left;
            rc.top = g_rcBox[0].top;
            rc.right = g_rcBox[47].right + 5;
            rc.bottom = g_rcBox[47].bottom + 5;
            i = 6;
            idx = 0;
        } else if (hwndHit == g_hwndCustom) {
            rc.left = g_rcBox[48].left;
            rc.top = g_rcBox[48].top;
            rc.right = g_rcBox[63].right + 5;
            rc.bottom = g_rcBox[63].bottom + 5;
            i = 2;
            idx = 48;
        } else
            return FALSE;
        if (GetFocus() != hwndHit) SetFocus(hwndHit);
        if (!PtInRect(&rc, pt)) return TRUE;
        if ((WORD)(pt.x - rc.left) % (WORD)g_cxBox >= (WORD)(g_cxBox - 5)) goto tail;
        if ((WORD)(pt.y - rc.top) % (WORD)g_cyBox >= (WORD)(g_cyBox - 5)) goto tail;
        idx += 8 * (WORD)((WORD)((pt.y - rc.top) * i) / (WORD)(rc.bottom - rc.top)) +
               (WORD)((WORD)((pt.x - rc.left) * 8) / (WORD)(rc.right - rc.left));
        if (idx >= g_nBasic && idx < 48) goto tail;
        if (g_hCust && (wParam & MK_CONTROL)) {
            g_rgbCust = g_rgbBox[idx];
            CustSetColor(g_rgbCust);
            SetHLSEdits(0);
            SetRGBEdits(0);
            fApply = FALSE;
        } else {
            if (wParam & MK_SHIFT) {
                SetFocusBox(idx);
            } else {
                SetSelectedBox(idx);
                g_iSel = idx;
                SetFocusBox(idx);
                if (idx >= 48) g_iFocusCustom = g_iSel;
                else g_iFocusBasic = g_iSel;
                g_rgbCur = g_rgbBox[g_iSel];
            }
            fApply = TRUE;
        }
        hdc = GetDC(hDlg);
        PaintColorBox(hdc, g_iFocusBasic);
        PaintColorBox(hdc, g_iFocusCustom);
        ReleaseDC(hDlg, hdc);
        goto tail;

    case WM_LBUTTONUP:
        if (!g_fTracking) goto tail;
        g_fTracking = FALSE;
        SetCapture(NULL);
        ClipCursor(NULL);
        if (PtInRect(&g_rcSample, pt)) {
reselect:                               /* seg6:12B9 */
            g_rgbCur = g_rgbElem[g_iElem];
            SelectColorIfInPalette(g_rgbCur);
        }
        goto tail;

    case WM_CHAR:
        if (wParam != ' ') goto tail;
        if (GetFocus() == g_hwndBasic) idx = g_iFocusBasic;
        else if (GetFocus() == g_hwndCustom) idx = g_iFocusCustom;
        else return FALSE;
        if (g_hCust && (GetKeyState(VK_CONTROL) & 0x80)) {
            g_rgbCust = g_rgbBox[idx];
            CustSetColor(g_rgbCust);
            fApply = FALSE;
            goto tail;
        }
        SetSelectedBox(idx);
        g_iSel = idx;
        fApply = TRUE;
        goto tail;

    case WM_KEYDOWN:
        if (GridKeyNav(wParam, &idx)) SetFocusBox(idx);
        goto tail;

    case WM_SETFONT: {
        TEXTMETRIC tm;
        hdc = GetDC(hDlg);
        HGDIOBJ hOld = SelectObject(hdc, (HFONT)wParam);
        GetTextMetrics(hdc, &tm);
        if (hOld) SelectObject(hdc, hOld);
        ReleaseDC(hDlg, hdc);
        g_cyItem = tm.tmHeight;
        goto tail;
    }

    case WM_MEASUREITEM:
        if (g_cyItem) ((LPMEASUREITEMSTRUCT)lParam)->itemHeight = g_cyItem;
        goto tail;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT)lParam;
        if (lpdis->CtlID >= 0x20 && (WORD)(lpdis->CtlID - 0x20) < 0x40) return DrawColorBoxItem(lpdis);
        return DrawComboItem(lpdis, SchemeNameLen);
    }

    case WM_CUSTADD:                    /* "Add Color" in the Custom Color Selector */
        g_rgbBox[g_iFocusCustom] = (COLORREF)lParam;
        InvalidateRect(hDlg, &g_rcBox[g_iFocusCustom], FALSE);
        if (g_iFocusCustom >= 63) e = 48;
        else if (g_iFocusCustom > 55) e = g_iFocusCustom - 7;
        else e = g_iFocusCustom + 8;
        g_iFocusCustom = e;             /* (the focus rectangle stays where it was) */
        goto tail;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS | DLGC_HASSETSEL;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hDlg, &ps);
        PaintColorDlg(ps.hdc, &ps.rcPaint);
        EndPaint(hDlg, &ps);
        goto tail;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            if (LOWORD(wParam) == WA_CLICKACTIVE) UpdateWindow(hDlg);
            return FALSE;
        }
        /* (the original then compares hDlg with the grids, which never matches) */
        goto dflt;

    case WM_COMMAND:
        switch (wParam) {
        case IDOK:                      /* seg6:149D */
            HourGlass(TRUE);
            hdc = GetDC(hDlg);
            g_rgbElem[4] = GetNearestColor(hdc, g_rgbElem[4]);
            g_rgbElem[5] = GetNearestColor(hdc, g_rgbElem[5]);
            g_rgbElem[8] = GetNearestColor(hdc, g_rgbElem[8]);
            g_rgbElem[2] = GetNearestColor(hdc, g_rgbElem[2]);
            g_rgbElem[3] = GetNearestColor(hdc, g_rgbElem[3]);
            g_rgbElem[11] = GetNearestColor(hdc, g_rgbElem[11]);
            g_rgbElem[16] = GetNearestColor(hdc, g_rgbElem[16]);
            g_rgbElem[17] = GetNearestColor(hdc, g_rgbElem[17]);
            g_rgbElem[18] = GetNearestColor(hdc, g_rgbElem[18]);
            g_rgbElem[13] = GetNearestColor(hdc, g_rgbElem[13]);
            g_rgbElem[15] = GetNearestColor(hdc, g_rgbElem[15]);
            g_rgbElem[19] = GetNearestColor(hdc, g_rgbElem[19]);
            SetSysColors(21, g03F8, g_rgbElem);
            SaveColorsToWinIni();
            i = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
            if (i < 0 || SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, i, (LPARAM)szItem) < 0) szItem[0] = 0;
            {
                const char *lpName = SchemeMatchesCurrent(hdc, szItem) ? szItem : "";
                int r = WritePrivateProfileString(szCurrent, szColorSchemes, lpName, g_szIni);
                ReleaseDC(hDlg, hdc);
                if (g_bFlags & 1) {
                    lstrcpy(szMsg, "ColorA");
                    for (idx = 48; idx < 64; idx++) {
                        szMsg[5] = (char)(idx + 0x11);
                        wsprintf(szItem, "%lX", g_rgbBox[idx]);
                        if (r == 0) continue;   /* nothing more after the first failure */
                        r = WritePrivateProfileString(szCustomColors, szMsg, szItem, g_szIni) ? 1 : 0;
                    }
                }
            }
            HourGlass(FALSE);
            /* fall through */
        case IDCANCEL:                  /* seg6:16F0 */
            if (g_fTracking) {
                g_fTracking = FALSE;
                SetCapture(NULL);
                ClipCursor(NULL);
            }
            if (g_fPalette) {
                HWND f = GetFocus();
                if (g_hwndBasic == f || g_hwndCustom == f) SendMessage(f, WM_KILLFOCUS, 0, 0);
            }
            if (g_hrgnDesktop) DeleteObject(g_hrgnDesktop);
            g_hrgnDesktop = 0;
            if (g_hCust) SendMessage(g_hCust, WM_COMMAND, IDOK, 0);
            g_cyItem = 0;
            if (GetParent(hDlg)) EnableWindow(GetParent(hDlg), TRUE);
            g_hDlg = 0;
            DestroyWindow(hDlg);
            goto tail;

        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;

        case IDC_REMOVE: {              /* seg6:1792 */
            char *p;
            idx = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
            szItem[0] = 0;
            SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, idx, (LPARAM)szItem);
            p = szItem;
            if (*p) do p++; while (*p && *p != '=');
            *p = 0;
            if (idx == 0) {
                if (!LoadString(hInstMain, 100, szFmt, 0x50)) { OutOfMemory(hDlg); goto tail; }
                wsprintf(szMsg, szFmt, (LPSTR)szItem);
                MessageBox(hDlg, szMsg, szCaption, MB_ICONINFORMATION);
                goto tail;
            }
            if (!ConfirmMsg(g_hDlg, szItem, 236)) goto tail;
            GetPrivateProfileString(szCurrent, szColorSchemes, "", szMsg, sizeof szMsg, g_szIni);
            if (lstrcmpi(szItem, szMsg) == 0) WritePrivateProfileString(szCurrent, szColorSchemes, "", g_szIni);
            WritePrivateProfileString(szColorSchemes, szItem, NULL, g_szIni);
            i = (int)SendMessage(g_hwndSchemeCombo, CB_DELETESTRING, idx, 0);
            if (i == -1) idx = 0;
            else if (idx >= i) idx--;
            SendMessage(g_hwndSchemeCombo, CB_SETCURSEL, idx, 0);
            szItem[0] = 0;
            SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, idx, (LPARAM)szItem);
            ApplySchemeString(szItem);
            goto tail;
        }

        case IDC_SAVE: {                /* seg6:1911 */
            char *p = g_szScheme;
            i = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
            if (i >= 0) {
                SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, i, (LPARAM)g_szScheme);
                if (*p) do p++; while (*p && *p != '=');
            }
            *p = 0;
            if (DoDialogBoxParam(27, g_hDlg, SaveSchemeDlgProc, 0x1F5B, 0) != IDOK) goto tail;
            lstrcpy(szItem, g_szScheme);
            lstrcat(szItem, "=");
            idx = (int)SendMessage(g_hwndSchemeCombo, CB_FINDSTRING, (WPARAM)-1, (LPARAM)szItem);
            if (idx != -1) SendMessage(g_hwndSchemeCombo, CB_DELETESTRING, idx, 0);
            p = szItem;
            for (i = 0; i < 21; i++) {
                wsprintf(p, "%lX,", g_rgbElem[i]);
                p += lstrlen(p);
            }
            szItem[lstrlen(szItem) - 1] = 0;
            WritePrivateProfileString(szColorSchemes, g_szScheme, szItem, g_szIni);
            lstrcat(g_szScheme, "=");
            lstrcat(g_szScheme, szItem);
            i = (int)SendMessage(g_hwndSchemeCombo, idx == -1 ? CB_ADDSTRING : CB_INSERTSTRING, idx, (LPARAM)g_szScheme);
            SendMessage(g_hwndSchemeCombo, CB_SETCURSEL, i, 0);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), TRUE);
            goto tail;
        }

        case IDC_SCHEMES:               /* seg6:1AA8 */
            if (HIWORD(lParam) != CBN_SELCHANGE) goto tail;
            i = (int)SendMessage(g_hwndSchemeCombo, CB_GETCURSEL, 0, 0);
            if (i < 0) goto tail;
            szItem[0] = 0;
            SendMessage(g_hwndSchemeCombo, CB_GETLBTEXT, i, (LPARAM)szItem);
            ApplySchemeString(szItem);
            hwndHit = GetDlgItem(hDlg, IDC_REMOVE);
            EnableWindow(hwndHit, i != 0);
            InvalidateRect(hwndHit, NULL, TRUE);
            goto reselect;

        case IDC_ELEMENT:               /* seg6:1B0F */
            if (HIWORD(lParam) != CBN_SELCHANGE) goto tail;
            i = (int)SendMessage(g_hwndElemCombo, CB_GETCURSEL, 0, 0);
            if (i < 0 || i >= 21) goto tail;    /* (the original reads ds:03CC for CB_ERR) */
            g_iElem = g03CE[i];
            goto reselect;

        case IDC_PALETTE:               /* seg6:1B3A */
            HourGlass(TRUE);
            g_fPalette = TRUE;
            EnableWindow(GetDlgItem(hDlg, IDC_PALETTE), FALSE);
            OpenPalette();
            HourGlass(FALSE);
            goto tail;

        case IDC_DEFINE:                /* seg6:1B67 */
            HourGlass(TRUE);
            EnableWindow(GetDlgItem(hDlg, IDC_DEFINE), FALSE);
            g_hCust = CreateDialog(hInstMain, MAKEINTRESOURCE(26), g_hDlg, CustColorDlgProc);
            HourGlass(FALSE);
            if (!g_hCust) {
                OutOfMemory(hDlg);
                EnableWindow(GetDlgItem(hDlg, IDC_DEFINE), TRUE);
                goto tail;
            }
            SetFocus(g_hCust);
            goto tail;

        default:
            goto tail;
        }

    default:
dflt:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }

tail:                                   /* seg6:1125 */
    if (fApply && g_fPalette) {
        g_rgbElem[g_iElem] = g_rgbCur;
        hdc = GetDC(hDlg);
        PaintElement(hdc, g_iElem);
        ReleaseDC(hDlg, hdc);
    }
    return TRUE;
}

/* ================================================================== the Custom Color Selector (seg7) */

/* ------------------------------------------------------------------ seg7:17DB
 * RGB -> hue 0..239 (160 = undefined), luminosity and saturation 0..240 */
static void RGBtoHLS(COLORREF rgb)
{
    WORD R = GetRValue(rgb), G = GetGValue(rgb), B = GetBValue(rgb);
    WORD cMax = max(max(G, R), B), cMin = min(min(G, R), B);
    WORD sum = cMax + cMin, dif = cMax - cMin;
    g_Lcalc = (WORD)(((DWORD)sum * 240 + 255) / 510);
    if (dif == 0) {
        g_Scalc = 0;
        g_Hcalc = 160;
        return;
    }
    if (g_Lcalc <= 120) g_Scalc = (WORD)(((DWORD)dif * 240 + (sum >> 1)) / sum);
    else g_Scalc = (WORD)(((DWORD)dif * 240 + ((WORD)(510 - sum) >> 1)) / (WORD)(510 - sum));
    WORD Rd = (WORD)(((DWORD)(cMax - R) * 40 + (dif >> 1)) / dif);
    WORD Gd = (WORD)(((DWORD)(cMax - G) * 40 + (dif >> 1)) / dif);
    WORD Bd = (WORD)(((DWORD)(cMax - B) * 40 + (dif >> 1)) / dif);
    SHORT H;
    if (R == cMax) H = (SHORT)(Bd - Gd);
    else if (G == cMax) H = (SHORT)(80 + Rd - Bd);
    else H = (SHORT)(160 + Gd - Rd);
    if (H < 0) H += 240;
    if (H > 240) H -= 240;
    g_Hcalc = H;
}

/* ------------------------------------------------------------------ seg7:195A */
static WORD HueToRGB(WORD n1, WORD n2, WORD hue)
{
    if (hue > 240) hue -= 240;
    if (hue < 40) return (WORD)(n1 + (WORD)((WORD)((WORD)(n2 - n1) * hue) + 20) / 40);
    if (hue < 120) return n2;
    if (hue < 160) return (WORD)(n1 + (WORD)((WORD)((WORD)(n2 - n1) * (WORD)(160 - hue)) + 20) / 40);
    return n1;
}

/* ------------------------------------------------------------------ seg7:19B0 */
static COLORREF HLStoRGB(WORD hue, WORD lum, WORD sat)
{
    WORD R, G, B;
    if (sat == 0) {
        R = G = B = (WORD)((WORD)(lum * 255) / 240);
    } else {
        WORD Magic2;
        if (lum <= 120) Magic2 = (WORD)(((DWORD)(sat + 240) * lum + 120) / 240);
        else Magic2 = (WORD)(lum + sat - (WORD)(((DWORD)(WORD)(lum * sat) + 120) / 240));
        WORD Magic1 = (WORD)(2 * lum - Magic2);
        R = (WORD)((WORD)(HueToRGB(Magic1, Magic2, (WORD)(hue + 80)) * 255 + 120) / 240);
        G = (WORD)((WORD)(HueToRGB(Magic1, Magic2, hue) * 255 + 120) / 240);
        B = (WORD)((WORD)(HueToRGB(Magic1, Magic2, (WORD)(hue - 80)) * 255 + 120) / 240);
    }
    return RGB((BYTE)R, (BYTE)G, (BYTE)B);
}

/* ------------------------------------------------------------------ seg7:00C8
 * the white triangle right of the luminosity bar (at g_yLum; the parameter is unused) */
static void DrawLumArrow(HDC hdc, int yUnused)
{
    POINT pt[3];
    (void)yUnused;
    int d = g_rcLumArrow.left - g_rcLumArrow.right + 3;
    pt[0].x = g_rcLumArrow.left + 2;
    pt[0].y = g_yLum;
    pt[1].x = g_rcLumArrow.right - 1;
    pt[1].y = g_yLum - d;
    pt[2].x = g_rcLumArrow.right - 1;
    pt[2].y = d + g_yLum;
    HGDIOBJ hOldPen = SelectObject(hdc, GetStockObject(BLACK_PEN));
    HGDIOBJ hOldBr = SelectObject(hdc, GetStockObject(WHITE_BRUSH));
    Polygon(hdc, pt, 3);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBr);
}

/* ------------------------------------------------------------------ seg7:014E
 * (the rectangle is upside down - top > bottom - and filled as its normalised self, as Win 3.1's
 * FillRect -> PatBlt does) */
static void EraseLumArrow(HDC hdc)
{
    RECT rc;
    HBRUSH hbr = CreateSolidBrush(GetNearestColor(hdc, g_rgbOrig[2]));
    if (!hbr) return;
    int d = g_rcLumArrow.left - g_rcLumArrow.right + 3;
    rc.left = g_rcLumArrow.left + 1;
    rc.top = g_yLum - d + 1;
    rc.right = g_rcLumArrow.right;
    rc.bottom = d + g_yLum - 1;
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
}

/* ------------------------------------------------------------------ seg7:01BC
 * restores the spectrum under the crosshair (clamped with unsigned compares, as coded) */
static void EraseCrosshair(HDC hdc)
{
    int top = g_yCross - 10 * g_cyBorder;
    if ((WORD)top < (WORD)g_rcSpec.top) top = g_rcSpec.top;
    int bottom = g_yCross + 10 * g_cyBorder;
    if ((WORD)bottom > (WORD)g_rcSpec.bottom) bottom = g_rcSpec.bottom;
    int left = g_xCross - 10 * g_cxBorder;
    if ((WORD)left < (WORD)g_rcSpec.left) left = g_rcSpec.left;
    int right = g_xCross + 10 * g_cxBorder;
    if ((WORD)right > (WORD)g_rcSpec.right) right = g_rcSpec.right;
    HGDIOBJ hOld = SelectObject(g_hdcMem, g_hbmSpectrum);
    BitBlt(hdc, left, top, right - left, bottom - top, g_hdcMem, left - g_rcSpec.left, top - g_rcSpec.top, SRCCOPY);
    if (hOld) SelectObject(g_hdcMem, hOld);
}

/* ------------------------------------------------------------------ seg7:026C
 * the crosshair: four arms of three lines, in the DC's pen (black) */
static void DrawCrosshair(HDC hdc, int x, int y)
{
    int dy = 5 * g_cyBorder, dx = 5 * g_cxBorder;
    int top = y - 2 * dy;       if (top < g_rcSpec.top) top = g_rcSpec.top;
    int bottom = y + 2 * dy;    if (bottom > g_rcSpec.bottom) bottom = g_rcSpec.bottom;
    int left = x - 2 * dx;      if (left < g_rcSpec.left) left = g_rcSpec.left;
    int right = x + 2 * dx;     if (right > g_rcSpec.right) right = g_rcSpec.right;
    int iTop = y - dy;          if (iTop < g_rcSpec.top) iTop = g_rcSpec.top;
    int iBot = y + dy;          if (iBot > g_rcSpec.bottom) iBot = g_rcSpec.bottom;
    int iLeft = x - dx;         if (iLeft < g_rcSpec.left) iLeft = g_rcSpec.left;
    int iRight = x + dx;        if (iRight > g_rcSpec.right) iRight = g_rcSpec.right;
    if (iTop > g_rcSpec.top) {
        if (x - 1 >= g_rcSpec.left) { MoveTo(hdc, x - 1, iTop); LineTo(hdc, x - 1, top); }
        if (x < g_rcSpec.right) { MoveTo(hdc, x, iTop); LineTo(hdc, x, top); }
        if (x + 1 < g_rcSpec.right) { MoveTo(hdc, x + 1, iTop); LineTo(hdc, x + 1, top); }
    }
    if (iBot < g_rcSpec.bottom) {
        if (x - 1 >= g_rcSpec.left) { MoveTo(hdc, x - 1, iBot); LineTo(hdc, x - 1, bottom); }
        if (x < g_rcSpec.right) { MoveTo(hdc, x, iBot); LineTo(hdc, x, bottom); }
        if (x + 1 < g_rcSpec.right) { MoveTo(hdc, x + 1, iBot); LineTo(hdc, x + 1, bottom); }
    }
    if (g_rcSpec.left < iLeft) {
        if (y - 1 >= g_rcSpec.top) { MoveTo(hdc, iLeft, y - 1); LineTo(hdc, left, y - 1); }
        if (y < g_rcSpec.bottom) { MoveTo(hdc, iLeft, y); LineTo(hdc, left, y); }
        if (y + 1 < g_rcSpec.bottom) { MoveTo(hdc, iLeft, y + 1); LineTo(hdc, left, y + 1); }
    }
    if (iRight < g_rcSpec.right) {
        if (y - 1 >= g_rcSpec.top) { MoveTo(hdc, iRight, y - 1); LineTo(hdc, right, y - 1); }
        if (y < g_rcSpec.bottom) { MoveTo(hdc, iRight, y); LineTo(hdc, right, y); }
        if (y + 1 < g_rcSpec.bottom) { MoveTo(hdc, iRight, y + 1); LineTo(hdc, right, y + 1); }
    }
}

/* ------------------------------------------------------------------ seg7:107D
 * crosshair / arrow position -> hue, saturation, luminosity (16-bit products, unsigned division) */
static void PosToHLS(int id)
{
    switch (id) {
    case 703: g_H = (WORD)((WORD)((g_xCross - g_rcSpec.left) * 239) / (WORD)(g_cxSpec - 1)); break;
    case 704: g_S = 240 - (WORD)((WORD)((g_yCross - g_rcSpec.top) * 240) / (WORD)(g_cySpec - 1)); break;
    case 705: g_L = 240 - (WORD)((WORD)((g_yLum - g_rcLumBar.top) * 240) / (WORD)(g_cyLumBar - 1)); break;
    default:
        g_H = (WORD)((WORD)((g_xCross - g_rcSpec.left) * 239) / (WORD)g_cxSpec);
        g_S = 240 - (WORD)((WORD)((g_yCross - g_rcSpec.top) * 240) / (WORD)g_cySpec);
        g_L = 240 - (WORD)((WORD)((g_yLum - g_rcLumBar.top) * 240) / (WORD)g_cyLumBar);
    }
}

/* ------------------------------------------------------------------ seg7:1127
 * hue, saturation, luminosity -> positions (16-bit product, the high word dropped) */
static void HLSToPos(int id)
{
    switch (id) {
    case 703: g_xCross = (WORD)((WORD)(g_H * g_cxSpec) / 239) + g_rcSpec.left; break;
    case 704: g_yCross = (WORD)((WORD)((240 - g_S) * (g_cySpec - 1)) / 240) + g_rcSpec.top; break;
    case 705: g_yLum = (WORD)((WORD)((240 - g_L) * (g_cyLumBar - 1)) / 240) + g_rcLumBar.top; break;
    default:
        g_xCross = (WORD)((WORD)(g_H * g_cxSpec) / 239) + g_rcSpec.left;
        g_yCross = (WORD)((WORD)((240 - g_S) * g_cySpec) / 240) + g_rcSpec.top;
        g_yCross = (WORD)((WORD)((240 - g_S) * (g_cySpec - 1)) / 240) + g_rcSpec.top;
        g_yLum = (WORD)((WORD)((240 - g_L) * (g_cyLumBar - 1)) / 240) + g_rcLumBar.top;
    }
}

/* ------------------------------------------------------------------ seg7:11EB */
static void SetHLSEdits(int id)
{
    switch (id) {
    case 703: SetDlgItemInt(g_hCust, 703, g_H, FALSE); break;
    case 704: SetDlgItemInt(g_hCust, 704, g_S, FALSE); break;
    case 705: SetDlgItemInt(g_hCust, 705, g_L, FALSE); break;
    default:
        SetDlgItemInt(g_hCust, 703, g_H, FALSE);
        SetDlgItemInt(g_hCust, 704, g_S, FALSE);
        SetDlgItemInt(g_hCust, 705, g_L, FALSE);
    }
}

/* ------------------------------------------------------------------ seg7:1252 */
static void SetRGBEdits(int id)
{
    switch (id) {
    case 706: SetDlgItemInt(g_hCust, 706, GetRValue(g_rgbCust), FALSE); break;
    case 707: SetDlgItemInt(g_hCust, 707, GetGValue(g_rgbCust), FALSE); break;
    case 708: SetDlgItemInt(g_hCust, 708, GetBValue(g_rgbCust), FALSE); break;
    default:
        SetDlgItemInt(g_hCust, 706, GetRValue(g_rgbCust), FALSE);
        SetDlgItemInt(g_hCust, 707, GetGValue(g_rgbCust), FALSE);
        SetDlgItemInt(g_hCust, 708, GetBValue(g_rgbCust), FALSE);
    }
}

/* ------------------------------------------------------------------ seg7:05B3 */
static void CustLayout(HWND hDlg)
{
    GetWindowRect(GetDlgItem(hDlg, 710), &g_rcSpec);
    g_rcSpec.left++; g_rcSpec.top++; g_rcSpec.right--; g_rcSpec.bottom--;
    CopyRect(&g_rcSpecScr, &g_rcSpec);
    ScreenToClient(hDlg, (LPPOINT)&g_rcSpec.left);
    ScreenToClient(hDlg, (LPPOINT)&g_rcSpec.right);
    GetWindowRect(g_hwndLumBar, &g_rcLumBar);
    CopyRect(&g_rcLumScr, &g_rcLumBar);
    g_rcLumScr.right += g_cxSize >> 1;
    ScreenToClient(hDlg, (LPPOINT)&g_rcLumBar.left);
    ScreenToClient(hDlg, (LPPOINT)&g_rcLumBar.right);
    CopyRect(&g_rcLumArrow, &g_rcLumBar);
    g_rcLumArrow.left = g_rcLumArrow.right;
    g_rcLumArrow.right += g_cxSize >> 1;
    g_cyLumBar = g_rcLumBar.bottom - g_rcLumBar.top;
    GetWindowRect(GetDlgItem(hDlg, 709), &rc103E);
    rc1396.right = rc103E.right - 1;
    rc103E.top++;
    rc1396.top = rc103E.top;
    rc103E.bottom--;
    rc1396.bottom = rc103E.bottom;
    rc103E.left++;
    rc103E.right = (rc103E.right + rc103E.left) / 2;
    rc1396.left = rc103E.right;
    ScreenToClient(hDlg, (LPPOINT)&rc103E.left);
    ScreenToClient(hDlg, (LPPOINT)&rc103E.right);
    ScreenToClient(hDlg, (LPPOINT)&rc1396.left);
    ScreenToClient(hDlg, (LPPOINT)&rc1396.right);
    rc11BA.left = rc103E.left;
    rc11BA.right = rc1396.right;
    rc11BA.top = rc103E.top;
    rc11BA.bottom = rc1396.bottom;
}

/* ------------------------------------------------------------------ seg7:151C */
static void PaintSpectrumRect(HDC hdc, const RECT *lprc)
{
    if (!g_hbmSpectrum) return;
    HGDIOBJ hOld = SelectObject(g_hdcMem, g_hbmSpectrum);
    BitBlt(hdc, lprc->left, lprc->top, lprc->right - lprc->left, lprc->bottom - lprc->top, g_hdcMem,
           lprc->left - g_rcSpec.left, lprc->top - g_rcSpec.top, SRCCOPY);
    if (hOld) SelectObject(g_hdcMem, hOld);
    DrawCrosshair(hdc, g_xCross, g_yCross);
    UpdateWindow(g_hCust);
}

/* ------------------------------------------------------------------ seg7:159B */
static void CustPaint(HDC hdc, const RECT *lprc)
{
    RECT rc;
    HBRUSH hbr;
    if (IntersectRect(&rc, lprc, &rc103E)) {        /* "Color": dithered */
        hbr = CreateSolidBrush(g_rgbCust);
        if (hbr) { FillRect(hdc, &rc, hbr); DeleteObject(hbr); }
    }
    if (IntersectRect(&rc, lprc, &rc1396)) {        /* "Solid" */
        hbr = CreateSolidBrush(GetNearestColor(hdc, g_rgbCust));
        if (hbr) { FillRect(hdc, &rc, hbr); DeleteObject(hbr); }
    }
    if (IntersectRect(&rc, lprc, &g_rcLumBar)) {    /* the whole bar is redrawn */
        rc.left = g_rcLumBar.left;
        rc.right = g_rcLumBar.right;
        rc.top = g_rcLumBar.bottom - 4;
        rc.bottom = g_rcLumBar.bottom;
        hbr = CreateSolidBrush(HLStoRGB(g_H, 0, g_S));
        if (hbr) { FillRect(hdc, &rc, hbr); DeleteObject(hbr); }
        for (WORD lum = 8; lum < 240; lum += 8) {
            rc.bottom = rc.top;
            rc.top = (WORD)(((DWORD)(LONG)((g_rcLumBar.bottom + 4) * 240L) - (DWORD)(WORD)((lum + 8) * g_cyLumBar)) / 240UL);
            hbr = CreateSolidBrush(HLStoRGB(g_H, lum, g_S));
            if (hbr) { FillRect(hdc, &rc, hbr); DeleteObject(hbr); }
        }
        rc.bottom = rc.top;
        rc.top = g_rcLumBar.top;
        hbr = CreateSolidBrush(HLStoRGB(g_H, 240, g_S));
        if (hbr) { FillRect(hdc, &rc, hbr); DeleteObject(hbr); }
        if (!EqualRect(lprc, &g_rcLumBar)) {
            HGDIOBJ hOld = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, g_rcLumBar.left - 1, g_rcLumBar.top - 1, g_rcLumBar.right + 1, g_rcLumBar.bottom + 1);
            SelectObject(hdc, hOld);
        }
    }
    if (IntersectRect(&rc, lprc, &g_rcLumArrow)) DrawLumArrow(hdc, g_yLum);
    if (IntersectRect(&rc, lprc, &g_rcSpec)) PaintSpectrumRect(hdc, &rc);
}

/* ------------------------------------------------------------------ seg7:0000
 * shows rgb in the selector (crosshair, arrow, bar, sample); does not store it */
static void CustSetColor(COLORREF rgb)
{
    HDC hdc;
    RGBtoHLS(rgb);
    if (g_L != g_Lcalc) {
        hdc = GetDC(g_hCust);
        EraseLumArrow(hdc);
        g_L = g_Lcalc;
        HLSToPos(705);
        DrawLumArrow(hdc, g_yLum);
        ReleaseDC(g_hCust, hdc);
    }
    if (g_H != g_Hcalc || g_S != g_Scalc) {
        g_H = g_Hcalc;
        g_S = g_Scalc;
        InvalidateRect(g_hCust, &g_rcLumBar, FALSE);
        hdc = GetDC(g_hCust);
        EraseCrosshair(hdc);
        HLSToPos(703);
        HLSToPos(704);
        DrawCrosshair(hdc, g_xCross, g_yCross);
        ReleaseDC(g_hCust, hdc);
    }
    InvalidateRect(g_hCust, &rc11BA, FALSE);
    UpdateWindow(g_hCust);
}

/* ------------------------------------------------------------------ seg7:04E6
 * "Solid": the nearest solid colour */
static void SnapToSolid(HWND hDlg)
{
    HDC hdc = GetDC(hDlg);
    COLORREF c = GetNearestColor(hdc, g_rgbCust);
    if (c == g_rgbCust) {
        ReleaseDC(hDlg, hdc);
        return;
    }
    g_rgbCust = c;
    EraseCrosshair(hdc);
    EraseLumArrow(hdc);
    g_rgbCust = GetNearestColor(hdc, g_rgbCust);
    RGBtoHLS(g_rgbCust);
    g_H = g_Hcalc;
    g_L = g_Lcalc;
    g_S = g_Scalc;
    HLSToPos(0);
    DrawCrosshair(hdc, g_xCross, g_yCross);
    DrawLumArrow(hdc, g_yLum);
    ReleaseDC(hDlg, hdc);
    SetHLSEdits(0);
    SetRGBEdits(0);
    InvalidateRect(hDlg, &rc11BA, FALSE);
    InvalidateRect(hDlg, &g_rcLumBar, FALSE);
}

/* ------------------------------------------------------------------ seg7:1A9F: R, G or B typed */
static int RGBEditChanged(int id)
{
    BOOL fOK;
    char sz[4];
    int shift;
    switch (id) {
    case 706: shift = 0; break;
    case 707: shift = 8; break;
    case 708: shift = 16; break;
    default: return 2;
    }
    int val = (int)GetDlgItemInt(g_hCust, id, &fOK, FALSE);
    if (fOK) {
        if (val > 255) {
            val = 255;
            SetDlgItemInt(g_hCust, id, 255, FALSE);
        }
        if ((int)((g_rgbCust >> shift) & 0xFF) != val) {
            g_rgbCust = (g_rgbCust & ~((COLORREF)0xFF << shift)) | ((COLORREF)(BYTE)val << shift);
            CustSetColor(g_rgbCust);
            SetHLSEdits(id);
        }
    } else if (GetDlgItemText(g_hCust, id, sz, 2)) {
        SetRGBEdits(id);
        SendDlgItemMessage(g_hCust, id, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
    }
    return fOK == 0;
}

/* ------------------------------------------------------------------ seg7:12BD (WM_INITDIALOG) */
static BOOL CustInit(HWND hDlg)
{
    RECT rc;
    for (int id = 720; id <= 725; id++) AdjustArrowWidth(GetDlgItem(hDlg, id));
    g_rgbCust = g_rgbCur;
    RGBtoHLS(g_rgbCust);
    g_hCust = hDlg;
    g_hwndLumBar = GetDlgItem(hDlg, 702);
    for (int i = 0; i < 6; i++) g_hwndArrow[i] = GetDlgItem(hDlg, 720 + i);
    CustLayout(hDlg);
    g_cxSpec = g_rcSpec.right - g_rcSpec.left;
    g_cySpec = g_rcSpec.bottom - g_rcSpec.top;
    g_H = g_Hcalc;
    g_S = g_Scalc;
    g_L = g_Lcalc;
    HLSToPos(0);
    SetRGBEdits(0);
    SetHLSEdits(0);
    HDC hdc = GetDC(hDlg);
    g_hbmSpectrum = CreateCompatibleBitmap(hdc, g_cxSpec, g_cySpec);
    if (!g_hbmSpectrum) {
        ReleaseDC(hDlg, hdc);           /* (the original leaks the DC here) */
        return FALSE;
    }
    /* the spectrum: 60 hue columns x 30 saturation rows at luminosity 120 */
    HGDIOBJ hOld = SelectObject(g_hdcMem, g_hbmSpectrum);
    rc.bottom = 0;
    for (WORD sat = 240; sat != 0; sat -= 8) {
        rc.top = rc.bottom;
        rc.bottom = (WORD)((WORD)((248 - sat) * g_cySpec) / 240);
        rc.right = 0;
        for (WORD hue = 0; hue < 239; hue += 4) {
            rc.left = rc.right;
            rc.right = (WORD)((WORD)((hue + 4) * g_cxSpec) / 240);
            HBRUSH hbr = CreateSolidBrush(HLStoRGB(hue, 120, sat));
            if (hbr) {
                FillRect(g_hdcMem, &rc, hbr);
                DeleteObject(hbr);
            }
        }
    }
    SelectObject(g_hdcMem, hOld);
    ReleaseDC(hDlg, hdc);
    UpdateWindow(hDlg);
    return TRUE;
}

/* ------------------------------------------------------------------ seg7:06F8, dialog 26 (modeless) */
static BOOL CALLBACK CustColorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt = {(SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam)};
    HDC hdc;
    int val, id;
    BOOL fOK;
    char sz[4];

    switch (msg) {
    case WM_MOVE:
        CustLayout(hDlg);
        return FALSE;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) CustSetColor(g_rgbCust);
        return FALSE;

    case WM_INITDIALOG: {
        HourGlass(TRUE);
        BOOL r = CustInit(hDlg);
        HourGlass(FALSE);
        if (!r) {
            EndDialog(hDlg, 0);         /* (on a modeless dialog, as coded) */
            return TRUE;
        }
        ShowWindow(hDlg, SW_SHOWNORMAL);
        UpdateWindow(hDlg);
        return TRUE;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hDlg, &ps);
        int oldMode = SetBkMode(ps.hdc, TRANSPARENT);
        CustPaint(ps.hdc, &ps.rcPaint);
        SetBkMode(ps.hdc, oldMode);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_LBUTTONDBLCLK:
        if (!PtInRect(&rc1396, pt)) return TRUE;
solid:
        SnapToSolid(hDlg);
        return TRUE;

    case WM_MOUSEMOVE:
        if (!g_fTracking) return TRUE;
        /* fall through */
    case WM_LBUTTONDOWN:
        if (PtInRect(&g_rcSpec, pt)) {
            CustLayout(hDlg);
            if (msg == WM_LBUTTONDOWN) {
                hdc = GetDC(hDlg);
                EraseCrosshair(hdc);
                ReleaseDC(hDlg, hdc);
            }
            g_xCross = pt.x;
            PosToHLS(703);
            SetHLSEdits(703);
            g_yCross = pt.y;
            PosToHLS(704);
            SetHLSEdits(704);
            g_rgbCust = HLStoRGB(g_H, g_L, g_S);
            hdc = GetDC(hDlg);
            CustPaint(hdc, &g_rcLumBar);
            CustPaint(hdc, &rc11BA);
            ReleaseDC(hDlg, hdc);
            SetRGBEdits(0);
            SetCapture(hDlg);
            ClipCursor(&g_rcSpecScr);
            g_fTracking = TRUE;
            return TRUE;
        }
        if (PtInRect(&g_rcLumBar, pt) || PtInRect(&g_rcLumArrow, pt)) {
            CustLayout(hDlg);
            hdc = GetDC(hDlg);
            EraseLumArrow(hdc);
            g_yLum = pt.y;
            DrawLumArrow(hdc, pt.y);
            PosToHLS(705);
            SetHLSEdits(705);
            g_rgbCust = HLStoRGB(g_H, g_L, g_S);
            CustPaint(hdc, &rc11BA);
            ReleaseDC(hDlg, hdc);
            ValidateRect(hDlg, &g_rcLumArrow);
            ValidateRect(hDlg, &rc11BA);
            SetRGBEdits(0);
            SetCapture(hDlg);
            ClipCursor(&g_rcLumScr);
            g_fTracking = TRUE;
            return TRUE;
        }
        return TRUE;

    case WM_LBUTTONUP:
        if (!g_fTracking) return TRUE;
        g_fTracking = FALSE;
        SetCapture(NULL);
        ClipCursor(NULL);
        if (PtInRect(&g_rcSpec, pt)) {
            hdc = GetDC(hDlg);
            g_xCross = pt.x;
            g_yCross = pt.y;
            DrawCrosshair(hdc, pt.x, pt.y);
            CustPaint(hdc, &g_rcLumBar);
            ReleaseDC(hDlg, hdc);
            ValidateRect(hDlg, &g_rcSpec);
            return TRUE;
        }
        if (PtInRect(&g_rcLumBar, pt)) {
            hdc = GetDC(hDlg);
            DrawLumArrow(hdc, g_yLum);
            ReleaseDC(hDlg, hdc);
            ValidateRect(hDlg, &g_rcLumBar);
        }
        return TRUE;

    case WM_RBUTTONDOWN:
        return TRUE;

    case WM_KEYDOWN: {                  /* only when the dialog itself has the focus */
        BOOL fHigh;
        switch (wParam) {
        case VK_UP:
            if ((WORD)g_S >= 0xEF) return TRUE;
            g_S++;
            fHigh = (WORD)g_L > 120;
            if ((WORD)((fHigh ? 240 - g_L : g_L) << 1) < (WORD)g_S) { if (fHigh) g_L--; else g_L++; }
            return TRUE;
        case VK_DOWN:
            if (g_S == 0) return TRUE;
            g_S--;
            return TRUE;
        case VK_LEFT:
            if (g_L == 0) return TRUE;
            g_L--;
            fHigh = (WORD)g_L > 120;
            if ((WORD)((fHigh ? 240 - g_L : g_L) << 1) < (WORD)g_S) { if (fHigh) g_S--; else g_S++; }
            return TRUE;
        case VK_RIGHT:
            if ((WORD)g_L >= 0xEF) return TRUE;
            g_L++;
            fHigh = (WORD)g_L > 120;
            if ((WORD)((fHigh ? 240 - g_L : g_L) << 1) < (WORD)g_S) g_S--;
            return TRUE;
        }
        return TRUE;                    /* (nothing is redrawn by these keys, as coded) */
    }

    case WM_VSCROLL: {                  /* from a cpArrow: LOWORD(lParam) its id, HIWORD its handle slot */
        int delta, orig, max;
        HWND hArrow = W16_CMD_HWND(HIWORD(lParam));
        for (id = 0; id < 6; id++)
            if (g_hwndArrow[id] == hArrow) break;
        if (id >= 6) return FALSE;
        id += 703;
        max = id == 703 ? 239 : id < 706 ? 240 : 255;
        val = (int)GetDlgItemInt(hDlg, id, &fOK, FALSE);
        orig = val;
        switch (wParam) {               /* jump table seg7:0C26 */
        case SB_LINEUP: delta = 1; break;
        case SB_LINEDOWN: delta = -1; break;
        case SB_PAGEUP: delta = id >= 706 ? 32 : 40; break;
        case SB_PAGEDOWN: delta = id >= 706 ? -32 : -40; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: val = -1; delta = 0; break;
        case SB_TOP: delta = 255; break;
        case SB_BOTTOM: delta = -255; break;
        case SB_ENDSCROLL: delta = 0; break;
        default: return FALSE;
        }
        if (wParam == SB_ENDSCROLL) {
            SendDlgItemMessage(hDlg, id, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
            return TRUE;
        }
        if (val == -1) val = (max + 1) / 2;
        else if (delta + val > max) { val = id == 703 ? 0 : max; delta = 0; }
        else if (delta + val < 0) { val = id == 703 ? max : 0; delta = 0; }
        if (delta + val != orig) {
            SetDlgItemInt(hDlg, id, delta + val, FALSE);
            SendMessage(hDlg, WM_COMMAND, id, MAKELPARAM(0, EN_CHANGE));
        }
        SetFocus(GetDlgItem(hDlg, id));
        return TRUE;
    }

    case WM_COMMAND:
        switch (wParam) {
        case 0:
            return TRUE;
        case IDOK:
        case IDCANCEL:                  /* "Close" */
            if (g_fTracking) {
                g_fTracking = FALSE;
                SetCapture(NULL);
                ClipCursor(NULL);
            }
            if (g_hbmSpectrum) DeleteObject(g_hbmSpectrum);
            g_hbmSpectrum = NULL;       /* (the original keeps the deleted handle) */
            DestroyWindow(hDlg);
            g_hCust = 0;
            if (g_hDlg) EnableWindow(GetDlgItem(g_hDlg, IDC_DEFINE), TRUE);
            return TRUE;
        case IDD_HELP:
            goto help;
        case 731:                       /* Add Color */
            g_bFlags |= 1;
            SendMessage(g_hDlg, WM_CUSTADD, 0, (LPARAM)g_rgbCust);
            return TRUE;
        case 713:                       /* the hidden "&o" button: Alt+O = "S&olid" */
            goto solid;
        case 706: case 707: case 708:
            if (HIWORD(lParam) == EN_CHANGE) {
                RGBEditChanged((int)wParam);
                return TRUE;
            }
            if (HIWORD(lParam) == EN_KILLFOCUS) {
                GetDlgItemInt(hDlg, (int)wParam, &fOK, FALSE);
                if (!fOK) SetRGBEdits((int)wParam);
            }
            return TRUE;
        case 703:                       /* Hue */
            if (HIWORD(lParam) != EN_CHANGE) return TRUE;
            val = (int)GetDlgItemInt(hDlg, 703, &fOK, FALSE);
            if (!fOK) goto badText;
            if (val > 239) { val = 239; SetDlgItemInt(g_hCust, 703, 239, FALSE); }
            if (g_H == val) return TRUE;
            hdc = GetDC(hDlg);
            EraseCrosshair(hdc);
            g_H = val;
            g_rgbCust = HLStoRGB(val, g_L, g_S);
            SetRGBEdits(0);
            HLSToPos(703);
            goto hsTail;
        case 704:                       /* Sat */
            if (HIWORD(lParam) != EN_CHANGE) return TRUE;
            val = (int)GetDlgItemInt(hDlg, 704, &fOK, FALSE);
            if (!fOK) goto badText;
            if (val > 240) { val = 240; SetDlgItemInt(g_hCust, 704, 240, FALSE); }
            if (g_S == val) return TRUE;
            hdc = GetDC(hDlg);
            EraseCrosshair(hdc);
            g_S = val;
            g_rgbCust = HLStoRGB(g_H, g_L, val);
            SetRGBEdits(0);
            HLSToPos(704);
hsTail:
            DrawCrosshair(hdc, g_xCross, g_yCross);
            ReleaseDC(hDlg, hdc);
            InvalidateRect(hDlg, &g_rcLumBar, FALSE);
            goto sampleTail;
        case 705:                       /* Lum */
            if (HIWORD(lParam) != EN_CHANGE) return TRUE;
            val = (int)GetDlgItemInt(hDlg, 705, &fOK, FALSE);
            if (!fOK) goto badText;
            if (val > 240) { val = 240; SetDlgItemInt(g_hCust, 705, 240, FALSE); }
            if (g_L == val) return TRUE;
            hdc = GetDC(hDlg);
            EraseLumArrow(hdc);
            g_L = val;
            HLSToPos(705);
            g_rgbCust = HLStoRGB(g_H, val, g_S);
            SetRGBEdits(0);
            DrawLumArrow(hdc, g_yLum);
            ReleaseDC(hDlg, hdc);
sampleTail:
            InvalidateRect(g_hCust, &rc11BA, FALSE);
            UpdateWindow(hDlg);
            return TRUE;
badText:                                /* not a number: restore it unless empty */
            if (GetDlgItemText(hDlg, (int)wParam, sz, 2)) {
                SetHLSEdits((int)wParam);
                SendDlgItemMessage(hDlg, (int)wParam, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
            }
            return TRUE;
        default:
            return TRUE;
        }

    default:
        if (msg != wHelpMessage) return FALSE;
help: {
        DWORD save = dwContext;
        dwContext = 0x00001F63;
        CPHelp(hDlg);
        dwContext = save;
        return TRUE;
    }
    }
}

/* ------------------------------------------------------------------ seg3:0756 + seg3:06AE
 * the Color dialog runs modeless in its own loop, which also serves the Custom Color Selector */
void ColorRun(HWND hwndOwner)
{
    MSG msg;
    if (!g_hdcMem) {
        /* seg3:013A: MAIN.CPL's memory DC and the system font metrics */
        TEXTMETRIC tm;
        g_hdcMem = CreateCompatibleDC(NULL);
        HDC hdc = GetDC(NULL);
        GetTextMetrics(hdc, &tm);
        ReleaseDC(NULL, hdc);
        g_sysTmHeight = tm.tmHeight;
        g_sysExtLead = tm.tmExternalLeading;
        GetWindowsDirectory(g_szIni, sizeof g_szIni - 16);
        lstrcat(g_szIni, "\\control.ini");
    }
    g_hDlg = CreateDialog(hInstMain, MAKEINTRESOURCE(100), hwndOwner, ColorDlgProc);
    if (!g_hDlg) return;
    if (hwndOwner) EnableWindow(hwndOwner, FALSE);
    while (g_hDlg) {
        /* (a failed WM_INITDIALOG leaves the dialog hidden; 3.1 would keep looping on it) */
        if (!IsWindowVisible(g_hDlg)) {
            DestroyWindow(g_hDlg);
            g_hDlg = 0;
            break;
        }
        if (!GetMessage(&msg, NULL, 0, 0)) {
            PostQuitMessage((int)msg.wParam);   /* (3.1 drops it) */
            break;
        }
        if (!IsDialogMessage(g_hDlg, &msg) && (!g_hCust || !IsDialogMessage(g_hCust, &msg))) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    if (hwndOwner) EnableWindow(hwndOwner, TRUE);
}

/* seg3:0371: the arrow bitmaps and the memory DC go when MAIN.CPL is unloaded */
void ColorExit(void)
{
    if (g_hbmUp) DeleteObject(g_hbmUp);
    if (g_hbmDn) DeleteObject(g_hbmDn);
    g_hbmUp = g_hbmDn = NULL;
    if (g_hdcMem) DeleteDC(g_hdcMem);
    g_hdcMem = NULL;
}
