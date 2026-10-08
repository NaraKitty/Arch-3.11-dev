/* Date & Time applet: port of MAIN.CPL seg8 (dialog 7) with the DOS clock calls of seg1:189B-1915.
 * The fields follow WIN.INI [intl] (iDate order, sShortDate leading zeros and century, sDate/sTime
 * separators, iTime and s1159/s2359 for the AM/PM box, iTLZero); a 950 ms timer keeps the fields
 * that do not have the focus ticking; the cpArrow controls step the focused field.
 *
 * MAIN.CPL set the DOS clock on every field it read and undid the changes on Cancel. arch311 keeps
 * the dialog's changes as an offset to the system clock and sets the Linux clock once, on OK
 * (timedatectl; automatic time sync is switched off when the user sets the time by hand). */
#include "maincpl.h"
#include "sysexec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { F_HOUR, F_MIN, F_SEC, F_MONTH, F_DAY, F_YEAR, NFIELDS };
#define IDC_FIELD 701           /* 701..706: hour, minute, second, month, day, year */
#define IDC_DATESEP 707         /* 707, 708 */
#define IDC_TIMESEP 709         /* 709, 710 */
#define IDC_DATEARROW 711
#define IDC_TIMEARROW 712
#define IDC_AMPM 713
#define IDC_DATEGROUP 714
#define IDC_TIMEGROUP 715
#define TIMER_CLOCK 2

int AdjustArrowWidth(HWND h);   /* arrow.c */

/* ds:04BE: per field the steps for SB_LINEUP/LINEDOWN/PAGEUP/PAGEDOWN, the range, the values codes 4
 * and 5 jump to, and the flags seg2:0664 leaves (2 = passed the top, 4 = passed the bottom) */
typedef struct { int step[4], max, min, v4, v5; int flag; } Field;
static Field aField[NFIELDS] = {
    {{1, -1, 5, -5}, 23, 0, 12, 12, 0},
    {{1, -1, 5, -5}, 59, 0, 30, 30, 0},
    {{1, -1, 5, -5}, 59, 0, 30, 30, 0},
    {{1, -1, 4, -4}, 12, 1, 0, 0, 0},
    {{1, -1, 5, -5}, 31, 1, 0, 0, 0},
    {{1, -1, 10, -10}, 2099, 1980, 1990, 1990, 0},
};

static int aValue[NFIELDS];     /* [0x1004] the clock as last read or set */
static int aShown[NFIELDS];     /* [0x1d78] */
static int aDelta[NFIELDS];     /* [0xe60] what the dialog changed */
static int aLZero[NFIELDS];     /* [0x4b2]: leading zero (year: four digits) */
#define fYear4 aLZero[F_YEAR]
static BOOL f24Hour;            /* [0x1fe6] iTime */
static BOOL fPM;                /* [0x1fba] */
static UINT idTimer;            /* [0x1fc8] */
static char szAM[4], szPM[4];   /* [0x1ddc], [0x1e2e] s1159, s2359 */
static time_t tOffset;          /* the dialog's clock minus the system clock, in seconds */

/* ------------------------------------------------------------------ the clock */
static void ClockNow(struct tm *t)
{
    time_t now = time(NULL) + tOffset;
    localtime_r(&now, t);
}

static void ClockSet(struct tm *t)
{
    t->tm_isdst = -1;
    time_t when = mktime(t);
    if (when != (time_t)-1) tOffset = when - time(NULL);
}

/* seg1:189B, DOS 2Ch */
static void GetTime(void)
{
    struct tm t;
    ClockNow(&t);
    aValue[F_HOUR] = t.tm_hour;
    aValue[F_MIN] = t.tm_min;
    aValue[F_SEC] = t.tm_sec;
}

/* seg1:18BB, DOS 2Ah */
static void GetDate(void)
{
    struct tm t;
    ClockNow(&t);
    aValue[F_MONTH] = t.tm_mon + 1;
    aValue[F_DAY] = t.tm_mday;
    aValue[F_YEAR] = t.tm_year + 1900;
}

/* seg1:18E1, DOS 2Dh: an impossible time leaves the clock alone */
static void SetTime(void)
{
    if (aValue[F_HOUR] < 0 || aValue[F_HOUR] > 23 || aValue[F_MIN] < 0 || aValue[F_MIN] > 59 ||
        aValue[F_SEC] < 0 || aValue[F_SEC] > 59)
        return;
    struct tm t;
    ClockNow(&t);
    t.tm_hour = aValue[F_HOUR];
    t.tm_min = aValue[F_MIN];
    t.tm_sec = aValue[F_SEC];
    ClockSet(&t);
}

/* seg1:18FE, DOS 2Bh: 1980-2099 and a real calendar date, else no change */
static void SetDate(void)
{
    static const int mdays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = aValue[F_MONTH], d = aValue[F_DAY], y = aValue[F_YEAR];
    if (y < 1980 || y > 2099 || m < 1 || m > 12 || d < 1 || d > mdays[m - 1]) return;
    if (m == 2 && d == 29 && (y % 4 || (y % 100 == 0 && y % 400))) return;
    struct tm t;
    ClockNow(&t);
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_year = y - 1900;
    ClockSet(&t);
}

/* arch311: the dialog's clock becomes the system clock */
static void ApplyClock(HWND hDlg)
{
    if (!tOffset) return;
    struct tm t;
    char when[32], err[256] = "";
    ClockNow(&t);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &t);
    if (sys_simulated()) {
        fprintf(stderr, "arch311: [simulated] timedatectl set-time %s\n", when);
        tOffset = 0;
        return;
    }
    const char *ntp[] = {"timedatectl", "set-ntp", "false", NULL};
    const char *set[] = {"timedatectl", "set-time", when, NULL};
    sys_run(ntp, NULL, NULL, 0, NULL, 0, 15000);
    if (sys_run(set, NULL, NULL, 0, err, sizeof err, 15000) != 0) {
        char msg[400];
        wsprintf(msg, "The date and time could not be changed.\n\n%s", err[0] ? err : "timedatectl failed.");
        MessageBox(hDlg, msg, "Date & Time", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    tOffset = 0;
}

/* ------------------------------------------------------------------ seg8:064E
 * days in a month; MAIN.CPL counts a year divisible by 400 (2000) as common */
static int DaysInMonth(int month, int year)
{
    switch (month) {
    case 2: return (year & 3) ? 28 : (year % 400 ? 29 : 28);
    case 4: case 6: case 9: case 11: return 30;
    default: return 31;
    }
}

/* ------------------------------------------------------------------ seg8:0000
 * one sShortDate element at *pp: M, d or y. Doubled M/d means a leading zero; yy is two digits and
 * yyyy four; returns the text after it or NULL */
static const char *ParseToken(const char *p, int *flag)
{
    if (*p != 'y' && *p != 'M' && *p != 'd') return NULL;
    p++;
    if (*p != p[-1]) { *flag = 0; return p; }
    *flag = 1;
    const char *second = p++;
    if (*second != 'y') return p;
    if (*p != 'y') { *flag = 0; return p; }
    p++;
    if (*p != 'y') return NULL;
    return p + 1;
}

/* ------------------------------------------------------------------ seg8:011B
 * the order of sShortDate (0 MDY, 1 DMY, 2 YMD) and its three flags; < 0 if it does not parse */
static int ParseDateFormat(const char *fmt, int *pMonth, int *pDay, int *pYear)
{
    int order;
    char letters[3];
    int *flags[3];
    switch (*fmt) {
    case 'M': order = 0; letters[0] = 'M'; letters[1] = 'd'; letters[2] = 'y'; flags[0] = pMonth; flags[1] = pDay; flags[2] = pYear; break;
    case 'd': order = 1; letters[0] = 'd'; letters[1] = 'M'; letters[2] = 'y'; flags[0] = pDay; flags[1] = pMonth; flags[2] = pYear; break;
    case 'y': order = 2; letters[0] = 'y'; letters[1] = 'M'; letters[2] = 'd'; flags[0] = pYear; flags[1] = pMonth; flags[2] = pDay; break;
    default: return 0;
    }
    const char *p = fmt;
    for (int k = 0; k < 3; k++) {
        if (*p != letters[k]) return -1 - order;
        p = ParseToken(p, flags[k]);
        if (!p) return -1 - order;
        p++;    /* the separator */
    }
    return order;
}

/* ------------------------------------------------------------------ seg8:01F6 */
static int MaxDigitWidth(HDC hdc)
{
    int w[10], m;
    GetCharWidth(hdc, '0', '9', w);
    m = w[0];
    for (int i = 1; i < 10; i++)
        if (w[i] > m) m = w[i];
    return m;
}

static void ClientRectOf(HWND hDlg, HWND h, RECT *rc)
{
    GetWindowRect(h, rc);
    ScreenToClient(hDlg, (POINT *)&rc->left);
    ScreenToClient(hDlg, (POINT *)&rc->right);
}

/* ------------------------------------------------------------------ seg8:0240
 * orders, sizes and centres one row of three fields and two separators in its group box */
static void SetupFields(HWND hDlg, int idFirst, int idSep, const char *sep, int dw, BOOL fDate)
{
    HWND h0 = GetDlgItem(hDlg, idFirst), h1 = GetDlgItem(hDlg, idFirst + 1), h2 = GetDlgItem(hDlg, idFirst + 2);
    HWND item[5];
    int order, wAmPm = 0;
    item[1] = GetDlgItem(hDlg, idSep);
    item[3] = GetDlgItem(hDlg, idSep + 1);
    if (fDate) {
        order = GetProfileInt("intl", "iDate", 0);
    } else {
        f24Hour = GetProfileInt("intl", "iTime", 0);
        if (!f24Hour) {
            GetProfileString("intl", "s1159", "AM", szAM, sizeof szAM);
            GetProfileString("intl", "s2359", "PM", szPM, sizeof szPM);
        }
        order = 0;
    }
    switch (order) {
    case 1: item[0] = h1; item[2] = h0; item[4] = h2; break;
    case 2: item[0] = h2; item[2] = h0; item[4] = h1; break;
    default: item[0] = h0; item[2] = h1; item[4] = h2; break;
    }
    if (fDate) {
        /* tab order follows the visual order */
        SetWindowPos(item[0], GetDlgItem(hDlg, IDC_DATEGROUP), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        SetWindowPos(item[2], item[0], 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        SetWindowPos(item[4], item[2], 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    /* measured in the DC's own (system) font, as MAIN.CPL does */
    HDC hdc = GetDC(hDlg);
    if (!fDate) {
        int a = LOWORD(GetTextExtent(hdc, szAM, lstrlen(szAM)));
        wAmPm = LOWORD(GetTextExtent(hdc, szPM, lstrlen(szPM)));
        if (wAmPm < a) wAmPm = a;
    }
    int sw = LOWORD(GetTextExtent(hdc, sep, lstrlen(sep)));
    ReleaseDC(hDlg, hdc);

    RECT rcField, rcGroup;
    ClientRectOf(hDlg, h2, &rcField);
    int cy = rcField.bottom - rcField.top;
    ClientRectOf(hDlg, GetDlgItem(hDlg, fDate ? IDC_DATEGROUP : IDC_TIMEGROUP), &rcGroup);
    int x = (WORD)(rcGroup.left + rcGroup.right - 2 * (3 * dw + sw)) >> 1;
    if (fDate) {
        if (fYear4) x -= dw;
    } else if (!f24Hour) {
        x -= wAmPm / 2;
    }
    for (int k = 0; k < 5; k++) {
        int w;
        if (k % 2 == 0) {
            w = 2 * dw;
            if (item[k] == h2 && fDate && fYear4) w *= 2;
        } else {
            w = sw;
        }
        w += 2;
        MoveWindow(item[k], x, rcField.top, w, cy, FALSE);
        x += w;
    }
    HWND hList = GetDlgItem(hDlg, IDC_AMPM);
    if (!fDate && !f24Hour) {
        MoveWindow(hList, x, rcField.top, wAmPm, cy, FALSE);
        SendMessage(hList, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)szAM);
        SendMessage(hList, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)szPM);
        SendMessage(hList, LB_SETCURSEL, (WPARAM)-1, 0);
    }
    EnableWindow(hList, !f24Hour);
    SendMessage(h2, EM_LIMITTEXT, fDate && fYear4 ? 4 : 2, 0);
    SendMessage(h0, EM_LIMITTEXT, 2, 0);
    SendMessage(h1, EM_LIMITTEXT, 2, 0);
    SetDlgItemText(hDlg, idSep, sep);
    SetDlgItemText(hDlg, idSep + 1, sep);
}

/* ------------------------------------------------------------------ seg8:0690 */
static void ShowField(HWND hDlg, int i)
{
    int v = aValue[i];
    char t[8];
    if (i == F_YEAR) {
        if (!fYear4) v %= 100;
    } else if (i == F_HOUR && !f24Hour) {
        fPM = v >= 12;
        SendDlgItemMessage(hDlg, IDC_AMPM, LB_SETTOPINDEX, fPM, 0);
        if (fPM) v -= 12;
        if (v == 0) v = 12;
    }
    if (v < 10 && (aLZero[i] || i == F_YEAR)) wsprintf(t, "0%d", v);
    else wsprintf(t, "%d", v);
    SetDlgItemText(hDlg, IDC_FIELD + i, t);
    SendDlgItemMessage(hDlg, IDC_FIELD + i, EM_SETSEL, 0, MAKELPARAM(0, 0x7fff));
    if (i == F_MONTH || i == F_YEAR) aField[F_DAY].max = DaysInMonth(aValue[F_MONTH], aValue[F_YEAR]);
}

/* ------------------------------------------------------------------ seg8:0070
 * takes a field's text into the clock */
static void ReadField(HWND hDlg, int i)
{
    BOOL ok;
    int v = (int)GetDlgItemInt(hDlg, IDC_FIELD + i, &ok, FALSE);
    GetDate();
    GetTime();
    if (i == F_HOUR) {
        if (!f24Hour) {
            if (v == 12) { if (!fPM) v = 0; }
            else if (fPM) v += 12;
        }
    } else if (i == F_YEAR && !fYear4) {
        v += v < 80 ? 2000 : 1900;
    }
    if (aValue[i] == v) return;
    aDelta[i] += v - aValue[i];
    aValue[i] = v;
    aShown[i] = v;
    if (i < F_MONTH) SetTime();
    else SetDate();
}

/* ------------------------------------------------------------------ seg2:0664
 * one step of a field: the step or jump for the scroll code, kept in range (flag 2 / 4 when it
 * went past the top / bottom) */
static int StepField(int v, int code, Field *f)
{
    int d = 0;
    switch (code) {
    case SB_LINEUP: d = f->step[0]; break;
    case SB_LINEDOWN: d = f->step[1]; break;
    case SB_PAGEUP: d = f->step[2]; break;
    case SB_PAGEDOWN: d = f->step[3]; break;
    case SB_THUMBPOSITION: v = f->v4; break;
    case SB_THUMBTRACK: v = f->v5; break;
    case SB_TOP: v = f->max; break;
    case SB_BOTTOM: v = f->min; break;
    case SB_ENDSCROLL: break;
    default: f->flag = 1; break;   /* (overwritten below) */
    }
    if (v + d > f->max) { f->flag = 2; return f->max; }
    if (v + d < f->min) { f->flag = 4; return f->min; }
    f->flag = 0;
    return v + d;
}

static BOOL IsNumber(HWND hDlg, int id)
{
    char t[5];
    GetDlgItemText(hDlg, id, t, sizeof t);
    for (const char *p = t; *p; p++)
        if (*p < '0' || *p > '9') return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ seg8:077C */
BOOL DateTimeDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        char fmt[12], sep[3];
        HourGlass(TRUE);
        memset(aLZero, 0, sizeof aLZero);
        aLZero[F_MIN] = aLZero[F_SEC] = 1;
        tOffset = 0;
        const char *fixed = getenv("ARCH311_CLOCK");   /* tests: start from a given time */
        if (fixed && *fixed) {
            struct tm t;
            memset(&t, 0, sizeof t);
            if (sscanf(fixed, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
                t.tm_year -= 1900;
                t.tm_mon -= 1;
                ClockSet(&t);
            }
        }
        HDC hdc = GetDC(hDlg);
        HFONT hFont = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0);
        HGDIOBJ old = hFont ? SelectObject(hdc, hFont) : NULL;
        int dw = MaxDigitWidth(hdc);
        if (old) SelectObject(hdc, old);
        ReleaseDC(hDlg, hdc);
        aLZero[F_HOUR] = GetProfileInt("intl", "iTLZero", 0);
        GetProfileString("intl", "sShortDate", "M/d/yy", fmt, sizeof fmt);
        ParseDateFormat(fmt, &aLZero[F_MONTH], &aLZero[F_DAY], &aLZero[F_YEAR]);
        GetProfileString("intl", "sDate", "/", sep, sizeof sep);
        SetupFields(hDlg, IDC_FIELD + F_MONTH, IDC_DATESEP, sep, dw, TRUE);
        GetProfileString("intl", "sTime", ":", sep, sizeof sep);
        SetupFields(hDlg, IDC_FIELD + F_HOUR, IDC_TIMESEP, sep, dw, FALSE);
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_DATEARROW));
        AdjustArrowWidth(GetDlgItem(hDlg, IDC_TIMEARROW));
        GetTime();
        GetDate();
        aField[F_DAY].max = DaysInMonth(aValue[F_MONTH], aValue[F_YEAR]);
        if (!fYear4) {
            aValue[F_YEAR] %= 100;
            aField[F_YEAR].max = 99;
            aField[F_YEAR].min = 0;
            aField[F_YEAR].v4 = aField[F_YEAR].v5 = 90;
        } else {
            aField[F_YEAR].max = 2099;
            aField[F_YEAR].min = 1980;
            aField[F_YEAR].v4 = aField[F_YEAR].v5 = 1990;
        }
        for (int i = 0; i < NFIELDS; i++) {
            aShown[i] = -1;
            aDelta[i] = 0;
        }
        idTimer = 1;
        SendMessage(hDlg, WM_TIMER, TIMER_CLOCK, 0);
        idTimer = SetTimer(hDlg, TIMER_CLOCK, 950, NULL);
        HourGlass(FALSE);
        return TRUE;
    }

    case WM_TIMER:
        if (!idTimer || wParam != TIMER_CLOCK) return TRUE;
        GetTime();
        GetDate();
        if (!fYear4) aValue[F_YEAR] %= 100;
        for (int i = 0; i < NFIELDS; i++)
            if (aValue[i] != aShown[i] && GetDlgItem(hDlg, IDC_FIELD + i) != GetFocus()) {
                aShown[i] = aValue[i];
                ShowField(hDlg, i);
            }
        return TRUE;

    case WM_VSCROLL: {
        /* the arrows act on the field with the focus, if it belongs to their row */
        HWND hFocus = GetFocus();
        int id = hFocus ? GetWindowWord(hFocus, GWW_ID) : 0;
        int idArrow = LOWORD(lParam);
        if (id >= IDC_FIELD + F_HOUR && id <= IDC_FIELD + F_SEC) {
            if (idArrow != IDC_TIMEARROW) return FALSE;
        } else if (id >= IDC_FIELD + F_MONTH && id <= IDC_FIELD + F_YEAR) {
            if (idArrow != IDC_DATEARROW) return FALSE;
        } else if (id == IDC_AMPM) {
            if (idArrow != IDC_TIMEARROW) return FALSE;
        } else {
            return FALSE;
        }
        int i = id - IDC_FIELD;
        GetDate();
        GetTime();
        if (wParam == SB_THUMBTRACK || wParam == SB_ENDSCROLL) return TRUE;
        if (id == IDC_AMPM) {
            int cur = (int)SendMessage(hFocus, LB_GETCURSEL, 0, 0);
            SendMessage(hFocus, LB_SETCURSEL, 1 - cur, 0);
            return TRUE;
        }
        BOOL ok;
        int v = (int)GetDlgItemInt(hDlg, id, &ok, FALSE);
        if (i == F_HOUR && !f24Hour) {
            if (fPM) v += 12;
            if (v % 12 == 0) v -= 12;
        }
        aValue[i] = StepField(v, (int)wParam, &aField[i]);
        if (aField[i].flag & 4) aValue[i] = aField[i].max;          /* below the bottom: wrap */
        else if (aField[i].flag & 2) aValue[i] = aField[i].min;
        if (i == F_HOUR) fPM = aValue[F_HOUR] >= 12;
        ShowField(hDlg, i);
        return TRUE;
    }

    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            HourGlass(TRUE);
            ApplyClock(hDlg);
            SendMessage(HWND_BROADCAST, WM_TIMECHANGE, 0, 0);
            HourGlass(FALSE);
            KillTimer(hDlg, TIMER_CLOCK);
            EndDialog(hDlg, 0);
            break;
        case IDCANCEL:
            /* (the focus leaving a field still reads it, then everything the dialog did is undone) */
            SetFocus(GetDlgItem(hDlg, IDCANCEL));
            GetDate();
            GetTime();
            for (int i = 0; i < NFIELDS; i++) aValue[i] -= aDelta[i];
            SetDate();
            SetTime();
            tOffset = 0;
            KillTimer(hDlg, TIMER_CLOCK);
            EndDialog(hDlg, 0);
            break;
        case IDD_HELP:
            CPHelp(hDlg);
            break;
        case IDC_AMPM:
            if (HIWORD(lParam) == LBN_SETFOCUS) {
                HWND hList = W16_CMD_HWND(lParam);
                SendMessage(hList, LB_SETCURSEL, SendMessage(hList, LB_GETTOPINDEX, 0, 0), 0);
            } else if (HIWORD(lParam) == LBN_KILLFOCUS) {
                HWND hList = W16_CMD_HWND(lParam);
                SendMessage(hList, LB_SETCURSEL, (WPARAM)-1, 0);
                fPM = (int)SendMessage(hList, LB_GETTOPINDEX, 0, 0);
                ReadField(hDlg, F_HOUR);
            }
            break;
        default:
            if (wParam >= IDC_FIELD && wParam <= IDC_FIELD + F_YEAR) {
                if (HIWORD(lParam) == EN_UPDATE) {
                    /* digits only */
                    if (!IsNumber(hDlg, (int)wParam)) SendMessage(W16_CMD_HWND(lParam), EM_UNDO, 0, 0);
                } else if (HIWORD(lParam) == EN_KILLFOCUS) {
                    ReadField(hDlg, (int)wParam - IDC_FIELD);
                }
            }
            break;
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
