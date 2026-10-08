/* "cpArrow": MAIN.CPL's spin control (seg2), the up/down arrow pair beside the Date & Time fields and
 * the Desktop spacing values. A press inverts the half under the pointer and sends the parent
 * WM_VSCROLL with LOWORD(lParam) = the control's ID, repeating every 50 ms after 200 ms.
 * Left button: SB_LINEUP/SB_LINEDOWN, right: SB_PAGEUP/SB_PAGEDOWN; Shift+left sends SB_TOP/SB_BOTTOM
 * and Shift+right codes 4/5, each followed by SB_ENDSCROLL. */
#include "maincpl.h"
#include <string.h>

static UINT msgDown;                /* [0x1df6] button being tracked (WM_LBUTTONDOWN/WM_RBUTTONDOWN) */
static HWND hwndParent;             /* [0x1172] */
static RECT rcUp, rcDown;           /* [0x117a], [0x11b2] screen coordinates */
static RECT *prcInverted;           /* [0x13bc] half shown pressed */
static BOOL fTimer;                 /* [0x1ce] */

/* the arrow points in its 15 x 15 window (ds:01B6, ds:01C2) */
static const POINT ptUp[3] = {{7, 1}, {3, 5}, {11, 5}};
static const POINT ptDown[3] = {{7, 13}, {3, 9}, {11, 9}};

static LPARAM ScrollParam(HWND h)
{
    return MAKELPARAM(GetWindowWord(h, GWW_ID), LOWORD(W16_CMD_LPARAM(h, 0)));
}

static void Notify(HWND h, int code)
{
    SendMessage(hwndParent, WM_VSCROLL, code, ScrollParam(h));
}

/* ------------------------------------------------------------------ seg2:0000
 * gives the control an odd width so the arrows' points sit on a pixel; returns width % 2 */
int AdjustArrowWidth(HWND h)
{
    RECT rc;
    GetWindowRect(h, &rc);
    int odd = (rc.right - rc.left) % 2;
    if (!odd) {
        rc.right++;
        HWND parent = GetParent(h);
        ScreenToClient(parent, (POINT *)&rc.left);
        ScreenToClient(parent, (POINT *)&rc.right);
        MoveWindow(h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, FALSE);
    }
    return odd;
}

/* ------------------------------------------------------------------ seg2:006F: 0 up, 1 down, -1 neither */
static int HitArrow(void)
{
    DWORD pos = GetMessagePos();
    POINT pt = {(SHORT)LOWORD(pos), (SHORT)HIWORD(pos)};
    if (PtInRect(&rcUp, pt)) return 0;
    if (PtInRect(&rcDown, pt)) return 1;
    return -1;
}

/* ------------------------------------------------------------------ seg2:010A */
static void InvertHalf(HWND h, int part)
{
    prcInverted = part == 0 ? &rcUp : &rcDown;
    RECT rc = *prcInverted;
    HDC hdc = GetDC(h);
    ScreenToClient(h, (POINT *)&rc.left);
    ScreenToClient(h, (POINT *)&rc.right);
    InvertRect(hdc, &rc);
    ReleaseDC(h, hdc);
    ValidateRect(h, &rc);
}

/* ------------------------------------------------------------------ seg2:00B8: auto-repeat */
static void ArrowTimer(HWND h, UINT msg, UINT id, DWORD time)
{
    (void)msg; (void)time;
    int part = HitArrow();
    if (part != -1) {
        if (msgDown == WM_RBUTTONDOWN) part += 2;
        Notify(h, part);
    }
    SetTimer(h, id, 50, ArrowTimer);
}

static void Paint(HWND h)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC hdc = BeginPaint(h, &ps);
    GetClientRect(h, &rc);
    HBRUSH hbr = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    if (hbr) {
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }
    SetTextColor(hdc, GetSysColor(COLOR_WINDOWFRAME));
    SetMapMode(hdc, MM_ANISOTROPIC);
    SetViewportOrg(hdc, rc.left, rc.top);
    SetViewportExt(hdc, rc.right - rc.left, rc.bottom - rc.top);
    SetWindowOrg(hdc, 0, 0);
    SetWindowExt(hdc, 15, 15);
    /* the divider */
    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME)), old = NULL;
    if (pen) old = SelectObject(hdc, pen);
    MoveTo(hdc, 0, 7);
    LineTo(hdc, 15, 7);
    if (pen) {
        if (old) SelectObject(hdc, old);
        DeleteObject(pen);
    }
    /* the two arrows in the button-text colour */
    pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNTEXT));
    old = pen ? SelectObject(hdc, pen) : NULL;
    HBRUSH brush = CreateSolidBrush(GetNearestColor(hdc, GetSysColor(COLOR_BTNTEXT)));
    BOOL stock = !brush;
    if (stock) brush = GetStockObject(BLACK_BRUSH);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Polygon(hdc, ptUp, 3);
    Polygon(hdc, ptDown, 3);
    if (pen) {
        if (old) SelectObject(hdc, old);
        DeleteObject(pen);
    }
    if (oldBrush) SelectObject(hdc, oldBrush);
    if (!stock) DeleteObject(brush);
    EndPaint(h, &ps);
}

/* ------------------------------------------------------------------ seg2:01A5 */
static LRESULT CALLBACK ArrowWndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        Paint(h);
        return 0;

    case WM_MOUSEMOVE: {
        if (!msgDown) return 0;
        int was = prcInverted == &rcUp ? 0 : prcInverted == &rcDown ? 1 : -1;
        int now = HitArrow();
        if (now == 0) {
            if (was == 1) InvertHalf(h, 1);
            if (was != 0) InvertHalf(h, now);
        } else if (now == 1) {
            if (was == 0) InvertHalf(h, 0);
            if (was != 1) InvertHalf(h, now);
        } else if (prcInverted) {
            InvertHalf(h, was);
            prcInverted = NULL;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        /* (the class has no CS_DBLCLKS, so these never come; MAIN.CPL treats them like Shift) */
        Notify(h, HitArrow() + (msg == WM_RBUTTONDBLCLK ? 4 : 6));
        Notify(h, SB_ENDSCROLL);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (msgDown) return 0;
        msgDown = msg;
        SetCapture(h);
        hwndParent = GetParent(h);
        GetWindowRect(h, &rcUp);
        CopyRect(&rcDown, &rcUp);
        rcUp.bottom = (rcUp.bottom + rcUp.top) / 2;
        rcDown.top = rcUp.bottom + 1;
        int part = HitArrow();
        InvertHalf(h, part);
        if (wParam & MK_SHIFT) {
            /* to the end of the range, then done */
            Notify(h, HitArrow() + (msgDown == WM_RBUTTONDOWN ? 4 : 6));
            Notify(h, SB_ENDSCROLL);
            return 0;
        }
        if (msgDown == WM_RBUTTONDOWN) part += 2;
        Notify(h, part);
        SetTimer(h, GetWindowWord(h, GWW_ID), 200, ArrowTimer);
        fTimer = TRUE;
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if ((int)msgDown - (int)msg != -1) return 0;
        msgDown = 0;
        ReleaseCapture();
        if (prcInverted) InvertHalf(h, prcInverted == &rcUp ? 0 : 1);
        prcInverted = NULL;
        if (fTimer) {
            Notify(h, SB_ENDSCROLL);
            KillTimer(h, GetWindowWord(h, GWW_ID));
            ReleaseCapture();
            fTimer = FALSE;
        }
        return 0;
    }
    return DefWindowProc(h, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ seg2:060E */
BOOL RegisterArrowClass(HINSTANCE hInst)
{
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ArrowWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "cpArrow";
    return RegisterClass(&wc);
}
