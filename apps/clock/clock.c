/*
 * Clock - native 64-bit port of the Windows 3.11 Clock (CLOCK.EXE 3.10, 16,416 bytes).
 *
 * Ported function by function from the disassembly of the user's own CLOCK.EXE (one code segment;
 * tools/rip/arch311rip/disasm.py). Each function notes its seg1 offset. The menu, the strings, the
 * icon and the circle table (resource "CLOCK" of type "data": 60 points on a circle of radius 8000)
 * are loaded at run time from the user's ripped CLOCK.EXE; nothing Microsoft-made is compiled in.
 *
 * Differences from the original, all deliberate:
 *  - the time comes from libw16's DOS clock (w16_dos_gettime / w16_dos_getdate: local time, or
 *    ARCH311_CLOCK for tests), as CLOCK.EXE read it with INT 21h AH=2Ch / AH=2Ah;
 *  - WinMain waits for the next second as 3.1 did, but sleeps between polls instead of spinning;
 *  - a second Clock cannot hand over to the first: libw16 runs every program in its own process and
 *    never passes a previous instance (the hPrevInstance branch of WinMain is kept, unreachable);
 *  - Pen Windows' RegisterPenApp is not called (libw16 has no Pen Windows: SM_PENWINDOWS is 0);
 *  - Set Font... calls libw16's ChooseFont, which does not open the font dialog yet (it returns
 *    FALSE, as when the user cancels), so the font stays as CLOCK.INI sFont names it.
 * 3.1's 16-bit arithmetic is kept where it shows: halving a negative width as an unsigned word.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "w16.h"
#include "commdlg.h"

const char *w16_app_module = "CLOCK.EXE";

/* menu ids (menu "CLOCK") and the item WinMain appends to the system menu */
#define IDM_ANALOG 1
#define IDM_DIGITAL 2
#define IDM_SETFONT 3
#define IDM_ABOUT 4
#define IDM_TOPMOST 5
#define IDM_NOTITLE 6
#define IDM_SECONDS 7
#define IDM_DATE 8

/* string resources */
#define IDS_APPNAME 2       /* "Clock" */
#define IDS_DATA 4          /* "data": type of the circle table resource */
#define IDS_TOOMANY 6       /* "Not enough timing resources for Clock..." */
#define IDS_DEFFONT 8       /* "arial" */
#define IDS_TOPMOST 9       /* "Always on &Top" */
#define IDS_INTL 10
#define IDS_ITIME 12
#define IDS_S1159 14
#define IDS_S2359 16
#define IDS_STIME 18
#define IDS_FONTKEY 22      /* "sFont" */
#define IDS_SSHORTDATE 24
#define IDS_DEFSHORTDATE 25 /* "MM/dd/yy" */
#define IDS_DEF1159 26      /* "" */
#define IDS_DEF2359 28      /* "" */
#define IDS_DEFTIME 30      /* ":" */
#define IDS_SECTION 32      /* "Clock": class, menu and window name, CLOCK.INI section */
#define IDS_INIFILE 34      /* "clock.ini" */
#define ID_FONTDLG 100      /* the ChooseFont template in CLOCK.EXE */

#define ANALOG 1
#define DIGITAL 2
#define TIMER_ID 1          /* [0x1c] */
#define CIRCLE 8000         /* radius of the circle table */
#define HOURSIDE 7          /* fat hands: half width, tip and tail in percent of the radius */
#define MINUTESIDE 5
#define HOURHAND 65
#define MINUTEHAND 80
#define SECONDHAND 80
#define HOURTAIL 15
#define MINUTETAIL 20

/* ds:0196: what seg1:2928 (DOS 2Ch) and seg1:2948 fill in */
typedef struct {
    int hour12m; /* hour % 12 */
    int hour12;  /* 1..12 */
    int hour;    /* 0..23 */
    int minute, second;
    int pm;
} TIME;

/* ds:021E: seg1:2971 (DOS 2Ah) */
typedef struct { int day, month, year; } DATE;

/* ds:00FE: the six CLOCK.INI Options, in their order */
typedef struct {
    int mode;           /* ANALOG / DIGITAL */
    BOOL fIconic;       /* also the WM_SIZE state */
    BOOL fNoSeconds;
    BOOL fNoTitle;
    BOOL fTopmost;
    BOOL fNoDate;
} OPTIONS;

typedef struct { int x, y; } CIRPT;

/* ------------------------------------------------------------------ globals (DGROUP) */
static BOOL fScaleCircle = TRUE;   /* [0x10] (never cleared) */
static BOOL fColorDisplay = TRUE;  /* [0x12] (set, never read) */
static BOOL fRecalc = TRUE;        /* [0x14] the digital layout must be redone */
static HFONT hFontTime;            /* [0x16] */
static HFONT hFontDate;            /* [0x18] */
static int iTLZero;                /* [0x1a] */
static int fFlatText;              /* [0x44] bit 0: mono display, bit 1: CLOCK.INI NoShadow */
static BOOL fHidden;               /* [0x74] hidden while the right button is down (on top) */
static BOOL fInMenu;               /* [0x76] */
static HMENU hMenuSaved;           /* [0xd0] the menu while the title is off */
static char szAppName[30];         /* [0xe0] */
static OPTIONS opt;                /* [0xfe] */
static int yTime, xTime, xSep1, xMin, xSep2, xSec, xAmPm; /* [0x10a]..[0x116] */
static int cxDigit, cxSep, cyTime, cxAmPm;                 /* [0x118]..[0x11e] */
static int xDate, yDate, cxDate, cyDate;                   /* [0x120]..[0x126] */
static int cShadow;                /* [0x128] offset of the 3-D text */
static HBITMAP hbmDigits;          /* [0x12a] */
static char szAmPm[2][7];          /* [0x12c] s1159, [0x133] s2359 */
static int iTime;                  /* [0x13a] */
static char szShortDate[14];       /* [0x13c] */
static char szDate[32];            /* [0x14b] */
static int cchDate;                /* [0x15a] */
static char szTimeTmpl[24];        /* [0x15c] "88:88:88 MM" */
static int cchTime;                /* [0x16b] */
static char chTimeSep;             /* [0x16d] */
static char szSection[30];         /* [0x16e] */
static HINSTANCE hInst;            /* [0x18c] */
static HPEN hpenShadow;            /* [0x18e] */
static HPEN hpenFore;              /* [0x190] */
static HWND hwndClock;             /* [0x192] */
static HBRUSH hbrFace;             /* [0x194] */
static TIME oTime;                 /* [0x196] the time on the screen */
static HGLOBAL hCirTab;            /* [0x1a2] */
static HCURSOR hcurWait;           /* [0x1a4] */
static int clockRadius;            /* [0x1a6] */
static RECT clockRect;             /* [0x1a8] */
static LOGFONT FontStruct;         /* [0x1b0] */
static char szFontKey[20];         /* [0x1e2] */
static RECT rcPos;                 /* [0x1f6] */
static char szIniFile[20];         /* [0x1fe] */
static HPEN hpenBackground;        /* [0x212] */
static int clockCenterX, clockCenterY; /* [0x214], [0x216] */
static HBRUSH hbrShadow;           /* [0x218] */
static HBRUSH hbrHour;             /* [0x21a] */
static HPEN hpenRed;               /* [0x21c] */
static DATE oDate;                 /* [0x21e] the date on the screen */
static int vertRes;                /* [0x224] */
static char szTitle[30];           /* [0x226] */
static int horzRes;                /* [0x244] */
static int aspectH, aspectV;       /* [0x246], [0x248]: pixels per 100 mm across and down */
static CIRPT cirTab[60];           /* [0x24a]: the locked resource (here a copy, scaled in place) */
static HPEN hpenCyan;              /* [0x24e] */

static void ClockPaint(HWND hwnd, HDC hdc, int fUpdate);

/* ------------------------------------------------------------------ seg1:2928 / 2948 / 2971: DOS clock */
static void GetTime(TIME *t) /* INT 21h AH=2Ch */
{
    w16_dos_gettime(&t->hour, &t->minute, &t->second, NULL);
}

static void ConvertTime(TIME *t)
{
    int h = t->hour;
    t->pm = 0;
    if (h >= 12) {
        t->pm = 1;
        h -= 12;
    }
    t->hour12m = h;
    t->hour12 = h ? h : 12;
}

static void GetDate(DATE *d) /* INT 21h AH=2Ah */
{
    w16_dos_getdate(&d->year, &d->month, &d->day, NULL);
}

/* ------------------------------------------------------------------ seg1:0010 / 00B8: pens and brushes */
static void CreateTools(void)
{
    hbrShadow = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
    hbrFace = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    hbrHour = CreateSolidBrush(RGB(0, 128, 128));
    hpenFore = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWTEXT));
    hpenShadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
    hpenBackground = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNFACE));
    hpenCyan = CreatePen(PS_SOLID, 1, RGB(0, 255, 255));
    hpenRed = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    hcurWait = LoadCursor(NULL, IDC_WAIT);
}

static void DeleteTools(void)
{
    DeleteObject(hbrShadow);
    DeleteObject(hbrFace);
    DeleteObject(hbrHour);
    DeleteObject(hpenFore);
    DeleteObject(hpenShadow);
    DeleteObject(hpenBackground);
    DeleteObject(hpenCyan);
    DeleteObject(hpenRed);
}

/* ------------------------------------------------------------------ seg1:0107: the date string */
/* WIN.INI sShortDate with its d / dd, M / MM, yy / yyyy fields; separators are copied but never
 * lead or trail. fShort (the icon) leaves the year out. */
static void FormatDate(const DATE *pd, BOOL fShort)
{
    int i = 0, o = 0;
    while (szShortDate[i] && o < 14) {
        BOOL fLZ = FALSE;
        char c = szShortDate[i++];
        switch (c) {
        case 'd':
            if (szShortDate[i] == 'd') { fLZ = TRUE; i++; }
            if (fLZ || pd->day / 10) szDate[o++] = pd->day / 10 + '0';
            szDate[o++] = pd->day % 10 + '0';
            break;
        case 'M':
            if (szShortDate[i] == 'M') { fLZ = TRUE; i++; }
            if (fLZ || pd->month / 10) szDate[o++] = pd->month / 10 + '0';
            szDate[o++] = pd->month % 10 + '0';
            break;
        case 'y':
            i++; /* "yy": the second y */
            if (szShortDate[i] == 'y') { fLZ = TRUE; i += 2; }
            if (fLZ && !fShort) {
                szDate[o++] = pd->year >= 2000 ? '2' : '1';
                szDate[o++] = pd->year >= 2000 ? '0' : '9';
            }
            if (!fShort) {
                szDate[o++] = pd->year % 100 / 10 + '0';
                szDate[o++] = pd->year % 100 % 10 + '0';
            }
            break;
        default:
            if (o) szDate[o++] = c;
            break;
        }
    }
    while (o > 0 && (szDate[o - 1] < '0' || szDate[o - 1] > '9')) o--;
    szDate[o] = 0;
    cchDate = o;
}

/* ------------------------------------------------------------------ seg1:0277: the time template */
/* the widest the digital time can be: "88:88" (icon), else "88:88:88" or "88:88", a blank and as many
 * Ms as the longer of s1159 / s2359 has characters */
static void BuildTimeTemplate(void)
{
    wsprintf(szTimeTmpl, "88%c88", chTimeSep);
    if (!opt.fIconic) {
        if (!opt.fNoSeconds) wsprintf(szTimeTmpl + 5, "%c88", chTimeSep);
        lstrcat(szTimeTmpl, " ");
        int n = lstrlen(lstrlen(szAmPm[1]) >= lstrlen(szAmPm[0]) ? szAmPm[1] : szAmPm[0]);
        for (; n > 0; n--) lstrcat(szTimeTmpl, "M");
    }
    cchTime = lstrlen(szTimeTmpl);
    if (szTimeTmpl[cchTime - 1] == ' ') szTimeTmpl[--cchTime] = 0;
}

/* ------------------------------------------------------------------ seg1:0333: digital layout */
/* The largest font (CLOCK.INI sFont, default Arial) in which the time template fits in 7/8 of the
 * width and 3/8 of the height (7/8 without the date), then the date in a font at most 3/4 as high. */
static void SizeFont(HWND hwnd, int cy, int cx)
{
    int iAmPm = 0;   /* [bp-0x8a] */
    int aw[24];      /* [bp-0xa8 + 2(i-1)]: extent of the first i characters */
    memset(aw, 0, sizeof aw);
    if (opt.mode != DIGITAL) return;
    HCURSOR hcurOld = SetCursor(hcurWait);
    HDC hdc = GetDC(hwnd);
    if (hFontTime) DeleteObject(hFontTime);
    if (opt.fIconic) { cx -= 4; cy -= 4; }
    int cxMax = cx * 7 / 8;
    int cyMax = (!opt.fNoDate && !opt.fIconic) ? cy * 3 / 8 : cy * 7 / 8;
    if (cyMax < 3) cyMax = 3;
    if (opt.fIconic) { cx += 4; cy += 4; }
    const int nBig = 1000;
    FontStruct.lfWidth = 0;
    FontStruct.lfHeight = -nBig;
    hFontTime = CreateFontIndirect(&FontStruct);
    HGDIOBJ hOld = hFontTime ? SelectObject(hdc, hFontTime) : NULL;
    DWORD ext = GetTextExtent(hdc, szTimeTmpl, cchTime);
    DWORD extDate = 0;
    if (!opt.fNoDate && !opt.fIconic) extDate = GetTextExtent(hdc, szDate, cchDate);
    if (hOld) SelectObject(hdc, hOld);
    FontStruct.lfHeight = -MulDiv(nBig, cxMax, LOWORD(ext));
    if (-cyMax > FontStruct.lfHeight) FontStruct.lfHeight = -cyMax;
    else if (FontStruct.lfHeight > -3) FontStruct.lfHeight = -3;
    while (FontStruct.lfHeight <= -3) {
        if (hFontTime) DeleteObject(hFontTime);
        hFontTime = CreateFontIndirect(&FontStruct);
        hOld = hFontTime ? SelectObject(hdc, hFontTime) : NULL;
        DWORD prev = 0;
        int nFit = 0;
        for (int i = 1; i <= cchTime; i++) {
            ext = GetTextExtent(hdc, szTimeTmpl, i);
            if (LOWORD(ext) > (WORD)cxMax) {
                ext = prev;
                break;
            }
            aw[i] = LOWORD(ext);
            prev = ext;
            nFit++;
        }
        if (hOld) SelectObject(hdc, hOld);
        if (nFit == cchTime) break;
        FontStruct.lfHeight += 2;
    }
    cxDigit = aw[1];
    cxSep = aw[3] - aw[2];
    xTime = (WORD)(cx - LOWORD(ext)) >> 1; /* 3.1: shr of the 16-bit difference */
    xSep1 = xTime + aw[2];
    xMin = xTime + aw[3];
    if (!opt.fIconic) {
        if (!opt.fNoSeconds) {
            xSep2 = xTime + aw[5];
            xSec = xTime + aw[6];
            if (cchTime > 8) iAmPm = 8;
        } else if (cchTime > 5)
            iAmPm = 5;
        if (iAmPm) {
            xAmPm = aw[iAmPm + 1] + xTime;
            cxAmPm = LOWORD(ext) - aw[iAmPm + 1];
        } else
            xAmPm = cxAmPm = 0;
    }
    cyTime = HIWORD(ext);
    cShadow = cyTime + cxDigit >= 90 ? GetSystemMetrics(SM_CXBORDER) * 2 : 0;
    if (hbmDigits) DeleteObject(hbmDigits);
    hbmDigits = CreateDiscardableBitmap(hdc, (cShadow + cxDigit) * 2, cyTime);
    if (!opt.fNoDate && !opt.fIconic) {
        int cyMaxDate = -(FontStruct.lfHeight * 3 / 4);
        if (cyMaxDate < 2) cyMaxDate = 2;
        FontStruct.lfHeight = -MulDiv(nBig, cxMax, LOWORD(extDate));
        if (-FontStruct.lfHeight > cyMaxDate) FontStruct.lfHeight = -cyMaxDate;
        if (FontStruct.lfHeight > -2) FontStruct.lfHeight = -2;
        while (FontStruct.lfHeight < 0) {
            if (hFontDate) DeleteObject(hFontDate);
            hFontDate = CreateFontIndirect(&FontStruct);
            hOld = hFontDate ? SelectObject(hdc, hFontDate) : NULL;
            extDate = GetTextExtent(hdc, szDate, cchDate);
            if (hOld) SelectObject(hdc, hOld);
            if (LOWORD(extDate) < (WORD)cxMax) break;
            FontStruct.lfHeight += 2;
        }
        xDate = (WORD)(cx - LOWORD(extDate)) >> 1;
        cxDate = LOWORD(extDate);
        cyDate = HIWORD(extDate);
        yTime = (cy - cyDate - cyTime) / 2;
        yDate = yTime + cyTime;
    } else
        yTime = (cy - cyTime) / 2;
    ReleaseDC(hwnd, hdc);
    SetCursor(hcurOld);
}

/* ------------------------------------------------------------------ seg1:074A: a round face */
/* shrink clockRect across or down so that the face is round on the display (VGA: square pixels) */
static void AdjustAspect(void)
{
    int cx = clockRect.right - clockRect.left, cy = clockRect.bottom - clockRect.top;
    if (cx > MulDiv(cy, aspectH, aspectV)) {
        int w = MulDiv(cy, aspectH, aspectV);
        clockRect.left += (cx - w) >> 1;
        clockRect.right = clockRect.left + w;
    } else {
        int h = MulDiv(cx, aspectV, aspectH);
        clockRect.top += (cy - h) >> 1;
        clockRect.bottom = clockRect.top + h;
    }
}

/* ------------------------------------------------------------------ seg1:07C1 / 080F */
static void SetClockTimer(HWND hwnd)
{
    KillTimer(hwnd, TIMER_ID);
    SetTimer(hwnd, TIMER_ID, (opt.fIconic || opt.fNoSeconds) ? 20000 : 450, NULL);
}

static void SetTitle(HWND hwnd)
{
    char buf[50];
    if ((opt.mode != DIGITAL || opt.fIconic) && !opt.fNoDate) {
        wsprintf(buf, "%s - %s", szTitle, szDate);
        SetWindowText(hwnd, buf);
    } else
        SetWindowText(hwnd, szTitle);
}

/* ------------------------------------------------------------------ seg1:085F: WM_SIZE */
static void ClockSize(HWND hwnd, int cx, int cy, WPARAM type)
{
    BOOL fChanged = FALSE;
    SetRect(&clockRect, 0, 0, cx, cy);
    AdjustAspect();
    if (type == SIZE_MINIMIZED) {
        opt.fIconic = TRUE;
        fChanged = TRUE;
    } else if (opt.fIconic) {
        opt.fIconic = FALSE;
        fChanged = TRUE;
    }
    if (fChanged) {
        SetClockTimer(hwnd);
        BuildTimeTemplate();
        if (!opt.fNoDate) {
            FormatDate(&oDate, opt.fIconic);
            SetTitle(hwnd);
        }
    }
}

/* ------------------------------------------------------------------ seg1:08CE: the icon's frame */
/* a black ring with its corners cut, a red ring and a black ring inside it */
static void DrawBorder(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    SelectObject(hdc, GetStockObject(BLACK_PEN));
    MoveTo(hdc, rc.left, rc.top + 1);
    LineTo(hdc, rc.left, rc.bottom - 1);
    MoveTo(hdc, rc.left + 1, rc.bottom - 1);
    LineTo(hdc, rc.right - 1, rc.bottom - 1);
    MoveTo(hdc, rc.right - 1, rc.bottom - 2);
    LineTo(hdc, rc.right - 1, rc.top);
    MoveTo(hdc, rc.right - 2, rc.top);
    LineTo(hdc, rc.left, rc.top);
    MoveTo(hdc, rc.left + 2, rc.top + 2);
    LineTo(hdc, rc.left + 2, rc.bottom - 3);
    LineTo(hdc, rc.right - 3, rc.bottom - 3);
    LineTo(hdc, rc.right - 3, rc.top + 2);
    LineTo(hdc, rc.left + 2, rc.top + 2);
    SelectObject(hdc, hpenRed);
    MoveTo(hdc, rc.left + 1, rc.top + 1);
    LineTo(hdc, rc.left + 1, rc.bottom - 2);
    LineTo(hdc, rc.right - 2, rc.bottom - 2);
    LineTo(hdc, rc.right - 2, rc.top + 1);
    LineTo(hdc, rc.left + 1, rc.top + 1);
}

/* ------------------------------------------------------------------ seg1:0A2C: the face */
static void DrawFace(HDC hdc)
{
    int dotW = MulDiv(25, clockRect.right - clockRect.left, horzRes);
    int dotH = MulDiv(dotW, aspectV, aspectH);
    if (dotH < 2) dotH = 1;
    if (dotW < 2) dotW = 2;
    InflateRect(&clockRect, -(dotH >> 1), -(dotW >> 1)); /* sic: 3.1 pairs the height with x */
    clockCenterY = ((clockRect.bottom - clockRect.top) >> 1) + clockRect.top - 1;
    clockRadius = (clockRect.right - clockRect.left - 8) >> 1;
    clockCenterX = clockRadius + clockRect.left + 3;
    for (int i = 0; i < 60; i++) {
        int y = MulDiv(cirTab[i].y, clockRadius, CIRCLE) + clockCenterY;
        int x = MulDiv(cirTab[i].x, clockRadius, CIRCLE) + clockCenterX;
        RECT rc;
        if (i % 5 == 0) {
            if (!opt.fIconic) {
                SetRect(&rc, x, y, dotW + x, dotH + y);
                OffsetRect(&rc, -(dotW >> 1), -(dotH >> 1));
                SelectObject(hdc, GetStockObject(BLACK_PEN));
                SelectObject(hdc, hbrHour);
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, hpenCyan);
                MoveTo(hdc, rc.left, rc.bottom - 1);
                LineTo(hdc, rc.left, rc.top);
                LineTo(hdc, rc.right - 1, rc.top);
            } else {
                PatBlt(hdc, x, y, 2, 2, BLACKNESS);
                PatBlt(hdc, x, y, 1, 1, WHITENESS);
            }
        } else if (dotW > 2 && dotH >= 2) {
            SetRect(&rc, x, y, x + 2, y + 2);
            FillRect(hdc, &rc, GetStockObject(WHITE_BRUSH));
            OffsetRect(&rc, -1, -1);
            FillRect(hdc, &rc, hbrShadow);
            rc.left++;
            rc.top++;
            FillRect(hdc, &rc, hbrFace);
        }
    }
    InflateRect(&clockRect, dotH >> 1, dotW >> 1);
}

/* ------------------------------------------------------------------ seg1:0C6D / 0CF6: hands */
/* a line from the centre, scale percent of the radius long */
static void DrawHand(HDC hdc, int pos, HPEN hpen, int scale, int rop)
{
    MoveTo(hdc, clockCenterX, clockCenterY);
    int r = MulDiv(clockRadius, scale, 100);
    SetROP2(hdc, rop);
    SelectObject(hdc, hpen);
    LineTo(hdc, MulDiv(cirTab[pos].x, r, CIRCLE) + clockCenterX, MulDiv(cirTab[pos].y, r, CIRCLE) + clockCenterY);
}

/* the outline of a hour or minute hand: from the sides (a quarter turn on) to the tip and to the tail */
static void DrawFatHand(HDC hdc, int pos, HPEN hpen, BOOL fHour)
{
    SetROP2(hdc, R2_COPYPEN);
    SelectObject(hdc, hpen);
    int w = MulDiv(clockRadius, fHour ? HOURSIDE : MINUTESIDE, 100);
    int k = (pos + 15) % 60;
    int dy = MulDiv(cirTab[k].y, w, CIRCLE);
    int dx = MulDiv(cirTab[k].x, w, CIRCLE);
    int len = fHour ? HOURHAND : MINUTEHAND;
    int tipY = MulDiv(cirTab[pos].y, MulDiv(clockRadius, len, 100), CIRCLE);
    int tipX = MulDiv(cirTab[pos].x, MulDiv(clockRadius, len, 100), CIRCLE);
    MoveTo(hdc, clockCenterX + dx, clockCenterY + dy);
    LineTo(hdc, clockCenterX + tipX, clockCenterY + tipY);
    MoveTo(hdc, clockCenterX - dx, clockCenterY - dy);
    LineTo(hdc, clockCenterX + tipX, clockCenterY + tipY);
    k = (pos + 30) % 60;
    w = MulDiv(clockRadius, fHour ? HOURTAIL : MINUTETAIL, 100);
    tipY = MulDiv(cirTab[k].y, w, CIRCLE);
    tipX = MulDiv(cirTab[k].x, w, CIRCLE);
    MoveTo(hdc, clockCenterX + dx, clockCenterY + dy);
    LineTo(hdc, clockCenterX + tipX, clockCenterY + tipY);
    MoveTo(hdc, clockCenterX - dx, clockCenterY - dy);
    LineTo(hdc, clockCenterX + tipX, clockCenterY + tipY);
}

/* ------------------------------------------------------------------ seg1:0EE7: digital text */
/* Flat (button text on button face) without a memory DC or on a mono display / with NoShadow;
 * otherwise raised: highlight up-left, shadow down-right, face colour on top - straight to the
 * screen when the field is wider than two digits, else built in the memory DC and blitted. */
static void ClockText(HDC hdc, int x, int y, int cx, int cy, const char *psz, int cch, HDC hdcMem)
{
    RECT rc;
    if (!hdcMem || fFlatText) {
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        rc.top = y;
        rc.bottom = y + cy;
        rc.left = x;
        rc.right = x + cx;
        ExtTextOut(hdc, x, rc.top, ETO_OPAQUE | ETO_CLIPPED, &rc, psz, cch, NULL);
        return;
    }
    int sh = cShadow;
    if (cxDigit * 2 < cx) {
        SetTextColor(hdc, GetSysColor(COLOR_BTNHIGHLIGHT));
        rc.bottom = cy + y + sh;
        rc.left = x - sh;
        rc.right = cx + x + sh;
        rc.top = y - sh;
        ExtTextOut(hdc, x - sh, y - sh, ETO_OPAQUE | ETO_CLIPPED, &rc, psz, cch, NULL);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_BTNSHADOW));
        ExtTextOut(hdc, x + sh, y + sh, ETO_CLIPPED, &rc, psz, cch, NULL);
        SetTextColor(hdc, GetSysColor(COLOR_BTNFACE));
        ExtTextOut(hdc, x, y, ETO_CLIPPED, &rc, psz, cch, NULL);
        return;
    }
    rc.left = rc.top = 0;
    rc.right = sh * 2 + cx;
    rc.bottom = sh * 2 + cy;
    SetTextColor(hdcMem, GetSysColor(COLOR_BTNHIGHLIGHT));
    ExtTextOut(hdcMem, 0, 0, ETO_OPAQUE | ETO_CLIPPED, &rc, psz, cch, NULL);
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, GetSysColor(COLOR_BTNSHADOW));
    ExtTextOut(hdcMem, sh * 2, sh * 2, ETO_CLIPPED, &rc, psz, cch, NULL);
    SetTextColor(hdcMem, GetSysColor(COLOR_BTNFACE));
    ExtTextOut(hdcMem, sh, sh, ETO_CLIPPED, &rc, psz, cch, NULL);
    BitBlt(hdc, x - sh, y - sh, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
}

/* ------------------------------------------------------------------ seg1:10DE: painting */
/* fUpdate 0: everything (WM_PAINT; the analog hands are drawn where the stored time has them);
 * 1: what changed since the stored time (WM_TIMER), which then becomes the stored time */
static void ClockPaint(HWND hwnd, HDC hdc, int fUpdate)
{
    TIME tNow;
    DATE dNow;
    HDC hdcMem = NULL;
    HGDIOBJ hOldFont, hbmOld = NULL;
    char buf[2];
    GetTime(&tNow);
    ConvertTime(&tNow);
    GetDate(&dNow);
    if (dNow.day != oDate.day) {
        FormatDate(&dNow, opt.fIconic);
        SetTitle(hwnd);
        fRecalc = TRUE;
    }
    if (opt.mode == DIGITAL) {
        if (!hFontTime || fRecalc) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            SizeFont(hwnd, rc.bottom - rc.top, rc.right - rc.left);
            fRecalc = FALSE;
        }
        hOldFont = hFontTime ? SelectObject(hdc, hFontTime) : NULL;
        SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
        SetTextAlign(hdc, TA_TOP | TA_LEFT);
        if (cShadow && (hdcMem = CreateCompatibleDC(hdc))) {
            if (!hbmDigits || !(hbmOld = SelectObject(hdcMem, hbmDigits))) {
                if (hbmDigits) DeleteObject(hbmDigits);
                hbmDigits = CreateDiscardableBitmap(hdc, (cShadow + cxDigit) * 2, cyTime);
                if (!hbmDigits || !(hbmOld = SelectObject(hdcMem, hbmDigits))) {
                    DeleteDC(hdcMem);
                    hdcMem = NULL;
                }
            }
            if (hdcMem) {
                if (hFontTime) SelectObject(hdcMem, hFontTime);
                SetBkColor(hdcMem, GetSysColor(COLOR_BTNFACE));
                SetTextAlign(hdcMem, TA_TOP | TA_LEFT);
            }
        }
        if (!fUpdate || opt.fIconic) {
            SelectObject(hdc, hbrFace);
            oTime.hour = 25; /* every field differs from the stored time: all are drawn */
            oTime.minute = 60;
            oTime.pm = 2;
            oDate.day = 0;
            ClockText(hdc, xSep1, yTime, cxSep, cyTime, &chTimeSep, 1, hdcMem);
            if (!opt.fIconic && !opt.fNoSeconds) ClockText(hdc, xSep2, yTime, cxSep, cyTime, &chTimeSep, 1, hdcMem);
        }
        if (tNow.pm != oTime.pm && !opt.fIconic) {
            const char *s = szAmPm[tNow.pm];
            ClockText(hdc, xAmPm, yTime, cxAmPm, cyTime, s, lstrlen(s), hdcMem);
        }
        if (!opt.fIconic && !opt.fNoSeconds) {
            buf[0] = tNow.second / 10 + '0';
            buf[1] = tNow.second % 10 + '0';
            ClockText(hdc, xSec, yTime, cxDigit * 2, cyTime, buf, 2, hdcMem);
        }
        if (tNow.minute != oTime.minute) {
            buf[0] = tNow.minute / 10 + '0';
            buf[1] = tNow.minute % 10 + '0';
            ClockText(hdc, xMin, yTime, cxDigit * 2, cyTime, buf, 2, hdcMem);
        }
        if (tNow.hour != oTime.hour) {
            int h = iTime ? tNow.hour : tNow.hour12;
            buf[0] = h / 10 + '0';
            buf[1] = h % 10 + '0';
            if (!iTLZero && buf[0] == '0') {
                ClockText(hdc, cxDigit + xTime, yTime, cxDigit, cyTime, buf + 1, 1, hdcMem);
                if (h == 1 || h == 0) { /* from 12 or 23: wipe the tens digit */
                    RECT rc = {xTime, yTime, xTime + cxDigit, yTime + cyTime};
                    FillRect(hdc, &rc, hbrFace);
                }
            } else
                ClockText(hdc, xTime, yTime, cxDigit * 2, cyTime, buf, 2, hdcMem);
        }
        if (hdcMem) {
            /* 3.1 hands the old bitmap to the window DC here (GDI refuses it) and deletes the
             * memory DC with the digits bitmap still in it; deselecting it is the same */
            SelectObject(hdcMem, hbmOld);
            DeleteDC(hdcMem);
            hdcMem = NULL;
        }
        if (dNow.day != oDate.day && !opt.fNoDate && !opt.fIconic) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.top = yDate;
            rc.bottom = yDate + cyDate;
            FillRect(hdc, &rc, hbrFace);
            hOldFont = hFontDate ? SelectObject(hdc, hFontDate) : NULL;
            ClockText(hdc, xDate, yDate, cxDate, cyDate, szDate, cchDate, hdcMem);
        }
        if (hOldFont) SelectObject(hdc, hOldFont);
    } else {
        if (!fUpdate) {
            SetBkMode(hdc, TRANSPARENT);
            DrawFace(hdc);
            int pos = oTime.minute / 12 + oTime.hour12m * 5;
            if (!opt.fIconic) {
                DrawFatHand(hdc, pos, hpenFore, TRUE);
                DrawFatHand(hdc, oTime.minute, hpenFore, FALSE);
            } else {
                DrawHand(hdc, pos, hpenFore, HOURHAND, R2_COPYPEN);
                DrawHand(hdc, oTime.minute, hpenFore, MINUTEHAND, R2_COPYPEN);
            }
            if (!opt.fIconic && !opt.fNoSeconds) DrawHand(hdc, oTime.second, hpenBackground, SECONDHAND, R2_NOT);
            if (opt.fIconic) DrawBorder(hwnd, hdc);
            return; /* the stored time stays: the timer moves the hands on from it */
        }
        if (fUpdate == 1) {
            SetBkMode(hdc, TRANSPARENT);
            if (!opt.fIconic && !opt.fNoSeconds && tNow.second != oTime.second)
                DrawHand(hdc, oTime.second, hpenBackground, SECONDHAND, R2_NOT);
            if (tNow.minute != oTime.minute || tNow.hour12m != oTime.hour12m) {
                int oldpos = oTime.minute / 12 + oTime.hour12m * 5, newpos = tNow.minute / 12 + tNow.hour12m * 5;
                if (opt.fIconic) {
                    DrawHand(hdc, oTime.minute, hpenBackground, MINUTEHAND, R2_COPYPEN);
                    DrawHand(hdc, oldpos, hpenBackground, HOURHAND, R2_COPYPEN);
                    DrawHand(hdc, tNow.minute, hpenFore, MINUTEHAND, R2_COPYPEN);
                    DrawHand(hdc, newpos, hpenFore, HOURHAND, R2_COPYPEN);
                } else {
                    DrawFatHand(hdc, oTime.minute, hpenBackground, FALSE);
                    DrawFatHand(hdc, oldpos, hpenBackground, TRUE);
                    DrawFatHand(hdc, tNow.minute, hpenFore, FALSE);
                    DrawFatHand(hdc, newpos, hpenFore, TRUE);
                }
            }
            if (!opt.fIconic && !opt.fNoSeconds && tNow.second != oTime.second)
                DrawHand(hdc, tNow.second, hpenBackground, SECONDHAND, R2_NOT);
        }
    }
    if (opt.fIconic) DrawBorder(hwnd, hdc);
    oTime = tNow;
    oDate = dNow;
}

/* ------------------------------------------------------------------ seg1:16F1: WM_TIMER */
static void ClockTimer(HWND hwnd, WPARAM id)
{
    TIME t;
    DATE d;
    (void)id;
    GetTime(&t);
    GetDate(&d);
    if (opt.fNoSeconds || opt.fIconic) {
        KillTimer(hwnd, TIMER_ID);
        SetTimer(hwnd, TIMER_ID, (61 - t.second) * 100, NULL);
    }
    if ((t.second != oTime.second && !opt.fIconic && !opt.fNoSeconds) || t.minute != oTime.minute ||
        t.hour != oTime.hour || (d.day != oDate.day && !opt.fNoDate)) {
        HDC hdc = GetDC(hwnd);
        ClockPaint(hwnd, hdc, 1);
        ReleaseDC(hwnd, hdc);
    }
}

/* ------------------------------------------------------------------ seg1:1799: display geometry */
static void ClockCreate(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    vertRes = GetDeviceCaps(hdc, VERTRES);
    horzRes = GetDeviceCaps(hdc, HORZRES);
    int vertSize = GetDeviceCaps(hdc, VERTSIZE);
    int horzSize = GetDeviceCaps(hdc, HORZSIZE);
    ReleaseDC(hwnd, hdc);
    aspectV = MulDiv(vertRes, 100, vertSize);
    aspectH = MulDiv(horzRes, 100, horzSize);
    CreateTools();
    if (fScaleCircle)
        for (int i = 0; i < 60; i++) cirTab[i].y = MulDiv(cirTab[i].y, aspectV, aspectH);
}

/* ------------------------------------------------------------------ seg1:186D: WIN.INI [intl] */
static void GetIntlInfo(HINSTANCE h)
{
    char szIntl[22], szKey[22], szDef[24], szSep[24];
    LoadString(h, IDS_INTL, szIntl, 20);
    LoadString(h, IDS_ITIME, szKey, 20);
    iTime = GetProfileInt(szIntl, szKey, 0);
    LoadString(h, IDS_S1159, szKey, 20);
    LoadString(h, IDS_DEF1159, szDef, 20);
    GetProfileString(szIntl, szKey, szDef, szAmPm[0], 7);
    LoadString(h, IDS_S2359, szKey, 20);
    LoadString(h, IDS_DEF2359, szDef, 20);
    GetProfileString(szIntl, szKey, szDef, szAmPm[1], 7);
    iTLZero = GetProfileInt(szIntl, "iTLzero", 0);
    LoadString(h, IDS_STIME, szKey, 20);
    LoadString(h, IDS_DEFTIME, szDef, 20);
    GetProfileString(szIntl, szKey, szDef, szSep, 20); /* 3.1 reads into the default's own buffer */
    chTimeSep = szSep[0];
    LoadString(h, IDS_SSHORTDATE, szKey, 20);
    LoadString(h, IDS_DEFSHORTDATE, szDef, 20);
    GetProfileString(szIntl, szKey, szDef, szShortDate, 14);
}

/* ------------------------------------------------------------------ seg1:19AD: class and circle table */
static LRESULT ClockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static BOOL ClockInit(HINSTANCE h)
{
    char szData[5];
    GetIntlInfo(h);
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.lpszClassName = szSection;
    wc.lpszMenuName = szSection;
    wc.hbrBackground = NULL;
    wc.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
    wc.hInstance = h;
    wc.lpfnWndProc = ClockWndProc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = NULL; /* the minimised clock paints itself */
    if (!RegisterClass(&wc)) return FALSE;
    HDC hdc = GetDC(NULL);
    fColorDisplay = GetDeviceCaps(hdc, NUMCOLORS) > 2;
    ReleaseDC(NULL, hdc);
    LoadString(h, IDS_DATA, szData, 5);
    HANDLE hres = FindResource(h, szSection, szData);
    if (!hres) return FALSE;
    hCirTab = LoadResource(h, hres);
    const BYTE *p = LockResource(hCirTab);
    if (!p || SizeofResource(h, hres) < 60 * 4) return FALSE;
    for (int i = 0; i < 60; i++) { /* 60 x {x, y}, 16-bit little-endian */
        cirTab[i].x = (SHORT)(p[i * 4] | p[i * 4 + 1] << 8);
        cirTab[i].y = (SHORT)(p[i * 4 + 2] | p[i * 4 + 3] << 8);
    }
    hbmDigits = NULL;
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:1A8E: title on / off */
/* No Title takes the caption, system menu and min/max boxes away (WS_DLGFRAME, WS_SYSMENU and the
 * boxes: the high style byte & 0xB4) and the menu bar; the frame stays */
static void SetMenuBar(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
    if (opt.fNoTitle) {
        style &= ~(DWORD)(WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        hMenuSaved = (HMENU)w16_SetWindowPtr(hwnd, GWL_ID, 0); /* 3.1: SetWindowWord(hwnd, GWW_ID, 0) */
    } else {
        style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        w16_SetWindowPtr(hwnd, GWL_ID, (intptr_t)hMenuSaved);
    }
    SetWindowLong(hwnd, GWL_STYLE, (LONG)style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, SW_SHOW);
}

/* ------------------------------------------------------------------ seg1:257A..27BF: CLOCK.INI values */
static int ClockAtoi(const char *s)
{
    int n = 0, neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

static void FormatPosition(const RECT *r, char *buf)
{
    wsprintf(buf, "%i,%i,%i,%i", r->left, r->top, r->right, r->bottom);
}

/* "left,top,right,bottom" -> x, y, cx, cy for CreateWindow, kept on the screen; anything else
 * gives the default window, a 16 mm square face plus the frame and a caption's height */
static void ParsePosition(RECT *prc, const char *psz)
{
    int cxFrame = GetSystemMetrics(SM_CXFRAME), cxSize = GetSystemMetrics(SM_CXSIZE);
    int cyFrame = GetSystemMetrics(SM_CYFRAME), cySize = GetSystemMetrics(SM_CYSIZE);
    int v[4] = {prc->left, prc->top, prc->right, prc->bottom};
    int n = 0;
    while (*psz) {
        if (n >= 4) break;
        v[n] = ClockAtoi(psz);
        while (*psz && *psz != ',') psz++;
        while (*psz && *psz == ',') psz++;
        n++;
    }
    SetRect(prc, v[0], v[1], v[2], v[3]);
    if (n < 4 || prc->right <= prc->left || prc->top >= prc->bottom) {
        HDC hdc = GetDC(NULL);
        int xmm = GetDeviceCaps(hdc, HORZRES) / GetDeviceCaps(hdc, HORZSIZE);
        int ymm = GetDeviceCaps(hdc, VERTRES) / GetDeviceCaps(hdc, VERTSIZE);
        ReleaseDC(NULL, hdc);
        prc->left = CW_USEDEFAULT;
        prc->top = 1;
        prc->right = (xmm * 16 + cxFrame) * 4;
        prc->bottom = (ymm * 16 + cyFrame) * 4 + cySize;
        return;
    }
    int cxScreen = GetSystemMetrics(SM_CXSCREEN), cyScreen = GetSystemMetrics(SM_CYSCREEN);
    prc->right -= prc->left;
    prc->bottom -= prc->top;
    int a = cxScreen - cxSize - cxFrame;
    if (a < prc->left)
        prc->left = a;
    else if ((a = cxSize - prc->right + cxFrame) > prc->left)
        prc->left = a;
    a = cyScreen - cyFrame - cySize;
    if (a < prc->top)
        prc->top = a;
    else if ((a = cxSize - prc->bottom + cxFrame) > prc->top) /* sic: the x metrics */
        prc->top = a;
}

static void FormatOptions(const OPTIONS *o, char *buf)
{
    wsprintf(buf, "%i,%i,%i,%i,%i,%i", o->mode == ANALOG, o->fIconic != 0, o->fNoSeconds != 0, o->fNoTitle != 0,
             o->fTopmost != 0, o->fNoDate != 0);
}

/* "analog,iconic,noseconds,notitle,topmost,nodate"; an empty string gives a digital clock */
static void ParseOptions(OPTIONS *o, const char *psz)
{
    BOOL *f[5] = {&o->fIconic, &o->fNoSeconds, &o->fNoTitle, &o->fTopmost, &o->fNoDate};
    o->mode = DIGITAL;
    for (int i = 0; i < 5; i++) *f[i] = FALSE;
    if (!psz) return;
    o->mode = ClockAtoi(psz) ? ANALOG : DIGITAL;
    for (int i = 0; i < 5; i++) {
        while (*psz && *psz != ',') psz++;
        while (*psz && *psz == ',') psz++;
        if (!*psz) return;
        *f[i] = ClockAtoi(psz) != 0;
    }
}

/* ------------------------------------------------------------------ seg1:1B06: CLOCK.INI */
static void SaveClockOptions(HWND hwnd)
{
    char buf[80];
    BOOL fZoomed = IsZoomed(hwnd);
    if (!opt.fIconic && !fZoomed) GetWindowRect(hwnd, &rcPos);
    wsprintf(buf, "%i", fZoomed);
    WritePrivateProfileString(szSection, "Maximized", buf, szIniFile);
    FormatOptions(&opt, buf);
    WritePrivateProfileString(szSection, "Options", buf, szIniFile);
    FormatPosition(&rcPos, buf);
    WritePrivateProfileString(szSection, "Position", buf, szIniFile);
}

/* ------------------------------------------------------------------ seg1:1B9E: ClockWndProc */
static void ToggleTitle(HWND hwnd) /* seg1:1EDF */
{
    opt.fNoTitle = !opt.fNoTitle;
    SetMenuBar(hwnd);
}

static LRESULT ClockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HMENU hMenu;
    switch (msg) {
    case WM_COMMAND:
        switch (wParam) {
        case IDM_ANALOG:
        case IDM_DIGITAL:
            if ((int)wParam != opt.mode) {
                hMenu = GetMenu(hwnd);
                CheckMenuItem(hMenu, opt.mode, MF_UNCHECKED);
                opt.mode = (int)wParam;
                CheckMenuItem(hMenu, opt.mode, MF_CHECKED);
                EnableMenuItem(hMenu, IDM_SETFONT, wParam == IDM_ANALOG ? MF_GRAYED | MF_DISABLED : MF_ENABLED);
                SetTitle(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        case IDM_SETFONT: {
            CHOOSEFONT cf;
            memset(&cf, 0, sizeof cf);
            cf.lStructSize = sizeof cf;
            cf.hwndOwner = hwnd;
            cf.hDC = NULL;
            cf.lpLogFont = &FontStruct;
            cf.Flags = CF_SCREENFONTS | CF_ENABLETEMPLATE | CF_INITTOLOGFONTSTRUCT | CF_ANSIONLY | CF_NOVECTORFONTS |
                       CF_NOSIMULATIONS;
            cf.lpfnHook = NULL;
            cf.lpTemplateName = MAKEINTRESOURCE(ID_FONTDLG);
            cf.hInstance = hInst;
            FontStruct.lfItalic = 0;
            FontStruct.lfWeight = FW_NORMAL;
            int lfHeightOld = FontStruct.lfHeight;
            int dbuY = HIWORD(GetDialogBaseUnits());
            if (dbuY * 36 / 8 < -FontStruct.lfHeight) FontStruct.lfHeight = dbuY * -36 / 8; /* 4.5 dialog units */
            if (ChooseFont(&cf)) {
                WritePrivateProfileString(szSection, szFontKey, FontStruct.lfFaceName, szIniFile);
                fRecalc = TRUE;
                InvalidateRect(hwnd, NULL, TRUE);
            } else
                FontStruct.lfHeight = lfHeightOld;
            return 0;
        }
        case IDM_ABOUT:
            ShellAbout(hwnd, szTitle, "", LoadIcon(hInst, "cckk"));
            return 0;
        case IDM_NOTITLE:
            ToggleTitle(hwnd);
            return 0;
        case IDM_SECONDS:
            hMenu = GetMenu(hwnd);
            if (opt.fNoSeconds) {
                CheckMenuItem(hMenu, IDM_SECONDS, MF_CHECKED);
                opt.fNoSeconds = FALSE;
            } else {
                CheckMenuItem(hMenu, IDM_SECONDS, MF_UNCHECKED);
                opt.fNoSeconds = TRUE;
            }
            SetClockTimer(hwnd);
            BuildTimeTemplate();
            fRecalc = TRUE;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case IDM_DATE:
            hMenu = GetMenu(hwnd);
            if (opt.fNoDate) {
                CheckMenuItem(hMenu, IDM_DATE, MF_CHECKED);
                opt.fNoDate = FALSE;
            } else {
                CheckMenuItem(hMenu, IDM_DATE, MF_UNCHECKED);
                opt.fNoDate = TRUE;
            }
            fRecalc = TRUE;
            if (opt.mode == DIGITAL) InvalidateRect(hwnd, NULL, TRUE);
            else SetTitle(hwnd);
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE: /* a click of the right button does not activate */
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) return MA_NOACTIVATE;
        break;
    case WM_INITMENU:
        fInMenu = TRUE;
        break;
    case WM_MENUSELECT:
        if ((SHORT)LOWORD(lParam) == -1) fInMenu = FALSE; /* the menu closed */
        break;
    case WM_RBUTTONDOWN:
    case WM_NCRBUTTONDOWN: /* on top: hide while the button is down, to look underneath */
        if (fHidden) return 0;
        if (opt.fTopmost && !fInMenu) {
            ShowWindow(hwnd, SW_HIDE);
            SetCapture(hwnd);
            fHidden = TRUE;
        }
        return 0;
    case WM_RBUTTONUP:
    case WM_NCRBUTTONUP:
        if (!fHidden) return 0;
        ReleaseCapture();
        if (opt.fIconic) ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        else if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        else ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        fHidden = FALSE;
        return 0;
    case WM_KEYDOWN: /* Esc, not repeated, toggles the title; other keys are eaten */
        if (wParam == VK_ESCAPE && !(lParam & 0x40000000)) ToggleTitle(hwnd);
        return 0;
    case WM_NCLBUTTONDBLCLK:
        if (!opt.fNoTitle) break;
        ToggleTitle(hwnd);
        return 0;
    case WM_LBUTTONDBLCLK:
        ToggleTitle(hwnd);
        return 0;
    case WM_NCHITTEST: { /* without a title the clock is dragged by its face */
        LRESULT r = DefWindowProc(hwnd, msg, wParam, lParam);
        if (opt.fNoTitle && r == HTCLIENT && !IsZoomed(hwnd)) return HTCAPTION;
        return r;
    }
    case WM_SIZE:
        fRecalc = TRUE;
        ClockSize(hwnd, LOWORD(lParam), HIWORD(lParam), wParam);
        UpdateWindow(hwnd);
        return 0;
    case WM_QUERYDRAGICON:
        return (LRESULT)LoadIcon(hInst, "cckk");
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        DeleteTools();
        if (hFontTime) DeleteObject(hFontTime);
        if (hFontDate) DeleteObject(hFontDate);
        if (hbmDigits) DeleteObject(hbmDigits);
        SetCapture(hwnd);
        SetCursor(LoadCursor(NULL, IDC_WAIT));
        SaveClockOptions(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_WININICHANGE:
        GetIntlInfo(hInst);
        FormatDate(&oDate, opt.fIconic);
        SetTitle(hwnd);
        BuildTimeTemplate();
        fRecalc = TRUE;
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        InvalidateRect(hwnd, NULL, TRUE); /* always the whole window */
        BeginPaint(hwnd, &ps);
        ClockPaint(hwnd, ps.hdc, 0);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMECHANGE:
        oDate.day = 0;
        InvalidateRect(hwnd, NULL, TRUE);
        ClockTimer(hwnd, wParam);
        return 0;
    case WM_TIMER:
        ClockTimer(hwnd, wParam);
        return 0;
    case WM_SYSCOMMAND:
        switch (wParam) {
        case IDM_TOPMOST:
            hMenu = GetSystemMenu(hwnd, FALSE);
            if (opt.fTopmost) {
                CheckMenuItem(hMenu, IDM_TOPMOST, MF_UNCHECKED);
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
                opt.fTopmost = FALSE;
            } else {
                CheckMenuItem(hMenu, IDM_TOPMOST, MF_CHECKED);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
                opt.fTopmost = TRUE;
            }
            break;
        case SC_MINIMIZE:
            if (!IsZoomed(hwnd)) GetWindowRect(hwnd, &rcPos);
            if (opt.fTopmost) { /* the icon is put on top again once it is an icon */
                opt.fTopmost = FALSE;
                PostMessage(hwnd, WM_SYSCOMMAND, IDM_TOPMOST, 0);
            }
            break;
        case SC_MAXIMIZE:
            if (!IsIconic(hwnd)) GetWindowRect(hwnd, &rcPos);
            break;
        }
        break;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdc = GetDC(hwnd);
        SetBkMode(hdc, OPAQUE);
        HBRUSH hbr = CreateSolidBrush(GetNearestColor(hdc, GetSysColor(COLOR_BTNFACE)));
        FillRect((HDC)wParam, &rc, hbr);
        ReleaseDC(hwnd, hdc);
        DeleteObject(hbr);
        return 0;
    }
    case WM_SYSCOLORCHANGE:
        DeleteTools();
        CreateTools();
        return 0;
    case WM_ENDSESSION:
        if (wParam) SaveClockOptions(hwnd);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:2171: WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    char buf[80], szDefFont[20], szTopmost[80];
    TIME t;
    (void)lpCmdLine;
    LoadString(hInstance, IDS_APPNAME, szAppName, 30);
    LoadString(hInstance, IDS_INIFILE, szIniFile, 20);
    LoadString(hInstance, IDS_SECTION, szSection, 30);
    if (hPrev) {
        /* 3.1 took the first Clock's window (GetInstanceData) and activated it or its last popup;
         * libw16 never passes a previous instance, so this does not happen */
        return 0;
    }
    if (!ClockInit(hInstance)) return 0;
    ClockCreate(NULL);
    LoadString(hInstance, IDS_APPNAME, szTitle, 30);
    HDC hdc = GetDC(NULL);
    if (GetDeviceCaps(hdc, NUMCOLORS) <= 2) fFlatText = 1;
    ReleaseDC(NULL, hdc);
    if (GetPrivateProfileInt(szSection, "NoShadow", 0, szIniFile)) fFlatText |= 2;
    GetPrivateProfileString(szSection, "Position", "", buf, sizeof buf, szIniFile);
    ParsePosition(&rcPos, buf);
    HWND hwnd = CreateWindow(szSection, szSection, WS_OVERLAPPEDWINDOW, rcPos.left, rcPos.top, rcPos.right,
                             rcPos.bottom, NULL, NULL, hInstance, NULL);
    hwndClock = hwnd;
    /* start on a new second (3.1 polled the DOS clock in a loop) */
    GetTime(&t);
    for (;;) {
        GetTime(&oTime);
        if (oTime.second != t.second || oTime.minute != t.minute || oTime.hour != t.hour) break;
        usleep(2000);
    }
    ConvertTime(&oTime);
    GetDate(&oDate);
    if (!SetTimer(hwnd, TIMER_ID, 450, NULL)) {
        char text[160];
        LoadString(hInstance, IDS_TOOMANY, text, sizeof text);
        MessageBox(NULL, text, szAppName, MB_ICONHAND | MB_SYSTEMMODAL);
        DeleteTools();
        return 0;
    }
    LoadString(hInstance, IDS_DEFFONT, szDefFont, 20);
    LoadString(hInstance, IDS_FONTKEY, szFontKey, 20);
    GetPrivateProfileString(szSection, szFontKey, szDefFont, FontStruct.lfFaceName, 20, szIniFile);
    FontStruct.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    FontStruct.lfWeight = FW_NORMAL;
    FontStruct.lfEscapement = FontStruct.lfOrientation = 0;
    FontStruct.lfCharSet = FontStruct.lfQuality = FontStruct.lfUnderline = 0;
    FontStruct.lfStrikeOut = FontStruct.lfOutPrecision = FontStruct.lfClipPrecision = 0;
    GetPrivateProfileString(szSection, "Options", "", buf, sizeof buf, szIniFile);
    ParseOptions(&opt, buf);
    BuildTimeTemplate();
    FormatDate(&oDate, opt.fIconic);
    HMENU hMenu = GetMenu(hwnd);
    CheckMenuItem(hMenu, opt.mode, MF_CHECKED);
    if (opt.mode == ANALOG) EnableMenuItem(hMenu, IDM_SETFONT, MF_GRAYED | MF_DISABLED);
    if (!opt.fNoSeconds) {
        CheckMenuItem(hMenu, IDM_SECONDS, MF_CHECKED);
        SetClockTimer(hwnd);
    }
    if (!opt.fNoDate) {
        CheckMenuItem(hMenu, IDM_DATE, MF_CHECKED);
        SetTitle(hwnd);
    }
    hMenu = GetSystemMenu(hwnd, FALSE);
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    LoadString(hInstance, IDS_TOPMOST, szTopmost, 79);
    if (opt.fTopmost) {
        AppendMenu(hMenu, MF_CHECKED, IDM_TOPMOST, szTopmost);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    } else
        AppendMenu(hMenu, MF_STRING, IDM_TOPMOST, szTopmost);
    if (opt.fNoTitle) SetMenuBar(hwnd);
    hInst = hInstance;
    if (!opt.fIconic) {
        if (GetPrivateProfileInt(szSection, "Maximized", 0, szIniFile))
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        else {
            ShowWindow(hwnd, nCmdShow);
            GetWindowRect(hwnd, &rcPos);
        }
    } else
        ShowWindow(hwnd, SW_MINIMIZE);
    /* 3.1 registered with Pen Windows here (RegisterPenApp) when it was loaded */
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
