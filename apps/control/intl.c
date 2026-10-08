/* International applet: port of MAIN.CPL seg12 (dialog 3, IntlDlgProc seg12:194D) with its Date
 * Format (seg11, dialog 15), Time Format (seg14, dialog 16), Number Format (seg13, dialog 17) and
 * Currency Format (seg10, dialog 18) dialogs, the INF reader of seg23 and the seg1/seg4/seg20
 * string helpers they use. The countries are read at run time from CONTROL.INF [country], the
 * languages and keyboard layouts from SETUP.INF [language] and [keyboard.tables].
 *
 * The samples show the clock (DOS functions 2Ah/2Ch in 3.1); tests start it at ARCH311_CLOCK, as
 * the Date & Time applet does.
 *
 * Language drivers and keyboard layouts are 16-bit DLLs. Where MAIN.CPL copies one from the
 * Windows disks (InstallFiles seg1:07BA, with the "driver already installed" dialog 23 and the
 * disk prompt 30), loads the language driver (SPI_SETLANGDRIVER) or has KEYBOARD.DRV load the
 * layout (NewTable), the port does nothing; the INI entries are written as 3.1 writes them.
 * TODO: apply the keyboard layout to Linux (XKB). */
#include "maincpl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* dialog 3 */
#define IDC_COUNTRY 200
#define IDC_LANGUAGE 201
#define IDC_KEYBOARD 202
#define IDC_MEASURE 203
#define IDC_LISTSEP 205
#define IDC_SHORTSAMPLE 209
#define IDC_LONGSAMPLE 210
#define IDC_TIMESAMPLE 214
#define IDC_NUMSAMPLE 218
#define IDC_POSSAMPLE 222
#define IDC_NEGSAMPLE 223
#define IDC_COUNTRYLIST 225    /* hidden list boxes: the whole INF line of each combo item */
#define IDC_LANGLIST 226
#define IDC_KBDLIST 227
/* dialog 15 */
#define IDC_DATESEP 224
#define IDC_SHORTMDY 231       /* 231..233 */
#define IDC_DAYLZERO 234
#define IDC_MONLZERO 235
#define IDC_CENTURY 236
#define IDC_LONGMDY 237        /* 237..239 */
#define IDC_LDDOW 240
#define IDC_LDDAY 241
#define IDC_LDMONTH 242
#define IDC_LDYEAR 243
#define IDC_LDSEP1 244         /* 244..246 */
#define IDC_LDSAMPLE 247
/* dialog 16 */
#define IDC_AMEDIT 250
#define IDC_PMEDIT 251
#define IDC_12HOUR 252
#define IDC_24HOUR 253
#define IDC_TLZERO0 254
#define IDC_TLZERO1 255
#define IDC_TIMESEP 256
#define IDC_AMRANGE 257
#define IDC_PMRANGE 258
/* dialog 17 */
#define IDC_THOUSAND 260
#define IDC_DECIMAL 261
#define IDC_DIGITS 262
#define IDC_LZERO0 263
#define IDC_LZERO1 264
/* dialog 18 */
#define IDC_PLACEMENT 270
#define IDC_NEGATIVE 274
#define IDC_SYMBOL 275
#define IDC_CURRDIGITS 276

/* ------------------------------------------------------------------ data */
/* the INTL record (0x104 bytes): working copy ds:06F4, defaults ds:07F8. The last four fields are
 * derived from sShortDate / sLongDate and never written to WIN.INI. */
typedef struct {
    char sCountry[24];
    int iCountry, iDate, iTime, iTLZero, iCurrency, iCurrDigits, iNegCurr, iLzero, iDigits, iMeasure;
    char s1159[9], s2359[9], sCurrency[6];
    char sThousand[4], sDecimal[4], sDate[4], sTime[4], sList[4];
    char sLongDate[80], sShortDate[80];
    char sLanguage[4];
    int iDayLzero, iMonLzero, iCentury, iLDate;
} INTL;

static INTL g_intl = {"", 1, 0, 0, 0, 0, 2, 0, 1, 2, 1, "AM", "PM", "$", ",", ".", "/", ":", ",",
                      "dddd, MMMM dd, yyyy", "M/d/yy", "USA", 1, 0, 1, 0};
static const INTL g_intlDef = {"Other Country", 1, 0, 0, 0, 0, 2, 0, 1, 2, 1, "AM", "PM", "$", ",", ".", "/",
                               ":", ",", "dddd, MMMM d, yyyy", "M/d/yy", "USA", 1, 0, 1, 0};

static const char szIntl[] = "intl";

/* ds:08FC the [intl] key names, ds:092E the positive and ds:0932 the negative currency formats,
 * ds:0948 the Currency dialog's placement formats. They are one table in DGROUP: an index past
 * one list reads the next one (iNegCurr comes from WIN.INI or from a list with nothing selected,
 * and is not range-checked), which TableEntry keeps; beyond the table it gives "" (3.1 reads the
 * bytes of the next string as pointers there). */
static const char *const aszTable[] = {
    "iCountry", "iDate", "iTime", "iTLZero", "iCurrency", "iCurrDigits", "iNegCurr", "iLzero", "iDigits",
    "iMeasure", "s1159", "s2359", "sCurrency", "sThousand", "sDecimal", "sDate", "sTime", "sList",
    "sShortDate", "sLongDate", "sLanguage", "sCountry", "iDayLzero", "iMonLzero", "iCentury",
    "%s%s", "%s %s",
    "(%s%s)", "-%s%s", "%s-%s", "%s%s-", "(%s%s)", "-%s%s", "%s-%s", "%s%s-", "-%s %s", "-%s %s", "%s %s-",
    "%s1", "1%s", "%s 1", "1 %s",
};
#define POSFMT 25
#define NEGFMT 27
#define PLACEFMT 38
static const char *TableEntry(int i)
{
    return i >= 0 && i < (int)(sizeof aszTable / sizeof aszTable[0]) ? aszTable[i] : "";
}
#define KEY(i) aszTable[i]

static int g_iCountrySel;       /* [0x18aa] */
static int g_iLangSel;          /* [0x1d6e] */
static int g_iKbdSel;           /* [0x0e7a] */
static char *g_pInf;            /* [0x0e3c] the country line being parsed */
static int g_hour, g_min, g_sec, g_month, g_day, g_year, g_dow;   /* [0x1004].. */
static char g_szDllName[0x9E];  /* [0x11f0] */
static WORD g_wDisk;            /* [0x0e6c] disk character of the last "N:file" */
static char g_szSrcPath[0x9E], g_szSrcPathAlt[0x9E];   /* [0x0f40], [0x0e98] installer source */
/* from the CPL_INIT code (seg3:013A) */
static char g_szSharedDir[0x9E];   /* [0x1ecc] where VerFindFile puts shared files */
static char g_szSystemIni[0x9E];   /* [0x174e] */
static char g_szSetupInf[0x9E];    /* [0x12f0] */
/* Date Format dialog */
static HWND g_hLD[7];           /* [0x0fea] long date row in display order: dow, sep, c1, sep, c2, sep, c3 */
static HWND g_hDay, g_hMonth, g_hYear;   /* [0x1742], [0x1744], [0x1da6] */
static RECT g_rcLD;             /* [0x1036] the long date row */
static RECT g_rcSep0;           /* [0x11e4] edit 244: position, then width/height */
static int g_dxLDGap;           /* [0x1dbe] */
/* Time Format dialog */
static char g_sz24Suffix[9];    /* [0x1a18] */
/* Currency Format dialog */
static BOOL g_fCurrDirty;       /* [0x0e3a] */

static long long llClockOffset;   /* arch311: the clock minus the system clock in ns (tests) */

/* ------------------------------------------------------------------ the clock */
static long long NowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void ClockNow(struct tm *t)
{
    time_t now = (time_t)((NowNs() + llClockOffset) / 1000000000LL);
    localtime_r(&now, t);
}

/* arch311: tests start the clock at ARCH311_CLOCK ("1993-11-08 09:30:00") when the applet opens,
 * to the nanosecond, so a script's pauses decide which second each sample shows */
static void ClockFromEnv(void)
{
    const char *fixed = getenv("ARCH311_CLOCK");
    struct tm t;
    llClockOffset = 0;
    if (!fixed || !*fixed) return;
    memset(&t, 0, sizeof t);
    if (sscanf(fixed, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
        return;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    time_t when = mktime(&t);
    if (when != (time_t)-1) llClockOffset = (long long)when * 1000000000LL - NowNs();
}

/* seg1:189B, DOS 2Ch */
static void GetTime(void)
{
    struct tm t;
    ClockNow(&t);
    g_hour = t.tm_hour;
    g_min = t.tm_min;
    g_sec = t.tm_sec;
}

/* seg1:18BB, DOS 2Ah */
static void GetDate(void)
{
    struct tm t;
    ClockNow(&t);
    g_month = t.tm_mon + 1;
    g_day = t.tm_mday;
    g_year = t.tm_year + 1900;
    g_dow = t.tm_wday;
}

/* ------------------------------------------------------------------ string helpers */
/* seg1:1028 (never finds the NUL) */
static char *StrChr(const char *s, int ch)
{
    for (; *s; s++)
        if (*s == (char)ch) return (char *)s;
    return NULL;
}

/* seg1:1058: the last ch in [s, end); end NULL = the whole string */
static char *StrRChrRange(const char *s, const char *end, int ch)
{
    const char *r = NULL;
    if (!end) end = s + lstrlen(s);
    for (; s < end; s++)
        if (*s == (char)ch) r = s;
    return (char *)r;
}

/* seg1:12C8: copies at most n characters (n < 0: nothing); returns how many */
static int StrCpyN(char *dst, const char *src, int n)
{
    if (n < 0) return n;
    int len = lstrlen(src);
    if (n > len) n = len;
    memmove(dst, src, n);
    dst[n] = 0;
    return n;
}

/* seg1:11DC: lstrcmp of the first n characters of each */
static int StrNCmp(const char *a, const char *b, int n)
{
    int la = lstrlen(a), lb = lstrlen(b);
    if (la > n) la = n;
    if (lb > n) lb = n;
    int c = memcmp(a, b, la < lb ? la : lb);
    return c ? c : la - lb;
}

/* seg1:14CD */
static char *StrStr(const char *s, const char *sub)
{
    int len = lstrlen(sub);
    for (;;) {
        s = StrChr(s, sub[0]);
        if (!s) return NULL;
        if (StrNCmp(s, sub, len) == 0) return (char *)s;
        s++;
    }
}

/* arch311: a bounded copy where 3.1 trusted the length */
static void CopyTrunc(char *dst, const char *src, size_t cb)
{
    size_t n = strlen(src);
    if (n >= cb) n = cb - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* seg4:0168 */
static char *StrStrOrEnd(const char *s, const char *sub)
{
    char *r = StrStr(s, sub);
    return r ? r : (char *)s + lstrlen(s);
}

/* seg1:061C: a trailing '\' unless there is one; returns the end of the string */
static char *AddBackslash(char *path)
{
    char *e = path + lstrlen(path);
    if (!*path || e[-1] != '\\') {
        *e++ = '\\';
        *e = 0;
    }
    return e;
}

/* seg20:0CAF */
static void ReplaceFileName(char *path, const char *name)
{
    char *q = path;
    for (char *p = path; *p; p++)
        if (*p == '\\' || *p == ':') q = p + 1;
    lstrcpy(q, name);
}

/* seg1:06D8: the text up to ch (not trimmed) */
static const char *GetField(char *dst, const char *src, int ch, int cbDst)
{
    const char *p = StrChr(src, ch);
    int len = p ? (int)(p - src) : lstrlen(src);
    if (cbDst - 1 < len) len = cbDst - 1;
    return src + StrCpyN(dst, src, len);
}

/* seg1:0727: "3:kbdgr.dll , ..." -> disk '3', "kbdgr.dll " */
static const char *ParseDiskFile(const char *src, WORD *pDisk, char *dst, int cbDst)
{
    const char *q = StrChr(src + 1, ':');
    if (q && !StrRChrRange(src + 1, q, ',')) {
        src = q + 1;
        *pDisk = (WORD)(int)(signed char)q[-1];
    } else
        *pDisk = 0;
    return GetField(dst, src, ',', cbDst);
}

/* ------------------------------------------------------------------ INF reader (seg23) */
/* seg23:0000: the INF named by g_szSetupInf */
static HFILE InfOpen(void)
{
    OFSTRUCT of;
    return LZOpenFile(g_szSetupInf, &of, OF_READ);
}

/* seg23:0017 */
static char *SkipWhite(char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* seg23:0041: where the line at p ends (CR, LF, a ';' outside quotes, or end); NULL at a NUL */
static char *FindLineEnd(char *p, char *end)
{
    BOOL fQuote = FALSE;
    while (end > p) {
        switch (*p) {
        case ';':
            if (!fQuote) return p;
            break;
        case 0:
            return NULL;
        case '\n':
        case '\r':
            return p;
        case '"':
            fQuote = !fQuote;
            break;
        }
        p++;
    }
    return p;
}

/* seg23:009B: the file offset of the line after "[section" (the ']' is not checked; first match),
 * 0 if there is none. Reads in 0x401-byte chunks that overlap by the header length, as 3.1 does. */
static DWORD InfFindSection(HFILE hf, const char *section)
{
    char buf[0x402];
    DWORD pos = 0;
    char *pNL = NULL, *p, *q;
    int lenSect = lstrlen(section), cbChunk = 0x401, n;
    LZSeek(hf, 0, 0);
    cbChunk -= lenSect + 3;
    if (cbChunk <= 0) return 0;
    n = LZRead(hf, buf, lenSect + 2);
    if (n < 0) n = 0;
    buf[n] = 0;
    for (;;) {
        n = LZRead(hf, buf + lenSect + 2, cbChunk);
        if (n <= 0) break;
        buf[lenSect + 2 + n] = 0;
        for (p = buf; (pNL = StrChr(p, '\n')) != NULL; p = pNL) {
            pNL++;
            q = p++;
            if (*q == '[' && StrNCmp(p, section, lenSect) == 0) {
                pos += (WORD)(pNL - buf);
                return pos;
            }
        }
        if (cbChunk > n) break;
        pos += n;
        memmove(buf, buf + n, lstrlen(buf + n) + 1);   /* the last lenSect + 2 bytes */
    }
    return 0;
}

/* seg23:0414: copies the next line of the section from pos to buf (leading blanks skipped,
 * comment lines and blank lines passed over). Returns the distance from pos to the end of the
 * line, -1 if it does not fit in cbBuf - 1, -2 at the next section or the end of the file. */
static int InfReadLine(HFILE hf, DWORD pos, char *buf, int cbBuf)
{
    DWORD posOrig = pos;
    char *bufStart = buf, *end, *p, *q, *scan = buf;
    int n, cbRead = --cbBuf;
reseek:
    pos += (DWORD)(scan - bufStart);
    LZSeek(hf, (LONG)pos, 0);
    scan = bufStart;
    n = cbBuf = LZRead(hf, bufStart, cbBuf);
    if (n <= 0) return -2;
    end = bufStart + n;
    *end = 0;
next:
    p = SkipWhite(scan);
    if (p >= end) {
        scan = p;
        goto reseek;
    }
    if (*p == ';') {
        /* a comment line; one that runs past the buffer is read on sequentially, and 3.1 skips
         * the first byte of each new chunk unchecked */
        for (;;) {
            if (*p == '\n') {
                scan = p;
                goto next;
            }
            if (end > p) {
                p++;
                continue;
            }
            pos += (DWORD)(p - bufStart);
            n = cbBuf = LZRead(hf, bufStart, cbBuf);
            if (n <= 0) return -2;
            end = bufStart + n;
            *end = 0;
            p = bufStart + 1;
        }
    }
    if (*p == '[') return -2;
    q = FindLineEnd(p, end);
    if (!q) return -2;
    if (q >= end && cbRead == n) {
        /* not terminated inside a full buffer: too long, or read again from its start */
        if (p == bufStart) return -1;
        scan = p;
        goto reseek;
    }
    *q = 0;
    memmove(bufStart, p, lstrlen(p) + 1);
    return (int)(pos - posOrig) + (int)(q - bufStart);
}

/* seg23:052E: the section's lines, NUL-separated with an extra NUL at the end (malloc'd), or
 * NULL. A last line that ends the file without a newline is left out, as in 3.1. */
static char *InfGetSection(const char *section)
{
    HFILE hf = InfOpen();
    if (hf == HFILE_ERROR) return NULL;
    DWORD pos = InfFindSection(hf, section);
    if (pos == 0) {
        LZClose(hf);
        return NULL;
    }
    DWORD fileSize = (DWORD)LZSeek(hf, 0, 2);
    int size = 0x400;
    char *base = calloc(1, size), *p = base, *limit = base + size - 1;
    if (!base) {
        LZClose(hf);
        return NULL;
    }
    for (;;) {
        int r = InfReadLine(hf, pos, p, (int)(limit - p));
        if (r == 0 || r == -2) break;
        if (r == -1) {
            /* grow by 0x400 and read the same line again */
            int off = (int)(p - base);
            char *nb = realloc(base, size + 0x400);
            if (!nb) {
                free(base);
                LZClose(hf);
                return NULL;
            }
            memset(nb + size, 0, 0x400);
            size += 0x400;
            base = nb;
            limit = base + size;
            p = base + off;
            continue;
        }
        pos += r;
        if (pos >= fileSize) break;
        p += lstrlen(p) + 1;
    }
    *p = 0;
    LZClose(hf);
    return base;
}

/* seg23:0682: one combo box item per line of the section (its description from lpfnDesc), and
 * the whole line at the same index in the hidden list box; returns the number of items */
static int InfFillLists(HWND hCombo, HWND hList, UINT msgAdd, const char *section, int (*lpfnDesc)(char *out, char *line))
{
    int count = 0, cb = 0x400;
    UINT msgDel = (msgAdd == LB_ADDSTRING || msgAdd == LB_INSERTSTRING) ? LB_DELETESTRING : CB_DELETESTRING;
    char desc[0x100 + 2];
    HFILE hf = InfOpen();
    if (hf == HFILE_ERROR) return 0;
    DWORD pos = InfFindSection(hf, section);
    if (pos) {
        DWORD fileSize = (DWORD)LZSeek(hf, 0, 2);
        char *buf = malloc(cb);
        if (buf) {
            do {
                int r = InfReadLine(hf, pos, buf, cb);
                if (r == -2 || r == 0) break;
                if (r == -1) {
                    char *nb = realloc(buf, cb + 0x400);
                    if (!nb) break;
                    buf = nb;
                    cb += 0x400;
                    continue;
                }
                lpfnDesc(desc, buf);
                int idx = (int)SendMessage(hCombo, msgAdd, (WPARAM)-1, (LPARAM)desc);
                if (idx >= 0) {
                    if ((int)SendMessage(hList, LB_INSERTSTRING, idx, (LPARAM)buf) < 0)
                        SendMessage(hCombo, msgDel, idx, 0);
                    else
                        count++;
                }
                pos += r;
            } while (pos < fileSize);
            free(buf);
        }
    }
    LZClose(hf);
    return count;
}

/* seg1:1B30: a copy of the section line whose file name (after "N:", up to the ',') is `file`
 * (malloc'd), or NULL */
static char *InfFindLineByFile(const char *section, const char *file)
{
    char *hSect, *p, *result = NULL;
    if (!file || !*file) return NULL;
    if (!(hSect = InfGetSection(section))) return NULL;
    for (p = hSect; *p; p += lstrlen(p) + 1) {
        char *q = p, *r;
        while (*q)
            if (*q++ == ':') break;
        r = q;
        while (*r && *r != ',') r++;
        do r--;
        while (r > p && *r == ' ');
        r++;
        char save = *r;
        *r = 0;
        int c = lstrcmpi(q, file);
        *r = save;
        if (c == 0) {
            result = strdup(p);
            break;
        }
    }
    free(hSect);
    return result;
}

/* seg12:178F: InfFillLists' description: the text between the first two quotes */
static int GetQuotedString(char *out, char *line)
{
    char *p, *q;
    *out = 0;
    if (!(p = StrChr(line, '"'))) return 0;
    p++;
    if (!(q = StrChr(p, '"'))) return 0;
    if (q - p > 0x100) q = p + 0x100;   /* arch311: the buffer's size (CONTROL.INF/SETUP.INF fit) */
    StrCpyN(out, p, (int)(q - p));
    return (int)(q - p);
}

/* ------------------------------------------------------------------ date formats */
/* a date picture ("dddd, MMMM dd, yyyy") taken apart by ParseDateFormat (seg12:17EE) */
typedef struct {
    int wDow;           /* the token code of a leading ddd/dddd, else 0 */
    char sepDow[6];     /* the literal after it */
    int comp[3];        /* token codes of the three components (0 = not M/d/y) */
    char sep[2][6];     /* the literals after comp[0] and comp[1] */
} DATEFMT;

/* seg12:0000: the token at src into dst. Returns its code: 0x30 + n for n 'M's, 0x40 + n 'd's,
 * 0x50 + n 'y's (n up to 4), 0x60 + the source characters a literal used (quotes included; one
 * trailing blank is dropped from the copy), 0 at the end. */
static int GetDateToken(const char *src, char *dst)
{
    const char *start = src;
    char *dst0 = dst;
    char c = *src, ch;
    int type, count;
    switch (c) {
    case 0:
        return 0;
    case 'y':
        type = 0x50;
        goto run;
    case 'M':
        type = 0x30;
        goto run;
    case 'd':
        type = 0x40;
        goto run;
    case '\'':
        src++;              /* the opening quote */
        /* fall through */
    default:
        type = 0x60;
        break;
    }
    for (;;) {
        ch = *src;
        if (ch == 'y' || ch == 'M' || ch == 'd') {
            if (c == '\'') goto copy;           /* letters are literal inside quotes */
            break;
        }
        if (ch == 0) break;
        if (ch == '\'') {
            src++;
            if (c != '\'') {
                /* an unquoted literal copies the character after a quote; 3.1 copies even a NUL
                 * there and reads on past the string (arch311: ends the literal) */
                if (!*src) break;
                goto copy;
            }
            if (*src != '\'') break;            /* the closing quote */
            /* '' is one quote */
        }
    copy:
        *dst++ = *src++;
    }
    if (dst > dst0) {
        /* one trailing blank is dropped (3.1 tests dst[-1] even when nothing was copied) */
        dst--;
        if (*dst != ' ') dst++;
    }
    *dst = 0;
    return (int)(src - start) + type;
run:
    count = 0;
    do {
        count++;
        *dst++ = *src++;
    } while (*src == c && (unsigned)count < 4);
    *dst = 0;
    return (int)(src - start) + type;
}

/* seg12:17EE (also called by the Date Format dialog). Slots the picture does not reach keep
 * what they held: callers pass a zeroed DATEFMT (3.1's is uninitialised stack). */
static void ParseDateFormat(const char *fmt, DATEFMT *out)
{
    char buf[0x80 + 0x80];
    int i, code = GetDateToken(fmt, buf);
    if ((code & 0xF0) == 0x40 && (unsigned)(code & 0x0F) > 2) {
        out->wDow = code;
        fmt += code & 0x0F;
        code = GetDateToken(fmt, buf);
    } else
        out->wDow = 0;
    if ((BYTE)(code & 0xF0) >= 0x60) {
        StrCpyN(out->sepDow, buf, 5);
        fmt += code - 0x60;
        code = GetDateToken(fmt, buf);
    } else
        out->sepDow[0] = 0;
    for (i = 0; i < 3 && code != 0; i++) {
        int t = code & 0xF0;
        out->comp[i] = (t == 0x30 || t == 0x40 || t == 0x50) ? code : 0;
        if (i < 2) {
            fmt += code & 0x0F;
            code = GetDateToken(fmt, buf);
            if ((BYTE)(code & 0xF0) >= 0x60) {
                StrCpyN(out->sep[i], buf, 5);
                fmt += code - 0x60;
                code = GetDateToken(fmt, buf);
            } else
                out->sep[i][0] = 0;
        }
    }
}

/* seg12:00EA: iDate (the order) and the leading zero / century flags from sShortDate */
static void ParseShortDate(INTL *p)
{
    DATEFMT df;
    memset(&df, 0, sizeof df);
    ParseDateFormat(p->sShortDate, &df);
    /* M -> 0, d -> 1, y -> 2; anything else gives 0x0FFD (a 16-bit logical shift) */
    p->iDate = (WORD)(((BYTE)df.comp[0] & 0xF0) - 0x30) >> 4;
    for (int i = 0; i < 3; i++) {
        int v = df.comp[i];
        switch (v & 0xF0) {
        case 0x30: p->iMonLzero = (v & 0x0F) - 1; break;
        case 0x40: p->iDayLzero = (v & 0x0F) - 1; break;
        case 0x50: p->iCentury = (v & 0x0F) - 2; break;    /* yy 0, yyyy 2 */
        }
    }
}

/* ------------------------------------------------------------------ the samples */
/* seg12:029A */
static void UpdateDateSamples(HWND hDlg, INTL *p)
{
    char szYear[8], szMon[0x10], szDay[0x10], szDow[0x10], szOut[0x100];
    const char *a, *b, *c;
    DATEFMT df;
    int yr, v = 0;
    GetDate();
    yr = g_year;
    if (p->iCentury == 0) yr = g_year % 100;   /* no leading zero is added: 2005 -> "5" */
    wsprintf(szYear, "%d", yr);
    wsprintf(szMon, p->iMonLzero ? "%02d" : "%d", g_month);
    wsprintf(szDay, p->iDayLzero ? "%02d" : "%d", g_day);
    if (p->iDate == 2) { a = szYear; b = szMon; c = szDay; }
    else if (p->iDate == 1) { a = szDay; b = szMon; c = szYear; }
    else { a = szMon; b = szDay; c = szYear; }
    wsprintf(szOut, "%s%s%s%s%s", a, p->sDate, b, p->sDate, c);
    SetDlgItemText(hDlg, IDC_SHORTSAMPLE, szOut);

    /* the long date reuses the buffers: a component sLongDate lacks keeps its short form */
    memset(&df, 0, sizeof df);
    ParseDateFormat(p->sLongDate, &df);
    if (df.wDow)
        LoadString(hInstMain, (((BYTE)df.wDow & 0x0F) == 4 ? 0 : 7) + g_dow + 260, szDow, sizeof szDow);
    else
        szDow[0] = 0;
    for (int i = 2; i >= 0; i--) {
        v = df.comp[i];
        switch (v & 0xF0) {
        case 0x40:
            wsprintf(szDay, ((BYTE)v & 0x0F) == 2 ? "%02d" : "%d", g_day);
            break;
        case 0x30:
            switch (v & 0x0F) {
            case 3: LoadString(hInstMain, g_month + 0x0C + 0x113, szMon, sizeof szMon); break;
            case 4: LoadString(hInstMain, g_month + 0x113, szMon, sizeof szMon); break;
            default: wsprintf(szMon, ((BYTE)v & 0x0F) == 2 ? "%02d" : "%d", g_month); break;
            }
            break;
        case 0x50: {
            int yr2 = g_year;
            if (((BYTE)v & 0x0F) == 2) yr2 = g_year % 100;
            wsprintf(szYear, "%d", yr2);
            break;
        }
        }
    }
    /* the order follows comp[0] alone */
    switch (v & 0xF0) {
    case 0x40: a = szDay; b = szMon; c = szYear; break;
    case 0x50: a = szYear; b = szMon; c = szDay; break;
    default: a = szMon; b = szDay; c = szYear; break;
    }
    wsprintf(szOut, "%s%s %s%s %s%s %s", szDow, df.sepDow, a, df.sep[0], b, df.sep[1], c);
    SetDlgItemText(hDlg, IDC_LONGSAMPLE, szOut);
}

/* seg12:017A */
static void UpdateTimeSample(HWND hDlg, INTL *p)
{
    char buf[0x40];
    const char *pAmPm;
    GetTime();
    pAmPm = g_hour >= 12 ? p->s2359 : p->s1159;    /* picked before the 12-hour fold */
    if (p->iTime == 0) {
        g_hour %= 12;
        if (g_hour == 0) g_hour = 12;
    }
    wsprintf(buf, p->iTLZero ? "%02d%s%02d%s%02d %s" : "%d%s%02d%s%02d %s", g_hour, p->sTime, g_min, p->sTime,
             g_sec, pAmPm);
    SetDlgItemText(hDlg, IDC_TIMESAMPLE, buf);
}

/* seg12:0212: "1,234.22" - the digit shown is the number of decimals (iLzero is not shown) */
static void UpdateNumberSample(HWND hDlg, INTL *p)
{
    char buf[0x20], *q;
    int n;
    wsprintf(buf, "1%s234%s", p->sThousand, p->sDecimal);
    q = buf + lstrlen(buf);
    n = p->iDigits;
    if (n > 6) n = 6;
    /* a negative count is not clamped in 3.1 (it fills memory until the 16-bit count wraps) */
    if (n < 0) n = 0;
    char ch = (char)(n + '0');
    while (n != 0) {
        *q++ = ch;
        n--;
    }
    *q = 0;
    SetDlgItemText(hDlg, IDC_NUMSAMPLE, buf);
}

/* seg12:0575 */
static void FormatPair(char *out, const char *fmt, const char *x, const char *y, BOOL fSwap)
{
    if (fSwap) {
        const char *t = x;
        x = y;
        y = t;
    }
    wsprintf(out, fmt, x, y);
}

/* seg12:05C4 */
static void UpdateCurrencySamples(HWND hDlg, INTL *p)
{
    char num[0x40], out[0x80], *q;
    BOOL fSwap;
    int n;
    wsprintf(num, "1%s", p->sDecimal);
    q = num + lstrlen(num);
    char ch = (char)(p->iCurrDigits + '0');
    /* not clamped in 3.1 (a large count runs past the buffer); arch311 stops at its end */
    for (n = p->iCurrDigits; n > 0 && q < num + sizeof num - 1; n--) *q++ = ch;
    *q = 0;
    /* iCurrency: 0 "$1.22", 1 "1.22$", 2 "$ 1.22", 3 "1.22 $" */
    FormatPair(out, TableEntry(POSFMT + p->iCurrency / 2), p->sCurrency, num, p->iCurrency & 1);
    SetDlgItemText(hDlg, IDC_POSSAMPLE, out);
    fSwap = p->iNegCurr < 4 || p->iNegCurr == 9;
    FormatPair(out, TableEntry(NEGFMT + p->iNegCurr), num, p->sCurrency, fSwap);
    SetDlgItemText(hDlg, IDC_NEGSAMPLE, out);
}

/* seg12:09EA */
static void FillIntlDialog(HWND hDlg, INTL *p)
{
    SetDlgItemText(hDlg, IDC_LISTSEP, p->sList);
    SendDlgItemMessage(hDlg, IDC_MEASURE, CB_SETCURSEL, p->iMeasure, 0);
    UpdateDateSamples(hDlg, p);
    UpdateTimeSample(hDlg, p);
    UpdateNumberSample(hDlg, p);
    UpdateCurrencySamples(hDlg, p);
}

/* ------------------------------------------------------------------ WIN.INI and CONTROL.INF */
/* seg12:077E */
static void IntlReadProfile(void)
{
    g_intl.iCountry = GetProfileInt(szIntl, KEY(0), g_intlDef.iCountry);
    g_intl.iDate = GetProfileInt(szIntl, KEY(1), g_intlDef.iDate);
    g_intl.iTime = GetProfileInt(szIntl, KEY(2), g_intlDef.iTime);
    g_intl.iTLZero = GetProfileInt(szIntl, KEY(3), g_intlDef.iTLZero);
    g_intl.iCurrency = GetProfileInt(szIntl, KEY(4), g_intlDef.iCurrency);
    g_intl.iCurrDigits = GetProfileInt(szIntl, KEY(5), g_intlDef.iCurrDigits);
    g_intl.iNegCurr = GetProfileInt(szIntl, KEY(6), g_intlDef.iNegCurr);
    g_intl.iLzero = GetProfileInt(szIntl, KEY(7), g_intlDef.iLzero);
    g_intl.iDigits = GetProfileInt(szIntl, KEY(8), g_intlDef.iDigits);
    g_intl.iMeasure = GetProfileInt(szIntl, KEY(9), g_intlDef.iMeasure);
    if (g_intl.iMeasure > 1) g_intl.iMeasure = 0;
    GetProfileString(szIntl, KEY(10), g_intlDef.s1159, g_intl.s1159, 9);
    GetProfileString(szIntl, KEY(11), g_intlDef.s2359, g_intl.s2359, 9);
    GetProfileString(szIntl, KEY(12), g_intlDef.sCurrency, g_intl.sCurrency, 6);
    GetProfileString(szIntl, KEY(13), g_intlDef.sThousand, g_intl.sThousand, 4);
    GetProfileString(szIntl, KEY(14), g_intlDef.sDecimal, g_intl.sDecimal, 4);
    GetProfileString(szIntl, KEY(15), g_intlDef.sDate, g_intl.sDate, 4);
    GetProfileString(szIntl, KEY(16), g_intlDef.sTime, g_intl.sTime, 4);
    GetProfileString(szIntl, KEY(17), g_intlDef.sList, g_intl.sList, 4);
    GetProfileString(szIntl, KEY(18), g_intlDef.sShortDate, g_intl.sShortDate, 0x50);
    GetProfileString(szIntl, KEY(19), g_intlDef.sLongDate, g_intl.sLongDate, 0x50);
    GetProfileString(szIntl, KEY(20), g_intlDef.sLanguage, g_intl.sLanguage, 4);
    GetProfileString(szIntl, KEY(21), g_intlDef.sCountry, g_intl.sCountry, 0x18);
    ParseShortDate(&g_intl);
}

/* seg12:06BD: the next field of the country line at g_pInf (up to '!' or '"'). An all-blank field
 * is " ". Returns the number it starts with. (3.1 does not limit the copy; cbOut is arch311's.) */
static int GetInfField(char *out, int cbOut)
{
    BOOL fSpace = FALSE;
    int val = 0;
    char *start;
    if (*g_pInf == ' ') {
        fSpace = TRUE;
        while (*g_pInf == ' ') g_pInf++;
    } else if (*g_pInf == '"')
        g_pInf++;
    start = g_pInf;
    while (*g_pInf != '"') {
        if (*g_pInf == '!' || *g_pInf == 0) break;
        fSpace = FALSE;
        g_pInf++;
    }
    int n = (int)(g_pInf - start);
    if (n > cbOut - 1) n = cbOut - 1;
    StrCpyN(out, start, n);
    if (*g_pInf) g_pInf++;
    if (fSpace) {
        out[0] = ' ';
        out[1] = 0;
    }
    while ((signed char)*out >= '0') {
        if ((signed char)*out > '9') break;
        val = val * 10 + *out - '0';
        out++;
    }
    return val;
}

/* seg12:0E22: a CONTROL.INF [country] line:
 *   "name", "iCountry!iDate!iCurrency!iDigits!iTime!iLzero!iMeasure!iCurrDigits!iNegCurr!iTLZero!
 *            s1159!s2359!sCurrency!sThousand!sDecimal!sDate!sTime!sList!sShortDate!sLongDate!sLanguage" */
static void ParseCountryRecord(INTL *p)
{
    char tmp[0x14];
    GetInfField(p->sCountry, sizeof p->sCountry);
    g_pInf = StrStrOrEnd(g_pInf, "\"");
    if (*g_pInf) g_pInf++;
    p->iCountry = GetInfField(tmp, sizeof tmp);
    p->iDate = GetInfField(tmp, sizeof tmp);
    p->iCurrency = GetInfField(tmp, sizeof tmp);
    p->iDigits = GetInfField(tmp, sizeof tmp);
    p->iTime = GetInfField(tmp, sizeof tmp);
    p->iLzero = GetInfField(tmp, sizeof tmp);
    p->iMeasure = GetInfField(tmp, sizeof tmp);
    p->iCurrDigits = GetInfField(tmp, sizeof tmp);
    p->iNegCurr = GetInfField(tmp, sizeof tmp);
    p->iTLZero = GetInfField(tmp, sizeof tmp);
    GetInfField(p->s1159, sizeof p->s1159);
    GetInfField(p->s2359, sizeof p->s2359);
    GetInfField(p->sCurrency, sizeof p->sCurrency);
    GetInfField(p->sThousand, sizeof p->sThousand);
    GetInfField(p->sDecimal, sizeof p->sDecimal);
    GetInfField(p->sDate, sizeof p->sDate);
    GetInfField(p->sTime, sizeof p->sTime);
    GetInfField(p->sList, sizeof p->sList);
    GetInfField(p->sShortDate, sizeof p->sShortDate);
    GetInfField(p->sLongDate, sizeof p->sLongDate);
    GetInfField(p->sLanguage, sizeof p->sLanguage);
}

/* seg12:0F7A: the new country's settings (the language and keyboard stay as they are) */
static void OnCountrySelChange(HWND hDlg)
{
    char line[0x100];
    int sel = (int)SendDlgItemMessage(hDlg, IDC_COUNTRY, CB_GETCURSEL, 0, 0);
    if (g_iCountrySel == sel) return;
    if ((WORD)SendDlgItemMessage(hDlg, IDC_COUNTRYLIST, LB_GETTEXTLEN, sel, 0) >= 0x100) return;
    g_iCountrySel = sel;
    g_pInf = line;
    SendDlgItemMessage(hDlg, IDC_COUNTRYLIST, LB_GETTEXT, sel, (LPARAM)line);
    ParseCountryRecord(&g_intl);
    ParseShortDate(&g_intl);
    FillIntlDialog(hDlg, &g_intl);
}

/* ------------------------------------------------------------------ OK */
/* seg12:102F: a changed language or keyboard layout. 3.1 first installs the DLL the INF line
 * names ("3:kbdgr.dll") from the Windows disks (InstallFiles seg1:07BA with dialogs 23 and 30;
 * a cancelled install keeps the dialog open; it also records the file in CONTROL.INI
 * [installed]), then loads it. Neither is done here (16-bit DLLs). The INI entries match a real
 * 3.11 run that installed German from an A: drive: SYSTEM.INI [boot] language.dll=langger.dll,
 * [boot.description] language.dll=German, [keyboard] keyboard.dll=kbdgr.dll (KERNEL drops the
 * blank MAIN.CPL leaves after the name). */
static BOOL InstallDriverFromList(HWND hDlg, int idCombo)
{
    char line[0x100], desc[0x100], szSect[0x14], szKey[0x14], szOld[0x9E];
    int origSel, idList, sel;
    HWND hCombo, hList;
    if (idCombo == IDC_LANGUAGE) {
        origSel = g_iLangSel;
        idList = IDC_LANGLIST;
    } else {
        origSel = g_iKbdSel;
        idList = IDC_KBDLIST;
    }
    hCombo = GetDlgItem(hDlg, idCombo);
    hList = GetDlgItem(hDlg, idList);
    sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel == origSel) return TRUE;
    line[0] = 0;
    if ((int)SendMessage(hList, LB_GETTEXTLEN, sel, 0) < (int)sizeof line)
        SendMessage(hList, LB_GETTEXT, sel, (LPARAM)line);
    if (StrChr(line, ':')) {
        /* TODO: InstallFiles(hDlg, {line}, 1, DriverExistsCB seg12:0FEF, 1) copies the DLL */
        ParseDiskFile(line, &g_wDisk, g_szDllName, sizeof g_szDllName);   /* "kbdgr.dll " (not trimmed) */
    } else {
        g_szDllName[0] = 0;
        lstrcpy(g_intl.sLanguage, g_intlDef.sLanguage);   /* (never written to WIN.INI) */
    }
    if (idCombo == IDC_LANGUAGE) {
        /* USER loads the language driver here; libw16 only records it in SYSTEM.INI [boot]
         * (there are no 16-bit language drivers on arch311) */
        if (!SystemParametersInfo(SPI_SETLANGDRIVER, 0, g_szDllName, SPIF_UPDATEINIFILE)) {
            SetFocus(hCombo);
            return FALSE;
        }
        desc[0] = 0;
        if ((int)SendMessage(hCombo, CB_GETLBTEXTLEN, sel, 0) < (int)sizeof desc)
            SendMessage(hCombo, CB_GETLBTEXT, sel, (LPARAM)desc);
        LoadString(hInstMain, 71, szSect, sizeof szSect);   /* "boot.description" */
        LoadString(hInstMain, 68, szKey, sizeof szKey);     /* "language.dll" */
        WritePrivateProfileString(szSect, szKey, desc, g_szSystemIni);
        return TRUE;
    }
    LoadString(hInstMain, 69, szSect, sizeof szSect);       /* "keyboard" */
    LoadString(hInstMain, 70, szKey, sizeof szKey);         /* "keyboard.dll" */
    GetPrivateProfileString(szSect, szKey, "", szOld, sizeof szOld, g_szSystemIni);
    WritePrivateProfileString(szSect, szKey, g_szDllName, g_szSystemIni);
    /* 3.1 then calls KEYBOARD.DRV's NewTable and, if VerFindFile does not report the new DLL in
     * use, writes szOld back and fails. There is no NewTable here (GetProcAddress gives NULL),
     * which 3.1 takes as success. TODO: switch the Linux keyboard layout (XKB). */
    (void)szOld;
    return TRUE;
}

/* seg12:1274: WIN.INI would lose a blank or empty separator: write "" or " " quoted */
static void FixBlankSeparator(char *s)
{
    if (*s == 0) {
        lstrcpy(s, "\"\"");
        return;
    }
    const char *p = s;
    int n = lstrlen(s);
    while (n > 0 && *p == ' ') {
        n--;
        p++;
    }
    if (n > 0) return;
    lstrcpy(s, "\" \"");
}

/* seg12:12C9 */
static BOOL IntlSave(HWND hDlg)
{
    char sz[0x100], sz0[8], sz1[8], sz2[8], sz3[8], sz4[8], sz5[8], sz6[8], sz7[8], sz8[8], sz9[8];
    int sel = (int)SendDlgItemMessage(hDlg, IDC_COUNTRY, CB_GETCURSEL, 0, 0);
    /* (3.1 also fetches the country name here, or "Other Country"; it is never used) */
    (void)sel;
    if (!InstallDriverFromList(hDlg, IDC_LANGUAGE) || !InstallDriverFromList(hDlg, IDC_KEYBOARD)) return FALSE;
    HourGlass(TRUE);
    wsprintf(sz0, "%d", g_intl.iCountry);
    wsprintf(sz1, "%d", g_intl.iDate);
    wsprintf(sz2, "%d", g_intl.iTime);
    wsprintf(sz3, "%d", g_intl.iTLZero);
    wsprintf(sz4, "%d", g_intl.iCurrency);
    wsprintf(sz5, "%d", g_intl.iCurrDigits);
    wsprintf(sz6, "%d", g_intl.iNegCurr);
    wsprintf(sz7, "%d", g_intl.iLzero);
    wsprintf(sz8, "%d", g_intl.iDigits);
    wsprintf(sz9, "%d", (int)SendDlgItemMessage(hDlg, IDC_MEASURE, CB_GETCURSEL, 0, 0));
    WriteProfileString(szIntl, KEY(0), sz0);
    WriteProfileString(szIntl, KEY(1), sz1);
    WriteProfileString(szIntl, KEY(2), sz2);
    WriteProfileString(szIntl, KEY(3), sz3);
    WriteProfileString(szIntl, KEY(4), sz4);
    WriteProfileString(szIntl, KEY(5), sz5);
    WriteProfileString(szIntl, KEY(6), sz6);
    WriteProfileString(szIntl, KEY(7), sz7);
    WriteProfileString(szIntl, KEY(8), sz8);
    WriteProfileString(szIntl, KEY(9), sz9);
    WriteProfileString(szIntl, KEY(10), g_intl.s1159);
    WriteProfileString(szIntl, KEY(11), g_intl.s2359);
    WriteProfileString(szIntl, KEY(12), g_intl.sCurrency);
    FixBlankSeparator(g_intl.sThousand);
    WriteProfileString(szIntl, KEY(13), g_intl.sThousand);
    FixBlankSeparator(g_intl.sDecimal);
    WriteProfileString(szIntl, KEY(14), g_intl.sDecimal);
    FixBlankSeparator(g_intl.sDate);
    WriteProfileString(szIntl, KEY(15), g_intl.sDate);
    FixBlankSeparator(g_intl.sTime);
    WriteProfileString(szIntl, KEY(16), g_intl.sTime);
    GetDlgItemText(hDlg, IDC_LISTSEP, g_intl.sList, 3);
    FixBlankSeparator(g_intl.sList);
    WriteProfileString(szIntl, KEY(17), g_intl.sList);
    WriteProfileString(szIntl, KEY(18), g_intl.sShortDate);
    WriteProfileString(szIntl, KEY(19), g_intl.sLongDate);
    /* sLanguage: the first three letters of the language's [language] line ("enu") */
    sz[0] = 0;
    sel = (int)SendDlgItemMessage(hDlg, IDC_LANGUAGE, CB_GETCURSEL, 0, 0);
    if ((int)SendDlgItemMessage(hDlg, IDC_LANGLIST, LB_GETTEXTLEN, sel, 0) < (int)sizeof sz)
        SendDlgItemMessage(hDlg, IDC_LANGLIST, LB_GETTEXT, sel, (LPARAM)sz);
    sz[3] = 0;
    WriteProfileString(szIntl, KEY(20), sz);
    WriteProfileString(szIntl, KEY(21), g_intl.sCountry);
    HourGlass(FALSE);
    BroadcastWinIniChange(5);
    return TRUE;
}

/* ------------------------------------------------------------------ Date Format (dialog 15, seg11) */
/* seg11:0000: the first checked button of id..idLast-1, else idLast (not tested) */
static int GetCheckedRadio(HWND hDlg, int id, int idLast)
{
    for (; idLast > id; id++)
        if (IsDlgButtonChecked(hDlg, id)) break;
    return id;
}

/* seg11:0024: the row's texts joined, a blank after each separator */
static void UpdateLongDateSample(HWND hDlg)
{
    char sz[7 * 0x29 + 4], tmp[0x2A];
    sz[0] = 0;
    for (int i = 0; i < 7; i++) {
        tmp[0] = 0;
        SendMessage(g_hLD[i], WM_GETTEXT, 0x29, (LPARAM)tmp);
        lstrcat(sz, tmp);
        if (i & 1) lstrcat(sz, " ");
    }
    SetDlgItemText(hDlg, IDC_LDSAMPLE, sz);
}

/* seg11:013F: "dd", "MM" and "yyyy" cut to the flags, in the short order */
static void BuildShortDateFormat(char *out)
{
    char szD[3], szM[3], szY[5];
    const char *a, *b, *c;
    if (!LoadString(hInstMain, 175, szD, 3)) return;   /* "dddd" -> "dd" */
    if (!LoadString(hInstMain, 176, szM, 3)) return;   /* "MMMM" -> "MM" */
    if (!LoadString(hInstMain, 177, szY, 5)) return;   /* "yyyy" */
    if (g_intl.iDayLzero == 0) szD[1] = 0;
    if (g_intl.iMonLzero == 0) szM[1] = 0;
    if (g_intl.iCentury == 0) szY[2] = 0;
    if (g_intl.iDate == 2) { a = szY; b = szM; c = szD; }
    else if (g_intl.iDate == 1) { a = szD; b = szM; c = szY; }
    else { a = szM; b = szD; c = szY; }
    wsprintf(out, "%s%s%s%s%s", a, g_intl.sDate, b, g_intl.sDate, c);
}

/* seg11:022D: e.g. dddd', 'MMMM' 'dd', 'yyyy - each separator quoted, with a blank at its end */
static void BuildLongDateFormat(char *dst)
{
    char buf[0x200], sep[0x50], *out = buf, *limit = buf + 0x4F;
    int sel = (int)SendMessage(g_hLD[0], CB_GETCURSEL, 0, 0);
    if (sel != 0) {   /* 1 "ddd", 2 "dddd" (none selected: "d") */
        int n = sel + 2;
        while (n-- > 0) *out++ = 'd';
    }
    for (int i = 1; i < 7;) {
        int j = i++;
        int len = (int)SendMessage(g_hLD[j], WM_GETTEXT, sizeof sep, (LPARAM)sep);
        if (out + len < limit) {
            *out++ = '\'';
            for (const char *src = sep; *src;) {
                if (*src == '\'') *out++ = '\'';
                *out++ = *src++;
            }
            if (out[-1] != ' ') *out++ = ' ';   /* GetDateToken drops one trailing blank */
            *out++ = '\'';
        }
        int cnt = (int)SendMessage(g_hLD[i], CB_GETCURSEL, 0, 0) + 1;
        char ch;
        if (g_hLD[i] == g_hYear) {
            cnt <<= 1;   /* yy, yyyy */
            ch = 'y';
        } else if (g_hLD[i] == g_hMonth)
            ch = 'M';
        else
            ch = 'd';
        if (out + cnt < limit)
            while (cnt-- > 0) *out++ = ch;
        i++;
    }
    *out = 0;
    /* (the length checks above leave out the quotes; the 5-character separators keep it inside
     * the 80 bytes) */
    CopyTrunc(dst, buf, 0x50);
}

/* seg11:008C: places the day, month and year lists in `order` (0 MDY, 1 DMY, 2 YMD) after the
 * separators; the z-order (and so the tab order) follows */
static void ArrangeLongDate(HWND hDlg, int order)
{
    RECT rc;
    int x, y, w;
    switch (order) {
    case 1: g_hLD[2] = g_hDay; g_hLD[4] = g_hMonth; g_hLD[6] = g_hYear; break;
    case 2: g_hLD[2] = g_hYear; g_hLD[4] = g_hMonth; g_hLD[6] = g_hDay; break;
    default: g_hLD[2] = g_hMonth; g_hLD[4] = g_hDay; g_hLD[6] = g_hYear; break;
    }
    x = g_rcSep0.left;
    y = g_rcSep0.top;
    w = g_rcSep0.right;
    for (int i = 2; i < 7; i++) {
        x += g_dxLDGap + w;
        GetWindowRect(g_hLD[i], &rc);
        w = rc.right - rc.left;
        SetWindowPos(g_hLD[i], g_hLD[i - 1], x, y, 0, 0, SWP_NOSIZE | SWP_NOREDRAW);
    }
    InvalidateRect(hDlg, &g_rcLD, TRUE);
}

/* seg11:0388: sLongDate into the row's lists and separators; returns its order or -1 */
static int ParseLongDateIntoControls(const char *fmt)
{
    DATEFMT df;
    int v = 0;
    memset(&df, 0, sizeof df);
    ParseDateFormat(fmt, &df);
    if (df.wDow) v = ((BYTE)df.wDow & 0x0F) - 2;   /* ddd 1, dddd 2 */
    SendMessage(g_hLD[0], CB_SETCURSEL, v, 0);
    for (int i = 2; i >= 0; i--) {
        v = df.comp[i];
        switch (v & 0xF0) {
        case 0x40: SendMessage(g_hDay, CB_SETCURSEL, ((BYTE)v & 0x0F) - 1, 0); break;
        case 0x30: SendMessage(g_hMonth, CB_SETCURSEL, ((BYTE)v & 0x0F) - 1, 0); break;
        case 0x50: SendMessage(g_hYear, CB_SETCURSEL, (((BYTE)v & 0x0E) >> 1) - 1, 0); break;
        }
    }
    int order;
    switch ((BYTE)v & 0xF0) {   /* comp[0] */
    case 0x30: order = 0; break;
    case 0x40: order = 1; break;
    case 0x50: order = 2; break;
    default: order = -1; break;
    }
    SendMessage(g_hLD[1], WM_SETTEXT, 0, (LPARAM)df.sepDow);
    SendMessage(g_hLD[3], WM_SETTEXT, 0, (LPARAM)df.sep[0]);
    SendMessage(g_hLD[5], WM_SETTEXT, 0, (LPARAM)df.sep[1]);
    return order;
}

static void ClientRect(HWND hDlg, HWND h, RECT *rc)
{
    GetWindowRect(h, rc);
    ScreenToClient(hDlg, (POINT *)&rc->left);
    ScreenToClient(hDlg, (POINT *)&rc->right);
}

/* seg11:0495 */
static void DateInitDlg(HWND hDlg)
{
    char sz[0x40];
    int order;
    RECT r;
    g_hLD[0] = GetDlgItem(hDlg, IDC_LDDOW);
    g_hDay = g_hLD[2] = GetDlgItem(hDlg, IDC_LDDAY);
    g_hMonth = g_hLD[4] = GetDlgItem(hDlg, IDC_LDMONTH);
    g_hYear = g_hLD[6] = GetDlgItem(hDlg, IDC_LDYEAR);
    g_hLD[1] = GetDlgItem(hDlg, IDC_LDSEP1);
    g_hLD[3] = GetDlgItem(hDlg, IDC_LDSEP1 + 1);
    g_hLD[5] = GetDlgItem(hDlg, IDC_LDSEP1 + 2);
    SendMessage(g_hLD[1], EM_LIMITTEXT, 5, 0);
    SendMessage(g_hLD[3], EM_LIMITTEXT, 5, 0);
    SendMessage(g_hLD[5], EM_LIMITTEXT, 5, 0);

    /* the row: from the day list's top left to the year list's bottom right */
    ClientRect(hDlg, g_hDay, &r);
    g_rcLD.left = r.left;
    g_rcLD.top = r.top;
    ClientRect(hDlg, g_hYear, &r);
    g_rcLD.right = r.right;
    g_rcLD.bottom = r.bottom;
    ClientRect(hDlg, g_hLD[1], &g_rcSep0);
    if (g_rcLD.bottom < g_rcSep0.bottom) g_rcLD.bottom = g_rcSep0.bottom;
    g_dxLDGap = g_rcLD.left - g_rcSep0.right;
    g_rcSep0.bottom -= g_rcSep0.top;
    g_rcSep0.right -= g_rcSep0.left;

    GetDate();
    wsprintf(sz, "%d", g_year % 100);   /* "26" (no leading zero) */
    SendMessage(g_hYear, CB_ADDSTRING, 0, (LPARAM)sz);
    wsprintf(sz, "%d", g_year);
    SendMessage(g_hYear, CB_ADDSTRING, 0, (LPARAM)sz);
    for (int i = 0; i < 3; i++) {
        if (!LoadString(hInstMain, 170 + i, sz, 0x29)) return;   /* " ", "Sun", "Sunday" */
        SendMessage(g_hLD[0], CB_ADDSTRING, 0, (LPARAM)sz);
    }
    lstrcpy(sz, "05");
    SendMessage(g_hDay, CB_ADDSTRING, 0, (LPARAM)(sz + 1));
    SendMessage(g_hDay, CB_ADDSTRING, 0, (LPARAM)sz);
    sz[1] = '3';
    SendMessage(g_hMonth, CB_ADDSTRING, 0, (LPARAM)(sz + 1));
    SendMessage(g_hMonth, CB_ADDSTRING, 0, (LPARAM)sz);
    for (int i = 0; i < 2; i++) {
        if (!LoadString(hInstMain, 173 + i, sz, 0x29)) return;   /* "Mar", "March" */
        SendMessage(g_hMonth, CB_ADDSTRING, 0, (LPARAM)sz);
    }

    g_intl.iLDate = ParseLongDateIntoControls(g_intl.sLongDate);
    if (g_intl.iDate < 0 || g_intl.iDate > 2) {
        /* (3.1 tests the short date order here) */
        SendMessage(g_hLD[0], CB_SETCURSEL, 2, 0);
        SendMessage(g_hDay, CB_SETCURSEL, 0, 0);
        SendMessage(g_hMonth, CB_SETCURSEL, 3, 0);
        SendMessage(g_hYear, CB_SETCURSEL, 1, 0);
        SetDlgItemText(hDlg, IDC_LDSEP1, ",");
        SetDlgItemText(hDlg, IDC_LDSEP1 + 2, ",");
        order = 0;
    } else
        order = g_intl.iDate;
    CheckRadioButton(hDlg, IDC_SHORTMDY, IDC_SHORTMDY + 2, IDC_SHORTMDY + order);
    SetDlgItemText(hDlg, IDC_DATESEP, g_intl.sDate);
    SendDlgItemMessage(hDlg, IDC_DATESEP, EM_LIMITTEXT, 1, 0);
    CheckDlgButton(hDlg, IDC_DAYLZERO, g_intl.iDayLzero);
    CheckDlgButton(hDlg, IDC_MONLZERO, g_intl.iMonLzero);
    CheckDlgButton(hDlg, IDC_CENTURY, g_intl.iCentury);   /* 2 for "yyyy": a check box shows it checked */
    order = g_intl.iLDate >= 0 && g_intl.iLDate <= 2 ? g_intl.iLDate : 0;
    CheckRadioButton(hDlg, IDC_LONGMDY, IDC_LONGMDY + 2, IDC_LONGMDY + order);
    ArrangeLongDate(hDlg, order);
}

/* seg11:0824 */
static BOOL DateDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BOOL fUpdate = FALSE;
    char szSep[4];
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        DateInitDlg(hDlg);
        fUpdate = TRUE;
        HourGlass(FALSE);
        break;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            if (GetDlgItemText(hDlg, IDC_DATESEP, szSep, 2) == 0) {
                MyMessageBox(hDlg, 802, 1, MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(hDlg, IDC_DATESEP));
                break;
            }
            lstrcpy(g_intl.sDate, szSep);
            g_intl.iDate = GetCheckedRadio(hDlg, IDC_SHORTMDY, IDC_SHORTMDY + 2) - IDC_SHORTMDY;
            g_intl.iLDate = GetCheckedRadio(hDlg, IDC_LONGMDY, IDC_LONGMDY + 2) - IDC_LONGMDY;
            g_intl.iDayLzero = IsDlgButtonChecked(hDlg, IDC_DAYLZERO);
            g_intl.iMonLzero = IsDlgButtonChecked(hDlg, IDC_MONLZERO);
            g_intl.iCentury = IsDlgButtonChecked(hDlg, IDC_CENTURY);
            BuildLongDateFormat(g_intl.sLongDate);
            BuildShortDateFormat(g_intl.sShortDate);
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        case IDD_HELP:
            CPHelp(hDlg);
            break;
        case IDC_SHORTMDY:
        case IDC_SHORTMDY + 1:
        case IDC_SHORTMDY + 2:
            CheckRadioButton(hDlg, IDC_SHORTMDY, IDC_SHORTMDY + 2, (int)wParam);
            break;
        case IDC_DAYLZERO:
        case IDC_MONLZERO:
        case IDC_CENTURY:
            CheckDlgButton(hDlg, (int)wParam, IsDlgButtonChecked(hDlg, (int)wParam) == 0 ? 1 : 0);
            break;
        case IDC_LONGMDY:
        case IDC_LONGMDY + 1:
        case IDC_LONGMDY + 2:
            if (!IsDlgButtonChecked(hDlg, (int)wParam)) {
                CheckRadioButton(hDlg, IDC_LONGMDY, IDC_LONGMDY + 2, (int)wParam);
                ArrangeLongDate(hDlg, (int)wParam - IDC_LONGMDY);
                fUpdate = TRUE;
            }
            break;
        case IDC_LDDOW:
        case IDC_LDDAY:
        case IDC_LDMONTH:
        case IDC_LDYEAR:
            if (HIWORD(lParam) == CBN_SELCHANGE) fUpdate = TRUE;
            break;
        case IDC_LDSEP1:
        case IDC_LDSEP1 + 1:
        case IDC_LDSEP1 + 2:
            if (HIWORD(lParam) == EN_CHANGE) fUpdate = TRUE;
            break;
        }
        break;
    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            break;
        }
        return FALSE;
    }
    if (fUpdate) UpdateLongDateSample(hDlg);
    return TRUE;
}

/* ------------------------------------------------------------------ Time Format (dialog 16, seg14) */
static const char sz12Range0[] = "00:00-11:59", sz12Range1[] = "12:00-23:59", sz24Range[] = "00:00-23:59";

/* seg14:0000 */
static BOOL TimeDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        SendDlgItemMessage(hDlg, IDC_AMEDIT, EM_LIMITTEXT, 8, 0);
        SendDlgItemMessage(hDlg, IDC_PMEDIT, EM_LIMITTEXT, 8, 0);
        if (g_intl.iTime == 0) {
            CheckRadioButton(hDlg, IDC_12HOUR, IDC_24HOUR, IDC_12HOUR);
            SetDlgItemText(hDlg, IDC_AMEDIT, g_intl.s1159);
            SetDlgItemText(hDlg, IDC_AMRANGE, sz12Range0);
            SetDlgItemText(hDlg, IDC_PMRANGE, sz12Range1);
            g_sz24Suffix[0] = 0;
        } else {
            CheckRadioButton(hDlg, IDC_12HOUR, IDC_24HOUR, IDC_24HOUR);
            SetDlgItemText(hDlg, IDC_AMRANGE, "");
            SetDlgItemText(hDlg, IDC_PMRANGE, sz24Range);
            SetDlgItemText(hDlg, IDC_AMEDIT, "");
            EnableWindow(GetDlgItem(hDlg, IDC_AMEDIT), FALSE);
            ShowWindow(GetDlgItem(hDlg, IDC_AMEDIT), SW_HIDE);
            lstrcpy(g_sz24Suffix, g_intl.s2359);
        }
        SetDlgItemText(hDlg, IDC_PMEDIT, g_intl.s2359);
        SetDlgItemText(hDlg, IDC_TIMESEP, g_intl.sTime);   /* (no length limit on the separator) */
        CheckRadioButton(hDlg, IDC_TLZERO0, IDC_TLZERO1, g_intl.iTLZero != 0 ? IDC_TLZERO1 : IDC_TLZERO0);
        HourGlass(FALSE);
        return TRUE;

    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            GetDlgItemText(hDlg, IDC_TIMESEP, g_intl.sTime, 4);
            g_intl.iTime = IsDlgButtonChecked(hDlg, IDC_24HOUR) ? 1 : 0;
            if (g_intl.iTime) {
                GetDlgItemText(hDlg, IDC_PMEDIT, g_intl.s2359, 9);
                lstrcpy(g_intl.s1159, g_intl.s2359);   /* 24 hours: both suffixes alike */
            } else {
                GetDlgItemText(hDlg, IDC_AMEDIT, g_intl.s1159, 9);
                GetDlgItemText(hDlg, IDC_PMEDIT, g_intl.s2359, 9);
            }
            g_intl.iTLZero = IsDlgButtonChecked(hDlg, IDC_TLZERO1) ? 1 : 0;
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_12HOUR:
            if (IsDlgButtonChecked(hDlg, IDC_12HOUR)) return TRUE;
            GetDlgItemText(hDlg, IDC_PMEDIT, g_sz24Suffix, 9);   /* the 24-hour suffix, for later */
            CheckRadioButton(hDlg, IDC_12HOUR, IDC_24HOUR, IDC_12HOUR);
            EnableWindow(GetDlgItem(hDlg, IDC_AMEDIT), TRUE);
            ShowWindow(GetDlgItem(hDlg, IDC_AMEDIT), SW_SHOW);
            SetDlgItemText(hDlg, IDC_AMRANGE, sz12Range0);
            SetDlgItemText(hDlg, IDC_PMRANGE, sz12Range1);
            SetDlgItemText(hDlg, IDC_AMEDIT, g_intl.s1159);
            SetDlgItemText(hDlg, IDC_PMEDIT, g_intl.s2359);
            return TRUE;
        case IDC_24HOUR:
            if (IsDlgButtonChecked(hDlg, IDC_24HOUR)) return TRUE;
            /* (this keeps the 12-hour suffixes in g_intl even if the dialog is cancelled) */
            GetDlgItemText(hDlg, IDC_AMEDIT, g_intl.s1159, 9);
            GetDlgItemText(hDlg, IDC_PMEDIT, g_intl.s2359, 9);
            CheckRadioButton(hDlg, IDC_12HOUR, IDC_24HOUR, IDC_24HOUR);
            SetDlgItemText(hDlg, IDC_AMRANGE, "");
            SetDlgItemText(hDlg, IDC_PMRANGE, sz24Range);
            SetDlgItemText(hDlg, IDC_AMEDIT, "");
            SetDlgItemText(hDlg, IDC_PMEDIT, g_sz24Suffix);
            EnableWindow(GetDlgItem(hDlg, IDC_AMEDIT), FALSE);
            ShowWindow(GetDlgItem(hDlg, IDC_AMEDIT), SW_HIDE);
            return TRUE;
        case IDC_TLZERO0:
        case IDC_TLZERO1:
            CheckRadioButton(hDlg, IDC_TLZERO0, IDC_TLZERO1, (int)wParam);
            return TRUE;
        default:
            return TRUE;
        }
    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}

/* ------------------------------------------------------------------ Number Format (dialog 17, seg13) */
/* seg13:0000 */
static BOOL NumberDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szDec[4], szMsg[0x84];
    BOOL fOk;
    int n;
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        CheckRadioButton(hDlg, IDC_LZERO0, IDC_LZERO1, IDC_LZERO0 + g_intl.iLzero);
        SetDlgItemInt(hDlg, IDC_DIGITS, g_intl.iDigits, FALSE);
        SetDlgItemText(hDlg, IDC_THOUSAND, g_intl.sThousand);
        SetDlgItemText(hDlg, IDC_DECIMAL, g_intl.sDecimal);
        SendDlgItemMessage(hDlg, IDC_DIGITS, EM_LIMITTEXT, 1, 0);
        SendDlgItemMessage(hDlg, IDC_THOUSAND, EM_LIMITTEXT, 1, 0);
        SendDlgItemMessage(hDlg, IDC_DECIMAL, EM_LIMITTEXT, 1, 0);
        HourGlass(FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            if (GetDlgItemText(hDlg, IDC_DECIMAL, szDec, 2) == 0) {
                LoadString(hInstMain, 800, szMsg, sizeof szMsg);
                MessageBox(hDlg, szMsg, szCaption, MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(hDlg, IDC_DECIMAL));
                return TRUE;
            }
            n = (int)GetDlgItemInt(hDlg, IDC_DIGITS, &fOk, FALSE);
            if (!fOk) {   /* only "not a number": 7..9 pass */
                LoadString(hInstMain, 801, szMsg, sizeof szMsg);
                MessageBox(hDlg, szMsg, szCaption, MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(hDlg, IDC_DIGITS));
                SendDlgItemMessage(hDlg, IDC_DIGITS, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                return TRUE;
            }
            GetDlgItemText(hDlg, IDC_THOUSAND, g_intl.sThousand, 2);
            g_intl.iLzero = IsDlgButtonChecked(hDlg, IDC_LZERO1) ? 1 : 0;
            g_intl.iDigits = n;
            lstrcpy(g_intl.sDecimal, szDec);
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_LZERO0:
        case IDC_LZERO1:
            CheckRadioButton(hDlg, IDC_LZERO0, IDC_LZERO1, (int)wParam);
            return TRUE;
        default:
            return TRUE;
        }
    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}

/* ------------------------------------------------------------------ Currency Format (dialog 18, seg10) */
/* seg10:0000 */
static void FillPlacementCombo(HWND hCombo, const char *sym)
{
    char sz[0x40];
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 4; i++) {
        wsprintf(sz, TableEntry(PLACEFMT + i), sym);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)sz);
    }
}

/* seg10:0056: "($123.22)" .. "123.22 $-" */
static void FillNegativeCombo(HWND hCombo, const char *sym, const char *dec, int digits)
{
    char num[0x40], out[0x80], *q;
    char ch = (char)(digits + '0');   /* taken before the clamp */
    if (digits > 8) digits = 8;
    lstrcpy(num, "123");
    lstrcat(num, dec);
    q = num + lstrlen(num);
    for (int k = 0; k < digits; k++) *q++ = ch;
    *q = 0;
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 11; i++) {
        if (i < 4 || i == 9)
            wsprintf(out, TableEntry(NEGFMT + i), sym, num);
        else
            wsprintf(out, TableEntry(NEGFMT + i), num, sym);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)out);
    }
}

/* seg10:0139 */
static BOOL CurrencyDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char sym[0x14];
    int sel, digits;
    BOOL fOk;
    HWND hc;
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        SetDlgItemText(hDlg, IDC_SYMBOL, g_intl.sCurrency);
        SendDlgItemMessage(hDlg, IDC_SYMBOL, EM_LIMITTEXT, 5, 0);   /* (none on the digits) */
        SetDlgItemInt(hDlg, IDC_CURRDIGITS, g_intl.iCurrDigits, FALSE);
        hc = GetDlgItem(hDlg, IDC_PLACEMENT);
        FillPlacementCombo(hc, g_intl.sCurrency);
        SendMessage(hc, CB_SETCURSEL, g_intl.iCurrency, 0);
        hc = GetDlgItem(hDlg, IDC_NEGATIVE);
        FillNegativeCombo(hc, g_intl.sCurrency, g_intl.sDecimal, g_intl.iCurrDigits);
        SendMessage(hc, CB_SETCURSEL, g_intl.iNegCurr, 0);
        g_fCurrDirty = FALSE;
        HourGlass(FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            digits = (int)GetDlgItemInt(hDlg, IDC_CURRDIGITS, &fOk, FALSE);
            if (!fOk) {   /* no range check beyond "a number" */
                MyMessageBox(hDlg, 804, 1, MB_OK | MB_ICONINFORMATION);
                SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_CURRDIGITS), MAKELPARAM(1, 0));
                SendDlgItemMessage(hDlg, IDC_CURRDIGITS, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                return TRUE;
            }
            if (GetDlgItemText(hDlg, IDC_SYMBOL, sym, 6) == 0) {
                MyMessageBox(hDlg, 803, 1, MB_OK | MB_ICONINFORMATION);
                SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_SYMBOL), MAKELPARAM(1, 0));
                return TRUE;
            }
            lstrcpy(g_intl.sCurrency, sym);
            g_intl.iCurrDigits = digits;
            g_intl.iCurrency = (int)SendMessage(GetDlgItem(hDlg, IDC_PLACEMENT), CB_GETCURSEL, 0, 0);
            g_intl.iNegCurr = (int)SendMessage(GetDlgItem(hDlg, IDC_NEGATIVE), CB_GETCURSEL, 0, 0);
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_SYMBOL:
        case IDC_CURRDIGITS:
            if (HIWORD(lParam) == EN_CHANGE) {
                g_fCurrDirty = TRUE;
                return TRUE;
            }
            if (g_fCurrDirty && HIWORD(lParam) == EN_KILLFOCUS) {
                /* the lists follow the symbol and the digits as an edit is left */
                g_fCurrDirty = FALSE;
                if (GetDlgItemText(hDlg, IDC_SYMBOL, sym, 6) == 0) return TRUE;
                digits = (int)GetDlgItemInt(hDlg, IDC_CURRDIGITS, &fOk, FALSE);
                hc = GetDlgItem(hDlg, IDC_NEGATIVE);
                sel = (int)SendMessage(hc, CB_GETCURSEL, 0, 0);
                FillNegativeCombo(hc, sym, g_intl.sDecimal, digits);
                SendMessage(hc, CB_SETCURSEL, sel, 0);
                hc = GetDlgItem(hDlg, IDC_PLACEMENT);
                sel = (int)SendMessage(hc, CB_GETCURSEL, 0, 0);
                FillPlacementCombo(hc, sym);
                SendMessage(hc, CB_SETCURSEL, sel, 0);
            }
            return TRUE;
        default:
            return TRUE;
        }
    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}

/* ------------------------------------------------------------------ the International dialog */
/* the part of the CPL_INIT code (seg3:013A) International uses: SYSTEM.INI in the Windows
 * directory, the shared-file directory VerFindFile names (the system directory) and where
 * OpenFile finds SETUP.INF */
static void IntlInitPaths(void)
{
    static BOOL fDone;
    char win[0x9E];
    OFSTRUCT of;
    if (fDone) return;
    fDone = TRUE;
    GetWindowsDirectory(win, sizeof win);
    AddBackslash(win);
    wsprintf(g_szSystemIni, "%s%s", win, "SYSTEM.INI");
    GetSystemDirectory(g_szSharedDir, sizeof g_szSharedDir);
    if (OpenFile("SETUP.INF", &of, OF_EXIST) != HFILE_ERROR)
        CopyTrunc(g_szSetupInf, of.szPathName, sizeof g_szSetupInf - 16);   /* room for CONTROL.INF */
    else
        lstrcpy(g_szSetupInf, "SETUP.INF");
}

/* seg12:0A50 */
static BOOL IntlInitDialog(HWND hDlg)
{
    char buf[0x100], *p, *h;
    const char *pszRemove;
    HWND hCombo, hList;
    int start, len, nCountries;

    IntlInitPaths();
    ClockFromEnv();
    p = AddBackslash(g_szSharedDir);
    if ((int)(p - g_szSharedDir) > 3) p[-1] = 0;
    w16_chdir(g_szSharedDir);   /* ChDirDrive seg9:0034 (LZOpenFile looks in the current directory first) */
    LoadString(hInstMain, 161, g_szSrcPathAlt, sizeof g_szSrcPathAlt);   /* "A:\" */
    LoadString(hInstMain, 161, g_szSrcPath, sizeof g_szSrcPath);
    IntlReadProfile();

    /* countries: CONTROL.INF [country] */
    ReplaceFileName(g_szSetupInf, "CONTROL.INF");
    hCombo = GetDlgItem(hDlg, IDC_COUNTRY);
    hList = GetDlgItem(hDlg, IDC_COUNTRYLIST);
    nCountries = InfFillLists(hCombo, hList, CB_INSERTSTRING, "country", GetQuotedString);
    ReplaceFileName(g_szSetupInf, "SETUP.INF");
    if (nCountries == 0) goto fail;
    /* the name in WIN.INI, exactly (CB_FINDSTRING finds prefixes); else the last, "Other Country" */
    start = -1;
    len = lstrlen(g_intl.sCountry);
    for (;;) {
        g_iCountrySel = (int)SendMessage(hCombo, CB_FINDSTRING, start, (LPARAM)g_intl.sCountry);
        if (!(g_iCountrySel > start)) {
            g_iCountrySel = nCountries - 1;
            break;
        }
        if ((int)SendMessage(hCombo, CB_GETLBTEXTLEN, g_iCountrySel, 0) == len) break;
        start = g_iCountrySel;
    }
    SendMessage(hCombo, CB_SETCURSEL, g_iCountrySel, 0);

    /* languages: SETUP.INF [language], selected by SYSTEM.INI [boot.description] language.dll */
    hCombo = GetDlgItem(hDlg, IDC_LANGUAGE);
    hList = GetDlgItem(hDlg, IDC_LANGLIST);
    if (InfFillLists(hCombo, hList, CB_INSERTSTRING, "language", GetQuotedString) == 0) goto fail;
    len = GetPrivateProfileString("boot.description", "language.dll", "", buf, 0x80, g_szSystemIni);
    if (len != 0) {
        start = -1;
        for (;;) {
            g_iLangSel = (int)SendMessage(hCombo, CB_FINDSTRING, start, (LPARAM)buf);
            if (g_iLangSel <= start) goto lang_default;
            if ((int)SendMessage(hCombo, CB_GETLBTEXTLEN, g_iLangSel, 0) == len) goto lang_set;
            /* 3.1 searches on from the COUNTRY's index here, so a second inexact match below it
             * would search forever; arch311 takes the default then */
            if (start == g_iCountrySel) goto lang_default;
            start = g_iCountrySel;
        }
    }
lang_default:
    LoadString(hInstMain, 141, buf, 0x80);   /* "usa" (no [language] line starts so: the first) */
    g_iLangSel = (int)SendMessage(hList, LB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
    if (g_iLangSel < 0) g_iLangSel = 0;
lang_set:
    SendMessage(hCombo, CB_SETCURSEL, g_iLangSel, 0);

    /* keyboard layouts: SETUP.INF [keyboard.tables] less the US entry the keyboard type rules
     * out, selected by SYSTEM.INI [keyboard] keyboard.dll */
    hCombo = GetDlgItem(hDlg, IDC_KEYBOARD);
    hList = GetDlgItem(hDlg, IDC_KBDLIST);
    if (InfFillLists(hCombo, hList, CB_INSERTSTRING, "keyboard.tables", GetQuotedString) == 0) goto fail;
    pszRemove = GetPrivateProfileInt("keyboard", "type", 1, g_szSystemIni) == 2 ? "nodll" : "usadll";
    g_iKbdSel = (int)SendMessage(hList, LB_FINDSTRING, (WPARAM)-1, (LPARAM)pszRemove);
    if (g_iKbdSel >= 0) {
        SendMessage(hCombo, CB_DELETESTRING, g_iKbdSel, 0);
        SendMessage(hList, LB_DELETESTRING, g_iKbdSel, 0);
    }
    if (GetPrivateProfileString("keyboard", "keyboard.dll", "", buf, 0x80, g_szSystemIni) != 0) {
        h = InfFindLineByFile("keyboard.tables", buf);
        if (h) {
            CopyTrunc(buf, h, 0x80);
            free(h);
            g_iKbdSel = (int)SendMessage(hList, LB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
            if (g_iKbdSel >= 0) goto kbd_set;
        }
    }
    LoadString(hInstMain, 142, buf, 0x80);   /* "nodll" */
    g_iKbdSel = (int)SendMessage(hList, LB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
    if (g_iKbdSel < 0) g_iKbdSel = 0;
kbd_set:
    SendMessage(hCombo, CB_SETCURSEL, g_iKbdSel, 0);

    for (int i = 0; i < 2; i++) {
        LoadString(hInstMain, 144 + i, buf, 0x80);   /* "Metric", "English" */
        SendDlgItemMessage(hDlg, IDC_MEASURE, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    FillIntlDialog(hDlg, &g_intl);
    SendDlgItemMessage(hDlg, IDC_LISTSEP, EM_LIMITTEXT, 1, 0);
    return TRUE;

fail:
    /* "Control Panel cannot display the International dialog box. The SETUP.INF and CONTROL.INF
     * files may be damaged ..." */
    MyMessageBox(hDlg, 808, 1, MB_OK | MB_ICONINFORMATION);
    return FALSE;
}

/* seg12:1668 */
static void IntlCommand(HWND hDlg, WORD id, WORD code)
{
    switch (id) {
    case IDOK:
        HourGlass(TRUE);
        if (!IntlSave(hDlg)) {
            HourGlass(FALSE);
            return;
        }
        HourGlass(FALSE);
        /* fall through */
    case IDCANCEL:
        /* (3.1 also clears the Color applet's cached item height [0x1936] here) */
        EndDialog(hDlg, 0);
        return;
    case IDC_COUNTRY:
        if (code == CBN_SELCHANGE) OnCountrySelChange(hDlg);
        return;
    }
    /* the group box, Change... and a hidden button with the group's mnemonic, any notification */
    if (id >= 206 && id <= 208) {
        DoDialogBoxParam(15, hDlg, DateDlgProc, 0x1F4F, 0);
        UpdateDateSamples(hDlg, &g_intl);
    } else if (id >= 211 && id <= 213) {
        DoDialogBoxParam(16, hDlg, TimeDlgProc, 0x1F50, 0);
        UpdateTimeSample(hDlg, &g_intl);
    } else if (id >= 215 && id <= 217) {
        DoDialogBoxParam(17, hDlg, NumberDlgProc, 0x1F51, 0);
        UpdateNumberSample(hDlg, &g_intl);
    } else if (id >= 219 && id <= 221) {
        DoDialogBoxParam(18, hDlg, CurrencyDlgProc, 0x1F52, 0);
        UpdateCurrencySamples(hDlg, &g_intl);
    }
}

/* seg12:194D */
BOOL IntlDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HourGlass(TRUE);
        BOOL fOk = IntlInitDialog(hDlg);
        HourGlass(FALSE);
        if (!fOk) EndDialog(hDlg, -1);
        return TRUE;
    }
    case WM_COMMAND:
        if (wParam == IDD_HELP)
            CPHelp(hDlg);
        else
            IntlCommand(hDlg, (WORD)wParam, HIWORD(lParam));
        return TRUE;
    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}
