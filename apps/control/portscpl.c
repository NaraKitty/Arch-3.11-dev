/* MAIN.CPL Ports applet (dialog 4, seg19), with its Settings (19) and Advanced (33) dialogs and the
 * "System Setting Change" restart dialog (37, seg9:05A9, shared with Fonts). COM1..COM4 settings
 * live in WIN.INI [ports] as in 3.1; the base address / IRQ in SYSTEM.INI [386Enh] COMnBase/COMnIrq
 * (kept for fidelity - Linux's serial driver does not read them). */
#include "maincpl.h"
#include <string.h>

#define IDD_PORTS 4
#define IDD_PORTSETTINGS 19
#define IDD_ADVANCED 33
#define IDD_RESTART 37
#define IDC_SETTINGS 828
#define IDC_PORT1 830            /* owner-drawn port buttons 830..833 */
#define IDC_BAUD 800
#define IDC_DATABITS 801
#define IDC_PARITY 802
#define IDC_STOPBITS 803
#define IDC_FLOW 804
#define IDC_ADVANCED 805
#define IDC_BASE 806
#define IDC_IRQ 807
#define IDC_RESTARTTEXT 100

static int iPort = 1;                 /* [0x0B92] current port, 1-based, kept between openings */
static char szComKey[] = "COM1:";     /* [0x0BB0] digit patched before each WIN.INI access */
static char szOldBase[4];             /* [0x0E48] Advanced: base text at WM_INITDIALOG (3 chars) */
static int iOldIrqSel;                /* [0x0E4E] Advanced: IRQ selection at WM_INITDIALOG */

static const char *const ParityStr[5] = {",e", ",o", ",n", ",m", ",s"};   /* [0x0BB6] */
static const char *const DataBitsStr[5] = {",4", ",5", ",6", ",7", ",8"}; /* [0x0BC0] */
static const char *const StopBitsStr[3] = {",1", ",1.5", ",2"};           /* [0x0BCA] */
static const char *const FlowStr[3] = {",x", ",p", " "};                  /* [0x0BD0] None: a blank */
static const WORD BaudTable[] = {110, 300, 600, 1200, 2400, 4800, 9600, 19200, 0}; /* [0x0BD6] */
static const WORD DataBitsTable[] = {4, 5, 6, 7, 8, 0};                   /* [0x0BE8] */
static const char *const BaseAddrList[] = {"03F8", "02F8", "03E8", "02E8", "02E0", ""}; /* [0x0C3C] */

static BOOL PortSettingsDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL AdvancedDlgProc(HWND, UINT, WPARAM, LPARAM);

/* ------------------------------------------------------------------ seg4 / seg1 string helpers */
/* seg1:1028: strchr that never matches the terminator */
static LPSTR StrChr(LPSTR s, char c)
{
    for (; *s; s++)
        if (*s == c) return s;
    return NULL;
}

/* seg4:0168: the first `c`, else the end of the string */
static LPSTR StrStrOrEnd(LPSTR s, char c)
{
    LPSTR p = StrChr(s, c);
    return p ? p : s + lstrlen(s);
}

/* seg4:019E: leading and trailing blanks (spaces only) removed in place */
static void StripBlanks(LPSTR s)
{
    LPSTR p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, lstrlen(p) + 1);
    p = s + lstrlen(s);
    if (p != s) {
        p--;
        while (*p == ' ') p--;
        p[1] = 0;
    }
}

/* seg1:184F: unsigned decimal, stops at the first non-digit, 16-bit */
static WORD StrToUInt(LPCSTR s)
{
    WORD n = 0;
    for (; (BYTE)(*s - '0') <= 9; s++) n = (WORD)(n * 10 + (*s - '0'));
    return n;
}

/* seg4:0210: decimal itoa (no sign) */
static void IntToStr(int n, LPSTR buf)
{
    LPSTR p = buf;
    do {
        *p++ = (char)(n % 10 + '0');
        n /= 10;
    } while (n > 0);
    *p = 0;
    for (LPSTR a = buf, b = p - 1; a < b; a++, b--) { char t = *a; *a = *b; *b = t; }
}

/* ------------------------------------------------------------------ seg9:05A9 */
/* "System Setting Change": lParam is the string id of the first sentence (1002 ports, 1001 TrueType) */
BOOL RestartDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szText[0xC8], szTail[0x64];
    if (msg == WM_INITDIALOG) {
        LoadString(hInstMain, LOWORD(lParam), szText, sizeof szText);
        if (LoadString(hInstMain, 1003, szTail, sizeof szTail)) lstrcat(szText, szTail);
        SetDlgItemText(hDlg, IDC_RESTARTTEXT, szText);
        return FALSE;
    }
    if (msg == WM_COMMAND) {
        if (wParam == IDOK) { ExitWindows(EW_RESTARTWINDOWS, 0); return TRUE; }  /* "Restart Now" */
        if (wParam == IDCANCEL) { EndDialog(hDlg, 0); return TRUE; }             /* "Don't Restart Now" */
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg19:04EC */
static void SetCurPort(HWND hDlg, int iNew, BOOL fNoRedrawNew)
{
    HWND hOld = GetDlgItem(hDlg, iPort + IDC_PORT1 - 1);
    iPort = iNew;
    InvalidateRect(hOld, NULL, TRUE);
    if (!fNoRedrawNew) InvalidateRect(GetDlgItem(hDlg, iNew + IDC_PORT1 - 1), NULL, TRUE);
}

/* ------------------------------------------------------------------ seg19:053B */
/* the Ports icon centred in the button, framed 2 px outside in the highlight colour for the current
 * port and the window colour for the others; focusing a button makes its port current */
static void DrawPortButton(HWND hDlg, const DRAWITEMSTRUCT *di)
{
    int cx = GetSystemMetrics(SM_CXICON), cy = GetSystemMetrics(SM_CYICON);
    int x = (di->rcItem.left + di->rcItem.right - cx) / 2, y = (di->rcItem.top + di->rcItem.bottom - cy) / 2;
    RECT rc = {x, y, x + cx, y + cy};
    DrawIcon(di->hDC, x, y, LoadIcon(hInstMain, MAKEINTRESOURCE(28)));
    if (di->itemAction == ODA_FOCUS) SetCurPort(hDlg, (int)di->CtlID - IDC_PORT1 + 1, TRUE);
    HBRUSH hbr = CreateSolidBrush(GetSysColor((int)di->CtlID - iPort == IDC_PORT1 - 1 ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    if (hbr) {
        InflateRect(&rc, 2, 2);
        FrameRect(di->hDC, &rc, hbr);
        DeleteObject(hbr);
    }
}

/* ------------------------------------------------------------------ seg19:04C6 */
/* the Settings dialog for port iNew (1-based); also run by the Printers applet's Connect */
int DoPortSettings(HWND hwndOwner, int iNew)
{
    iPort = iNew;
    return DoDialogBoxParam(IDD_PORTSETTINGS, hwndOwner, PortSettingsDlgProc, 8019, 0);
}

/* ------------------------------------------------------------------ seg19:062E */
BOOL PortsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DRAWITEM:
        DrawPortButton(hDlg, (const DRAWITEMSTRUCT *)lParam);
        return TRUE;
    case WM_INITDIALOG:
        return TRUE; /* the current port is the one from last time */
    case WM_COMMAND:
        switch (wParam) {
        case IDOK: case IDCANCEL:
            EndDialog(hDlg, (int)wParam);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_SETTINGS:
            break;
        case IDC_PORT1: case IDC_PORT1 + 1: case IDC_PORT1 + 2: case IDC_PORT1 + 3:
            SetCurPort(hDlg, (int)wParam - IDC_PORT1 + 1, FALSE);
            SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_SETTINGS), 1);
            if (HIWORD(lParam) != BN_DOUBLECLICKED) return TRUE;
            break;
        default:
            return TRUE;
        }
        if (DoPortSettings(hDlg, iPort) == IDOK) SetDlgItemText(hDlg, IDOK, szClose);
        return TRUE;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg19:06F5 */
/* the resource string's first character separates the items */
static void FillComboFromString(HWND hCombo, int ids, int iSel)
{
    char buf[0x102];
    if (!LoadString(hInstMain, ids, buf, sizeof buf)) return;
    char sep = buf[0];
    for (LPSTR p = buf + 1, q; p; p = q) {
        q = StrChr(p, sep);
        if (q) *q++ = 0;
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)p);
    }
    SendMessage(hCombo, CB_SETCURSEL, iSel, 0);
}

/* ------------------------------------------------------------------ seg19:00CB */
/* [ports] COMn:=baud,parity,data,stop,flow; a missing entry shows 1200 / None / 8 / 1 / None */
static void LoadPortSettings(HWND hDlg, int iPort0)
{
    char buf[0x52];
    int i;
    szComKey[3] = (char)(iPort0 + '1');
    GetProfileString("ports", szComKey, "", buf, 0x51);
    StripBlanks(buf);

    LPSTR p = buf, next = StrStrOrEnd(p, ',');
    if (*next) *next++ = 0;
    if (*p) {
        WORD baud = StrToUInt(p);
        for (i = 0; BaudTable[i] && BaudTable[i] != baud; i++) ;
        if (!BaudTable[i]) SendDlgItemMessage(hDlg, IDC_BAUD, CB_ADDSTRING, 0, (LPARAM)p); /* index 8 */
        SendDlgItemMessage(hDlg, IDC_BAUD, CB_SETCURSEL, i, 0);
    }

    p = next; next = StrStrOrEnd(p, ',');
    if (*next) *next++ = 0;
    StripBlanks(p);
    switch (*p) { /* case-sensitive, as in 3.1 */
    case 'e': i = 0; break;
    case 'o': i = 1; break;
    case 'm': i = 3; break;
    case 's': i = 4; break;
    default: i = 2; break;
    }
    SendDlgItemMessage(hDlg, IDC_PARITY, CB_SETCURSEL, i, 0);

    p = next; next = StrStrOrEnd(p, ',');
    if (*next) *next++ = 0;
    StripBlanks(p);
    i = (signed char)*p - '4';
    SendDlgItemMessage(hDlg, IDC_DATABITS, CB_SETCURSEL, i < 0 || i > 4 ? 4 : i, 0);

    p = next; next = StrStrOrEnd(p, ',');
    if (*next) *next++ = 0;
    StripBlanks(p);
    i = !lstrcmp(p, "1.5") ? 1 : !lstrcmp(p, "2") ? 2 : 0;
    SendDlgItemMessage(hDlg, IDC_STOPBITS, CB_SETCURSEL, i, 0);

    p = next; next = StrStrOrEnd(p, ',');
    if (*next) *next++ = 0;
    StripBlanks(p);
    i = *p == 'p' ? 1 : *p == 'x' ? 0 : 2;
    SendDlgItemMessage(hDlg, IDC_FLOW, CB_SETCURSEL, i, 0);
}

/* ------------------------------------------------------------------ seg19:0000 */
/* the baud rate must be digits (an empty field passes) */
static BOOL ValidateBaud(HWND hDlg)
{
    char buf[0x86];
    SendDlgItemMessage(hDlg, IDC_BAUD, WM_GETTEXT, 0x85, (LPARAM)buf);
    for (LPSTR p = buf; *p; p++)
        if ((signed char)*p < '0' || (signed char)*p > '9') {
            if (!LoadString(hInstMain, 32, buf, 0x85)) { OutOfMemory(hDlg); return FALSE; }
            MessageBox(hDlg, buf, szCaption, MB_ICONASTERISK);
            return FALSE;
        }
    return TRUE;
}

/* ------------------------------------------------------------------ seg19:03D7 */
/* "9600,n,8,1,x" into [ports] COMn: (flow None leaves a trailing blank: "9600,n,8,1 ") */
static void SavePortSettings(HWND hDlg)
{
    char buf[0x9E];
    SendDlgItemMessage(hDlg, IDC_BAUD, WM_GETTEXT, 0x12, (LPARAM)buf);
    int i = (int)SendDlgItemMessage(hDlg, IDC_PARITY, CB_GETCURSEL, 0, 0);
    if (i >= 0) lstrcat(buf, ParityStr[i]);
    i = (int)SendDlgItemMessage(hDlg, IDC_DATABITS, CB_GETCURSEL, 0, 0);
    if (i >= 0) lstrcat(buf, DataBitsStr[i]);
    i = (int)SendDlgItemMessage(hDlg, IDC_STOPBITS, CB_GETCURSEL, 0, 0);
    if (i >= 0) lstrcat(buf, StopBitsStr[i]);
    i = (int)SendDlgItemMessage(hDlg, IDC_FLOW, CB_GETCURSEL, 0, 0);
    if (i >= 0) lstrcat(buf, FlowStr[i]);
    szComKey[3] = (char)(iPort + '0');
    WriteProfileString("ports", szComKey, buf);
    BroadcastWinIniChange(4); /* "ports" */
}

/* ------------------------------------------------------------------ seg19:079D */
static BOOL PortSettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szFmt[0x52], szBuf[0x52];
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        for (int i = 0; BaudTable[i]; i++) {
            IntToStr(BaudTable[i], szBuf);
            SendDlgItemMessage(hDlg, IDC_BAUD, CB_ADDSTRING, 0, (LPARAM)szBuf);
        }
        SendDlgItemMessage(hDlg, IDC_BAUD, CB_SETCURSEL, 3, 0);
        for (int i = 0; DataBitsTable[i]; i++) {
            IntToStr(DataBitsTable[i], szBuf);
            SendDlgItemMessage(hDlg, IDC_DATABITS, CB_ADDSTRING, 0, (LPARAM)szBuf);
        }
        SendDlgItemMessage(hDlg, IDC_DATABITS, CB_SETCURSEL, 4, 0);
        FillComboFromString(GetDlgItem(hDlg, IDC_PARITY), 245, 2);
        FillComboFromString(GetDlgItem(hDlg, IDC_STOPBITS), 246, 0);
        FillComboFromString(GetDlgItem(hDlg, IDC_FLOW), 247, 2);
        LoadString(hInstMain, 249, szFmt, 0x51); /* "Settings for COM%d:" */
        wsprintf(szBuf, szFmt, iPort);
        SetWindowText(hDlg, szBuf);
        if (HIWORD(EscapeCommFunction(iPort - 1, GETBASEIRQ)) == 0) EnableWindow(GetDlgItem(hDlg, IDC_ADVANCED), FALSE);
        LoadPortSettings(hDlg, iPort - 1);
        ShowWindow(hDlg, SW_SHOWNORMAL);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            if (ValidateBaud(hDlg)) {
                HourGlass(TRUE);
                SavePortSettings(hDlg);
                HourGlass(FALSE);
                EndDialog(hDlg, IDOK);
            } else {
                SetFocus(GetDlgItem(hDlg, IDC_BAUD));
                /* 3.1 sends 0x401 here, which on a combo box is CB_LIMITTEXT(0), not a selection */
                SendDlgItemMessage(hDlg, IDC_BAUD, CB_LIMITTEXT, 0, MAKELPARAM(0, -1));
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_ADVANCED:
            if (DoDialogBoxParam(IDD_ADVANCED, hDlg, AdvancedDlgProc, 8033, 0) == 1) {
                SetDlgItemText(hDlg, IDCANCEL, szClose);
                SetDlgItemText(GetParent(hDlg), IDOK, szClose);
            }
            return TRUE;
        }
        return TRUE;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg19:007F */
/* upper-cases s in place; TRUE when it is hex digits only ("" too) */
static BOOL IsHexString(LPSTR s)
{
    AnsiUpper(s);
    for (LPSTR p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'))) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ seg19:09D1 */
static BOOL AdvancedDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szBase[8], szMsg[0xC8], szDefault[0x14], szBuf[0x78];
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        HourGlass(TRUE);
        LoadString(hInstMain, 253, szMsg, sizeof szMsg); /* "Advanced Settings for COM%d:" */
        wsprintf(szBuf, szMsg, iPort);
        SetWindowText(hDlg, szBuf);
        SendDlgItemMessage(hDlg, IDC_BASE, CB_LIMITTEXT, 4, 0);
        LoadString(hInstMain, 244, szDefault, sizeof szDefault); /* "Default" */
        SendDlgItemMessage(hDlg, IDC_BASE, CB_ADDSTRING, 0, (LPARAM)szDefault);
        for (int i = 0; *BaseAddrList[i]; i++) SendDlgItemMessage(hDlg, IDC_BASE, CB_ADDSTRING, 0, (LPARAM)BaseAddrList[i]);
        DWORD dw = (DWORD)EscapeCommFunction(iPort - 1, GETBASEIRQ); /* low word base, high word IRQ */
        if ((SHORT)LOWORD(dw) >= 0) wsprintf(szBuf, "%04X", LOWORD(dw));
        else lstrcpy(szBuf, szDefault);
        SetDlgItemText(hDlg, IDC_BASE, szBuf);
        SendDlgItemMessage(hDlg, IDC_IRQ, CB_ADDSTRING, 0, (LPARAM)szDefault);
        for (int i = 15; i >= 2; i--) { /* "15" .. "2": index = 16 - IRQ */
            wsprintf(szBuf, "%d", i);
            SendDlgItemMessage(hDlg, IDC_IRQ, CB_ADDSTRING, 0, (LPARAM)szBuf);
        }
        int sel = HIWORD(dw) >= 2 && HIWORD(dw) <= 15 ? 16 - HIWORD(dw) : 0;
        SendDlgItemMessage(hDlg, IDC_IRQ, CB_SETCURSEL, sel, 0);
        iOldIrqSel = (int)SendDlgItemMessage(hDlg, IDC_IRQ, CB_GETCURSEL, 0, 0);
        GetDlgItemText(hDlg, IDC_BASE, szOldBase, sizeof szOldBase); /* 3 characters, as in 3.1 */
        ShowWindow(hDlg, SW_SHOW);
        UpdateWindow(hDlg);
        HourGlass(FALSE);
        return TRUE;
    }
    case WM_COMMAND: {
        if (wParam == IDD_HELP) { CPHelp(hDlg); return TRUE; }
        if (wParam == IDCANCEL) { EndDialog(hDlg, 0); return TRUE; }
        if (wParam != IDOK) return FALSE;
        int iIrqSel = (int)SendDlgItemMessage(hDlg, IDC_IRQ, CB_GETCURSEL, 0, 0);
        GetDlgItemText(hDlg, IDC_BASE, szBase, sizeof szBase);
        LoadString(hInstMain, 244, szDefault, sizeof szDefault);
        BOOL fDefault = !lstrcmpi(szDefault, szBase);
        if (!fDefault && !IsHexString(szBase)) {
            LoadString(hInstMain, 255, szMsg, sizeof szMsg);
            GetWindowText(hDlg, szBuf, sizeof szBuf);
            MessageBox(hDlg, szMsg, szBuf, MB_ICONASTERISK);
            return TRUE;
        }
        HourGlass(TRUE); /* 3.1 never turns it off on this path */
        wsprintf(szBuf, "COM%dIrq", iPort);
        IntToStr(16 - iIrqSel, szMsg);
        WritePrivateProfileString("386Enh", szBuf, iIrqSel > 0 ? szMsg : NULL, "SYSTEM.INI");
        wsprintf(szBuf, "COM%dBase", iPort);
        WritePrivateProfileString("386Enh", szBuf, fDefault ? NULL : szBase, "SYSTEM.INI");
        /* changed? (after writing, comparing 3 characters of the base, like 3.1) */
        GetDlgItemText(hDlg, IDC_BASE, szBuf, 4);
        if (SendDlgItemMessage(hDlg, IDC_IRQ, CB_GETCURSEL, 0, 0) == iOldIrqSel && !lstrcmp(szBuf, szOldBase)) {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        HWND hParent = GetParent(hDlg);
        if (!ValidateBaud(hParent)) { SetFocus(hDlg); return TRUE; }
        SavePortSettings(hParent);
        DialogBoxParam(hInstMain, MAKEINTRESOURCE(IDD_RESTART), hDlg, RestartDlgProc, 1002);
        EndDialog(hDlg, 1);
        return TRUE;
    }
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}
