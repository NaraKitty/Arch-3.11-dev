/*
 * Clipboard Viewer - native 64-bit port of the Windows 3.11 Clipboard Viewer (CLIPBRD.EXE 3.10.0.103,
 * one 11 KB code segment).
 *
 * Ported function by function from the disassembly of the user's own CLIPBRD.EXE
 * (tools/rip/arch311rip/disasm.py); each function notes its seg1 offset. The menu, accelerators,
 * strings and icon are loaded at run time from the user's ripped CLIPBRD.EXE; nothing Microsoft-made
 * is compiled into this file. USER's clipboard itself (the format table, delayed rendering, the
 * viewer chain, sharing between arch311 programs) is libw16's clipbrd.c.
 *
 * The viewer joins the clipboard viewer chain, shows the format picked in the Display menu (Auto:
 * the first of a fixed priority list the clipboard has) with its own scrolling, deletes the
 * clipboard's contents, and reads and writes .CLP files: WORD 0xC350, WORD count, then count 89-byte
 * entries {WORD format, DWORD length, DWORD offset, char name[79]} and the data. A file opened is
 * not read at once: its formats are promised (SetClipboardData with no handle) and rendered from
 * the file when asked for (WM_RENDERFORMAT), as 3.1 does.
 *
 * 64-bit notes: 3.1 passes a global handle in the low word of lParam for WM_SIZECLIPBOARD and
 * WM_PAINTCLIPBOARD and a far pointer for WM_ASKCBFORMATNAME; here lParam is the handle or pointer
 * (LPARAM is pointer-sized). The 16-bit structures inside .CLP files (BITMAP, METAFILEPICT) are read
 * and written byte by byte in their 3.1 layout.
 */
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h"

const char *w16_app_module = "CLIPBRD.EXE";

/* menu ids (menu 1) */
#define IDM_AUTO 0x400
#define IDM_DELETE 0x401
#define IDM_OPEN 0x402
#define IDM_SAVEAS 0x403
#define IDM_ABOUT 0x404
#define IDM_EXIT 0x405
#define IDM_SEARCHHELP 0x21
#define IDM_HELPONHELP 0xFFFC
#define IDM_INDEX 0xFFFF

/* strings */
#define IDS_APPNAME 100
#define IDS_NOTVALID 102
#define IDS_BINARY 103
#define IDS_CANTDELETE 104
#define IDS_SAVEFAIL 105
#define IDS_UNKNOWN 107
#define IDS_NOMEMDISP 108
#define IDS_HELPFILE 109
#define IDS_CLEARTITLE 110
#define IDS_CLEARTEXT 111
#define IDS_CANTOPEN 112
#define IDS_OPENCAP 114
#define IDS_SAVECAP 115
#define IDS_FILTER 116
#define IDS_ALLFILES 117
#define IDS_NOMEM 118
#define IDS_BADFILE 200 /* + OpenClipboardFile's error: 201 wrong format, 202 in use */

#define CLP_MAGIC 0xC350
#define CLP_ENTRY 0x59          /* WORD format, DWORD length, DWORD offset, char name[79] */
#define CLP_MAXFORMATS 100

static const char szClass[] = "Clipboard";   /* ds:0016, also the icon / accelerator name */

/* ------------------------------------------------------------------ globals (DGROUP) */
static BOOL fOwnFile;          /* [0x10] the clipboard's formats are promised from a .CLP file */
static BOOL fOwnerDisplay;     /* [0x12] showing CF_OWNERDISPLAY (the owner paints) */
static BOOL fLayoutDirty;      /* [0x14] work out the display rectangle at the next paint */
static HWND hwndNextViewer;    /* [0x20] */
static HMENU hDispMenu;        /* [0x22] the Display popup */
static LONG vRange = -1;       /* [0x24] content height beyond the window; -1 = not known yet */
static LONG vPos;              /* [0x28] */
static int hRange = -1;        /* [0x2c] */
static int hPos;               /* [0x2e] */
static UINT curFmt = IDM_AUTO; /* [0x30] the Display menu choice: a format, or IDM_AUTO */
/* [0x32] Auto shows the first of these the clipboard has */
static const UINT autoFormats[16] = {
    CF_OWNERDISPLAY, CF_DSPTEXT, CF_DSPBITMAP, CF_DSPMETAFILEPICT, CF_TEXT, CF_OEMTEXT, CF_METAFILEPICT,
    CF_BITMAP, CF_DIB, CF_PALETTE, CF_RIFF, CF_WAVE, CF_PENDATA, CF_SYLK, CF_DIF, CF_TIFF};
static int cPalChanges;        /* [0x62] */
/* the owner-display scroll bars, kept while another format is shown (16B7/16FB) */
static int vOwnMin, vOwnMax = 100, hOwnMin, hOwnMax = 100, vOwnPos, hOwnPos; /* [53c] [b0] [53e] [1a2] [1a0] [1a4] */
static OFSTRUCT ofsOpen;       /* [0xb2] the .CLP file the clipboard was opened from */
static char szNoMem[100];      /* [0x13a] */
static HINSTANCE hInst;        /* [0x19e] */
static UINT wHlpMsg;           /* [0x1a6] "commdlg_help" */
static char szHelpFile[20];    /* [0x1a8] */
static char szAllFiles[100];   /* [0x1bc] */
static OFSTRUCT ofsSave;       /* [0x2a2] */
static OPENFILENAME ofn;       /* [0x32a] */
static char szSaveCap[30];     /* [0x372] */
static RECT rcDisp;            /* [0x390] where the contents are drawn */
static int cxMaxChar;          /* [0x398] */
static char szTitle[40];       /* [0x39a] */
static int cyLine;             /* [0x3c2] tmHeight + tmExternalLeading */
static HMENU hMainMenu;        /* [0x3c4] */
static char szOpenCap[30];     /* [0x3c6] */
static HWND hwndMain;          /* [0x3e4] */
static int cxChar;             /* [0x3e6] tmAveCharWidth */
static HFONT hOemFont;         /* [0x3e8] OEM_FIXED_FONT */
static char szCustFilter[100]; /* [0x3ea] */
static int cxMargin;           /* [0x4ce] */
static HACCEL hAccel;          /* [0x4d0] */
static int cyMargin;           /* [0x4d2] */
static HBRUSH hbrBack;         /* [0x4d4] COLOR_WINDOW */
static HFONT hSysFont;         /* [0x4d6] SYSTEM_FONT */
static char szFilter[104];     /* [0x4d8] */

static WORD getw(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD getd(const BYTE *p) { return (DWORD)getw(p) | (DWORD)getw(p + 2) << 16; }
static void putw(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void putd(BYTE *p, DWORD v) { putw(p, (WORD)v); putw(p + 2, (WORD)(v >> 16)); }

/* ------------------------------------------------------------------ seg1:0010: the out-of-memory box */
static void OutOfMemory(void)
{
    MessageBeep(0);
    MessageBox(hwndMain, szNoMem, NULL, MB_ICONHAND);
}

static void ShowString(HDC hdc, int id);

/* ------------------------------------------------------------------ seg1:003C */
static BOOL MyOpenClipboard(HWND hwnd)
{
    RECT rc;
    if (OpenClipboard(hwnd)) return TRUE;
    HDC hdc = GetDC(hwnd);
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, hbrBack);
    ShowString(hdc, IDS_CANTOPEN);
    ReleaseDC(hwnd, hdc);
    return FALSE;
}

/* ------------------------------------------------------------------ seg1:00A2 */
static void SetCharDimensions(HWND hwnd, HFONT hFont)
{
    TEXTMETRIC tm;
    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, hFont);
    GetTextMetrics(hdc, &tm);
    ReleaseDC(hwnd, hdc);
    cxMaxChar = tm.tmMaxCharWidth;
    cyLine = tm.tmHeight + tm.tmExternalLeading;
    cxChar = tm.tmAveCharWidth;
    cxMargin = (WORD)cxChar >> 1;
    cyMargin = (WORD)cyLine >> 2;
}

/* ------------------------------------------------------------------ seg1:00F9: OEM text has its own font */
static void ChangeCharDimensions(HWND hwnd, UINT oldFmt, UINT newFmt)
{
    if (oldFmt == CF_OEMTEXT) {
        if (newFmt != CF_OEMTEXT) SetCharDimensions(hwnd, hSysFont);
    } else if (newFmt == CF_OEMTEXT)
        SetCharDimensions(hwnd, hOemFont);
}

/* ------------------------------------------------------------------ seg2 export: _lread of any size */
static DWORD ReadHuge(HFILE fh, BYTE *p, DWORD n)
{
    DWORD left = n;
    while (left > 0xF000) {
        if (_lread(fh, p, 0xF000) != 0xF000) return 0;
        left -= 0xF000;
        p += 0xF000;
    }
    if ((DWORD)_lread(fh, p, (UINT)left) != left) return 0;
    return n;
}

/* ------------------------------------------------------------------ seg1:2406: _lwrite of any size */
static DWORD WriteHuge(HFILE fh, const BYTE *p, DWORD n)
{
    DWORD left = n;
    while (left > 0xF000) {
        if (_lwrite(fh, p, 0xF000) != 0xF000) return 0;
        left -= 0xF000;
        p += 0xF000;
    }
    if ((DWORD)_lwrite(fh, p, (UINT)left) != left) return 0;
    return n;
}

/* ------------------------------------------------------------------ seg1:0127: one format from the file */
static BOOL RenderFormat(BYTE *ent, HFILE fh)
{
    UINT fmt = getw(ent);
    DWORD len = getd(ent + 2), off = getd(ent + 6), skip;
    HGLOBAL h;
    HANDLE hData;
    BYTE *p;

    if (fmt >= 0xC000) {
        fmt = RegisterClipboardFormat((LPCSTR)ent + 10);
        putw(ent, (WORD)fmt);
    }
    /* a bitmap's BITMAP and a picture's METAFILEPICT come before their bits */
    skip = fmt == CF_BITMAP ? 14 : fmt == CF_METAFILEPICT ? 8 : 0;
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, len - skip);
    if (!h) return FALSE;
    p = GlobalLock(h);
    if (!p) goto fail;
    _llseek(fh, (LONG)(off + skip), 0);
    if (!ReadHuge(fh, p, len - skip)) {
        GlobalUnlock(h);
        goto fail;
    }
    GlobalUnlock(h);
    hData = h;
    switch (fmt) {
    case CF_PALETTE: {
        p = GlobalLock(h);
        if (!p) goto fail;
        /* LOGPALETTE: palVersion, palNumEntries, PALETTEENTRYs - the same layout as libw16's */
        HPALETTE hpal = CreatePalette((const LOGPALETTE *)p);
        if (!hpal) {
            GlobalUnlock(h);
            goto fail;
        }
        GlobalUnlock(h);
        GlobalFree(h);
        hData = hpal;
        break;
    }
    case CF_BITMAP: {
        BYTE b[14];
        BITMAP bm;
        _llseek(fh, (LONG)off, 0);
        _lread(fh, b, 14);
        p = GlobalLock(h);
        if (!p) goto fail;
        memset(&bm, 0, sizeof bm);
        bm.bmType = (short)getw(b);
        bm.bmWidth = (short)getw(b + 2);
        bm.bmHeight = (short)getw(b + 4);
        bm.bmWidthBytes = (short)getw(b + 6);
        bm.bmPlanes = b[8];
        bm.bmBitsPixel = b[9];
        bm.bmBits = p;
        hData = CreateBitmapIndirect(&bm);
        GlobalUnlock(h);
        GlobalFree(h);
        break;
    }
    case CF_METAFILEPICT: {
        BYTE b[8];
        HMETAFILE hmf = SetMetaFileBits(h);
        if (!hmf) goto fail;
        HGLOBAL hp = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(METAFILEPICT));
        if (!hp) return FALSE;
        METAFILEPICT *mp = GlobalLock(hp);
        if (!mp) {
            GlobalFree(hp);
            return FALSE;
        }
        _llseek(fh, (LONG)off, 0);
        _lread(fh, b, 8);
        mp->mm = (short)getw(b);
        mp->xExt = (short)getw(b + 2);
        mp->yExt = (short)getw(b + 4);
        mp->hMF = hmf;
        GlobalUnlock(hp);
        hData = hp;
        break;
    }
    }
    if (!hData) return FALSE;
    SetClipboardData(fmt, hData);
    return TRUE;
fail:
    GlobalFree(h);
    return FALSE;
}

/* ------------------------------------------------------------------ seg1:0324: vertical scrolling */
static void VScroll(HWND hwnd, int code, int pos)
{
    LONG newPos, delta, rem;
    int page, step;

    UpdateWindow(hwnd);
    newPos = vPos;
    page = rcDisp.bottom - rcDisp.top;
    switch (code) {
    case SB_LINEUP: newPos -= (WORD)cyLine; break;
    case SB_LINEDOWN: newPos += (WORD)cyLine; break;
    case SB_PAGEUP:
    case SB_PAGEDOWN:
        step = (WORD)(page - cyLine);
        if ((WORD)step < (WORD)cyLine) step = (WORD)cyLine;
        newPos += code == SB_PAGEUP ? -(LONG)(short)step : (LONG)(short)step;
        break;
    case SB_THUMBPOSITION: newPos = (LONG)((WORD)pos) * vRange / 100; break;
    default: return;
    }
    if (newPos < 0 || vRange <= 0)
        newPos = 0;
    else if (vRange < newPos)
        newPos = vRange;
    else if ((rem = newPos % cyLine) != 0) {
        /* to the nearest whole line */
        if (rem > (LONG)((WORD)cyLine >> 1)) newPos += cyLine;
        newPos -= rem;
    }
    delta = vPos - newPos;
    if (delta == 0) return;
    vPos = newPos;
    if ((LONG)page > (delta < 0 ? -delta : delta))
        ScrollWindow(hwnd, 0, (short)delta, &rcDisp, &rcDisp);
    else
        InvalidateRect(hwnd, &rcDisp, TRUE);
    UpdateWindow(hwnd);
    SetScrollPos(hwnd, SB_VERT, vRange > 0 ? (int)(WORD)((DWORD)(newPos * 100) / (DWORD)vRange) : 0, TRUE);
}

/* ------------------------------------------------------------------ seg1:050D: horizontal scrolling */
static void HScroll(HWND hwnd, int code, int pos)
{
    int newPos = hPos, width = rcDisp.right - rcDisp.left, step, delta, d;

    switch (code) {
    case SB_LINEUP: newPos -= cxChar; break;
    case SB_LINEDOWN: newPos += cxChar; break;
    case SB_PAGEUP:
    case SB_PAGEDOWN:
        step = width - cxChar;
        if ((WORD)step < (WORD)cxChar) step = cxChar;
        if (code == SB_PAGEUP) newPos -= step; else newPos += step;
        break;
    case SB_THUMBPOSITION: newPos = (short)((LONG)((WORD)pos) * hRange / 100); break;
    default: return;
    }
    if (newPos < 0 || hRange <= 0)
        newPos = 0;
    else if (hRange < newPos)
        newPos = hRange;
    else {
        WORD rem = (WORD)newPos % (WORD)cxChar;
        if (rem) {
            if (rem > (WORD)((WORD)cxChar >> 1)) newPos += cxChar;
            newPos -= rem;
        }
    }
    delta = hPos - newPos;
    if (delta > 0) d = delta;
    else if (delta < 0) d = -delta;
    else return;
    hPos = newPos;
    if (rcDisp.right - rcDisp.left > d)
        ScrollWindow(hwnd, delta, 0, &rcDisp, &rcDisp);
    else
        InvalidateRect(hwnd, &rcDisp, TRUE);
    UpdateWindow(hwnd);
    SetScrollPos(hwnd, SB_HORZ, hRange > 0 ? (int)(WORD)((DWORD)(LONG)(newPos * 100) / (DWORD)(LONG)hRange) : 0, TRUE);
}

/* ------------------------------------------------------------------ seg1:0645: colour table bytes */
static int DibColorTableSize(const BYTE *bi)
{
    int bits;
    if (getd(bi) == 12) {                       /* BITMAPCOREHEADER: RGBTRIPLEs */
        bits = getw(bi + 10);
        return bits == 24 ? 0 : 3 << bits;
    }
    if (getd(bi + 0x20)) return (WORD)(getw(bi + 0x20) << 2);
    bits = getw(bi + 14);
    return bits == 24 ? 0 : 1 << (bits + 2);
}

/* ------------------------------------------------------------------ seg1:069A: a DIB's width and height */
static void DibDimensions(HGLOBAL hdib, int *w, int *h)
{
    const BYTE *bi = GlobalLock(hdib);
    if (!bi) return;
    if (getd(bi) == 12) {
        *w = (short)getw(bi + 4);
        *h = (short)getw(bi + 6);
    } else {
        *w = (short)getw(bi + 4);
        *h = (short)getw(bi + 8);
    }
    GlobalUnlock(hdib);
}

/* ------------------------------------------------------------------ seg1:06F9 */
static BOOL DrawDib(HDC hdc, int x, int y, HGLOBAL hdib)
{
    int w = 0, h = 0;
    if (!hdib) return FALSE;
    BYTE *bi = GlobalLock(hdib);
    if (!bi) return FALSE;
    DibDimensions(hdib, &w, &h);
    const BYTE *bits = bi + getw(bi) + DibColorTableSize(bi);
    SetDIBitsToDevice(hdc, x, y, w, h, 0, 0, 0, h, bits, (const BITMAPINFO *)bi, DIB_RGB_COLORS);
    GlobalUnlock(hdib);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0773: CF_DIB */
static BOOL ShowDib(HDC hdc, const RECT *prc, HGLOBAL hdib, int x, int y)
{
    int w = 0, h = 0;
    DibDimensions(hdib, &w, &h);
    if (vRange == -1) {
        vRange = (short)(rcDisp.top - rcDisp.bottom + h);
        if (vRange < 0) vRange = 0;
    }
    if (hRange == -1) {
        hRange = (short)(rcDisp.left - rcDisp.right + w);
        if (hRange < 0) hRange = 0;
    }
    SaveDC(hdc);
    IntersectClipRect(hdc, prc->left, prc->top, prc->right, prc->bottom);
    SetViewportOrg(hdc, prc->left - x, prc->top - y);
    DrawDib(hdc, 0, 0, hdib);
    RestoreDC(hdc, -1);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:081F: CF_BITMAP */
static BOOL ShowBitmap(HDC hdc, const RECT *prc, HBITMAP hbm, int x, int y)
{
    BITMAP bm;
    int w, h, cx, cy;
    HDC hdcMem = CreateCompatibleDC(hdc);
    if (!hdcMem) return FALSE;
    SelectObject(hdcMem, hbm);
    GetObject(hbm, sizeof bm, &bm);
    if (vRange == -1) {
        vRange = (short)(rcDisp.top - rcDisp.bottom + bm.bmHeight);
        if (vRange < 0) vRange = 0;
    }
    if (hRange == -1) {
        hRange = (short)(rcDisp.left - rcDisp.right + bm.bmWidth);
        if (hRange < 0) hRange = 0;
    }
    w = prc->right - prc->left;
    h = prc->bottom - prc->top;
    cx = w <= bm.bmWidth - x ? w : bm.bmWidth - x;
    cy = h <= bm.bmHeight - y ? h : bm.bmHeight - y;
    /* the last partial column and row start a pixel earlier (as 3.1 does) */
    if (cx != w && x > 0) x--;
    if (cy != h && y > 0) y--;
    BitBlt(hdc, prc->left, prc->top, cx, cy, hdcMem, x, y, SRCCOPY);
    DeleteDC(hdcMem);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:091B: CF_PALETTE, one framed
 * square per entry, a line high, as many to a row as fit */
static BOOL ShowPalette(HDC hdc, const RECT *prc, HPALETTE hpal, int x, int y)
{
    WORD n = 0;
    int perRow, rows, i, xs, ys;
    RECT r;
    if (!hpal) return FALSE;
    GetObject(hpal, 2, &n);
    perRow = (WORD)(rcDisp.right - rcDisp.left) / (WORD)cyLine;
    if (!perRow) perRow = 1;
    rows = (short)(n + perRow - 1) / perRow;
    if (vRange == -1) vRange = (WORD)(rows * cyLine - rcDisp.bottom + rcDisp.top);
    if (hRange == -1) hRange = 0;
    SaveDC(hdc);
    IntersectClipRect(hdc, prc->left, prc->top, prc->right, prc->bottom);
    SetViewportOrg(hdc, prc->left - x, prc->top - y);
    SelectPalette(hdc, hpal, FALSE);
    RealizePalette(hdc);
    ys = -cyLine;
    xs = 0;
    for (i = 0; (short)n > i; i++) {
        if (i % perRow == 0) {
            xs = 0;
            ys += cyLine;
        }
        r.left = xs;
        r.top = ys;
        r.right = xs + cyLine;
        r.bottom = ys + cyLine;
        if (RectVisible(hdc, &r)) {
            InflateRect(&r, -1, -1);
            FrameRect(hdc, &r, GetStockObject(BLACK_BRUSH));
            InflateRect(&r, -1, -1);
            HBRUSH hbr = CreateSolidBrush(PALETTEINDEX(i));
            FillRect(hdc, &r, hbr);
            DeleteObject(hbr);
        }
        xs += cyLine;
    }
    RestoreDC(hdc, -1);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0A9A: a picture extent in
 * device pixels, rounded up; 0 if it does not fit in 15 bits */
static int ScaleExtent(int hSize, int hRes, int ext, int mm)
{
    DWORD mul = 1, div = 1, r;
    if (!hSize) return 0;
    switch (mm) {
    case MM_TEXT: return ext;
    case MM_LOMETRIC: div = 10; break;          /* 0.1 mm */
    case MM_HIMETRIC: div = 100; break;         /* 0.01 mm */
    case MM_LOENGLISH: mul = 2540; div = 10000; break;
    case MM_HIENGLISH: mul = 254; div = 10000; break;
    case MM_TWIPS: mul = 254; div = 14400; break;
    default: return 0;
    }
    r = ((DWORD)((LONG)(short)hRes * (short)ext) * mul + (DWORD)(LONG)(short)hSize * div - 1) /
        ((DWORD)(LONG)(short)hSize * div);
    return r > 0x7FFF ? 0 : (int)r;
}

/* ------------------------------------------------------------------ seg1:0B46: CF_METAFILEPICT */
static BOOL ShowMetafile(HDC hdc, const RECT *prc, HGLOBAL hmfp, int x, int y)
{
    METAFILEPICT mfp;
    BOOL ret = FALSE;
    int cx, cy, save;
    const METAFILEPICT *p = GlobalLock(hmfp);
    if (!p) return FALSE;
    mfp = *p;
    GlobalUnlock(hmfp);
    if (!(save = SaveDC(hdc))) return FALSE;
    if (mfp.mm == MM_ISOTROPIC || mfp.mm == MM_ANISOTROPIC) {
        /* stretched to the window: nothing to scroll */
        vRange = 0;
        hRange = 0;
        cx = rcDisp.right - rcDisp.left;
        cy = rcDisp.bottom - rcDisp.top;
    } else {
        cx = ScaleExtent(GetDeviceCaps(hdc, HORZSIZE), GetDeviceCaps(hdc, HORZRES), mfp.xExt, mfp.mm);
        cy = ScaleExtent(GetDeviceCaps(hdc, VERTSIZE), GetDeviceCaps(hdc, VERTRES), mfp.yExt, mfp.mm);
        if (!cx || !cy) goto done;
        if (hRange == -1) {
            hRange = (short)(rcDisp.left - rcDisp.right + cx);
            if (hRange < 0) hRange = 0;
        }
        if (vRange == -1) {
            vRange = (short)(rcDisp.top - rcDisp.bottom + cy);
            if (vRange < 0) vRange = 0;
        }
    }
    IntersectClipRect(hdc, prc->left, prc->top, prc->right, prc->bottom);
    SetMapMode(hdc, mfp.mm);
    SetViewportOrg(hdc, prc->left - x, prc->top - y);
    switch (mfp.mm) {
    case MM_ISOTROPIC:
        if (mfp.xExt && mfp.yExt) SetWindowExt(hdc, mfp.xExt, mfp.yExt);
        /* fall through */
    case MM_ANISOTROPIC:
        SetViewportExt(hdc, cx, cy);
        break;
    }
    SetBrushOrg(hdc, x - prc->left, y - prc->top);
    ret = PlayMetaFile(hdc, mfp.hMF);
done:
    RestoreDC(hdc, save);
    return ret;
}

/* ------------------------------------------------------------------ seg1:0CFF: a message in the window */
static void ShowString(HDC hdc, int id)
{
    char sz[160];
    vPos = 0;
    hPos = 0;
    LoadString(hInst, id, sz, sizeof sz);
    FillRect(hdc, &rcDisp, hbrBack);
    DrawText(hdc, sz, -1, &rcDisp, DT_CENTER | DT_SINGLELINE);
}

/* ------------------------------------------------------------------ seg1:0D52: the next line of text,
 * tabs expanded to 8-column stops, broken at the width by characters; returns the bytes consumed
 * (a CR, LF or a pair of them ends the line) and puts the line's length in *pcch */
static int FormatLine(HDC hdc, char *buf, const char *p, int cchMax, int width, int *pcch)
{
    int in = 0, out = 0, n;
    WORD ext = 0;
    WORD guess = (WORD)width / (WORD)cxMaxChar;   /* surely fits without measuring */

    while (out < cchMax) {
        char c = p[in++];
        if (c == '\r' || c == '\n') {
            if (p[in] == '\r' || p[in] == '\n') in++;
            break;
        }
        if (c == 0) {
            in--;
            break;
        }
        if (c == '\t') {
            n = 8 - out % 8;
            if ((WORD)(n * cxChar + ext) > (WORD)width) break;
            if (n + out >= cchMax) break;
            while (n--) buf[out++] = ' ';
        } else
            buf[out++] = c;
        if ((short)guess > out) continue;
        ext = LOWORD(GetTextExtent(hdc, buf, out));
        if (ext == (WORD)width) break;
        if ((WORD)width >= ext) {
            guess += (WORD)((WORD)width - ext) / (WORD)cxMaxChar;
            continue;
        }
        out--;                                  /* the last character did not fit */
        in--;
        break;
    }
    *pcch = out;
    return in;
}

/* ------------------------------------------------------------------ seg1:0E4E: CF_TEXT / CF_OEMTEXT */
static void ShowText(HDC hdc, const RECT *prc, HGLOBAL h, WORD y)
{
    char buf[200];
    RECT rc = *prc;
    WORD rem;
    int width, nLines, nSkip, count = 0, ys, cch, lpp;
    const char *p;

    /* whole lines only */
    rc.top -= (WORD)(rc.top - rcDisp.top) % (WORD)cyLine;
    if ((rem = (WORD)(rc.bottom - rc.top) % (WORD)cyLine) != 0) {
        rc.bottom += cyLine - rem;
        if (rc.bottom > rcDisp.bottom) rc.bottom -= cyLine;
    }
    if (rc.top >= rc.bottom) return;
    width = rcDisp.right - rcDisp.left;
    if (!width || (nLines = (short)((WORD)(rc.bottom - rc.top) / (WORD)cyLine)) <= 0) {
        ShowString(hdc, IDS_NOTVALID);
        return;
    }
    if (!(p = GlobalLock(h))) {
        ShowString(hdc, IDS_NOTVALID);
        return;
    }
    /* skip the lines above */
    nSkip = y / (WORD)cyLine;
    while (*p) {
        if (nSkip-- == 0) break;
        p += FormatLine(hdc, buf, p, sizeof buf, width, &cch);
        count++;
    }
    ys = rc.top;
    for (;;) {
        if (nLines-- == 0) break;
        int used = FormatLine(hdc, buf, p, sizeof buf, width, &cch);
        TextOut(hdc, rc.left, ys, buf, cch);
        p += used;
        ys += cyLine;
        count++;
        if (!*p) break;
    }
    if (hRange == -1) hRange = 0;
    if (vRange == -1) {
        /* count the rest; the window then ends at a whole line */
        lpp = (WORD)(rcDisp.bottom - rcDisp.top) / (WORD)cyLine;
        do {
            count++;
            p += FormatLine(hdc, buf, p, sizeof buf, width, &cch);
        } while (*p);
        vRange = (WORD)((WORD)(count - lpp) * (WORD)cyLine);
        rcDisp.bottom = (short)(cyLine * lpp + rcDisp.top);
    }
    GlobalUnlock(h);
}

/* ------------------------------------------------------------------ seg1:106C */
static LRESULT SendOwner(UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hwndOwner = GetClipboardOwner();
    if (hwndOwner) return SendMessage(hwndOwner, msg, wParam, lParam);
    return 0;
}

/* ------------------------------------------------------------------ seg1:1094: WM_SIZECLIPBOARD */
static void SendOwnerSize(HWND hwnd, int left, int top, int right, int bottom)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(RECT));
    if (!h) return;
    RECT *r = GlobalLock(h);
    if (r) {
        r->top = top;
        r->bottom = bottom;
        r->left = left;
        r->right = right;
        GlobalUnlock(h);
        SendOwner(WM_SIZECLIPBOARD, (WPARAM)hwnd, (LPARAM)h);
    }
    GlobalFree(h);
}

/* ------------------------------------------------------------------ seg1:10FA: the format to show */
static UINT RealFormat(UINT fmt)
{
    if (fmt != IDM_AUTO) return fmt;
    for (int i = 0; i < 16; i++)
        if (IsClipboardFormatAvailable(autoFormats[i])) return autoFormats[i];
    return 0;
}

/* ------------------------------------------------------------------ seg1:1138: Edit > Delete (and
 * before File > Open): ask, then empty the clipboard; FALSE if the user said no */
static BOOL ClearClipboard(HWND hwnd)
{
    char szCaption[90], szText[160];
    if (CountClipboardFormats() <= 0) return TRUE;
    LoadString(hInst, IDS_CLEARTITLE, szCaption, sizeof szCaption);
    LoadString(hInst, IDS_CLEARTEXT, szText, sizeof szText);
    if (MessageBox(hwnd, szText, szCaption, MB_YESNO | MB_ICONEXCLAMATION) != IDYES) return FALSE;
    BOOL ok = OpenClipboard(hwnd);
    if (ok) {
        ok &= EmptyClipboard();
        ok &= CloseClipboard();
    }
    if (!ok) {
        /* (3.1 takes the caption from string 102) */
        LoadString(hInst, IDS_NOTVALID, szCaption, sizeof szCaption);
        LoadString(hInst, IDS_CANTDELETE, szText, sizeof szText);
        MessageBox(hwnd, szText, szCaption, MB_SYSTEMMODAL | MB_ICONHAND);
    }
    InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:1208: a format's name for the
 * Display menu and .CLP files */
static void GetFormatName(UINT fmt, LPSTR buf, int cb)
{
    *buf = 0;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb + 1);
    if (!h) return;
    LPSTR p = GlobalLock(h);
    if (p) {
        if ((fmt >= 1 && fmt <= 12) || (fmt >= CF_DSPTEXT && fmt <= CF_DSPMETAFILEPICT))
            LoadString(hInst, fmt, p, cb);
        else if (fmt == CF_OWNERDISPLAY) {
            /* the owner is asked, but 3.1 then loads its own "Owner display" over the answer */
            *p = 0;
            SendOwner(WM_ASKCBFORMATNAME, cb, (LPARAM)p);
            LoadString(hInst, fmt, p, cb);
        } else
            GetClipboardFormatName(fmt, p, cb);
        lstrcpy(buf, p);
        GlobalUnlock(h);
    }
    GlobalFree(h);
}

/* ------------------------------------------------------------------ seg1:12B3: draw one format */
static void DrawFormat(HDC hdc, RECT *prc, int x, WORD y, UINT fmt)
{
    BOOL ok = TRUE;
    UINT cfFileName = 0;
    HANDLE h;

    if (fmt == 0 && CountClipboardFormats()) {
        /* nothing Auto knows: a file name (File Manager) is shown as OEM text */
        cfFileName = RegisterClipboardFormat("FileName");
        if (!cfFileName || !IsClipboardFormatAvailable(cfFileName)) {
            ShowString(hdc, IDS_UNKNOWN);
            return;
        }
        fmt = CF_OEMTEXT;
    }
    h = GetClipboardData(cfFileName ? cfFileName : fmt);
    if (h) {
        switch (fmt) {
        case CF_TEXT:
        case CF_DSPTEXT:
            ShowText(hdc, prc, h, y);
            break;
        case CF_OEMTEXT: {
            HGDIOBJ hOld = SelectObject(hdc, hOemFont);
            ShowText(hdc, prc, h, y);
            SelectObject(hdc, hOld);
            break;
        }
        case CF_BITMAP:
        case CF_DSPBITMAP:
            ok = ShowBitmap(hdc, prc, h, x, y);
            break;
        case CF_DIB:
            ok = ShowDib(hdc, prc, h, x, y);
            break;
        case CF_PALETTE:
            ok = ShowPalette(hdc, prc, h, x, y);
            break;
        case CF_METAFILEPICT:
        case CF_DSPMETAFILEPICT:
            ok = ShowMetafile(hdc, prc, h, x, y);
            break;
        case CF_SYLK: case CF_DIF: case CF_TIFF:
        case CF_PENDATA: case CF_RIFF: case CF_WAVE:
            ShowString(hdc, IDS_BINARY);
            break;
        }
    } else if (CountClipboardFormats()) {
        ShowString(hdc, IDS_NOMEMDISP);
        return;
    }
    if (!ok) ShowString(hdc, IDS_NOTVALID);
}

/* ------------------------------------------------------------------ seg1:13FA: WM_PAINT's work */
static void PaintIt(HWND hwnd, PAINTSTRUCT *ps)
{
    HDC hdc = ps->hdc;
    RECT rcClient, r;
    UINT fmt;

    if (ps->fErase) FillRect(hdc, &ps->rcPaint, hbrBack);
    GetClientRect(hwnd, &rcClient);
    fmt = RealFormat(curFmt);
    fOwnerDisplay = fmt == CF_OWNERDISPLAY;
    if (fLayoutDirty) {
        CopyRect(&rcDisp, &rcClient);
        if (fOwnerDisplay)
            SendOwnerSize(hwnd, rcDisp.left, rcDisp.top, rcDisp.right, rcDisp.bottom);
        else
            InflateRect(&rcDisp, -cxMargin, -cyMargin);
        fLayoutDirty = FALSE;
    }
    if (fOwnerDisplay) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(PAINTSTRUCT));
        if (!h) return;
        PAINTSTRUCT *p = GlobalLock(h);
        if (!p) return;
        *p = *ps;
        IntersectRect(&p->rcPaint, &ps->rcPaint, &rcDisp);
        GlobalUnlock(h);
        SendOwner(WM_PAINTCLIPBOARD, (WPARAM)hwnd, (LPARAM)h);
        GlobalFree(h);
        return;
    }
    IntersectRect(&r, &ps->rcPaint, &rcDisp);
    r.left = rcDisp.left;
    if (r.top < r.bottom && r.left < r.right)
        DrawFormat(hdc, &r, hPos, (WORD)((WORD)vPos - rcDisp.top + r.top), fmt);
}

/* ------------------------------------------------------------------ seg1:154F: the Display menu lists
 * the clipboard's formats (those the viewer cannot show grayed) */
static void UpdateCBMenu(HWND hwnd)
{
    char sz[40];
    int n = CountClipboardFormats(), i;
    BOOL fNotFound;
    UINT fmt, flags;

    if (n == 0) {
        EnableMenuItem(hMainMenu, 2, MF_BYPOSITION | MF_GRAYED);
        EnableMenuItem(hMainMenu, IDM_DELETE, MF_GRAYED);
        EnableMenuItem(hMainMenu, IDM_SAVEAS, MF_GRAYED);
        DrawMenuBar(hwnd);
        return;
    }
    if (!hDispMenu) hDispMenu = GetSubMenu(GetMenu(hwnd), 2);
    int items = GetMenuItemCount(hDispMenu);
    for (i = 2; i < items; i++) ChangeMenu(hDispMenu, 2, NULL, 0, MF_DELETE | MF_BYPOSITION);
    fNotFound = TRUE;
    if (!OpenClipboard(hwnd)) {
        DrawMenuBar(hwnd);
        return;
    }
    fmt = 0;
    for (i = 1; i <= n; i++) {
        flags = 0;
        fmt = EnumClipboardFormats(fmt);
        GetFormatName(fmt, sz, sizeof sz);
        if (!((fmt >= CF_TEXT && fmt <= CF_METAFILEPICT) || (fmt >= CF_OEMTEXT && fmt <= CF_PALETTE) ||
              (fmt >= CF_OWNERDISPLAY && fmt <= CF_DSPMETAFILEPICT)))
            flags |= MF_GRAYED;
        flags |= MF_APPEND;
        if (curFmt == fmt) {
            fNotFound = FALSE;
            flags |= MF_CHECKED;
        }
        ChangeMenu(hDispMenu, 0, sz, fmt, flags);
    }
    CloseClipboard();
    if (fNotFound) {
        curFmt = IDM_AUTO;
        CheckMenuItem(hDispMenu, IDM_AUTO, MF_CHECKED);
    }
    EnableMenuItem(hMainMenu, 2, MF_BYPOSITION | MF_ENABLED);
    EnableMenuItem(hMainMenu, IDM_DELETE, MF_ENABLED);
    EnableMenuItem(hMainMenu, IDM_SAVEAS, MF_ENABLED);
    DrawMenuBar(hwnd);
}

/* ------------------------------------------------------------------ seg1:16B7: keep the owner's scroll bars */
static void SaveOwnerScroll(HWND hwnd)
{
    GetScrollRange(hwnd, SB_VERT, &vOwnMin, &vOwnMax);
    GetScrollRange(hwnd, SB_HORZ, &hOwnMin, &hOwnMax);
    vOwnPos = GetScrollPos(hwnd, SB_VERT);
    hOwnPos = GetScrollPos(hwnd, SB_HORZ);
}

/* ------------------------------------------------------------------ seg1:16FB */
static void RestoreOwnerScroll(HWND hwnd)
{
    SetScrollRange(hwnd, SB_VERT, vOwnMin, vOwnMax, FALSE);
    SetScrollRange(hwnd, SB_HORZ, hOwnMin, hOwnMax, FALSE);
    SetScrollPos(hwnd, SB_VERT, vOwnPos, TRUE);
    SetScrollPos(hwnd, SB_HORZ, hOwnPos, TRUE);
}

/* ------------------------------------------------------------------ seg1:1749 */
static void ResetOwnerScroll(void)
{
    hOwnMin = vOwnMin = 0;
    hOwnPos = vOwnPos = 0;
    vOwnMax = hOwnMax = 100;
}

/* ------------------------------------------------------------------ seg1:2114: formats a file can keep */
static BOOL IsWriteable(UINT fmt)
{
    return !((fmt >= 0x200 && fmt <= 0x2FF) || fmt == CF_OWNERDISPLAY);
}

/* ------------------------------------------------------------------ seg1:2137: promise a .CLP file's
 * formats; 1 = not a .CLP file, 2 = the clipboard cannot be opened */
static int OpenClipboardFile(HWND hwnd, HFILE fh)
{
    BYTE hdr[4], ent[CLP_ENTRY];
    WORD n;
    if (_lread(fh, hdr, 4) != 4) hdr[0] = hdr[1] = 0;   /* (3.1 checks what the buffer holds) */
    n = getw(hdr + 2);
    if (getw(hdr) != CLP_MAGIC || n > CLP_MAXFORMATS) return 1;
    if (!OpenClipboard(hwnd)) return 2;
    EmptyClipboard();
    for (WORD i = 0; i < n; i++) {
        _lread(fh, ent, CLP_ENTRY);
        UINT fmt = getw(ent);
        if (fmt >= 0xC000) {
            ent[CLP_ENTRY - 1] = 0;
            fmt = RegisterClipboardFormat((LPCSTR)ent + 10);
        }
        SetClipboardData(fmt, NULL);
    }
    if (n) fOwnFile = TRUE;
    CloseClipboard();
    return 0;
}

/* ------------------------------------------------------------------ seg1:21C6: File > Open */
static void OpenFileCmd(HWND hwnd)
{
    char szFile[128], sz[256];
    HFILE fh;
    int err;
    memset(szFile, 0, sizeof szFile);
    ofn.lpstrTitle = szOpenCap;
    ofn.lpstrFile = szFile;
    ofn.Flags = OFN_FILEMUSTEXIST;
    ofn.lpstrDefExt = "CLP";
    ofn.lpstrFilter = szFilter;
    ofn.lpstrCustomFilter = szCustFilter;
    /* (LockSegment / UnlockSegment around the dialog: nothing moves here) */
    if (GetOpenFileName(&ofn) && (fh = OpenFile(szFile, &ofsOpen, OF_PROMPT | OF_CANCEL | OF_READ)) > 0) {
        if (ClearClipboard(hwnd) && (err = OpenClipboardFile(hwnd, fh)) != 0) {
            LoadString(hInst, IDS_BADFILE + err, sz, sizeof sz);
            MessageBox(hwnd, sz, szClass, MB_ICONEXCLAMATION);
        }
        _lclose(fh);
    }
    if (CommDlgExtendedError()) OutOfMemory();
}

/* ------------------------------------------------------------------ seg1:2314: one directory entry */
static UINT WriteFormatHeader(HFILE fh, DWORD dirOff, DWORD dataOff, DWORD len, UINT fmt, LPCSTR name)
{
    /* 3.1 builds the entry on its stack: the bytes after the name's terminator are whatever was
     * there; here they are zero (UNTESTED against a file written by real 3.11) */
    BYTE ent[CLP_ENTRY];
    memset(ent, 0, sizeof ent);
    putw(ent, (WORD)fmt);
    putd(ent + 2, len);
    putd(ent + 6, dataOff);
    size_t n = strlen(name);
    memcpy(ent + 10, name, n < CLP_ENTRY - 11 ? n : CLP_ENTRY - 11);
    _llseek(fh, (LONG)dirOff, 0);
    return _lwrite(fh, ent, CLP_ENTRY);
}

/* ------------------------------------------------------------------ seg1:2496: one format's data;
 * returns the bytes written, 0 on failure */
static DWORD WriteFormat(HFILE fh, DWORD off, UINT fmt)
{
    HANDLE h = GetClipboardData(fmt);
    DWORD size;
    BYTE *p;
    if (!h) return 0;
    if ((DWORD)_llseek(fh, (LONG)off, 0) != off) return 0;
    switch (fmt) {
    case CF_METAFILEPICT: {
        METAFILEPICT *mp = GlobalLock(h);
        BYTE b[8];
        if (!mp) return 0;
        putw(b, (WORD)mp->mm);
        putw(b + 2, (WORD)mp->xExt);
        putw(b + 4, (WORD)mp->yExt);
        putw(b + 6, 0);   /* 3.1 writes the metafile's handle value here (UNTESTED what readers expect) */
        _lwrite(fh, b, 8);
        GlobalUnlock(h);
        HGLOBAL hBits = GetMetaFileBits(mp->hMF);
        size = GlobalSize(hBits);
        if (!(p = GlobalLock(hBits))) return 0;
        size = WriteHuge(fh, p, size);
        GlobalUnlock(hBits);
        if (size) size += 8;
        return size;
    }
    case CF_BITMAP: {
        BITMAP bm;
        BYTE b[14];
        GetObject(h, sizeof bm, &bm);
        size = (DWORD)(LONG)(short)bm.bmHeight * (DWORD)(WORD)(bm.bmPlanes * bm.bmWidthBytes);
        HGLOBAL hBits = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (!hBits) return 0;
        if (!(p = GlobalLock(hBits))) {
            GlobalFree(hBits);
            return 0;
        }
        GetBitmapBits(h, (LONG)size, p);
        putw(b, (WORD)bm.bmType);
        putw(b + 2, (WORD)bm.bmWidth);
        putw(b + 4, (WORD)bm.bmHeight);
        putw(b + 6, (WORD)bm.bmWidthBytes);
        b[8] = bm.bmPlanes;
        b[9] = bm.bmBitsPixel;
        putd(b + 10, 0);  /* bmBits as GetObject gives it */
        _lwrite(fh, b, 14);
        size = WriteHuge(fh, p, size);
        GlobalUnlock(hBits);
        GlobalFree(hBits);
        if (size) size += 14;
        return size;
    }
    case CF_PALETTE: {
        WORD n = 0;
        GetObject(h, 2, &n);
        size = ((DWORD)n + 2) << 2;
        HGLOBAL hPal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (!hPal) return 0;
        p = GlobalLock(hPal);
        if (!p)
            size = 0;
        else {
            putw(p, 0x300);
            putw(p + 2, n);
            if (!GetPaletteEntries(h, 0, n, (PALETTEENTRY *)(p + 4)))
                size = 0;
            else
                size = WriteHuge(fh, p, size);
            GlobalUnlock(hPal);
        }
        GlobalFree(hPal);
        return size;
    }
    default:
        size = GlobalSize(h);
        if (!(p = GlobalLock(h))) return 0;
        size = WriteHuge(fh, p, size);
        GlobalUnlock(h);
        return size;
    }
}

/* ------------------------------------------------------------------ seg1:2705: write a .CLP file, then
 * promise the clipboard from it */
static BOOL SaveClipboardToFile(HWND hwnd, LPSTR path)
{
    char szErr[160], szName[CLP_ENTRY - 10];
    BYTE hdr[4];
    WORD count = 0;
    DWORD dirOff = 4, dataOff, len;
    BOOL fErr = FALSE;
    UINT fmt;
    HFILE fh;
    HCURSOR hOld;

    if (!OpenClipboard(hwndMain)) return FALSE;
    fh = OpenFile(path, &ofsSave, OF_CREATE | OF_WRITE);
    if (fh <= 0) {
        CloseClipboard();
        return FALSE;
    }
    /* room for an entry per format, also for those not written */
    dataOff = (DWORD)(WORD)(CountClipboardFormats() * CLP_ENTRY) + 4;
    LoadString(hInst, IDS_SAVEFAIL, szErr, sizeof szErr);
    hOld = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ShowCursor(TRUE);
    fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
        if (!IsWriteable(fmt)) continue;
        GetFormatName(fmt, szName, sizeof szName);
        if (!(len = WriteFormat(fh, dataOff, fmt)) ||
            WriteFormatHeader(fh, dirOff, dataOff, len, fmt, szName) < CLP_ENTRY) {
            fErr = TRUE;
            break;
        }
        dirOff += CLP_ENTRY;
        dataOff += len;
        count++;
    }
    ShowCursor(FALSE);
    SetCursor(hOld);
    if (fErr) {
        MessageBox(hwnd, szErr, szClass, MB_ICONEXCLAMATION);
        CloseClipboard();
        _lclose(fh);
        return FALSE;
    }
    CloseClipboard();
    _llseek(fh, 0, 0);
    putw(hdr, CLP_MAGIC);
    putw(hdr + 2, count);
    _lwrite(fh, hdr, 4);
    _llseek(fh, 0, 0);
    ofsOpen = ofsSave;
    OpenClipboardFile(hwndMain, fh);
    _lclose(fh);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:28BE: File > Save As */
static void SaveAsCmd(HWND hwnd)
{
    char szFile[128];
    OFSTRUCT of;
    HFILE fh;
    memset(szFile, 0, sizeof szFile);
    ofn.lpstrTitle = szSaveCap;
    ofn.lpstrFile = szFile;
    ofn.Flags = OFN_NOREADONLYRETURN | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "CLP";
    ofn.lpstrFilter = szFilter;
    ofn.lpstrCustomFilter = szCustFilter;
    if (!GetSaveFileName(&ofn)) return;
    if ((fh = _lopen(szFile, OF_READ)) >= 0) {
        _lclose(fh);
        /* overwriting the file the clipboard is promised from: render everything first */
        if (lstrcmp(szFile, ofsOpen.szPathName) == 0) SendMessage(hwndMain, WM_RENDERALLFORMATS, 0, 0);
    }
    if (!SaveClipboardToFile(hwnd, szFile)) OpenFile(szFile, &of, OF_DELETE);
}

/* ------------------------------------------------------------------ seg1:1765: the window procedure */
LRESULT CALLBACK ClipbrdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UINT oldFmt, newFmt;
    HANDLE h;
    HDC hdc;
    HPALETTE hOldPal;
    PAINTSTRUCT ps;

    switch (msg) {
    case WM_CREATE:
        hMainMenu = GetMenu(hwnd);
        UpdateCBMenu(hwnd);
        break;

    case WM_RENDERALLFORMATS:
        if (!CountClipboardFormats() || !fOwnFile || !MyOpenClipboard(hwnd)) break;
        EmptyClipboard();
        /* fall through */
    case WM_RENDERFORMAT: {
        BYTE hdr[4], ent[CLP_ENTRY];
        DWORD off = 4;
        HFILE fh = OpenFile(NULL, &ofsOpen, OF_REOPEN);
        hdr[2] = hdr[3] = 0;
        _lread(fh, hdr, 4);
        for (WORD i = 0; i < getw(hdr + 2); i++) {
            _llseek(fh, (LONG)off, 0);
            if (_lread(fh, ent, CLP_ENTRY) < CLP_ENTRY) break;
            off += CLP_ENTRY;
            ent[CLP_ENTRY - 1] = 0;
            if (getw(ent) >= 0xC000) putw(ent, (WORD)RegisterClipboardFormat((LPCSTR)ent + 10));
            if (msg == WM_RENDERALLFORMATS || getw(ent) == (WORD)wParam) RenderFormat(ent, fh);
        }
        if (msg == WM_RENDERALLFORMATS) CloseClipboard();
        _lclose(fh);
        break;
    }

    case WM_DESTROYCLIPBOARD:
        fOwnFile = FALSE;
        break;

    case WM_COMMAND:
        switch (wParam) {
        case IDM_DELETE: ClearClipboard(hwnd); break;
        case IDM_EXIT: SendMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); break;
        case IDM_ABOUT:
            if (ShellAbout(hwnd, szTitle, "", LoadIcon(hInst, szClass)) == -1) OutOfMemory();
            break;
        case IDM_OPEN: OpenFileCmd(hwnd); break;
        case IDM_SAVEAS: SaveAsCmd(hwnd); break;
        case IDM_HELPONHELP:
            if (!WinHelp(hwnd, NULL, HELP_HELPONHELP, 0)) OutOfMemory();
            break;
        case IDM_INDEX:
            if (!WinHelp(hwnd, szHelpFile, HELP_INDEX, 0)) OutOfMemory();
            break;
        case IDM_SEARCHHELP:
            /* (3.1 passes a far pointer to ""; libw16's DWORD cannot hold a pointer, and the
             * empty key is what an absent one means) */
            if (!WinHelp(hwnd, szHelpFile, HELP_PARTIALKEY, 0)) OutOfMemory();
            break;
        default:
            if (!((wParam >= 1 && wParam <= 12) || (wParam >= CF_OWNERDISPLAY && wParam <= CF_DSPMETAFILEPICT) ||
                  wParam == IDM_AUTO))
                return DefWindowProc(hwnd, msg, wParam, lParam);
            /* a Display menu choice */
            if (curFmt == wParam) break;
            CheckMenuItem(hDispMenu, curFmt, MF_UNCHECKED);
            CheckMenuItem(hDispMenu, (UINT)wParam, MF_CHECKED);
            DrawMenuBar(hwnd);
            oldFmt = RealFormat(curFmt);
            newFmt = RealFormat((UINT)wParam);
            if (newFmt == oldFmt) {
                curFmt = (UINT)wParam;
                break;
            }
            ChangeCharDimensions(hwnd, oldFmt, newFmt);
            fLayoutDirty = TRUE;
            curFmt = (UINT)wParam;
            if (oldFmt == CF_OWNERDISPLAY) {
                SaveOwnerScroll(hwnd);
                goto newcontents;
            }
            if (newFmt != CF_OWNERDISPLAY) goto newcontents;
            RestoreOwnerScroll(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        break;

    case WM_CHANGECBCHAIN:
        if (!hwndNextViewer) break;
        if (hwndNextViewer == (HWND)wParam) {
            /* (3.1: LOWORD(lParam); libw16 passes the whole handle) */
            hwndNextViewer = (HWND)lParam;
            return 1;
        }
        return SendMessage(hwndNextViewer, WM_CHANGECBCHAIN, wParam, lParam);

    case WM_KEYDOWN: {
        UINT m, code;
        switch (wParam) {
        case VK_UP: m = WM_VSCROLL; code = SB_LINEUP; break;
        case VK_DOWN: m = WM_VSCROLL; code = SB_LINEDOWN; break;
        case VK_PRIOR: m = WM_VSCROLL; code = SB_PAGEUP; break;
        case VK_NEXT: m = WM_VSCROLL; code = SB_PAGEDOWN; break;
        case VK_LEFT: m = WM_HSCROLL; code = SB_LINEUP; break;
        case VK_RIGHT: m = WM_HSCROLL; code = SB_LINEDOWN; break;
        case VK_TAB: m = WM_HSCROLL; code = GetKeyState(VK_SHIFT) < 0 ? SB_PAGEUP : SB_PAGEDOWN; break;
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        SendMessage(hwnd, m, code, 0);
        break;
    }

    case WM_SIZE:
        fLayoutDirty = TRUE;
        if (wParam == SIZE_MINIMIZED) {
            if (fOwnerDisplay) SendOwnerSize(hwnd, 0, 0, 0, 0);
            break;
        }
        if (fOwnerDisplay) {
            SendOwnerSize(hwnd, 0, 0, LOWORD(lParam), HIWORD(lParam));
            break;
        }
        goto resetscroll;

    case WM_DESTROY:
        ChangeClipboardChain(hwnd, hwndNextViewer);
        if (fOwnerDisplay) SendOwnerSize(hwnd, 0, 0, 0, 0);
        DeleteObject(hbrBack);
        WinHelp(hwnd, szHelpFile, HELP_QUIT, 0);
        PostQuitMessage(0);
        break;

    case WM_DRAWCLIPBOARD:
        fLayoutDirty = TRUE;
        if (hwndNextViewer) SendMessage(hwndNextViewer, WM_DRAWCLIPBOARD, wParam, lParam);
        oldFmt = RealFormat(curFmt);
        UpdateCBMenu(hwnd);
        ChangeCharDimensions(hwnd, oldFmt, newFmt = RealFormat(curFmt));
        ResetOwnerScroll();
    newcontents:
        InvalidateRect(hwnd, NULL, TRUE);
    resetscroll:
        hRange = -1;
        vRange = -1;
        SetScrollRange(hwnd, SB_VERT, 0, 100, FALSE);
        vPos = 0;
        SetScrollPos(hwnd, SB_VERT, 0, TRUE);
        SetScrollRange(hwnd, SB_HORZ, 0, 100, FALSE);
        hPos = 0;
        SetScrollPos(hwnd, SB_HORZ, 0, TRUE);
        break;

    case WM_QUERYNEWPALETTE: {
        if (!MyOpenClipboard(hwnd)) break;
        h = GetClipboardData(CF_PALETTE);
        CloseClipboard();
        if (!h) break;
        hdc = GetDC(hwnd);
        hOldPal = SelectPalette(hdc, h, FALSE);
        UINT n = RealizePalette(hdc);
        SelectPalette(hdc, hOldPal, FALSE);
        ReleaseDC(hwnd, hdc);
        if (!n) break;
        InvalidateRect(hwnd, NULL, TRUE);
        cPalChanges = 0;
        return 1;
    }

    case WM_PALETTECHANGED:
        if (hwnd == (HWND)wParam || !IsClipboardFormatAvailable(CF_PALETTE) || !MyOpenClipboard(hwnd)) break;
        h = GetClipboardData(CF_PALETTE);
        CloseClipboard();
        if (!h) break;
        hdc = GetDC(hwnd);
        hOldPal = SelectPalette(hdc, h, FALSE);
        if (RealizePalette(hdc)) {
            UpdateColors(hdc);
            cPalChanges++;
        }
        SelectPalette(hdc, hOldPal, FALSE);
        ReleaseDC(hwnd, hdc);
        break;

    case WM_PAINT:
        if (cPalChanges > 1) {
            cPalChanges = 0;
            InvalidateRect(hwnd, NULL, TRUE);
        }
        BeginPaint(hwnd, &ps);
        hdc = ps.hdc;
        if (MyOpenClipboard(hwnd)) {
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            hOldPal = NULL;
            if ((h = GetClipboardData(CF_PALETTE)) != NULL) {
                hOldPal = SelectPalette(hdc, h, FALSE);
                RealizePalette(hdc);
            }
            PaintIt(hwnd, &ps);
            if (h) SelectPalette(hdc, hOldPal, FALSE);
            CloseClipboard();
        }
        EndPaint(hwnd, &ps);
        break;

    case WM_VSCROLL:
        if (wParam == SB_THUMBTRACK) break;
        if (fOwnerDisplay)
            SendOwner(WM_VSCROLLCLIPBOARD, (WPARAM)hwnd, MAKELPARAM(wParam, LOWORD(lParam)));
        else
            VScroll(hwnd, (int)wParam, LOWORD(lParam));
        break;

    case WM_HSCROLL:
        if (wParam == SB_THUMBTRACK) break;
        if (fOwnerDisplay)
            SendOwner(WM_HSCROLLCLIPBOARD, (WPARAM)hwnd, MAKELPARAM(wParam, LOWORD(lParam)));
        else
            HScroll(hwnd, (int)wParam, LOWORD(lParam));
        break;

    case WM_SYSCOLORCHANGE:
        DeleteObject(hbrBack);
        hbrBack = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* ------------------------------------------------------------------ seg1:1DF4 */
static BOOL InitClass(void)
{
    WNDCLASS wc;
    hAccel = LoadAccelerators(hInst, szClass);
    if (!hAccel) return FALSE;
    hbrBack = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    memset(&wc, 0, sizeof wc);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hInst, szClass);
    wc.lpszClassName = szClass;
    wc.hbrBackground = NULL;                    /* WM_PAINT fills with hbrBack */
    wc.style = CS_BYTEALIGNCLIENT | CS_HREDRAW | CS_VREDRAW;
    wc.lpszMenuName = MAKEINTRESOURCE(1);
    wc.hInstance = hInst;
    wc.lpfnWndProc = ClipbrdWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    return RegisterClass(&wc);
}

/* ------------------------------------------------------------------ seg1:1E81: WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    char *f;
    (void)lpCmdLine;

    hInst = hInstance;
    LoadString(hInst, IDS_APPNAME, szTitle, 40);
    LoadString(hInst, IDS_HELPFILE, szHelpFile, 20);
    LoadString(hInst, IDS_OPENCAP, szOpenCap, 30);
    LoadString(hInst, IDS_SAVECAP, szSaveCap, 30);
    LoadString(hInst, IDS_FILTER, szFilter, 100);
    LoadString(hInst, IDS_ALLFILES, szAllFiles, 100);
    LoadString(hInst, IDS_NOMEM, szNoMem, 100);
    if (hPrev) {
        /* never in arch311 (one process per program); 3.1 takes the window from the previous
         * instance's data segment (GetInstanceData) */
        HWND hwndPrev = FindWindow(szClass, NULL);
        if (IsIconic(hwndPrev)) ShowWindow(hwndPrev, SW_RESTORE);
        else SetActiveWindow(hwndPrev);
        return 0;
    }
    if (!InitClass()) return 0;
    hwndMain = CreateWindow(szClass, szTitle, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, CW_USEDEFAULT, 0,
                            GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2, NULL, NULL, hInst,
                            NULL);
    hDispMenu = GetSubMenu(GetMenu(hwndMain), 2);
    hSysFont = GetStockObject(SYSTEM_FONT);
    hOemFont = GetStockObject(OEM_FIXED_FONT);
    SetCharDimensions(hwndMain, hSysFont);
    hwndNextViewer = SetClipboardViewer(hwndMain);

    /* the filter: "Clipbrd Files (*.CLP)", "*.CLP", "All Files (*.*)", "*.*" */
    f = szFilter + lstrlen(szFilter) + 1;
    lstrcpy(f, "*.CLP");
    f += lstrlen(f) + 1;
    lstrcpy(f, szAllFiles);
    f += lstrlen(f) + 1;
    lstrcpy(f, "*.*");
    f += lstrlen(f) + 1;
    *f = 0;
    szCustFilter[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwndMain;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxCustFilter = 100;
    ofn.nFilterIndex = 1;
    ofn.nMaxFile = 128;
    ofn.lpfnHook = NULL;
    ofn.Flags = 0;
    wHlpMsg = RegisterWindowMessage("commdlg_help");
    if (!wHlpMsg) return 0;
    ShowWindow(hwndMain, nCmdShow);
    /* (3.1 calls Pen Windows' RegisterPenApp when SM_PENWINDOWS is set; there is none) */
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateAccelerator(hwndMain, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
