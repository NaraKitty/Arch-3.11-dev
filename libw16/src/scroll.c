/* scroll bars: standard (WS_VSCROLL/WS_HSCROLL) and the SCROLLBAR control.
 * WM_VSCROLL/WM_HSCROLL: wParam = code, LOWORD(lParam) = pos, HIWORD(lParam) = control slot
 * (W16_CMD_HWND(MAKELPARAM(HIWORD(lp),0)) gives the control; 0 for window scroll bars). */
#include "w16int.h"

WORD w16_cmd_slot(HWND h);

typedef struct { W16Scroll s; int pressed; } SbCtl;

static W16Scroll *bar_of(HWND h, int bar)
{
    if (bar == SB_CTL) {
        if (!h->ctl) h->ctl = calloc(1, sizeof(SbCtl));
        return &((SbCtl *)h->ctl)->s;
    }
    return &h->sb[bar == SB_VERT ? 1 : 0];
}

/* W16Window.sb[] is indexed [0]=horz [1]=vert; nc.c uses h->sb[bar] with SB_HORZ=0/SB_VERT=1 */

/* USER seg18:06F4: the focus caret follows the thumb, 2 px in */
static void place_focus_caret(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    int vert = (h->style & SBS_VERT) != 0;
    int tp = w16_sb_thumb(&r, vert, bar_of(h, SB_CTL));
    if (tp < 0) tp = 0;
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    if (vert) SetCaretPos(2 * cxb, tp + 2 * cyb);
    else SetCaretPos(tp + 2 * cxb, 2 * cyb);
}

static void redraw_bar(HWND h, int bar)
{
    if (!w16_window_visible(h)) return;
    if (bar == SB_CTL) { InvalidateRect(h, NULL, FALSE); UpdateWindow(h); return; }
    if (!(h->style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL))) return;
    HDC dc = GetWindowDC(h);
    w16_draw_sb(h, dc, bar, 0);
    ReleaseDC(h, dc);
}

int SetScrollPos(HWND h, int bar, int pos, BOOL redraw)
{
    if (!w16_valid(h)) return 0;
    W16Scroll *s = bar_of(h, bar);
    int old = s->pos;
    if (pos < s->min) pos = s->min;
    if (pos > s->max) pos = s->max;
    s->pos = pos;
    if (redraw && old != pos) redraw_bar(h, bar);
    return old;
}
int GetScrollPos(HWND h, int bar) { return w16_valid(h) ? bar_of(h, bar)->pos : 0; }

void SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw)
{
    if (!w16_valid(h)) return;
    W16Scroll *s = bar_of(h, bar);
    s->min = mn;
    s->max = mx;
    if (s->pos < mn) s->pos = mn;
    if (s->pos > mx) s->pos = mx;
    if (bar != SB_CTL) {
        /* a zero range hides a standard scroll bar */
        DWORD flag = bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL;
        int show = mx > mn;
        if (show != ((h->style & flag) != 0)) {
            if (show) h->style |= flag; else h->style &= ~flag;
            RECT pr = {0, 0, 0, 0};
            if (h->parent && h->parent != w16_desktop) pr = h->parent->rc;
            SetWindowPos(h, NULL, h->rw.left - pr.left, h->rw.top - pr.top, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            RECT rc;
            w16_nc_calc(h, &h->rw, &rc);
            h->rc = rc;
            w16_invalidate_window(h, NULL, 1, 1);
            SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
            return;
        }
    }
    if (redraw) redraw_bar(h, bar);
}
void GetScrollRange(HWND h, int bar, int *mn, int *mx)
{
    if (!w16_valid(h)) { *mn = *mx = 0; return; }
    W16Scroll *s = bar_of(h, bar);
    *mn = s->min;
    *mx = s->max;
}
void ShowScrollBar(HWND h, int bar, BOOL show)
{
    if (!w16_valid(h)) return;
    if (bar == SB_CTL) { ShowWindow(h, show ? SW_SHOW : SW_HIDE); return; }
    DWORD f = bar == SB_BOTH ? (WS_VSCROLL | WS_HSCROLL) : bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL;
    DWORD ns = show ? (h->style | f) : (h->style & ~f);
    if (ns == h->style) return;
    h->style = ns;
    RECT rc;
    w16_nc_calc(h, &h->rw, &rc);
    h->rc = rc;
    w16_invalidate_window(h, NULL, 1, 1);
    SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
}
BOOL EnableScrollBar(HWND h, int bar, UINT flags)
{
    if (!w16_valid(h)) return FALSE;
    if (bar == SB_BOTH) { EnableScrollBar(h, SB_HORZ, flags); return EnableScrollBar(h, SB_VERT, flags); }
    bar_of(h, bar)->disabled = flags;
    redraw_bar(h, bar);
    return TRUE;
}

/* ------------------------------------------------------------------ tracking */
static void notify(HWND notifywin, HWND ctl, int vert, int code, int pos)
{
    WORD slot = ctl ? w16_cmd_slot(ctl) : 0;
    SendMessage(notifywin, vert ? WM_VSCROLL : WM_HSCROLL, code, MAKELPARAM(pos, slot));
}

void w16_track_sb(HWND h, HWND notifywin, int bar, int x, int y, int ctl)
{
    int vert;
    RECT r;
    W16Scroll *s;
    HDC dc;
    if (ctl) {
        vert = (h->style & SBS_VERT) != 0;
        GetClientRect(h, &r);
        OffsetRect(&r, h->rc.left, h->rc.top);
        s = bar_of(h, SB_CTL);
        dc = GetDC(h);
        notifywin = h->parent;
    } else {
        vert = bar == SB_VERT;
        w16_get_sb_rect(h, bar, &r);
        OffsetRect(&r, h->rw.left, h->rw.top);
        s = bar_of(h, bar);
        dc = GetWindowDC(h);
    }
    RECT local = r;
    OffsetRect(&local, -(ctl ? h->rc.left : h->rw.left), -(ctl ? h->rc.top : h->rw.top));
    RECT part;
    int code = w16_sb_hit(&r, vert, s, x, y, &part);
    if (code == 0) { ReleaseDC(h, dc); return; }
    HWND nctl = ctl ? h : NULL;
    int sb_code = code == 1 ? SB_LINEUP : code == 2 ? SB_PAGEUP : code == 4 ? SB_PAGEDOWN : SB_LINEDOWN;
    SetCapture(h);
    MSG m;
    if (code == 3) {
        /* thumb drag: the thumb follows the mouse, app gets SB_THUMBTRACK, then SB_THUMBPOSITION */
        int len = vert ? r.bottom - r.top : r.right - r.left;
        int a = vert ? GetSystemMetrics(SM_CYVSCROLL) : GetSystemMetrics(SM_CXHSCROLL);
        int thumb = vert ? GetSystemMetrics(SM_CYVTHUMB) : GetSystemMetrics(SM_CXHTHUMB);
        int track = len - 2 * a + 2 - thumb;
        int grab = (vert ? y - part.top : x - part.left);
        W16Scroll ts = *s;
        int lastpos = s->pos;
        for (;;) {
            w16_present();
            if (!GetMessage(&m, NULL, 0, 0)) { PostQuitMessage(m.wParam); break; }
            if (m.message == WM_MOUSEMOVE || m.message == WM_LBUTTONUP) {
                int p = (vert ? m.pt.y - r.top : m.pt.x - r.left) - grab - (a - 1);
                int range = s->max - s->min;
                int np = track > 0 ? s->min + (int)(((long)p * range + track / 2) / track) : s->min;
                if (np < s->min) np = s->min;
                if (np > s->max) np = s->max;
                /* 3.1 snaps back if the mouse leaves the bar too far */
                int far = vert ? (m.pt.x < r.left - 30 || m.pt.x > r.right + 30) : (m.pt.y < r.top - 30 || m.pt.y > r.bottom + 30);
                if (far) np = s->pos;
                if (np != ts.pos || m.message == WM_LBUTTONUP) {
                    ts.pos = np;
                    w16_draw_sb_ctl(dc, &local, vert, &ts, 0, 1);
                    if (np != lastpos) notify(notifywin, nctl, vert, SB_THUMBTRACK, np);
                    lastpos = np;
                }
                if (m.message == WM_LBUTTONUP) {
                    notify(notifywin, nctl, vert, SB_THUMBPOSITION, ts.pos);
                    break;
                }
            } else if (m.message == WM_PAINT || m.message == WM_TIMER) DispatchMessage(&m);
        }
    } else {
        w16_draw_sb_ctl(dc, &local, vert, s, code, 1);
        notify(notifywin, nctl, vert, sb_code, s->pos);
        DWORD next = GetTickCount() + 400;
        int inside = 1;
        for (;;) {
            w16_present();
            MSG mm;
            if (PeekMessage(&mm, NULL, 0, 0, PM_REMOVE)) {
                if (mm.message == WM_LBUTTONUP) break;
                if (mm.message == WM_MOUSEMOVE) {
                    RECT p2;
                    int c2 = w16_sb_hit(&r, vert, s, mm.pt.x, mm.pt.y, &p2);
                    inside = c2 == code && PtInRect(&r, mm.pt);
                    w16_draw_sb_ctl(dc, &local, vert, s, inside ? code : 0, 1);
                } else if (mm.message == WM_PAINT) DispatchMessage(&mm);
                continue;
            }
            if ((int)(GetTickCount() - next) >= 0) {
                next = GetTickCount() + 50;
                if (inside) {
                    /* page repeats stop when the thumb reaches the mouse */
                    if (code == 2 || code == 4) {
                        RECT p2;
                        int c2 = w16_sb_hit(&r, vert, s, w16_mouse.x, w16_mouse.y, &p2);
                        if (c2 != code) continue;
                    }
                    notify(notifywin, nctl, vert, sb_code, s->pos);
                    w16_draw_sb_ctl(dc, &local, vert, s, code, 1);
                }
            }
            w16_pump(10);
        }
        w16_draw_sb_ctl(dc, &local, vert, s, 0, 1);
    }
    ReleaseCapture();
    notify(notifywin, nctl, vert, SB_ENDSCROLL, s->pos);
    ReleaseDC(h, dc);
}

/* ------------------------------------------------------------------ SCROLLBAR control */
LRESULT w16_scrollbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        W16Scroll *s = bar_of(h, SB_CTL);
        s->min = 0; s->max = 0; s->pos = 0;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        if (h->style & SBS_SIZEBOX) FillRect(dc, &r, w16_sys_brush(COLOR_SCROLLBAR));
        else w16_draw_sb_ctl(dc, &r, (h->style & SBS_VERT) != 0, bar_of(h, SB_CTL), 0, !(h->style & WS_DISABLED));
        EndPaint(h, &ps);
        if (GetFocus() == h) place_focus_caret(h);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p = {(SHORT)LOWORD(lp), (SHORT)HIWORD(lp)};
        ClientToScreen(h, &p);
        if (h->style & WS_TABSTOP) SetFocus(h);
        w16_track_sb(h, h->parent, SB_CTL, p.x, p.y, 1);
        return 0;
    }
    case WM_KEYDOWN: {
        int vert = (h->style & SBS_VERT) != 0, code = -1;
        switch (wp) {
        case VK_UP: case VK_LEFT: code = SB_LINEUP; break;
        case VK_DOWN: case VK_RIGHT: code = SB_LINEDOWN; break;
        case VK_PRIOR: code = SB_PAGEUP; break;
        case VK_NEXT: code = SB_PAGEDOWN; break;
        case VK_HOME: code = SB_TOP; break;
        case VK_END: code = SB_BOTTOM; break;
        }
        if (code >= 0) notify(h->parent, h, vert, code, bar_of(h, SB_CTL)->pos);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_ENABLE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_SETFOCUS: {
        /* USER seg18:0A48: a blinking gray caret over the thumb, 2 px inside it */
        RECT r;
        GetClientRect(h, &r);
        int vert = (h->style & SBS_VERT) != 0;
        int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
        int w = (vert ? r.right - r.left : GetSystemMetrics(SM_CXHTHUMB)) - 4 * cxb;
        int ht = (vert ? GetSystemMetrics(SM_CYVTHUMB) : r.bottom - r.top) - 4 * cyb;
        CreateCaret(h, (HBITMAP)1, w, ht);
        place_focus_caret(h);
        ShowCaret(h);
        return 0;
    }
    case WM_KILLFOCUS:
        DestroyCaret();
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(h, m, wp, lp);
}
