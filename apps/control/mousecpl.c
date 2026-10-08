/* Mouse applet: port of MAIN.CPL seg3:097F (run) and seg16 (dialog 6): tracking speed, double-click
 * speed with its TEST box, swapped buttons, mouse trails, and the L/R boxes that light up while a
 * button is held. */
#include "maincpl.h"
#include <string.h>

#define IDC_SWAP 500
#define IDC_LTEXT 503
#define IDC_RTEXT 504
#define IDC_LBOX 506
#define IDC_RBOX 507
#define IDC_TRAILS 509
#define IDC_TEST 529
#define IDC_DBLCLK 531
#define IDC_TRACK 532

static HWND hDlgMouse;              /* [0x11e2] */
static HWND hwndL, hwndR;           /* [0x1e2a], [0x11a4]: the "L" and "R" texts */
static RECT rcTest;                 /* [0x104c] TEST box, dialog client coordinates */
static RECT rcLeft, rcRight;        /* [0x1dc2], [0x1d90] inside the button boxes */
static BOOL fSwap, fSwapWas;        /* [0x17e2], [0x1402] */
static WORD wButtons;               /* [0x17e4] boxes shown down: 1 left, 2 right */
static HWND hwndDblClk;             /* [0x17e0] */
static int nDblClk, nDblClkWas;     /* [0xe8a], [0xe94] ms */
static int aMouse[3], aMouseWas[3]; /* [0x138e], [0x12d0]: threshold 1, threshold 2, speed */
static int nTrack;                  /* [0x1f88] tracking position 0..6 */
static BOOL fTrailsWas;             /* [0xe44] */

static WORD ButtonsDown(int (*state)(int))
{
    return ((state(VK_LBUTTON) & 0x80) ? 1 : 0) | ((state(VK_RBUTTON) & 0x80) ? 2 : 0);
}

static void SwapTexts(HWND hDlg)
{
    char l[3], r[3];
    GetDlgItemText(hDlg, IDC_LTEXT, l, sizeof l);
    GetDlgItemText(hDlg, IDC_RTEXT, r, sizeof r);
    SetDlgItemText(hDlg, IDC_LTEXT, r);
    SetDlgItemText(hDlg, IDC_RTEXT, l);
}

/* the control's window rectangle in dialog client coordinates, 1 pixel inside the frame */
static void InnerRect(HWND hDlg, int id, POINT org, RECT *rc)
{
    GetWindowRect(GetDlgItem(hDlg, id), rc);
    rc->left += 1 - org.x;
    rc->top += 1 - org.y;
    rc->right -= org.x + 1;
    rc->bottom -= org.y + 1;
}

/* the mouse trails escape of the display driver (none in libw16: the option stays disabled) */
static void SetTrails(BOOL fOn)
{
    HDC hdc = GetDC(NULL);
    if (!hdc) return;
    int n = fOn ? -1 : 0;
    Escape(hdc, MOUSETRAILS, 2, (LPCSTR)&n, NULL);
    ReleaseDC(NULL, hdc);
}

/* ------------------------------------------------------------------ seg16:0000 */
static void InitMouseDlg(HWND hDlg)
{
    POINT org = {0, 0};
    hDlgMouse = hDlg;
    ClientToScreen(hDlg, &org);
    hwndL = GetDlgItem(hDlg, IDC_LTEXT);
    hwndR = GetDlgItem(hDlg, IDC_RTEXT);
    GetWindowRect(GetDlgItem(hDlg, IDC_TEST), &rcTest);
    OffsetRect(&rcTest, -org.x, -org.y);
    InnerRect(hDlg, IDC_LBOX, org, &rcLeft);
    InnerRect(hDlg, IDC_RBOX, org, &rcRight);

    fSwap = fSwapWas = SwapMouseButton(TRUE);
    SwapMouseButton(fSwap);
    if (fSwap) SwapTexts(hDlg);
    CheckDlgButton(hDlg, IDC_SWAP, fSwap);

    HDC hdc = GetDC(NULL);
    if (!hdc) {
        CheckDlgButton(hDlg, IDC_TRAILS, 0);
        EnableWindow(GetDlgItem(hDlg, IDC_TRAILS), FALSE);
    } else {
        int esc = MOUSETRAILS;
        int n = Escape(hdc, QUERYESCSUPPORT, 2, (LPCSTR)&esc, NULL);
        EnableWindow(GetDlgItem(hDlg, IDC_TRAILS), n);
        CheckDlgButton(hDlg, IDC_TRAILS, n > 1);
        ReleaseDC(NULL, hdc);
    }
    wButtons = ButtonsDown(GetAsyncKeyState);

    hwndDblClk = GetDlgItem(hDlg, IDC_DBLCLK);
    nDblClk = nDblClkWas = GetProfileInt("windows", "DoubleClickSpeed", 500);
    SetScrollRange(hwndDblClk, SB_CTL, 100, 900, FALSE);
    SetScrollPos(hwndDblClk, SB_CTL, 1000 - nDblClk, TRUE);
    SetDoubleClickTime(nDblClk);

    SystemParametersInfo(SPI_GETMOUSE, 0, aMouse, 0);
    memcpy(aMouseWas, aMouse, sizeof aMouse);
    nTrack = 0;
    if (aMouse[2] == 2) nTrack = (24 - aMouse[1]) / 3;
    else if (aMouse[2] == 1) nTrack = (13 - aMouse[0]) / 3;
    SetScrollRange(GetDlgItem(hDlg, IDC_TRACK), SB_CTL, 0, 6, FALSE);
    SetScrollPos(GetDlgItem(hDlg, IDC_TRACK), SB_CTL, nTrack, TRUE);
}

/* ------------------------------------------------------------------ seg16:02F0 / seg16:0316 */
static void InvertButton(HDC hdc, WORD bit)
{
    InvertRect(hdc, (bit & 1) ? &rcLeft : &rcRight);
    wButtons ^= bit;
}

/* light the boxes of the buttons that are down, put out the others */
static void ShowButtons(HDC hdc)
{
    UpdateWindow(hwndL);
    UpdateWindow(hwndR);
    WORD now = ButtonsDown(GetKeyState);
    if ((wButtons ^ now) & 1) InvertButton(hdc, 1);
    if ((wButtons ^ now) & 2) InvertButton(hdc, 2);
}

static void ShowButtonsNow(HWND hDlg)
{
    HDC hdc = GetDC(hDlg);
    ShowButtons(hdc);
    ReleaseDC(hDlg, hdc);
    ValidateRect(hDlg, &rcLeft);
    ValidateRect(hDlg, &rcRight);
}

/* ------------------------------------------------------------------ seg16:037D */
BOOL MouseDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_MOUSEACTIVATE:
        InvalidateRect(hDlg, &rcLeft, TRUE);
        InvalidateRect(hDlg, &rcRight, TRUE);
        UpdateWindow(hDlg);
        wButtons = ButtonsDown(GetKeyState);
        return TRUE;

    case WM_PAINT: {
        RECT rcUpdate, rc;
        PAINTSTRUCT ps;
        GetUpdateRect(hDlg, &rcUpdate, FALSE);
        if (IntersectRect(&rc, &rcUpdate, &rcLeft)) { InvalidateRect(hDlg, &rcLeft, TRUE); wButtons = 0; }
        if (IntersectRect(&rc, &rcUpdate, &rcRight)) { InvalidateRect(hDlg, &rcRight, TRUE); wButtons = 0; }
        if (IntersectRect(&rc, &rcUpdate, &rcTest)) InvalidateRect(hDlg, &rcTest, TRUE);
        HDC hdc = BeginPaint(hDlg, &ps);
        ShowButtons(hdc);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_INITDIALOG:
        InitMouseDlg(hDlg);
        fTrailsWas = IsDlgButtonChecked(hDlg, IDC_TRAILS);
        return TRUE;

    case WM_HSCROLL: {
        HWND hScroll = W16_CMD_HWND(HIWORD(lParam));
        if (hScroll == hwndDblClk) {
            switch (wParam) {
            case SB_LINEUP: nDblClk += 16; break;
            case SB_LINEDOWN: nDblClk -= 16; break;
            case SB_PAGEUP: nDblClk += 160; break;
            case SB_PAGEDOWN: nDblClk -= 160; break;
            case SB_THUMBPOSITION: nDblClk = 1000 - (int)LOWORD(lParam); break;
            case SB_TOP: nDblClk = 100; break;
            case SB_BOTTOM: nDblClk = 900; break;
            case SB_THUMBTRACK:
            case SB_ENDSCROLL: return TRUE;
            }
            if (nDblClk > 900) nDblClk = 900;
            else if (nDblClk < 100) nDblClk = 100;
            SetScrollPos(hScroll, SB_CTL, 1000 - nDblClk, TRUE);
            SetDoubleClickTime(nDblClk);
            return TRUE;
        }
        switch (wParam) {
        case SB_LINEUP: nTrack--; break;
        case SB_LINEDOWN: nTrack++; break;
        case SB_PAGEUP: nTrack -= 3; break;
        case SB_PAGEDOWN: nTrack += 3; break;
        case SB_THUMBPOSITION: nTrack = (int)LOWORD(lParam); break;
        case SB_TOP: nTrack = 6; break;
        case SB_BOTTOM: nTrack = 0; break;
        case SB_THUMBTRACK:
        case SB_ENDSCROLL: return TRUE;
        }
        if (nTrack > 6) nTrack = 6;
        else if (nTrack < 0) nTrack = 0;
        SetScrollPos(hScroll, SB_CTL, nTrack, TRUE);
        /* 0: no acceleration; 1-3: doubling past threshold 13-3n; 4-6: doubling past 4 and
         * quadrupling past 3*(8-n) */
        if (nTrack == 0) {
            aMouse[0] = aMouse[1] = aMouse[2] = 0;
        } else if (nTrack < 4) {
            aMouse[2] = 1;
            aMouse[0] = 13 - 3 * nTrack;
            aMouse[1] = 0;
        } else {
            aMouse[2] = 2;
            aMouse[0] = 4;
            aMouse[1] = (8 - nTrack) * 3;
        }
        SystemParametersInfo(SPI_SETMOUSE, 0, aMouse, 0);
        return TRUE;
    }

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK: {
        /* a double click on TEST flips it */
        POINT pt = {(SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam)};
        if (PtInRect(&rcTest, pt)) {
            HDC hdc = GetDC(hDlg);
            InvertRect(hdc, &rcTest);
            ReleaseDC(hDlg, hdc);
            ValidateRect(hDlg, &rcTest);
        }
        ShowButtonsNow(hDlg);
        return TRUE;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        ShowButtonsNow(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            HourGlass(TRUE);
            if (fSwapWas != fSwap) WriteProfileString("windows", "SwapMouseButtons", fSwap ? "yes" : "no");
            if (IsWindowEnabled(GetDlgItem(hDlg, IDC_TRAILS))) SetTrails(IsDlgButtonChecked(hDlg, IDC_TRAILS));
            if (nDblClkWas != nDblClk) {
                char t[8];
                wsprintf(t, "%d", nDblClk);
                WriteProfileString("windows", "DoubleClickSpeed", t);
            }
            SystemParametersInfo(SPI_SETMOUSE, 0, aMouse, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
            HourGlass(FALSE);
            EndDialog(hDlg, 0);
            break;
        case IDCANCEL:
            if (IsWindowEnabled(GetDlgItem(hDlg, IDC_TRAILS))) SetTrails(fTrailsWas);
            SetDoubleClickTime(nDblClkWas);
            SwapMouseButton(fSwapWas);
            SystemParametersInfo(SPI_SETMOUSE, 0, aMouseWas, 0);
            EndDialog(hDlg, 0);
            break;
        case IDD_HELP:
            CPHelp(hDlg);
            break;
        case IDC_SWAP:
            fSwap = !IsDlgButtonChecked(hDlg, IDC_SWAP);
            CheckDlgButton(hDlg, IDC_SWAP, fSwap);
            SwapMouseButton(fSwap);
            SwapTexts(hDlg);
            InvalidateRect(hDlg, &rcLeft, TRUE);
            InvalidateRect(hDlg, &rcRight, TRUE);
            UpdateWindow(hDlg);
            wButtons = ButtonsDown(GetAsyncKeyState);
            break;
        case IDC_TRAILS:
            SetTrails(IsDlgButtonChecked(hDlg, IDC_TRAILS));
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

/* ------------------------------------------------------------------ seg3:097F */
void MouseRun(HWND hwnd)
{
    /* MAIN.CPL first offers the double click to a loaded MOUSE driver's own CplApplet (seg3:08C6);
     * there are no 16-bit mouse drivers here */
    if (!GetSystemMetrics(SM_MOUSEPRESENT)) {
        MyMessageBox(hwnd, 324, 1, MB_ICONINFORMATION);
        return;
    }
    DoDialogBoxParam(6, hwnd, MouseDlgProc, 5006, 0);
}
