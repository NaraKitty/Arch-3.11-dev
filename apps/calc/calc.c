/*
 * Calculator - native 64-bit port of the Windows 3.11 Calculator (CALC.EXE 3.10, 43,072 bytes,
 * "Developed for Microsoft by Kraig Brockschmidt").
 *
 * Ported function by function from the disassembly of the user's own CALC.EXE; each function
 * notes the original segment:offset. The window is a dialog of class "SciCalc" (template SC, the
 * class menu SM) whose radio buttons, check boxes and statics are controls while the keys, the
 * display frame and the indicator boxes are drawn by the program. Menus, dialogs (SC, SB), the
 * 78 strings, the accelerators and the icon are loaded at run time from the user's ripped
 * CALC.EXE; nothing Microsoft-made is compiled in.
 *
 * Arithmetic is done as CALC does it: 8087 operations on doubles through the Microsoft C 6
 * floating-point library, ported in mscrt.c. Every intermediate below is `ext` (an x87 register)
 * where the original keeps a value on the FPU stack and a double where it stores one, so results
 * and their formatting match 3.1 digit for digit - including its quirks (2.01-2 shows 0.00, only
 * one trailing zero of a typed fraction is shown, the multiplication overflow check uses ln).
 *
 * Deliberate differences: none in behaviour. Where 3.1 reads or writes memory it does not own
 * (DisplayNum after F-E formatting, deep parenthesis/precedence nesting) the port stays inside
 * its own buffers; see the comments there.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h"
#include "mscrt.h"

const char *w16_app_module = "CALC.EXE";

/* ------------------------------------------------------------------ commands (key ids) */
enum {
    K_RSH = 7, /* Inv + Lsh */
    K_SIGN = 0x50, K_CLEAR, K_CE, K_BACK, K_STA, K_POINT,
    K_AND = 0x56, K_OR, K_XOR, K_LSH, K_DIV, K_MUL, K_ADD, K_SUB, K_MOD, K_PWR,
    K_INT = 0x60, K_NOT, K_SIN, K_COS, K_TAN, K_LN, K_LOG, K_SQR, K_CUBE, K_FACT, K_RECIP, K_DMS, K_PERCENT,
    K_FE = 0x6D, K_PI, K_EQU, K_MC, K_MR, K_MS, K_MPLUS, K_EXP, K_AVE, K_SUM, K_DEV, K_DAT,
    K_BIN = 0x79, K_OCT, K_DEC, K_HEX, K_INV, K_HYP, K_DEG, K_RAD, K_GRAD,
    M_SEARCH = 33, M_COPY = 300, M_PASTE = 301, M_ABOUT = 303, M_SCI = 305, M_STD = 306,
    M_HELPONHELP = 0xFFFC, M_INDEX = 0xFFFF,
};
/* controls of dialogs SC and SB */
enum {
    IDC_STAT = 400, IDC_MEM = 401, IDC_PAREN = 403, IDC_CD = 404, IDC_CAD = 405, IDC_NOMEM = 406,
    IDC_LIST = 407, IDC_COUNT = 408, IDC_LOAD = 410, IDC_RET = 411, IDC_DISP = 413, IDC_EDIT = 495,
};
/* strings 0-60 are the key labels (in key table order) */
enum {
    S_POINTKEY = 38, S_DWORD = 61, S_DEG = 64, S_ERRORS = 67, S_HELPFILE = 72, S_NOCLIP = 73, S_NOMEM = 74,
    S_STAT = 75, S_CALC = 76, S_CREDITS = 77, NSTR = 78,
};

/* ------------------------------------------------------------------ tables (DGROUP) */
/* ds:0108: the 61 keys in drawing order {flags, id}. flags & 3 is the view the key is NOT shown in
 * (0 scientific, 1 standard, 2 both views), bits 2-4 the text colour, bits 5-7 the outline colour */
static const unsigned char keytab[61][2] = {
    {0x5D, 0x54}, {0x5D, 0x75}, {0x5D, 0x76}, {0x5D, 0x77}, {0x5D, 0x78}, {0x3D, 0x6D}, {0x3D, 0x6B},
    {0x3D, 0x62}, {0x3D, 0x63}, {0x3D, 0x64}, {0xFD, 0x28}, {0x3D, 0x74}, {0x3D, 0x5F}, {0x3D, 0x68},
    {0x3D, 0x67}, {0xFD, 0x29}, {0x3D, 0x65}, {0x3D, 0x66}, {0x3D, 0x69}, {0x3D, 0x6A}, {0x92, 0x70},
    {0x92, 0x71}, {0x92, 0x72}, {0x92, 0x73}, {0x49, 0x6E}, {0x4A, '7'}, {0x4A, '4'}, {0x4A, '1'},
    {0x4A, '0'}, {0x49, 'A'}, {0x4A, '8'}, {0x4A, '5'}, {0x4A, '2'}, {0x4A, 0x50}, {0x49, 'B'},
    {0x4A, '9'}, {0x4A, '6'}, {0x4A, '3'}, {0x4A, 0x55}, {0x49, 'C'}, {0x1E, 0x5A}, {0x1E, 0x5B},
    {0x1E, 0x5D}, {0x1E, 0x5C}, {0x49, 'D'}, {0x1D, 0x5E}, {0x1D, 0x57}, {0x1D, 0x59}, {0x3C, 0x67},
    {0x3C, 0x6C}, {0x3C, 0x6A}, {0x1E, 0x6F}, {0x49, 'E'}, {0x1D, 0x56}, {0x1D, 0x58}, {0x1D, 0x61},
    {0x1D, 0x60}, {0x49, 'F'}, {0x02, 0x51}, {0x02, 0x52}, {0x02, 0x53},
};
/* ds:00E8: red, dark cyan, blue, blue, magenta, red, white, black */
static const COLORREF colours[8] = {RGB(255, 0, 0), RGB(0, 128, 128), RGB(0, 0, 255), RGB(0, 0, 255),
                                    RGB(255, 0, 255), RGB(255, 0, 0), RGB(255, 255, 255), RGB(0, 0, 0)};
/* ds:01CA: frames in dialog units {left, top, right, bottom}: 0-6 scientific, 7-8 standard */
static const RECT frames[9] = {
    {130, 22, 235, 36}, {4, 22, 124, 36}, {87, 38, 164, 52}, {168, 38, 187, 52}, {194, 38, 209, 52},
    {216, 38, 235, 52}, {94, 4, 235, 17}, {96, 21, 116, 34}, {22, 4, 122, 17},
};
/* ds:01A0: controls shown in one view only; bit 15 marks the standard view's */
static const unsigned short ctltab[15] = {0x79, 0x7A, 0x7B, 0x7C, 0x7F, 0x80, 0x81, 0x7D, 0x7E,
                                          0x19D, 0x191, 0x193, 0x190, 0x8192, 0x819E};
/* ds:003A-0049 and 0248: per view (scientific, standard) */
static const int topY[2] = {38, 20}, keyY0[2] = {54, 36}, nRows[2] = {5, 4}, nKeys[2] = {58, 27},
                 nCols[2] = {11, 6};
/* ds:0230: operator precedence {op, level} */
static const unsigned char prectab[13][2] = {{0, 0}, {K_OR, 0}, {K_XOR, 0}, {K_AND, 1}, {K_ADD, 2}, {K_SUB, 2},
                                             {K_RSH, 3}, {K_LSH, 3}, {K_MOD, 3}, {K_DIV, 3}, {K_MUL, 3}, {K_PWR, 4},
                                             {0x0B, 0}};
/* ds:0224: Dword, Word, Byte; ds:024C: Bin, Oct, Dec, Hex */
static const uint32_t masks[3] = {0xFFFFFFFFu, 0xFFFFu, 0xFFu};
static const int radixtab[4] = {2, 8, 10, 16};
/* ds:0260: Edit > Paste characters and the keys they stand for; 0x80 marks ":x" sequences */
static const unsigned char pastetab[67][2] = {
    {'A', 'A'}, {'B', 'B'}, {'C', 'C'}, {'D', 'D'}, {'E', 'E'}, {'F', 'F'}, {'0', '0'}, {'1', '1'}, {'2', '2'},
    {'3', '3'}, {'4', '4'}, {'5', '5'}, {'6', '6'}, {'7', '7'}, {'8', '8'}, {'9', '9'}, {'!', K_FACT},
    {'S', K_SIN}, {'O', K_COS}, {'T', K_TAN}, {'R', K_RECIP}, {'Y', K_PWR}, {'#', K_CUBE}, {'@', K_SQR},
    {'M', K_DMS}, {'N', K_LN}, {'L', K_LOG}, {'V', K_FE}, {'X', K_EXP}, {'I', K_INV}, {'H', K_HYP}, {'P', K_PI},
    {'/', K_DIV}, {'*', K_MUL}, {'%', K_MOD}, {'-', K_SUB}, {'=', K_EQU}, {'+', K_ADD}, {'&', K_AND},
    {'|', K_OR}, {'^', K_XOR}, {'~', K_NOT}, {';', K_INT}, {'<', K_LSH}, {'(', '('}, {')', ')'}, {'.', K_POINT},
    {',', K_POINT}, {'\\', K_DAT}, {'Q', K_CLEAR}, {0x80 | 'S', K_STA}, {0x80 | 'M', K_MS}, {0x80 | 'P', K_MPLUS},
    {0x80 | 'C', K_MC}, {0x80 | 'R', K_MR}, {0x80 | 'A', K_AVE}, {0x80 | 'T', K_SUM}, {0x80 | 'D', K_DEV},
    {0x80 | '2', K_DEG}, {0x80 | '3', K_RAD}, {0x80 | '4', K_GRAD}, {0x80 | '5', K_HEX}, {0x80 | '6', K_DEC},
    {0x80 | '7', K_OCT}, {0x80 | '8', K_BIN}, {0x80 | '9', K_SIGN},
};
/* ds:0074: degrees, radians, grads per radian (57.2957795130823 and 63.6619772367581 as written
 * in 3.1, a few units in the last place off 180/pi and 200/pi) */
static const double trigFactor[3] = {0x1.ca5dc1a63c1f5p+5, 1.0, 0x1.fd4bbab8b4947p+5};

/* ------------------------------------------------------------------ state (DGROUP) */
static HINSTANCE hInst;        /* [0DF6] */
static HWND hwndCalc;          /* [0FCE] */
static HWND hwndEdit;          /* [0FDC] hidden edit control for Edit > Copy */
static HWND hStatBox;          /* [0FCC] */
static HWND hStatList;         /* [0F90] */
static HACCEL hAccel;          /* [0FDA] */
static HBRUSH hbrBack;         /* [0054] */
static COLORREF crBack = (COLORREF)-1; /* [00E0] */
static char *szStr[NSTR];      /* [0EE8] */
static char szDec[5] = ".";    /* [00D4] intl sDecimal */
static char szDisplay[64];     /* [008C] the number as displayed (sign or blank first) */
static int nLayout;            /* [0028] 0 scientific, 1 standard */
static int layoutSetting;      /* [0F84] */
static int nTrig;              /* [002A] Deg/Rad/Grad, or Dword/Word/Byte outside decimal */
static int nRadix = 10;        /* [002C] */
static int nDec;               /* [0030] digits after the point + 1 */
static int lastKey;            /* [0032] */
static int lastKeyPrev;        /* [0F92] */
static int nParens;            /* [0034] */
static int nPendingOp;         /* [0036] */
static int nPrecStack;         /* [0038] */
static int nColors = 16;       /* [004C] */
static int nSavedTrig;         /* [004E] */
static int nWordSize;          /* [0050] */
static int nInitShow;          /* [0052] nCmdShow while starting */
static int fHyp, fInv;         /* [0056], [0058] */
static int fError;             /* [005A] */
static int nErrCode;           /* [0FDE] */
static int fDecPt;             /* [005C] */
static int fFE;                /* [005E] scientific notation */
static int fColor = 1;         /* [0060] */
static int fRelayout;          /* [0062] (set by View, never read) */
static int fExpEntry;          /* [021C] typing an exponent */
static int fNewEntry = 1;      /* [021E] */
static int fOpPending;         /* [0220] */
static int nSign = 1;          /* [0222] */
static uint32_t wordMask = 0xFFFFFFFFu; /* [00E4] */
static int trailZeros;         /* [0FC6] zeros typed after the point, less one */
static double num;             /* [0064] */
static double mem;             /* [006C] */
static double lastOperand;     /* [0FD0] */
static double repeatOperand;   /* [0D70] */
/* precedence and parenthesis stacks (3.1: 25 entries, writes past them land in other data; the
 * port keeps slack instead) */
static double precVal[40];     /* [0DF8] */
static int precOp[40];         /* [10B0] */
static double parenVal[40];    /* [0FE0] */
static int parenOp[40];        /* [0F94] */
/* exponent entry */
static int fExpFirst = 1, expSign = 1, expVal; /* [031C], [031A], [0318] */
static double expT, expMant, expE;  /* [0D7C], [0D84], [0DBE] */
static char szExpMant[64];          /* [0D8C] */
/* statistics */
static double *statData;       /* [0FD8] */
static int32_t nStat;          /* [0308] */
static int32_t nStatBlock;     /* [10A8] */
static int32_t statCap;
/* geometry */
static int dlgW, dlgH;         /* [0F8E] = [10AC], [10AE]: the window as created */
static int keyW;               /* [0FC8] */
static int keyX0;              /* [0DF4] */
static int charH;              /* [0FCA] */
static int extraW;             /* [002E] */

static void DisplayNum(void);
static void DisplayError(int n);
static void ProcessCommand(int cmd);
static void StatBoxCreate(BOOL fCreate);
static void MenuCommand(int cmd);
static BOOL ExpEntry(int cmd);
static void SetRadix(int id);
static void EnableBaseButtons(BOOL f);
static void StatFunc(int cmd);
static void InitSettings(BOOL fForce);

/* the program's stores of x87 results (rt_st64: overflow raises SIGFPE and stores nothing) */
static void st_num(ext v) { rt_st64(v, &num); }

/* ------------------------------------------------------------------ seg3:0E91 DisplayError */
static void DisplayError(int n)
{
    SetDlgItemText(hwndCalc, IDC_DISP + nLayout, szStr[S_ERRORS + n]);
    fError = 1;
    nErrCode = n;
    if (nLayout == 0) EnableBaseButtons(FALSE);
}

/* seg3:0ED8 _matherr: DOMAIN, OVERFLOW and UNDERFLOW have their own message, the rest "undefined" */
static int CalcMatherr(int type)
{
    DisplayError(type == M_DOMAIN || type == M_OVERFLOW || type == M_UNDERFLOW ? type : 2);
    return 1;
}

/* seg3:0F12 the SIGFPE handler */
static void SignalHandler(int code)
{
    DisplayError(code == FPE_ZERODIVIDE_ ? 0 : code == FPE_OVERFLOW_ ? 3 : code == FPE_UNDERFLOW_ ? 4 : 2);
}

/* seg3:0F4B: the radix, angle/word-size and Inv/Hyp buttons follow the error state */
static void EnableBaseButtons(BOOL f)
{
    SetFocus(hwndCalc);
    for (int id = K_BIN; id <= K_GRAD; id++) EnableWindow(GetDlgItem(hwndCalc, id), f);
}

/* seg1:1B7F */
static void CheckButton(int id, int check) { CheckDlgButton(hwndCalc, id, check); }

/* ------------------------------------------------------------------ seg3:0000 DisplayNum */
static void DisplayNum(void)
{
    /* 3.1's buffers are 50 bytes; after F-E formatting `len` holds a counter (100..900) and the
     * trailing-zero code below indexes with it - into other stack data on 3.1, into the slack
     * of these buffers here */
    char buf[1100], buf2[1100], tmp[16];
    int fExpShort = 0, len, n, i;
    double x, t;
    if (nRadix == 10) {
        x = rt_fabs(num);
        t = x != 0.0 ? rt_pow(10.0, rt_log10(x)) : 1.0;
        /* "round to 13 digits" with t = x itself: only the last bits move */
        ext r = (ext)rt_floor((double)((ext)x / t * 1e13 + 0.5));
        rt_st64(r * 1e-13 * t, &x);
        rt_gcvt(x, 13, buf);
        len = lstrlen(buf);
        if ((ext)x < 1.0 && !(0.1 > (ext)x) && fFE == 1) {
            rt_gcvt((double)(10.0L * x), 13, buf);
            lstrcat(buf, "e-001");
        } else if (fFE == 0 && !lstrcmp(buf, "1.e-001")) lstrcpy(buf, "0.1");
        /* gcvt's e-format for 1e-13 < x < 0.1: rewritten as 0.000ddd */
        if (0.1 > (ext)x && !(1e-13 >= (ext)x) && fFE == 0 && len >= 5 && buf[len - 5] == 'e') {
            t = x;
            n = 0;
            if (1e-11 == (ext)t) {
                n = 11;
                t = 1.0;
            } else
                while (!((ext)t >= 1.0)) {
                    n++;
                    t = (double)(10.0L * t);
                }
            rt_gcvt((double)((ext)t * 0.1), 13, buf2);
            for (i = 0; i < 0x30; i++) buf2[i] = buf2[i + 2]; /* drop "0." (3.1 assumes it is there) */
            buf[0] = '0';
            buf[1] = '.';
            for (i = 1; i < n; i++) buf[i + 1] = '0';
            buf[i + 1] = 0;
            lstrcat(buf, buf2);
            len = lstrlen(buf);
        }
        /* F-E: 1 <= x < 1e13 as d.ddde+NNN */
        if ((ext)x >= 1.0 && !(1e13 <= (ext)x) && fFE != 0) {
            n = 0;
            while (!(10.0 > (ext)x)) {
                x = (double)(0.1L * x);
                n++;
            }
            lstrcpy(buf2, "e+");
            for (len = n; len < 100; len *= 10) {
                if (!len) break;
                lstrcat(buf2, "0");
            }
            lstrcat(buf2, rt_itoa(n, tmp, 10));
            rt_gcvt(x, 13, buf);
            lstrcat(buf, buf2);
        }
        for (i = 0; buf[i]; i++)
            if (buf[i] == '.') buf[i] = szDec[0];
        if (len > 5 && len < (int)sizeof buf && buf[len - 5] == 'e') fExpShort = 1;
        /* zeros typed after the point are not in the value: show them */
        if (lastKey == '0' && fDecPt) {
            trailZeros++;
            if (fExpShort) {
                lstrcpy(buf2, &buf[len - 5]);
                len -= 5;
            }
            if (len >= 0 && len + trailZeros < (int)sizeof buf - 1) {
                for (i = 0; i < trailZeros; i++) buf[len + i] = '0';
                buf[trailZeros + len] = 0;
            }
            trailZeros--;
            if (fExpShort) lstrcat(buf, buf2);
        } else trailZeros = 0;
        buf[0x21] = 0;
        lstrcpy(buf2, buf);
        lstrcpy(buf, 0.0 > (ext)num ? "-" : " ");
        lstrcat(buf, buf2);
    } else {
        double ip;
        rt_modf(num, &ip);
        num = ip;
        if ((ext)rt_fabs(ip) > 4294967295.0) {
            DisplayError((ext)ip >= 0.0 ? 3 : 4);
            return;
        }
        rt_ltoa((int32_t)((uint32_t)rt_ftol(ip) & wordMask), buf, nRadix);
        AnsiUpper(buf);
    }
    lstrcpy(szDisplay, buf);
    len = lstrlen(buf);
    if ((nLayout == 0 && len > 33) || (nLayout != 0 && len > 23)) {
        fError = 1;
        MessageBeep(0);
        return;
    }
    SetDlgItemText(hwndCalc, IDC_DISP + nLayout, buf);
}

/* ------------------------------------------------------------------ seg3:0604 DoSciFunc */
static void DoSciFunc(int cmd)
{
    double sign = 1.0, ip, ip2, fr, fr2, f60;
    if (!((ext)num >= 0.0)) sign = -1.0;
    switch (cmd) {
    case K_INT:
        fr = rt_modf(num, &ip);
        num = fInv ? fr : ip;
        break;
    case K_NOT:
        if ((ext)rt_fabs(num) > 4294967295.0) { DisplayError(1); return; }
        num = (double)(int32_t)~rt_ftol(num);
        break;
    case K_SIN:
        if (fInv) {
            if (fHyp) num = rt_log((double)((ext)rt_sqrt((double)(rt_cipow(num, 2.0) + 1.0L)) + num));
            else st_num((ext)trigFactor[nTrig] * rt_asin(num));
        } else if (fHyp) num = rt_sinh(num);
        else num = rt_sin((double)((ext)num / trigFactor[nTrig]));
        break;
    case K_COS:
        if (fInv) {
            if (fHyp) {
                if (!(1.0 <= (ext)num)) { DisplayError(1); return; }
                num = rt_log((double)((ext)rt_sqrt((double)(rt_cipow(num, 2.0) - 1.0L)) + num));
            } else st_num((ext)trigFactor[nTrig] * rt_acos(num));
        } else if (fHyp) num = rt_cosh(num);
        else {
            num = rt_cos((double)((ext)num / trigFactor[nTrig]));
            if ((ext)rt_fabs(num) < 1e-15) num = 0.0;
        }
        break;
    case K_TAN:
        if (fInv) {
            if (fHyp) {
                if (!((ext)rt_fabs(num) < 1.0)) { DisplayError(1); return; }
                ext a = 1.0L + num, b = 1.0L - num;
                st_num((ext)rt_log((double)(a / b)) * 0.5);
            } else st_num((ext)trigFactor[nTrig] * rt_atan(num));
        } else if (fHyp) num = rt_tanh(num);
        else {
            num = rt_tan((double)((ext)num / trigFactor[nTrig]));
            if ((ext)rt_fabs(num) > 1e15) DisplayError(2);
        }
        break;
    case K_LN:
    case K_LOG:
        if (fInv) num = rt_pow(cmd == K_LOG ? 10.0 : 2.71828182845905, num);
        else {
            if (!(0.0 < (ext)num)) { DisplayError(2); return; }
            num = cmd == K_LOG ? rt_log10(num) : rt_log(num);
        }
        break;
    case K_SQR:
        if (fInv || nLayout != 0) { num = rt_sqrt(num); break; }
        if ((ext)rt_fabs(num) > 1e154) { DisplayError(3); return; }
        st_num(rt_cipow(num, 2.0));
        break;
    case K_CUBE:
        if (fInv) st_num((ext)rt_pow(rt_fabs(num), 0x1.5555555555555p-2) * sign);
        else {
            if ((ext)rt_fabs(num) > 1e102) { DisplayError(3); return; }
            st_num(rt_cipow(num, 3.0));
        }
        break;
    case K_FACT: {
        if (!((ext)num >= 0.0)) { DisplayError(1); return; }
        if (!((ext)rt_fmod(num, 1.0) == 0.0)) { DisplayError(3); return; }
        if (!(170.0 >= (ext)num)) { DisplayError(3); return; }
        int32_t n = rt_ftol(num);
        num = 1.0;
        for (int32_t i = 2; i <= n; i++) st_num((ext)i * num);
        break;
    }
    case K_RECIP:
        if (num == 0.0) { DisplayError(0); return; }
        st_num(1.0L / num);
        break;
    case K_DMS: {
        /* degrees.minutesseconds <-> decimal degrees */
        fr = rt_modf((double)((ext)sign * num), &ip);
        f60 = (double)((fInv ? 40.0L : 0.0L) + 60.0L);
        fr2 = rt_modf((double)((ext)f60 * fr), &ip2);
        if (!(0.99999999999 >= (ext)fr2)) {
            fr2 = 0.0;
            ip2 = (double)(1.0L + ip2);
        }
        ext r;
        if (fInv) r = (ext)ip2 * 0x1.1111111111111p-6 + (ext)fr2 * 0x1.c71c71c71c71cp-6;
        else r = (ext)ip2 * 0.01 + (ext)fr2 * 0.006;
        st_num((r + ip) * sign);
        break;
    }
    case K_PERCENT:
        st_num((ext)lastOperand * 0.01 * num);
        break;
    }
}

/* ------------------------------------------------------------------ seg1:1E3C DoOperation */
static double DoOperation(int op, double x)
{
    double fnum = rt_fabs(num), fx = rt_fabs(x), r = 0.0, t1, t2;
    int32_t l;
    if (op >= K_AND && op <= K_LSH && ((ext)fnum > 4294967295.0 || (ext)fx > 4294967295.0)) {
        DisplayError(3);
        return 0.0;
    }
    switch (op) {
    case K_PWR:
        if (fInv) {
            fInv = 0;
            CheckButton(K_INV, 0);
            if (num == 0.0) { DisplayError(1); return 0.0; }
            st_num(1.0L / num);
        }
        return rt_pow(x, num);
    case K_RSH: {
        fInv = 0;
        CheckButton(K_INV, 0);
        int n = (unsigned char)rt_ftol(num);
        l = rt_ftol(x);
        while (n--) l >>= 1;
        return (double)l;
    }
    case K_AND: return (double)(rt_ftol(num) & rt_ftol(x));
    case K_OR: return (double)(rt_ftol(num) | rt_ftol(x));
    case K_XOR: return (double)(rt_ftol(num) ^ rt_ftol(x));
    case K_LSH: {
        int n = (unsigned char)rt_ftol(num);
        uint32_t u = (uint32_t)rt_ftol(x);
        while (n--) u <<= 1;
        return (double)(int32_t)u;
    }
    case K_DIV:
    case K_MOD:
        if (num == 0.0) { DisplayError(0); return 0.0; }
        if (op == K_MOD) return rt_fmod(x, num);
        if (fx != 0.0 && (ext)rt_log10(fx) - rt_log10(fnum) > 307.0) { DisplayError(3); return 0.0; }
        rt_st64((ext)x / num, &r);
        return r;
    case K_MUL:
        /* 3.1 checks log10|x| + ln|num| against 307 */
        if (num != 0.0 && x != 0.0 && (ext)rt_log10(fx) + rt_log(fnum) > 307.0) { DisplayError(3); return 0.0; }
        rt_st64((ext)x * num, &r);
        return r;
    case K_ADD:
        if (!(0.0 >= (ext)num) && !(0.0 >= (ext)x)) {
            t1 = (double)(0.1L * num);
            t2 = (double)(0.1L * x);
            if ((ext)t2 + t1 > 1e307) { DisplayError(3); return 0.0; }
        }
        rt_st64((ext)x + num, &r);
        return r;
    case K_SUB:
        if (!(0.0 <= (ext)num)) {
            t1 = (double)(0.1L * num);
            t2 = (double)(0.1L * x);
            if ((ext)t1 - 1e307 > (ext)t2) { DisplayError(3); return 0.0; }
        }
        rt_st64((ext)x - num, &r);
        return r;
    }
    return 0.0;
}

/* ------------------------------------------------------------------ seg1:1AF0 SetRadix */
static void SetRadix(int id)
{
    int fDecimal = 1;
    if (id == K_DEC) nTrig = nSavedTrig;
    else {
        fDecimal = 0;
        nTrig = nWordSize;
    }
    CheckRadioButton(hwndCalc, K_BIN, K_HEX, id);
    CheckRadioButton(hwndCalc, K_DEG, K_GRAD, nTrig + K_DEG);
    for (int i = 0; i < 3; i++) SetDlgItemText(hwndCalc, K_DEG + i, szStr[S_DWORD + fDecimal * 3 + i]);
    nRadix = radixtab[id - K_BIN];
    DisplayNum();
}

/* ------------------------------------------------------------------ seg1:1B96 ExpEntry */
/* digits and +/- after Exp build the exponent; returns TRUE when the key was used */
static BOOL ExpEntry(int cmd)
{
    char tmp[16];
    if (cmd == K_EXP || cmd == K_POINT) return TRUE;
    if (cmd < 0) {
        fExpFirst = 1;
        expSign = 1;
        expVal = 0;
        return FALSE;
    }
    if (cmd > 0x7F) return FALSE;
    if (cmd != K_SIGN && !(cmd >= '0' && cmd <= '9')) return FALSE;
    if (fExpFirst) {
        lstrcpy(szExpMant, szDisplay);
        expMant = num;
        expT = 0.0;
        while (!(10.0 >= (ext)num)) {
            expT = (double)(1.0L + expT);
            num = (double)(0.1L * num);
        }
    }
    fExpFirst = 0;
    if (cmd == K_SIGN) expSign = -expSign;
    else expVal = (int16_t)(10 * expVal + cmd - '0');
    expE = (double)(int16_t)(expSign * expVal);
    if ((ext)rt_fabs((double)((ext)expE + expT)) > 307.0 || (ext)rt_fabs(expE) > 307.0) {
        DisplayError(0.0 < (ext)expE + expT || (ext)expE > 307.0 ? 3 : 4);
        return TRUE;
    }
    st_num((ext)rt_pow(10.0, expE) * expMant);
    lstrcpy(szDisplay, szExpMant);
    lstrcat(szDisplay, expSign >= 0 ? "e+" : "e-");
    for (int z = abs(expVal); z < 100; z *= 10) {
        if (!z) break;
        lstrcat(szDisplay, "0");
    }
    lstrcat(szDisplay, rt_itoa(abs(expVal), tmp, 10));
    SetDlgItemText(hwndCalc, IDC_DISP + nLayout, szDisplay);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0B7E ProcessCommand */
static void ProcessCommand(int cmd)
{
    char buf[64], tmp[16];
    int i;
    if (cmd != K_INV && cmd != K_HYP && cmd != K_STA && cmd != K_FE && cmd != K_MC && cmd != K_BACK &&
        cmd != K_DEG && cmd != K_RAD && cmd != K_GRAD && (unsigned)cmd < 0x100 && (unsigned)cmd >= 0x20) {
        lastKeyPrev = lastKey;
        lastKey = cmd;
    }
    if (fError && cmd != K_CLEAR && cmd != K_CE && cmd != M_HELPONHELP && cmd != M_INDEX) {
        MessageBeep(0);
        return;
    }
    if (fExpEntry) {
        if (ExpEntry(cmd)) return;
        ExpEntry(-1);
    }
    fExpEntry = 0;
    /* a number typed after a function, = or a closing parenthesis starts afresh */
    if (((unsigned)cmd >= '0' && (unsigned)cmd <= '9') || ((unsigned)cmd >= 'A' && (unsigned)cmd <= 'F') ||
        cmd == K_POINT) {
        if ((lastKeyPrev >= K_INT && lastKeyPrev <= K_HEX) || (lastKeyPrev == ')' && nParens == 0) ||
            cmd == M_PASTE) {
            lastOperand = 0.0;
            num = 0.0;
            fDecPt = 0;
            lastKeyPrev = 0;
            nPendingOp = 0;
            fNewEntry = 1;
            nDec = 1;
            nSign = 1;
        }
    }
    /* digits */
    if ((unsigned)cmd <= 0x50 && (((unsigned)cmd >= '0' && (unsigned)cmd <= '9') ||
                                  ((unsigned)cmd >= 'A' && (unsigned)cmd <= 'F'))) {
        int d = cmd - (cmd >= 'A' ? 0x37 : 0x30);
        if ((unsigned)d > (unsigned)(nRadix - 1)) {
            MessageBeep(0);
            return;
        }
        if (fDecPt) {
            ext v = (ext)d * nSign;
            double p = rt_pow(10.0, (double)nDec++);
            st_num(v / p + num);
        } else {
            if (!(1e308 / (ext)nRadix > (ext)num)) {
                DisplayError(3);
                return;
            }
            st_num((ext)d * nSign + (ext)nRadix * num);
        }
        DisplayNum();
        return;
    }
    /* statistics */
    if ((unsigned)cmd >= K_AVE && (unsigned)cmd <= K_DAT) {
        if (hStatBox) {
            StatFunc(cmd);
            if (!fError) DisplayNum();
        } else MessageBeep(0);
        fInv = 0;
        CheckButton(K_INV, 0);
        return;
    }
    /* binary operators, with precedence in the scientific view */
    if ((unsigned)cmd >= K_AND && (unsigned)cmd <= K_PWR) {
        if (fInv && cmd == K_LSH) cmd = K_RSH;
        if (lastKeyPrev >= K_AND && lastKeyPrev <= K_PWR) {
            nPendingOp = cmd;
            return;
        }
        if (fOpPending) {
            for (;;) {
                int a = 0, b = 0;
                while (a < 12 && prectab[a][0] != cmd) a++;
                while (b < 12 && prectab[b][0] != nPendingOp) b++;
                if (a == 12) a = 0;
                if (b == 12) b = 0;
                if (prectab[b][1] < prectab[a][1] && nLayout == 0) {
                    if (nPrecStack < 25) {
                        precVal[nPrecStack] = lastOperand;
                        precOp[nPrecStack] = nPendingOp;
                    } else {
                        nPrecStack = 24;
                        MessageBeep(0);
                    }
                    nPrecStack++;
                    break;
                }
                num = DoOperation(nPendingOp, lastOperand);
                if (nPrecStack != 0 && precOp[nPrecStack - 1] != 0) {
                    nPrecStack--;
                    nPendingOp = precOp[nPrecStack];
                    lastOperand = precVal[nPrecStack];
                    continue;
                }
                if (!fError) DisplayNum();
                break;
            }
        }
        lastOperand = num;
        num = 0.0;
        nPendingOp = cmd;
        nSign = 1;
        nDec = 1;
        fOpPending = 1;
        fNewEntry = 1;
        fDecPt = 0;
        return;
    }
    /* functions of the displayed number */
    if ((unsigned)cmd >= K_INT && (unsigned)cmd <= K_PERCENT) {
        if (lastKeyPrev >= K_AND && lastKeyPrev <= K_PWR) num = lastOperand;
        DoSciFunc(cmd);
        if (fError) return;
        DisplayNum();
        if (fInv && (cmd == K_INT || cmd == K_SIN || cmd == K_COS || cmd == K_TAN || cmd == K_SQR || cmd == K_CUBE ||
                     cmd == K_LOG || cmd == K_LN || cmd == K_DMS)) {
            fInv = 0;
            CheckButton(K_INV, 0);
        }
        if (fHyp && (cmd == K_SIN || cmd == K_COS || cmd == K_TAN)) {
            fHyp = 0;
            CheckButton(K_HYP, 0);
        }
        fNewEntry = 1;
        nSign = 1;
        return;
    }
    if ((unsigned)cmd >= K_BIN && (unsigned)cmd <= K_HEX) {
        if (nLayout == 1) cmd = K_DEC;
        SetRadix(cmd);
        nDec = 0;
        fDecPt = 0;
        return;
    }
    switch (cmd) {
    case M_SEARCH:
    case M_COPY:
    case M_PASTE:
    case M_ABOUT:
    case M_SCI:
    case M_STD:
    case M_HELPONHELP:
    case M_INDEX:
        MenuCommand(cmd);
        DisplayNum();
        return;
    case '(':
    case ')': {
        int fOpen = cmd == '(';
        if ((nParens >= 25 && fOpen) || (nParens == 0 && !fOpen)) {
            MessageBeep(0);
            return;
        }
        if (fOpen) {
            parenVal[nParens] = lastOperand;
            parenOp[nParens] = nPendingOp;
            nParens++;
            precOp[nPrecStack] = 0;
            nPrecStack++;
            lastOperand = 0.0;
            lastKey = 0;
            nPendingOp = K_ADD;
        } else {
            num = DoOperation(nPendingOp, lastOperand);
            for (;;) {
                nPrecStack--;
                nPendingOp = precOp[nPrecStack];
                if (!nPendingOp) break;
                lastOperand = precVal[nPrecStack];
                num = DoOperation(nPendingOp, lastOperand);
            }
            nParens--;
            lastOperand = parenVal[nParens];
            nPendingOp = parenOp[nParens];
            fOpPending = nPendingOp != 0;
        }
        lstrcpy(buf, "(=");
        lstrcat(buf, rt_itoa(nParens, tmp, 10));
        SetDlgItemText(hwndCalc, IDC_PAREN, nParens ? buf : "");
        if (fError) return;
        if (!fOpen) {
            DisplayNum();
            return;
        }
        /* an opening parenthesis shows one "(" per open level in the display */
        for (i = 0; i < nParens; i++) buf[i] = '(';
        buf[i] = 0;
        SetDlgItemText(hwndCalc, IDC_DISP + nLayout, buf);
        fOpPending = 0;
        return;
    }
    case K_SIGN:
        st_num(-(ext)num);
        nSign = -nSign;
        DisplayNum();
        return;
    case K_CLEAR:
        lastOperand = 0.0;
        fOpPending = 0;
        fFE = 0;
        nParens = 0;
        nPendingOp = 0;
        lastKeyPrev = 0;
        lastKey = 0;
        nPrecStack = 0;
        fNewEntry = 1;
        SetDlgItemText(hwndCalc, IDC_PAREN, "");
        /* fall through */
    case K_CE:
        num = 0.0;
        nSign = 1;
        nDec = 0;
        if (nLayout == 0) {
            EnableBaseButtons(TRUE);
            fInv = 0;
            fExpEntry = 0;
            CheckButton(K_INV, 0);
            fHyp = 0;
            CheckButton(K_HYP, 0);
            ExpEntry(-1);
        }
        fError = 0;
        fDecPt = 0;
        DisplayNum();
        return;
    case K_STA:
        if (hStatBox) SetFocus(hStatBox);
        else StatBoxCreate(TRUE);
        return;
    case K_BACK:
        if (fDecPt && nDec == 1) {
            fDecPt = 0;
            nDec = 0;
        }
        if (fDecPt && nDec > 1) {
            double p, ip;
            nDec--;
            p = rt_pow(10.0, (double)(nDec - 1));
            rt_modf((double)((ext)p * num), &ip);
            st_num((ext)ip / p);
        } else
            rt_modf((double)((ext)num / nRadix), &num);
        if (num == 0.0) nSign = 1;
        if (lastKey == '0') {
            if (trailZeros >= 2) trailZeros -= 2;
            else {
                trailZeros = 0;
                lastKey = '.';
            }
        }
        if (num == 0.0 && nDec > 1) {
            lastKey = '0';
            trailZeros = nDec - 2;
        }
        DisplayNum();
        return;
    case K_POINT:
        if (!fDecPt && nRadix == 10) {
            fDecPt = 1;
            nDec = 1;
        }
        return;
    case K_FE:
        trailZeros = 0;
        fFE = !fFE;
        DisplayNum();
        return;
    case K_PI:
        if (nRadix != 10) {
            MessageBeep(0);
            return;
        }
        num = fInv ? 0x1.921fb54442d18p+2 : 0x1.921fb54442d18p+1;
        DisplayNum();
        fInv = 0;
        CheckButton(K_INV, 0);
        return;
    case K_EQU:
        for (;;) {
            if (lastKeyPrev >= K_AND && lastKeyPrev <= K_PWR) num = lastOperand;
            if (nPendingOp) {
                /* = again repeats the last operation with the same operand */
                if (fNewEntry) repeatOperand = num;
                else num = repeatOperand;
                lastOperand = DoOperation(nPendingOp, lastOperand);
                num = lastOperand;
                if (!fError) DisplayNum();
                fNewEntry = 0;
            }
            if (nPrecStack == 0 || nLayout == 1) break;
            nPrecStack--;
            nPendingOp = precOp[nPrecStack];
            lastOperand = precVal[nPrecStack];
            fNewEntry = 1;
            if (nPrecStack < 0) break;
        }
        fOpPending = 0;
        return;
    case K_MC:
    case K_MS:
        mem = cmd == K_MS ? num : 0.0;
        SetDlgItemText(hwndCalc, IDC_MEM + nLayout, mem != 0.0 ? " M" : "");
        return;
    case K_MR:
        num = mem;
        nSign = 0.0 <= (ext)num ? 1 : -1;
        DisplayNum();
        return;
    case K_MPLUS:
        if (!(0.0 >= (ext)num) && !(0.0 >= (ext)mem)) {
            double t1 = (double)(0.1L * num), t2 = (double)((ext)mem * 0.1);
            if ((ext)t1 + t2 > 1e307) {
                DisplayError(3);
                return;
            }
        }
        rt_st64((ext)num + mem, &mem);
        SetDlgItemText(hwndCalc, IDC_MEM + nLayout, mem != 0.0 ? " M" : "");
        return;
    case K_EXP: {
        if (fFE) DisplayNum();
        int len = lstrlen(szDisplay);
        /* 3.1 reads the bytes before the string for short displays: never an 'e' there */
        if ((len >= 5 && szDisplay[len - 5] == 'e') || (len >= 3 && szDisplay[len - 3] == 'e') || nRadix != 10) {
            MessageBeep(0);
            return;
        }
        if (num == 0.0) {
            num = 1.0;
            DisplayNum();
        }
        fExpEntry = 1;
        ExpEntry('0');
        return;
    }
    case K_INV:
        fInv = !fInv;
        CheckButton(K_INV, fInv);
        return;
    case K_HYP:
        fHyp = !fHyp;
        CheckButton(K_HYP, fHyp);
        return;
    case K_DEG:
    case K_RAD:
    case K_GRAD:
        nTrig = cmd - K_DEG;
        if (nRadix == 10) nSavedTrig = nTrig;
        else {
            wordMask = masks[nTrig];
            nWordSize = nTrig;
        }
        CheckRadioButton(hwndCalc, K_DEG, K_GRAD, nTrig + K_DEG);
        DisplayNum();
        return;
    }
}

/* ------------------------------------------------------------------ seg2: statistics */
/* seg2:0BA7 */
static void SetStatCount(int32_t n)
{
    char buf[20];
    SetDlgItemText(hStatBox, IDC_COUNT, rt_ltoa(n, buf, 10));
}

/* seg2:0C20 */
static void OutOfMem(void)
{
    char buf[0x32];
    MessageBeep(0);
    GetWindowText(hStatBox, buf, 0x31);
    MessageBox(hStatBox, szStr[S_NOMEM], buf, 0);
}

/* the statistics values live in a block grown and shrunk 12 at a time (GlobalReAlloc in 3.1) */
static BOOL StatResize(int32_t cap)
{
    double *p = realloc(statData, (size_t)(cap > 0 ? cap : 1) * sizeof *p);
    if (!p) return FALSE;
    statData = p;
    statCap = cap;
    return TRUE;
}

/* seg2:06C7: Ave, Sum, s, Dat */
static void StatFunc(int cmd)
{
    int32_t i;
    double v, t;
    switch (cmd) {
    case K_AVE:
    case K_SUM:
        num = 0.0;
        for (i = 0; i < nStat; i++) {
            v = statData[i];
            if (fInv) {
                /* sum of squares */
                if (!(1e154 >= (ext)v)) goto err;
                if (rt_cipow((ext)(double)0.1f * v, 2.0) + (ext)(double)0.1f * num > 1e307) goto err;
                num = (double)(rt_cipow(v, 2.0) + num);
            } else {
                if (((ext)num + v) * (double)0.1f > 1e307) goto err;
                num = (double)((ext)v + num);
            }
        }
        goto ave;
    err:
        DisplayError(3);
    ave:
        if (cmd == K_AVE) {
            if (nStat == 0) DisplayError(0);
            else num = (double)((ext)num / nStat);
        }
        break;
    case K_DEV:
        if (nStat <= 1) {
            num = 0.0;
            return;
        }
        t = 0.0;
        num = 0.0;
        for (i = 0; i < nStat; i++) {
            v = statData[i];
            if (!(1e153 >= (ext)v)) goto err2;
            if ((ext)(double)0.01f * v * v + (ext)(double)0.1f * num > 1e307) goto err2;
            if (((ext)v + t) * (double)0.1f > 1e307) goto err2;
            num = (double)(rt_cipow(v, 2.0) + num);
            t = (double)((ext)v + t);
        }
        goto dev;
    err2:
        DisplayError(3);
    dev:
        if (!(1e153 >= (ext)t)) {
            DisplayError(3);
            break;
        }
        t = (double)((ext)num - rt_cipow(t, 2.0) / nStat);
        if (t == 0.0) num = 0.0;
        else num = rt_sqrt((double)((ext)t / (int32_t)(fInv + nStat - 1))); /* Inv: population deviation */
        break;
    case K_DAT: {
        if (nStat % nStatBlock == 0 && !StatResize(statCap + nStatBlock)) {
            OutOfMem();
            return;
        }
        hStatList = GetDlgItem(hStatBox, IDC_LIST);
        LRESULT idx = SendMessage(hStatList, LB_ADDSTRING, 0, (LPARAM)szDisplay);
        if (idx == LB_ERR || idx == LB_ERRSPACE) {
            OutOfMem();
            return;
        }
        SendMessage(hStatList, LB_SETCURSEL, (WPARAM)idx, 0);
        statData[nStat] = num;
        nStat++;
        SetStatCount(nStat);
        break;
    }
    }
}

/* seg2:03D8 the Statistics Box */
static BOOL CALLBACK StatBoxProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    int32_t idx, n, i;
    switch (msg) {
    case WM_CLOSE:
        StatBoxCreate(FALSE);
        /* fall through */
    case WM_DESTROY:
        nStat = 0;
        return TRUE;
    case WM_INITDIALOG:
        hStatList = GetDlgItem(hDlg, IDC_LIST);
        return nInitShow != SW_SHOWMINNOACTIVE;
    case WM_COMMAND:
        if (HIWORD(lParam) == LBN_DBLCLK || wParam == IDC_LOAD) {
            /* LOAD (or a double click): the selected value into the display */
            idx = (int32_t)SendMessage(hStatList, LB_GETCURSEL, 0, 0);
            if (nStat > 0 && idx != -1) num = statData[idx];
            else MessageBeep(0);
            DisplayNum();
            lastKey = ' ';
            return FALSE;
        }
        switch (wParam) {
        case IDC_CD:
            idx = (int32_t)SendMessage(hStatList, LB_GETCURSEL, 0, 0);
            if (idx == -1 || nStat - 1 < idx || nStat == 0) {
                MessageBeep(0);
                return FALSE;
            }
            n = (int32_t)SendMessage(hStatList, LB_DELETESTRING, (WPARAM)idx, 0);
            if (--nStat == 0) goto cad;
            if (n > idx || n == 0) n = idx + 1;
            SendMessage(hStatList, LB_SETCURSEL, (WPARAM)(WORD)(n - 1), 0);
            for (i = idx; i < nStat; i++) statData[i] = statData[i + 1];
            SetStatCount(nStat);
            if (nStat % nStatBlock == 0) StatResize(statCap - nStatBlock);
            return TRUE;
        case IDC_CAD:
        cad:
            nStat = 0;
            SendMessage(hStatList, LB_RESETCONTENT, 0, 0);
            SetStatCount(0);
            free(statData);
            statData = NULL;
            statCap = 0;
            return TRUE;
        case IDC_RET:
            SetFocus(hwndCalc);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* seg2:0310 */
static void StatBoxCreate(BOOL fCreate)
{
    if (fCreate) {
        nStatBlock = 12;
        hStatBox = CreateDialog(hInst, "SB", NULL, StatBoxProc);
        statData = NULL;
        statCap = 0;
        ShowWindow(hStatBox, SW_SHOWNORMAL);
        SetDlgItemText(hwndCalc, IDC_STAT, szStr[S_STAT]);
    } else {
        DestroyWindow(hStatBox);
        free(statData);
        statData = NULL;
        statCap = 0;
        hStatBox = NULL;
        SetDlgItemText(hwndCalc, IDC_STAT, "");
    }
}

/* ------------------------------------------------------------------ seg2:0022 MenuCommand */
/* seg2:0000 */
static void HelpError(void)
{
    MessageBeep(0);
    MessageBox(hwndCalc, szStr[S_NOMEM], NULL, MB_ICONHAND);
}

static void MenuCommand(int cmd)
{
    char buf[0x38];
    switch (cmd) {
    case M_SEARCH:
        if (!WinHelp(hwndCalc, szStr[S_HELPFILE], HELP_PARTIALKEY, (DWORD)(uintptr_t)"")) HelpError();
        return;
    case M_COPY: {
        /* through a hidden edit control: the display as shown, its sign column included */
        lstrcpy(buf, szDisplay);
        if (!fDecPt) {
            int len = lstrlen(buf);
            if (len && buf[len - 1] == szDec[0]) buf[len - 1] = 0;
        }
        SetWindowText(hwndEdit, buf);
        SendMessage(hwndEdit, EM_SETSEL, 0, MAKELPARAM(0, 0x21));
        SendMessage(hwndEdit, WM_COPY, 0, 0);
        return;
    }
    case M_PASTE: {
        /* every character is a key press; ":x" sequences reach the keys that have no character */
        if (!OpenClipboard(hwndCalc)) {
            MessageBox(hwndCalc, szStr[S_NOCLIP], szStr[S_CALC], MB_ICONEXCLAMATION);
            return;
        }
        HGLOBAL h = GetClipboardData(CF_TEXT);
        DWORD size = h ? GlobalSize(h) : 0;
        const unsigned char *p = h ? GlobalLock(h) : NULL;
        int prev = 0;
        while (!fError && p) {
            if (size == 0) break;
            size--;
            int c = *p++;
            if (c == ' ' || c == '\n' || c == '\r') continue;
            if (prev == 0 && c == '-') {
                prev = c;
                c = K_SIGN;
            } else if ((c == 'x' || c == 'e') && nRadix == 10) {
                prev = 'x';
                c = K_EXP;
            } else if (prev == 'x' && c == '+' && nRadix == 10) continue;
            else if (prev == 'x' && c == '-' && nRadix == 10) {
                prev = c;
                c = K_SIGN;
            } else {
                int flag = prev == ':' ? 0x80 : 0;
                prev = c;
                if (c == ':') continue;
                if (c >= 'a' && c <= 'z') c -= 0x20;
                c = (c + flag) & 0xFF;
                int i = 0;
                while (i < 67 && pastetab[i][0] != c) i++;
                if (i == 67) break;
                c = pastetab[i][1];
            }
            SendMessage(hwndCalc, WM_COMMAND, (WPARAM)c, MAKELPARAM(0, 1));
        }
        if (h) GlobalUnlock(h);
        CloseClipboard();
        return;
    }
    case M_ABOUT:
        if (ShellAbout(hwndCalc, szStr[S_CALC], szStr[S_CREDITS], LoadIcon(hInst, "SC")) == -1) HelpError();
        return;
    case M_SCI:
    case M_STD: {
        if (cmd - nLayout == M_SCI) return;
        char s[2];
        layoutSetting = (layoutSetting & ~0xFF) | ((cmd - 0x31) & 0xFF);
        s[0] = (char)('0' + (layoutSetting & 0xFF));
        s[1] = 0;
        WriteProfileString("SciCalc", "layout", s);
        if (hStatBox && nLayout == 0) StatBoxCreate(FALSE);
        fRelayout = 1;
        InitSettings(TRUE);
        return;
    }
    case M_HELPONHELP:
        if (!WinHelp(hwndCalc, NULL, HELP_HELPONHELP, 0)) HelpError();
        return;
    case M_INDEX:
        if (!WinHelp(hwndCalc, szStr[S_HELPFILE], HELP_INDEX, 0)) HelpError();
        return;
    }
}

/* ------------------------------------------------------------------ seg1:0288 InitSettings */
static void InitSettings(BOOL fForce)
{
    char buf[20];
    BOOL fChanged = FALSE;
    if (nColors == 2) {
        GetProfileString("SciCalc", "background", "8421504", buf, 10);
        fColor = 0;
    } else GetProfileString("SciCalc", "background", "-1", buf, 10);
    COLORREF c = buf[0] == '-' ? GetSysColor(COLOR_APPWORKSPACE) : (COLORREF)rt_atol(buf);
    if (c != crBack) {
        crBack = c;
        hbrBack = CreateSolidBrush(c);
        fChanged = TRUE;
    }
    char oldDec = szDec[0];
    GetProfileString("intl", "sDecimal", ".", szDec, 5);
    if (oldDec == szDec[0] && !fChanged && !fForce) return;
    if (szStr[S_POINTKEY][0] == oldDec) szStr[S_POINTKEY][0] = szDec[0];
    if (IsIconic(hwndCalc)) return;
    if (nInitShow != SW_SHOWMINNOACTIVE) SetFocus(hwndCalc);
    InvalidateRect(hwndCalc, NULL, TRUE);
    nLayout = layoutSetting;
    if (nLayout == 0) {
        keyW = (dlgW - 52) / 11;
        keyX0 = (dlgW - 11 * keyW - 52) / 2;
        SetWindowPos(hwndCalc, NULL, 0, 0, dlgW, dlgH, SWP_NOMOVE | SWP_NOZORDER);
    } else {
        int stdW = dlgW * 13 / 24; /* [0F8C] */
        keyW = (stdW - 32) / 6;
        keyX0 = (stdW - 6 * keyW - 32) / 2;
        SetWindowPos(hwndCalc, NULL, 0, 0, stdW, dlgH * 4 / 5, SWP_NOMOVE | SWP_NOZORDER);
    }
    HMENU hm = GetSubMenu(GetMenu(hwndCalc), 1);
    CheckMenuItem(hm, nLayout, MF_BYPOSITION | MF_CHECKED);
    CheckMenuItem(hm, 1 - nLayout, MF_BYPOSITION | MF_UNCHECKED);
    RECT rc;
    SetRect(&rc, 0, 0, dlgW, dlgH);
    HDC dc = GetDC(hwndCalc);
    FillRect(dc, &rc, GetStockObject(WHITE_BRUSH));
    ReleaseDC(hwndCalc, dc);
    for (int i = 0; i < 15; i++) {
        int id = ctltab[i];
        ShowWindow(GetDlgItem(hwndCalc, id & 0x7FFF), ((id >> 15) ^ nLayout) == 0 ? SW_SHOWNORMAL : SW_HIDE);
    }
    if (nLayout != 0) SetRadix(K_DEC);
    SetDlgItemText(hwndCalc, IDC_MEM + nLayout, mem != 0.0 ? " M" : "");
}

/* ------------------------------------------------------------------ seg1:086B Paint */
static void Paint(void)
{
    PAINTSTRUCT ps;
    int vis = 0, col = 0, row = 0, yOff = nLayout * 18, x, y;
    COLORREF last = (COLORREF)-1;
    HPEN hPen = NULL;
    HCURSOR hOld = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ShowCursor(TRUE);
    HDC hdc = BeginPaint(hwndCalc, &ps);
    HGDIOBJ hOldPen = SelectObject(hdc, GetStockObject(BLACK_PEN));
    SelectObject(hdc, CreateSolidBrush(GetSysColor(COLOR_WINDOW)));
    FillRect(hdc, &ps.rcPaint, hbrBack);
    for (int i = nLayout * 7; i < nLayout * 2 + 7; i++) {
        RECT rc = frames[i];
        MapDialogRect(hwndCalc, &rc);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    }
    HGDIOBJ hbr = GetStockObject(WHITE_BRUSH);
    if (hbr) DeleteObject(SelectObject(hdc, hbr));
    for (int k = 0; k < 61; k++) {
        int flags = keytab[k][0];
        if ((flags & 3) == nLayout) continue;
        if (nKeys[nLayout] - 2 <= ++vis) { /* C, CE and Back, wider, on the top row */
            extraW = keyW / 3;
            x = (vis - nKeys[nLayout] + 2) * (extraW + keyW + 4) + 6;
            y = 38 - yOff;
        } else {
            extraW = 0;
            x = (keyW + 4) * col + keyX0 + 6;
            y = (row + 3) * 18 - yOff;
        }
        if (fColor) {
            COLORREF c = colours[(flags >> 5) & 7];
            if (c != last) hPen = CreatePen(PS_SOLID, 1, c);
            if (hPen) {
                HGDIOBJ old = SelectObject(hdc, hPen);
                if (old != hPen) DeleteObject(old);
            }
            last = c;
            SetTextColor(hdc, colours[(flags >> 2) & 7]);
        }
        RoundRect(hdc, x, charH * y / 8, x + keyW + extraW, charH * (y + 14) / 8, 10, 20);
        int len = lstrlen(szStr[k]);
        int w = LOWORD(GetTextExtent(hdc, szStr[k], len));
        TextOut(hdc, (2 * x - w + keyW + extraW) / 2, (y + 3) * charH * 2 / 16, szStr[k], len);
        row = (row + 1) % nRows[nLayout];
        if (row == 0) col++;
    }
    SelectObject(hdc, hOldPen);
    EndPaint(hwndCalc, &ps);
    if (fColor && hPen) DeleteObject(hPen);
    SetCursor(hOld);
    ShowCursor(FALSE);
}

/* ------------------------------------------------------------------ seg1:0722 FlashKey */
/* inverts the key twice (R2_NOTXORPEN): the key blinks for the time the two RoundRects take */
static void FlashKey(int id)
{
    int n, vis = 0, x, y, w;
    for (n = 0; n < 61; n++) {
        if (keytab[n][1] == id && (keytab[n][0] & 3) != nLayout) break;
        if ((keytab[n][0] & 3) != nLayout) vis++;
    }
    HDC hdc = GetDC(hwndCalc);
    if (nKeys[nLayout] - 3 <= vis) {
        w = keyW * 4 / 3;
        x = (vis - nKeys[nLayout] + 3) * (w + 4) + 6;
        y = topY[nLayout];
    } else {
        w = keyW;
        x = vis / nRows[nLayout] * (keyW + 4) + keyX0 + 6;
        y = vis % nRows[nLayout] * 18 + keyY0[nLayout];
    }
    HGDIOBJ b = GetStockObject(BLACK_BRUSH);
    if (b) SelectObject(hdc, b);
    SetROP2(hdc, R2_NOTXORPEN);
    for (int k = 2; k-- != 0;) RoundRect(hdc, x, charH * y / 8, x + w, charH * (y + 14) / 8, 10, 20);
    ReleaseDC(hwndCalc, hdc);
}

/* ------------------------------------------------------------------ seg1:18D6 HitTest */
static int HitTest(unsigned x, unsigned y)
{
    int extra = keyW * 4 / 3, pitch = extra + 4, L = nLayout;
    if ((unsigned)(charH * (topY[L] + 14) / 8) >= y && (unsigned)(charH * topY[L] / 8) <= y) {
        for (int i = 0, xo = 0; i < 3; i++, xo += pitch)
            if ((unsigned)(xo + 6) <= x && (unsigned)(xo + extra + 6) >= x) return i + K_CLEAR;
        return 0;
    }
    int x0 = keyX0 + 6, yTop = charH * keyY0[L] / 8;
    if ((unsigned)yTop > y || (unsigned)(charH * (keyY0[L] + 86) / 8) < y) return 0;
    int rowPitch = charH * 18 / 8, keyH = charH * 14 / 8;
    int found = 0, row = 0, yy = yTop, yo = 0;
    while (!found) {
        if (nRows[L] < row) break;
        if (y >= (unsigned)yy && (unsigned)(yo + keyH + yTop) >= y) found = 1;
        yy += rowPitch;
        yo += rowPitch;
        row++;
    }
    if (!found || nRows[L] < row) return 0;
    int colPitch = keyW + 4, col = 0, xx = x0, xo = 0;
    found = 0;
    while (!found) {
        if (nCols[L] <= col) break;
        if (x >= (unsigned)xx && (unsigned)(xo + keyW + x0) >= x) found = 1;
        xx += colPitch;
        xo += colPitch;
        col++;
    }
    if (!found) return 0;
    int k = nRows[L] * (col - 1) + row - 1, i = 0;
    for (int p = 0; k >= 0; p++, i++) {
        if (p >= 61) break;
        if ((keytab[p][0] & 3) != nLayout) k--;
    }
    return i ? keytab[i - 1][1] : 0;
}

/* ------------------------------------------------------------------ seg1:051C CalcWndProc */
static LRESULT CALLBACK CalcWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        WinHelp(hwndCalc, szStr[S_HELPFILE], HELP_QUIT, 0);
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        /* the Statistics Box hides while Calculator is an icon */
        if (hStatBox) ShowWindow(hStatBox, wParam == SIZE_MINIMIZED ? SW_HIDE : SW_SHOW);
        break;
    case WM_PAINT:
        Paint();
        if (fError) DisplayError(nErrCode);
        else if (lastKey >= K_AND && lastKey <= K_PWR) {
            double s = num;
            num = lastOperand;
            DisplayNum();
            num = s;
        } else DisplayNum();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwndCalc);
        return 0;
    case WM_WININICHANGE:
        if (!lParam || !lstrcmp((LPCSTR)lParam, "colors") || !lstrcmp((LPCSTR)lParam, "intl")) InitSettings(FALSE);
        return 0;
    case WM_COMMAND: {
        int n = 0, id = (int)(WORD)wParam;
        if (HIWORD(lParam) == 1 && id <= 0x78) {
            /* a key typed: only the keys of this view, flashed */
            if (id == K_MOD && nLayout == 1) id = K_PERCENT;
            for (n = 0; n < 61; n++)
                if (keytab[n][1] == id && (keytab[n][0] & 3) != nLayout) break;
            if (n < 61) FlashKey(id);
        }
        if (n < 61) ProcessCommand(id);
        return 0;
    }
    case WM_INITMENUPOPUP:
        EnableMenuItem(GetMenu(hwnd), M_PASTE, IsClipboardFormatAvailable(CF_TEXT) ? MF_ENABLED : MF_GRAYED | MF_DISABLED);
        return 0;
    case WM_LBUTTONDOWN: {
        int id = HitTest(LOWORD(lParam), HIWORD(lParam));
        if (id) {
            FlashKey(id);
            ProcessCommand(id);
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:0000 WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    RECT rc;
    TEXTMETRIC tm;
    char buf[0x6E];
    (void)lpCmdLine;
    nInitShow = nCmdShow;
    /* the C run time's floating-point setup and the SIGFPE handler 3.1 installs below */
    rt_init(CalcMatherr, SignalHandler);
    if (!hPrev) {
        WNDCLASS wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = CalcWndProc;
        wc.cbWndExtra = DLGWINDOWEXTRA;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIcon(hInstance, "SC");
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszMenuName = "SM";
        wc.lpszClassName = "SciCalc";
        if (!RegisterClass(&wc)) return 0;
    }
    hInst = hInstance;
    /* the 78 strings, at most 109 characters each (two messages are cut short by this, as in 3.1) */
    for (int i = 0; i < NSTR; i++) {
        buf[0] = 0;
        LoadString(hInst, (UINT)i, buf, sizeof buf);
        szStr[i] = strdup(buf);
    }
    hwndCalc = CreateDialog(hInst, "SC", NULL, NULL);
    if (!hwndCalc) return 0;
    GetWindowRect(hwndCalc, &rc);
    dlgH = rc.bottom - rc.top;
    dlgW = rc.right - rc.left;
    HDC dc = GetDC(NULL);
    GetTextMetrics(dc, &tm);
    charH = dlgH / 20;
    if (charH > tm.tmHeight) charH = tm.tmHeight;
    nColors = GetDeviceCaps(dc, NUMCOLORS);
    ReleaseDC(NULL, dc);
    layoutSetting = GetProfileInt("SciCalc", "layout", 1);
    InitSettings(TRUE);
    SetRadix(K_DEC);
    nTrig = 0;
    CheckRadioButton(hwndCalc, K_DEG, K_GRAD, K_DEG);
    /* (3.1 seeds rand() with GetTickCount() here and never calls it) */
    nInitShow = 0;
    ShowWindow(hwndCalc, nCmdShow);
    UpdateWindow(hwndCalc);
    hwndEdit = CreateWindow("EDIT", "", WS_CHILD, -270, -15, 270, 10, hwndCalc, (HMENU)(uintptr_t)IDC_EDIT, hInst, NULL);
    SendMessage(hwndEdit, EM_LIMITTEXT, 0, 0);
    hAccel = LoadAccelerators(hInst, "SA");
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (hStatBox && IsDialogMessage(hStatBox, &msg)) continue;
        if (TranslateAccelerator(hwndCalc, hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DeleteObject(hbrBack);
    return (int)msg.wParam;
}
