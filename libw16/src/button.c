/* BUTTON and STATIC controls, drawn the way 3.1 USER draws them (measured against 3.11 VGA) */
#include "w16int.h"

typedef struct { int check; int state; int pressed; int tracking; } Btn;
#define BST_FOCUS 0x0008
#define BST_PUSHED 0x0004

static Btn *btn(HWND h) { if (!h->ctl) h->ctl = calloc(1, sizeof(Btn)); return h->ctl; }
static int btype(HWND h) { return h->style & 0x0F; }
static int is_push(HWND h) { int t = btype(h); return t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON; }

HBRUSH w16_ctl_color(HWND ctl, HDC dc, int type)
{
    HWND parent = ctl->parent && ctl->parent != w16_desktop ? ctl->parent : ctl;
    HBRUSH b = (HBRUSH)SendMessage(parent, WM_CTLCOLOR, (WPARAM)dc, MAKELPARAM(0, type));
    if (!b) b = (HBRUSH)DefWindowProc(parent, WM_CTLCOLOR, (WPARAM)dc, MAKELPARAM(0, type));
    return b;
}

static void px_fill(HDC dc, int l, int t, int r, int b, int color)
{
    RECT rr = {l, t, r, b};
    FillRect(dc, &rr, w16_sys_brush(color));
}

/* the 2-pixel 3D bevel inside a push button; (L,T,R,B) = area inside the black border */
static void bevel(HDC dc, int L, int T, int R, int B, int pressed)
{
    if (pressed) {
        /* pushed: one-pixel shadow along the top and left edges, flat face */
        px_fill(dc, L, T, R, T + 1, COLOR_BTNSHADOW);
        px_fill(dc, L, T, L + 1, B, COLOR_BTNSHADOW);
        return;
    }
    px_fill(dc, L, T, R - 1, T + 1, COLOR_BTNHIGHLIGHT);
    px_fill(dc, R - 1, T, R, T + 1, COLOR_BTNSHADOW);
    px_fill(dc, L, T + 1, R - 2, T + 2, COLOR_BTNHIGHLIGHT);
    px_fill(dc, R - 2, T + 1, R, T + 2, COLOR_BTNSHADOW);
    px_fill(dc, L, T + 2, L + 2, B - 2, COLOR_BTNHIGHLIGHT);
    px_fill(dc, R - 2, T + 2, R, B - 2, COLOR_BTNSHADOW);
    px_fill(dc, L, B - 2, L + 1, B - 1, COLOR_BTNHIGHLIGHT);
    px_fill(dc, L + 1, B - 2, R, B - 1, COLOR_BTNSHADOW);
    px_fill(dc, L, B - 1, R, B, COLOR_BTNSHADOW);
}

static HFONT ctl_font(HWND h) { return h->font ? h->font : GetStockObject(SYSTEM_FONT); }

/* ------------------------------------------------------------------ USER BNDrawText geometry
 * seg25:102E maps the style to a text layout, seg25:1097 makes the text rectangle, seg25:1366
 * centres the text (vertically on tmAscent, not tmHeight) and derives the focus rectangle. */
enum { BT_CHECK = 2, BT_GROUP = 3, BT_PUSH = 5 };
/* USER [0x974]/[0x976]: the OBM_CHECKBOXES cell, bitmap width / 4 (14 with the gap between the boxes)
 * and height / 3 (13) */
static int checkbox_cx(void) { W16Bitmap *b = w16_obm(OBM_CHECKBOXES); return b ? b->w / 4 : 14; }
static int checkbox_cy(void) { W16Bitmap *b = w16_obm(OBM_CHECKBOXES); return b ? b->h / 3 : 13; }

typedef struct { RECT rc; int tx, ty, tw, th; } TextLayout;

static void text_layout(HWND h, HDC dc, int type, const char *text, int n, TextLayout *t)
{
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    TEXTMETRIC tm;
    GetTextMetrics(dc, &tm);
    t->tw = n ? w16_prefix_text_width(dc, text, n) : 0;
    t->th = tm.tmHeight;
    GetClientRect(h, &t->rc);
    switch (type) {
    case BT_CHECK:
        t->rc.left += (h->style & BS_LEFTTEXT) ? cxb : checkbox_cx() + 4;
        break;
    case BT_GROUP:
        if (!n) { SetRectEmpty(&t->rc); break; }
        t->rc.left += LOWORD(GetDialogBaseUnits()) - cxb;   /* system font width, [0x522] */
        t->rc.right = t->rc.left + t->tw + 4;
        t->rc.bottom = t->rc.top + t->th + 4;
        break;
    case BT_PUSH:
        t->rc.right -= 2 * cxb;
        t->rc.bottom -= 2 * cyb;
        break;
    }
    t->tx = t->rc.left;
    if (type != BT_CHECK) t->tx += (t->rc.right - t->tw - t->rc.left) / 2;
    t->ty = t->rc.top + (t->rc.bottom - t->rc.top - tm.tmAscent) / 2;
}

/* Disabled button text (seg25:1488): when GRAYTEXT is black or the same as BTNFACE (VGA) USER
 * grays it with GrayString - push buttons with the BTNTEXT brush, the other types with no brush,
 * which GrayString's BltColor (seg1:8BFA) replaces by the WINDOWTEXT brush; otherwise the text is
 * drawn in GRAYTEXT. */
static void disabled_text(HDC dc, int x, int y, const char *s, int n, int push)
{
    COLORREF g = GetSysColor(COLOR_GRAYTEXT);
    if (g == 0 || g == GetSysColor(COLOR_BTNFACE))
        w16_draw_stippled_text(dc, x, y, s, n, 0, GetSysColor(push ? COLOR_BTNTEXT : COLOR_WINDOWTEXT));
    else w16_draw_gray_text(dc, x, y, s, n, 0);
}

/* the dotted focus rectangle around the text (seg25:15D3) */
static void draw_focus(HWND h, HDC dc, const TextLayout *t, int push, int pressed)
{
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    RECT c, f;
    GetClientRect(h, &c);
    f.top = t->ty - cyb;
    if (f.top < 0) f.top = 0;
    f.bottom = f.top + t->th + 3 * cyb;
    if (f.bottom > c.bottom) f.bottom = c.bottom;
    if (push) {
        /* kept off the bevel: 3 border units down on displays over 300 lines (USER [0xea]) */
        int mt = (w16_screen.h > 300 ? 3 : 2) * cyb;
        if (f.top < mt) f.top = mt;
        if (f.bottom > c.bottom - 4 * cyb) f.bottom = c.bottom - 4 * cyb;
    }
    f.left = t->tx - 2 * cxb;
    if (f.left < 0) f.left = 0;
    f.right = f.left + t->tw + 4 * cxb;
    if (f.right > c.right) f.right = c.right;
    if (pressed) OffsetRect(&f, 1, 1);
    DrawFocusRect(dc, &f);
}

static void paint_push(HWND h, HDC dc)
{
    Btn *b = btn(h);
    RECT r;
    GetClientRect(h, &r);
    HBRUSH bg = w16_ctl_color(h, dc, CTLCOLOR_BTN);
    int l = r.left, t = r.top, rt = r.right, bt = r.bottom;
    /* USER seg25:18BC: a disabled default push button is drawn as a plain one */
    int def = btype(h) == BS_DEFPUSHBUTTON && !(h->style & WS_DISABLED);
    int pressed = (b->state & BST_PUSHED) != 0;
    /* corners show the parent's background */
    RECT c;
    SetRect(&c, l, t, l + 1, t + 1); FillRect(dc, &c, bg);
    SetRect(&c, rt - 1, t, rt, t + 1); FillRect(dc, &c, bg);
    SetRect(&c, l, bt - 1, l + 1, bt); FillRect(dc, &c, bg);
    SetRect(&c, rt - 1, bt - 1, rt, bt); FillRect(dc, &c, bg);
    px_fill(dc, l + 1, t, rt - 1, t + 1, COLOR_WINDOWFRAME);
    px_fill(dc, l + 1, bt - 1, rt - 1, bt, COLOR_WINDOWFRAME);
    px_fill(dc, l, t + 1, l + 1, bt - 1, COLOR_WINDOWFRAME);
    px_fill(dc, rt - 1, t + 1, rt, bt - 1, COLOR_WINDOWFRAME);
    int in = 1;
    if (def) {
        RECT f = {l + 1, t + 1, rt - 1, bt - 1};
        FrameRect(dc, &f, w16_sys_brush(COLOR_WINDOWFRAME));
        in = 2;
    }
    int L = l + in, T = t + in, R = rt - in, B = bt - in;
    px_fill(dc, L, T, R, B, COLOR_BTNFACE);
    bevel(dc, L, T, R, B, pressed);
    /* text */
    HGDIOBJ of = SelectObject(dc, ctl_font(h));
    SetBkMode(dc, TRANSPARENT);
    int n = strlen(h->text);
    TextLayout tl;
    text_layout(h, dc, BT_PUSH, h->text, n, &tl);
    int x = tl.tx, y = tl.ty;
    if (pressed) { x++; y++; }
    if (h->style & WS_DISABLED) disabled_text(dc, x, y, h->text, n, 1);
    else {
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        w16_draw_prefix_text(dc, x, y, h->text, n, 0);
    }
    if (b->state & BST_FOCUS) draw_focus(h, dc, &tl, 1, pressed);
    SelectObject(dc, of);
}

static void paint_check(HWND h, HDC dc)
{
    Btn *b = btn(h);
    RECT r;
    GetClientRect(h, &r);
    HBRUSH bg = w16_ctl_color(h, dc, CTLCOLOR_BTN);
    FillRect(dc, &r, bg);
    int t = btype(h);
    int radio = t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON;
    int three = t == BS_3STATE || t == BS_AUTO3STATE;
    W16Bitmap *bm = w16_obm(OBM_CHECKBOXES);
    HGDIOBJ of = SelectObject(dc, ctl_font(h));
    TEXTMETRIC tm;
    GetTextMetrics(dc, &tm);
    int left = (h->style & BS_LEFTTEXT) != 0;
    int by = (r.bottom - r.top - checkbox_cy()) / 2;
    int bx = left ? r.right - checkbox_cx() : 0;
    if (bm) {
        int col = (b->check ? 1 : 0) + ((b->state & BST_PUSHED) ? 2 : 0);
        int row = radio ? 1 : 0;
        if (three && b->check == 2) { row = 2; col = (b->state & BST_PUSHED) ? 3 : 1; }
        int dx = bx, dy = by;
        w16_lp_to_dp(dc, &dx, &dy);
        /* mono bitmap: black pixels in text colour, white pixels in the background colour */
        COLORREF bk = GetBkColor(dc);
        (void)bk;
        Region e;
        w16_dc_clip_iter_begin(dc, &e);
        for (int yy = 0; yy < 13; yy++)
            for (int xx = 0; xx < 13; xx++) {
                uint32_t p = bm->px[(row * 13 + yy) * bm->w + col * 14 + xx];
                int X = dx + xx, Y = dy + yy;
                if (!rgn_contains(&e, X, Y)) continue;
                if (!p) w16_screen.px[Y * w16_screen.w + X] = w16_rgb(GetSysColor(COLOR_WINDOWFRAME));
                else if (!radio) w16_screen.px[Y * w16_screen.w + X] = w16_rgb(GetSysColor(COLOR_WINDOW));
                else {
                    /* radio interior is the window colour, outside the circle stays background */
                    int cx = xx - 6, cy = yy - 6;
                    if (cx * cx + cy * cy <= 30) w16_screen.px[Y * w16_screen.w + X] = w16_rgb(GetSysColor(COLOR_WINDOW));
                }
            }
        rgn_free(&e);
        w16_screen_dirty = 1;
    }
    int n = strlen(h->text);
    TextLayout tl;
    text_layout(h, dc, BT_CHECK, h->text, n, &tl);
    SetBkMode(dc, TRANSPARENT);
    if (h->style & WS_DISABLED) disabled_text(dc, tl.tx, tl.ty, h->text, n, 0);
    else {
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        w16_draw_prefix_text(dc, tl.tx, tl.ty, h->text, n, 0);
    }
    if (b->state & BST_FOCUS) draw_focus(h, dc, &tl, 0, 0);
    SelectObject(dc, of);
}

static void paint_group(HWND h, HDC dc)
{
    RECT r;
    GetClientRect(h, &r);
    HGDIOBJ of = SelectObject(dc, ctl_font(h));
    TEXTMETRIC tm;
    GetTextMetrics(dc, &tm);
    int y = tm.tmHeight / 2;
    w16_ctl_color(h, dc, CTLCOLOR_BTN);
    RECT f = {r.left, r.top + y, r.right, r.bottom};
    FrameRect(dc, &f, w16_sys_brush(COLOR_WINDOWFRAME));
    int n = strlen(h->text);
    if (n) {
        /* the title's text rectangle (text extent + 4 wide and high) breaks the frame line */
        TextLayout tl;
        text_layout(h, dc, BT_GROUP, h->text, n, &tl);
        FillRect(dc, &tl.rc, w16_ctl_color(h, dc, CTLCOLOR_BTN));
        SetBkMode(dc, TRANSPARENT);
        if (h->style & WS_DISABLED) disabled_text(dc, tl.tx, tl.ty, h->text, n, 0);
        else { SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT)); w16_draw_prefix_text(dc, tl.tx, tl.ty, h->text, n, 0); }
    }
    SelectObject(dc, of);
}

/* BS_OWNERDRAW: the parent draws, told what changed - ODA_DRAWENTIRE on paint, ODA_FOCUS when the
 * focus comes or goes, ODA_SELECT when the button is pressed or released (as USER does) */
static void owner_draw(HWND h, HDC dc, UINT action)
{
    DRAWITEMSTRUCT di = {ODT_BUTTON, h->id, 0, action,
                         (btn(h)->state & BST_PUSHED ? ODS_SELECTED : 0) | (btn(h)->state & BST_FOCUS ? ODS_FOCUS : 0) |
                             (h->style & WS_DISABLED ? ODS_DISABLED : 0),
                         h, dc, {0, 0, 0, 0}, 0};
    GetClientRect(h, &di.rcItem);
    SendMessage(h->parent, WM_DRAWITEM, h->id, (LPARAM)&di);
}

static void paint(HWND h, HDC dc)
{
    switch (btype(h)) {
    case BS_PUSHBUTTON: case BS_DEFPUSHBUTTON: paint_push(h, dc); break;
    case BS_GROUPBOX: paint_group(h, dc); break;
    case BS_OWNERDRAW: owner_draw(h, dc, ODA_DRAWENTIRE); break;
    case BS_USERBUTTON: SendMessage(h->parent, WM_COMMAND, h->id, MAKELPARAM(0, BN_PAINT)); break;
    default: paint_check(h, dc); break;
    }
}

static void redraw(HWND h)
{
    if (!w16_window_visible(h)) return;
    HDC dc = GetDC(h);
    paint(h, dc);
    ReleaseDC(h, dc);
}

/* the focus or pushed state changed: an owner-drawn button is asked for just that part */
static void redraw_part(HWND h, UINT action)
{
    if (btype(h) != BS_OWNERDRAW) { redraw(h); return; }
    if (!w16_window_visible(h)) return;
    HDC dc = GetDC(h);
    owner_draw(h, dc, action);
    ReleaseDC(h, dc);
}

static void click(HWND h)
{
    Btn *b = btn(h);
    switch (btype(h)) {
    case BS_AUTOCHECKBOX: b->check = !b->check; break;
    case BS_AUTO3STATE: b->check = (b->check + 1) % 3; break;
    case BS_AUTORADIOBUTTON:
        /* uncheck the others in the group */
        if (h->parent) {
            HWND start = h;
            for (HWND p = h->parent->child; p; p = p->next) {
                if (p->style & WS_GROUP) start = p;
                if (p == h) break;
            }
            for (HWND p = start; p; p = p->next) {
                if (p != start && (p->style & WS_GROUP)) break;
                if (p != h && !strcasecmp(p->cls->name, "BUTTON") && btype(p) == BS_AUTORADIOBUTTON && btn(p)->check)
                    SendMessage(p, BM_SETCHECK, 0, 0);
            }
        }
        SendMessage(h, BM_SETCHECK, 1, 0);   /* (and the tab stop moves with the check) */
        break;
    }
    redraw(h);
    w16_notify_parent(h, BN_CLICKED);
}

LRESULT w16_button_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Btn *b = btn(h);
    switch (m) {
    case WM_CREATE: return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT: h->font = (HFONT)wp; if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_SETTEXT: DefWindowProc(h, m, wp, lp); InvalidateRect(h, NULL, TRUE); return TRUE;
    case WM_ENABLE: InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETDLGCODE: {
        int t = btype(h);
        if (t == BS_DEFPUSHBUTTON) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (t == BS_PUSHBUTTON) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        if (t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        if (t == BS_GROUPBOX) return DLGC_STATIC;
        return DLGC_BUTTON;
    }
    case BM_GETCHECK: return b->check;
    case BM_SETCHECK: {
        /* USER seg25:1E1B: a check box is checked by any non-zero value, a 3-state box takes up to 2,
         * and a radio button is a tab stop exactly while it is checked; other styles ignore it */
        int v;
        switch (btype(h)) {
        case BS_CHECKBOX: case BS_AUTOCHECKBOX: v = wp != 0; break;
        case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
            if (wp) h->style |= WS_TABSTOP; else h->style &= ~WS_TABSTOP;
            v = wp != 0;
            break;
        case BS_3STATE: case BS_AUTO3STATE: v = (UINT)wp > 2 ? 2 : (int)wp; break;
        default: return 0;
        }
        if (b->check != v) { b->check = v; redraw(h); }
        return 0;
    }
    case BM_GETSTATE: return b->state | b->check;
    case BM_SETSTATE:
        if (wp) b->state |= BST_PUSHED; else b->state &= ~BST_PUSHED;
        redraw_part(h, ODA_SELECT);
        return 0;
    case BM_SETSTYLE:
        h->style = (h->style & ~0x0F) | (wp & 0x0F);
        if (lp) redraw(h);
        return 0;
    case WM_SETFOCUS:
        b->state |= BST_FOCUS;
        redraw_part(h, ODA_FOCUS);
        return 0;
    case WM_KILLFOCUS:
        b->state &= ~BST_FOCUS;
        if (b->tracking) {
            b->tracking = 0;
            b->state &= ~BST_PUSHED;
            ReleaseCapture();
            redraw_part(h, ODA_SELECT);
        }
        redraw_part(h, ODA_FOCUS);
        return 0;
    case WM_NCHITTEST:
        /* a group box lets the mouse through to what lies under it */
        if (btype(h) == BS_GROUPBOX) return HTTRANSPARENT;
        return DefWindowProc(h, m, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (btype(h) == BS_GROUPBOX) return 0;
        if (m == WM_LBUTTONDBLCLK && (btype(h) == BS_RADIOBUTTON || btype(h) == BS_AUTORADIOBUTTON || btype(h) == BS_USERBUTTON || btype(h) == BS_OWNERDRAW)) {
            w16_notify_parent(h, BN_DOUBLECLICKED);
            return 0;
        }
        SetFocus(h);
        SetCapture(h);
        b->tracking = 1;
        b->state |= BST_PUSHED;
        redraw_part(h, ODA_SELECT);
        return 0;
    case WM_MOUSEMOVE:
        if (b->tracking) {
            RECT r;
            GetClientRect(h, &r);
            POINT p = {(SHORT)LOWORD(lp), (SHORT)HIWORD(lp)};
            int in = PtInRect(&r, p);
            if (in != ((b->state & BST_PUSHED) != 0)) {
                if (in) b->state |= BST_PUSHED; else b->state &= ~BST_PUSHED;
                redraw_part(h, ODA_SELECT);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (b->tracking) {
            b->tracking = 0;
            ReleaseCapture();
            int in = (b->state & BST_PUSHED) != 0;
            b->state &= ~BST_PUSHED;
            redraw_part(h, ODA_SELECT);
            if (in) click(h);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !b->tracking) {
            b->state |= BST_PUSHED;
            redraw_part(h, ODA_SELECT);
        }
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && (b->state & BST_PUSHED)) {
            b->state &= ~BST_PUSHED;
            redraw_part(h, ODA_SELECT);
            click(h);
        }
        return 0;
    case WM_CHAR:
        if (wp == '+' || wp == '=') { if (btype(h) == BS_AUTOCHECKBOX && !b->check) click(h); else if (btype(h) == BS_CHECKBOX) w16_notify_parent(h, BN_CLICKED); }
        else if (wp == '-') { if (btype(h) == BS_AUTOCHECKBOX && b->check) click(h); }
        return 0;
    }
    (void)is_push;
    return DefWindowProc(h, m, wp, lp);
}

/* ------------------------------------------------------------------ STATIC */
LRESULT w16_static_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    int t = h->style & 0x0F;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
        switch (t) {
        case SS_LEFT: case SS_CENTER: case SS_RIGHT: case SS_SIMPLE: case SS_LEFTNOWORDWRAP: {
            HBRUSH bg = w16_ctl_color(h, dc, CTLCOLOR_STATIC);
            if (t != SS_SIMPLE || 1) FillRect(dc, &r, bg);
            UINT fmt = t == SS_CENTER ? DT_CENTER | DT_WORDBREAK : t == SS_RIGHT ? DT_RIGHT | DT_WORDBREAK
                     : t == SS_LEFT ? DT_LEFT | DT_WORDBREAK : DT_LEFT | DT_SINGLELINE;
            fmt |= DT_EXPANDTABS;
            if (h->style & SS_NOPREFIX) fmt |= DT_NOPREFIX;
            SetBkMode(dc, TRANSPARENT);
            if (h->style & WS_DISABLED) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            DrawText(dc, h->text, -1, &r, fmt);
            break;
        }
        case SS_ICON: {
            HICON ic = (HICON)GetProp(h, "W16ICON");
            if (ic) DrawIcon(dc, 0, 0, ic);
            break;
        }
        case SS_BLACKRECT: FillRect(dc, &r, w16_sys_brush(COLOR_WINDOWFRAME)); break;
        case SS_GRAYRECT: FillRect(dc, &r, w16_sys_brush(COLOR_BACKGROUND)); break;
        case SS_WHITERECT: FillRect(dc, &r, w16_sys_brush(COLOR_WINDOW)); break;
        case SS_BLACKFRAME: FrameRect(dc, &r, w16_sys_brush(COLOR_WINDOWFRAME)); break;
        case SS_GRAYFRAME: FrameRect(dc, &r, w16_sys_brush(COLOR_BACKGROUND)); break;
        case SS_WHITEFRAME: FrameRect(dc, &r, w16_sys_brush(COLOR_WINDOW)); break;
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT: h->font = (HFONT)wp; if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_SETTEXT:
        DefWindowProc(h, m, wp, lp);
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
        return TRUE;
    case STM_SETICON: {
        HICON old = GetProp(h, "W16ICON");
        SetProp(h, "W16ICON", (HANDLE)wp);
        if (t == SS_ICON) {
            /* icon statics size themselves to the icon */
            RECT pr = h->parent->rc;
            SetWindowPos(h, NULL, h->rw.left - pr.left, h->rw.top - pr.top, 32, 32, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        }
        InvalidateRect(h, NULL, TRUE);
        return (LRESULT)old;
    }
    case STM_GETICON: return (LRESULT)GetProp(h, "W16ICON");
    case WM_GETDLGCODE: return DLGC_STATIC;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ENABLE: InvalidateRect(h, NULL, TRUE); return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}
