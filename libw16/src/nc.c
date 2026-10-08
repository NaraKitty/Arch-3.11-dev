/* USER non-client area: frames, captions, caption buttons, scroll bars, hit testing,
 * move/size tracking, minimise/maximise. Geometry measured against real 3.11 (VGA). */
#include "w16int.h"

#define CXFRAME (w16_border_width + 1)

int w16_has_caption(DWORD style) { return (style & WS_CAPTION) == WS_CAPTION; }

static int is_top(HWND h) { return !h->parent || h->parent == w16_desktop || h->parent == (HWND)1; }

static int has_dlgframe(HWND h)
{
    if (h->style & WS_THICKFRAME) return 0;
    if (h->exstyle & WS_EX_DLGMODALFRAME) return 1;
    return (h->style & WS_DLGFRAME) && !(h->style & WS_BORDER);
}

/* frame insets (left, top, right, bottom) up to where the caption / client starts */
static void frame_insets(HWND h, int *l, int *t, int *r, int *b)
{
    *l = *t = *r = *b = 0;
    if (h->style & WS_MINIMIZE) return;
    if (h->style & WS_THICKFRAME) {
        *l = *t = *r = *b = CXFRAME;
    } else if (has_dlgframe(h)) {
        /* border + dialog frame; measured on MAIN.CPL's Keyboard dialog (3.11 places its controls
         * from the client origin this gives). A caption sits on a white line inside the top band. */
        *l = *t = *r = *b = GetSystemMetrics(SM_CXDLGFRAME) + 1;
    } else if (h->style & WS_BORDER) {
        *l = *t = *r = *b = 1;
    }
}

void w16_nc_calc(HWND h, const RECT *rw, RECT *rc)
{
    *rc = *rw;
    if (h->style & WS_MINIMIZE) return; /* an icon window is all client area */
    int l, t, r, b;
    frame_insets(h, &l, &t, &r, &b);
    rc->left += l; rc->top += t; rc->right -= r; rc->bottom -= b;
    if (w16_has_caption(h->style)) rc->top += GetSystemMetrics(SM_CYCAPTION) - 1;
    if (is_top(h) && h->menu && h->parent != (HWND)1 && !(h->style & WS_CHILD)) {
        if (h->rw.right > h->rw.left) rc->top += w16_menubar_height(h, rc->right - rc->left);
        else rc->top += GetSystemMetrics(SM_CYMENU) + 1;
    }
    int framed = l > 0;
    if (h->style & WS_VSCROLL) rc->right -= GetSystemMetrics(SM_CXVSCROLL) - (framed ? 1 : 0);
    if (h->style & WS_HSCROLL) rc->bottom -= GetSystemMetrics(SM_CYHSCROLL) - (framed ? 1 : 0);
    if (rc->right < rc->left) rc->right = rc->left;
    if (rc->bottom < rc->top) rc->bottom = rc->top;
}

/* window-relative rectangles */
static void caption_rect(HWND h, RECT *c)
{
    int l, t, r, b;
    frame_insets(h, &l, &t, &r, &b);
    int w = h->rw.right - h->rw.left;
    SetRect(c, l, t, w - r, t + GetSystemMetrics(SM_CYCAPTION) - 2);
    if (has_dlgframe(h)) InflateRect(c, -1, 0); /* inside the white line beside the caption */
}

static int has_min(HWND h) { return w16_has_caption(h->style) && (h->style & WS_MINIMIZEBOX) && !(h->style & WS_CHILD); }
static int has_max(HWND h) { return w16_has_caption(h->style) && (h->style & WS_MAXIMIZEBOX) && !(h->style & WS_CHILD); }

static void button_rects(HWND h, RECT *rmin, RECT *rmax)
{
    RECT c;
    caption_rect(h, &c);
    int bw = 19;
    SetRectEmpty(rmin);
    SetRectEmpty(rmax);
    int x = c.right;
    if (has_max(h)) { SetRect(rmax, x - bw, c.top, x, c.bottom); x -= bw; }
    if (has_min(h)) { SetRect(rmin, x - bw, c.top, x, c.bottom); x -= bw; }
}

static void sysmenu_rect(HWND h, RECT *r)
{
    RECT c;
    caption_rect(h, &c);
    if (h->style & WS_SYSMENU) SetRect(r, c.left, c.top, c.left + GetSystemMetrics(SM_CXSIZE), c.bottom);
    else SetRectEmpty(r);
}

void w16_get_sb_rect(HWND h, int bar, RECT *r)
{
    int ox = h->rw.left, oy = h->rw.top;
    RECT c = h->rc;
    OffsetRect(&c, -ox, -oy);
    int l, t, rr, b;
    frame_insets(h, &l, &t, &rr, &b);
    int framed = l > 0;
    if (bar == SB_VERT)
        SetRect(r, c.right, c.top - (framed ? 1 : 0), c.right + GetSystemMetrics(SM_CXVSCROLL), c.bottom + (framed ? 1 : 0));
    else
        SetRect(r, c.left - (framed ? 1 : 0), c.bottom, c.right + (framed ? 1 : 0), c.bottom + GetSystemMetrics(SM_CYHSCROLL));
    if (!framed) {
        if (bar == SB_VERT) r->bottom = c.bottom;
        else r->right = c.right;
    }
}

/* ------------------------------------------------------------------ drawing helpers */
static void obm(HDC dc, int id, int x, int y, int sx, int w, int h)
{
    W16Bitmap *b = w16_obm(id);
    if (!b) return;
    if (w < 0) w = b->w;
    if (h < 0) h = b->h;
    int dx = x, dy = y;
    w16_lp_to_dp(dc, &dx, &dy);
    w16_blit_bitmap(dc, dx, dy, b, sx, 0, w, h, SRCCOPY);
}

/* an OBM bitmap stretched to w x h, as 3.1 does for scroll-bar arrows on controls whose thickness
 * differs from the system metric: nearest pixel, sampled at pixel centres (measured: a 17x17 arrow
 * on a 20-pixel-high control repeats source rows 2, 8 and 14) */
static void obm_stretch(HDC dc, int id, int x, int y, int w, int h)
{
    W16Bitmap *b = w16_obm(id);
    if (!b) return;
    if (w == b->w && h == b->h) { obm(dc, id, x, y, 0, w, h); return; }
    int dx = x, dy = y;
    w16_lp_to_dp(dc, &dx, &dy);
    for (int yy = 0; yy < h; yy++) {
        int sy = (2 * yy + 1) * b->h / (2 * h);
        if (w == b->w) { w16_blit_bitmap(dc, dx, dy + yy, b, 0, sy, w, 1, SRCCOPY); continue; }
        for (int xx = 0; xx < w; xx++)
            w16_blit_bitmap(dc, dx + xx, dy + yy, b, (2 * xx + 1) * b->w / (2 * w), sy, 1, 1, SRCCOPY);
    }
}

/* USER paints frames, captions and scroll-bar parts with its system-colour brushes (PatBlt), so a
 * colour the display cannot show solid is dithered by the driver as for any solid brush - measured:
 * real 3.11 after MAIN.CPL Color applies Arizona (ActiveTitle 64 128 128, ActiveBorder 255 128 64) */
static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    int a = l, bb = t, cc = r, d = b;
    w16_lp_to_dp(dc, &a, &bb);
    w16_lp_to_dp(dc, &cc, &d);
    HBRUSH br = CreateSolidBrush(c);
    w16_fill_rect_dev(dc, &(RECT){a, bb, cc, d}, br);
    DeleteObject(br);
}

static void draw_frame(HWND h, HDC dc, int active)
{
    int w = h->rw.right - h->rw.left, ht = h->rw.bottom - h->rw.top;
    COLORREF black = GetSysColor(COLOR_WINDOWFRAME);
    if (h->style & WS_THICKFRAME) {
        int f = CXFRAME;
        COLORREF fc = GetSysColor(active ? COLOR_ACTIVEBORDER : COLOR_INACTIVEBORDER);
        /* outer border */
        fill(dc, 0, 0, w, 1, black); fill(dc, 0, ht - 1, w, ht, black);
        fill(dc, 0, 0, 1, ht, black); fill(dc, w - 1, 0, w, ht, black);
        /* frame band */
        fill(dc, 1, 1, w - 1, f - 1, fc); fill(dc, 1, ht - f + 1, w - 1, ht - 1, fc);
        fill(dc, 1, f - 1, f - 1, ht - f + 1, fc); fill(dc, w - f + 1, f - 1, w - 1, ht - f + 1, fc);
        /* inner border */
        fill(dc, f - 1, f - 1, w - f + 1, f, black); fill(dc, f - 1, ht - f, w - f + 1, ht - f + 1, black);
        fill(dc, f - 1, f - 1, f, ht - f + 1, black); fill(dc, w - f, f - 1, w - f + 1, ht - f + 1, black);
        /* sizing notches */
        int n = f + GetSystemMetrics(SM_CXSIZE);
        if (w > 2 * n && ht > 2 * n) {
            fill(dc, n, 1, n + 1, f - 1, black); fill(dc, w - 1 - n, 1, w - n, f - 1, black);
            fill(dc, n, ht - f + 1, n + 1, ht - 1, black); fill(dc, w - 1 - n, ht - f + 1, w - n, ht - 1, black);
            fill(dc, 1, n, f - 1, n + 1, black); fill(dc, 1, ht - 1 - n, f - 1, ht - n, black);
            fill(dc, w - f + 1, n, w - 1, n + 1, black); fill(dc, w - f + 1, ht - 1 - n, w - 1, ht - n, black);
        }
    } else if (has_dlgframe(h)) {
        int d = GetSystemMetrics(SM_CXDLGFRAME);
        COLORREF fc = GetSysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
        COLORREF in = GetSysColor(COLOR_WINDOW);
        int cap = w16_has_caption(h->style);
        int td = cap ? d - 1 : d;
        fill(dc, 0, 0, w, 1, black); fill(dc, 0, ht - 1, w, ht, black);
        fill(dc, 0, 0, 1, ht, black); fill(dc, w - 1, 0, w, ht, black);
        fill(dc, 1, 1, w - 1, 1 + td, fc); fill(dc, 1, ht - 1 - d, w - 1, ht - 1, fc);
        fill(dc, 1, 1 + td, 1 + d, ht - 1 - d, fc); fill(dc, w - 1 - d, 1 + td, w - 1, ht - 1 - d, fc);
        if (cap) {
            /* white line around the caption, inside the frame; the client starts on it below */
            int cb = 1 + td + GetSystemMetrics(SM_CYCAPTION);
            fill(dc, 1 + d, 1 + td, w - 1 - d, 2 + td, in);
            fill(dc, 1 + d, 1 + td, 2 + d, cb, in); fill(dc, w - 2 - d, 1 + td, w - 1 - d, cb, in);
        }
    } else if (h->style & WS_BORDER) {
        fill(dc, 0, 0, w, 1, black); fill(dc, 0, ht - 1, w, ht, black);
        fill(dc, 0, 0, 1, ht, black); fill(dc, w - 1, 0, w, ht, black);
    }
}

HWND w16_sysbox_inverted; /* the window whose system-menu box the menu loop has selected */

void w16_draw_caption(HWND h, HDC dc, int active)
{
    if (!w16_has_caption(h->style)) return;
    RECT c;
    caption_rect(h, &c);
    COLORREF bg = GetSysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
    COLORREF fg = GetSysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
    COLORREF black = GetSysColor(COLOR_WINDOWFRAME);
    fill(dc, c.left, c.top, c.right, c.bottom, bg);
    fill(dc, c.left, c.bottom, c.right, c.bottom + 1, black);
    /* the border above the caption for frameless (WS_BORDER) captions is the outer border */
    RECT sm, rmin, rmax;
    sysmenu_rect(h, &sm);
    button_rects(h, &rmin, &rmax);
    int tl = c.left, tr = c.right;
    if (!IsRectEmpty(&sm)) {
        /* left half of OBM_CLOSE: application system menu; right half: MDI child */
        int half = (h->style & WS_CHILD) ? 18 : 0;
        obm(dc, OBM_CLOSE, sm.left, sm.top, half, 18, 18);
        if (h == w16_sysbox_inverted) InvertRect(dc, &sm);
        fill(dc, sm.right, c.top, sm.right + 1, c.bottom, black);
        tl = sm.right + 1;
    }
    if (!IsRectEmpty(&rmax)) {
        obm(dc, IsZoomed(h) ? OBM_RESTORE : OBM_ZOOM, rmax.left, rmax.top, 0, -1, -1);
        tr = rmax.left;
    }
    if (!IsRectEmpty(&rmin)) {
        obm(dc, OBM_REDUCE, rmin.left, rmin.top, 0, -1, -1);
        tr = rmin.left;
    }
    /* caption text, centred, clipped to the free area */
    W16Font *f = w16_font_system();
    int n = strlen(h->text);
    int tw = w16_text_width(f, h->text, n);
    int x = tl + ((tr - tl) - tw) / 2;
    if (x < tl + 1) x = tl + 1;
    int y = c.top + ((c.bottom - c.top) - f->height) / 2;
    int sv = SaveDC(dc);
    IntersectClipRect(dc, tl, c.top, tr, c.bottom);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    TextOut(dc, x, y, h->text, n);
    RestoreDC(dc, sv);
}

/* ------------------------------------------------------------------ scroll bars */
static int thumb_pos(W16Scroll *s, int track_len, int thumb)
{
    int range = s->max - s->min;
    if (range <= 0) return 0;
    int avail = track_len - thumb;
    int p = s->pos - s->min;
    if (p < 0) p = 0;
    if (p > range) p = range;
    return (int)(((long)avail * p + range / 2) / range); /* MulDiv rounding, as measured */
}

/* where the thumb starts along the bar (as drawn by w16_draw_sb_ctl), -1 when there is none */
int w16_sb_thumb(const RECT *r, int vert, W16Scroll *s)
{
    int len = vert ? r->bottom - r->top : r->right - r->left;
    int a = vert ? GetSystemMetrics(SM_CYVSCROLL) : GetSystemMetrics(SM_CXHSCROLL);
    if (len < 2 * a) return -1;
    int thumb = vert ? GetSystemMetrics(SM_CYVTHUMB) : GetSystemMetrics(SM_CXHTHUMB);
    int track = len - 2 * a + 2;
    if (s->max <= s->min || track < thumb + 2) return -1;
    return a - 1 + thumb_pos(s, track, thumb);
}

/* parts: 1 = line up, 2 = page up, 3 = thumb, 4 = page down, 5 = line down */
int w16_sb_hit(const RECT *r, int vert, W16Scroll *s, int x, int y, RECT *part)
{
    int len = vert ? r->bottom - r->top : r->right - r->left;
    int a = vert ? GetSystemMetrics(SM_CYVSCROLL) : GetSystemMetrics(SM_CXHSCROLL);
    if (len < 2 * a) a = len / 2;
    int pos = vert ? y - r->top : x - r->left;
    int thumb = vert ? GetSystemMetrics(SM_CYVTHUMB) : GetSystemMetrics(SM_CXHTHUMB);
    int track = len - 2 * a + 2;
    int enabled = s->max > s->min && track >= thumb;
    int tp = a - 1 + (enabled ? thumb_pos(s, track, thumb) : 0);
    int code;
    int p0, p1;
    if (pos < a) { code = 1; p0 = 0; p1 = a; }
    else if (pos >= len - a) { code = 5; p0 = len - a; p1 = len; }
    else if (!enabled) { code = 0; p0 = a; p1 = len - a; }
    else if (pos < tp) { code = 2; p0 = a; p1 = tp; }
    else if (pos < tp + thumb) { code = 3; p0 = tp; p1 = tp + thumb; }
    else { code = 4; p0 = tp + thumb; p1 = len - a; }
    if (part) {
        if (vert) SetRect(part, r->left, r->top + p0, r->right, r->top + p1);
        else SetRect(part, r->left + p0, r->top, r->left + p1, r->bottom);
    }
    return code;
}

static void bevel_box(HDC dc, int l, int t, int r, int b)
{
    COLORREF black = GetSysColor(COLOR_WINDOWFRAME);
    fill(dc, l, t, r, b, GetSysColor(COLOR_BTNFACE));
    fill(dc, l, t, r, t + 1, black); fill(dc, l, b - 1, r, b, black);
    fill(dc, l, t, l + 1, b, black); fill(dc, r - 1, t, r, b, black);
    COLORREF hi = GetSysColor(COLOR_BTNHIGHLIGHT), sh = GetSysColor(COLOR_BTNSHADOW);
    fill(dc, l + 1, t + 1, r - 2, t + 2, hi);
    fill(dc, l + 1, t + 1, l + 2, b - 2, hi);
    fill(dc, l + 1, b - 3, r - 1, b - 1, sh);
    fill(dc, r - 3, t + 1, r - 1, b - 1, sh);
    fill(dc, l + 2, b - 3, l + 3, b - 2, sh);
    /* top-right/bottom-left single-pixel details match the arrow bitmaps */
    fill(dc, r - 3, t + 1, r - 2, t + 2, hi);
    fill(dc, l + 1, b - 3, l + 2, b - 2, hi);
}

/* USER seg18:02DA: a bar with both arrows disabled fills its trough with the class background of
 * its window (of the parent, for a scroll bar control), else the window colour, instead of the
 * scroll bar colour (the empty Events list of SND.CPL shows a white trough) */
static HBRUSH disabled_trough(HWND bg)
{
    HBRUSH b = w16_valid(bg) && bg->cls ? bg->cls->wc.hbrBackground : NULL;
    if ((uintptr_t)b > 0 && (uintptr_t)b <= COLOR_BTNHIGHLIGHT + 1) return w16_sys_brush((int)(uintptr_t)b - 1);
    return b ? b : w16_sys_brush(COLOR_WINDOW);
}

void w16_draw_sb_ctl(HDC dc, const RECT *r, int vert, W16Scroll *s, int pressed, int enabled_win, HWND bg)
{
    COLORREF black = GetSysColor(COLOR_WINDOWFRAME);
    int len = vert ? r->bottom - r->top : r->right - r->left;
    int a = vert ? GetSystemMetrics(SM_CYVSCROLL) : GetSystemMetrics(SM_CXHSCROLL);
    int shrink = len < 2 * a;
    if (shrink) a = len / 2;
    int thumb = vert ? GetSystemMetrics(SM_CYVTHUMB) : GetSystemMetrics(SM_CXHTHUMB);
    int track = len - 2 * a + 2;
    int enabled = enabled_win && s->max > s->min && !(s->disabled == ESB_DISABLE_BOTH);
    /* trough */
    HBRUSH tb = s->disabled == ESB_DISABLE_BOTH ? disabled_trough(bg) : w16_sys_brush(COLOR_SCROLLBAR);
    if (vert) {
        RECT t = {r->left, r->top + a, r->right, r->bottom - a};
        int L = t.left, T = t.top, R = t.right, B = t.bottom;
        w16_lp_to_dp(dc, &L, &T); w16_lp_to_dp(dc, &R, &B);
        w16_fill_rect_dev(dc, &(RECT){L + 1, T, R - 1, B}, tb);
        fill(dc, t.left, t.top, t.left + 1, t.bottom, black);
        fill(dc, t.right - 1, t.top, t.right, t.bottom, black);
    } else {
        RECT t = {r->left + a, r->top, r->right - a, r->bottom};
        int L = t.left, T = t.top, R = t.right, B = t.bottom;
        w16_lp_to_dp(dc, &L, &T); w16_lp_to_dp(dc, &R, &B);
        w16_fill_rect_dev(dc, &(RECT){L, T + 1, R, B - 1}, tb);
        fill(dc, t.left, t.top, t.right, t.top + 1, black);
        fill(dc, t.left, t.bottom - 1, t.right, t.bottom, black);
    }
    /* arrows */
    int up = vert ? (enabled ? (pressed == 1 ? OBM_UPARROWD : OBM_UPARROW) : OBM_UPARROWI)
                  : (enabled ? (pressed == 1 ? OBM_LFARROWD : OBM_LFARROW) : OBM_LFARROWI);
    int dn = vert ? (enabled ? (pressed == 5 ? OBM_DNARROWD : OBM_DNARROW) : OBM_DNARROWI)
                  : (enabled ? (pressed == 5 ? OBM_RGARROWD : OBM_RGARROW) : OBM_RGARROWI);
    if (!shrink) {
        if (vert) {
            obm_stretch(dc, up, r->left, r->top, r->right - r->left, a);
            obm_stretch(dc, dn, r->left, r->bottom - a, r->right - r->left, a);
        } else {
            obm_stretch(dc, up, r->left, r->top, a, r->bottom - r->top);
            obm_stretch(dc, dn, r->right - a, r->top, a, r->bottom - r->top);
        }
    } else {
        /* too short for the bitmaps: plain boxes */
        if (vert) { bevel_box(dc, r->left, r->top, r->right, r->top + a); bevel_box(dc, r->left, r->bottom - a, r->right, r->bottom); }
        else { bevel_box(dc, r->left, r->top, r->left + a, r->bottom); bevel_box(dc, r->right - a, r->top, r->right, r->bottom); }
    }
    /* thumb */
    if (enabled && track >= thumb + 2) {
        int tp = a - 1 + thumb_pos(s, track, thumb);
        if (vert) bevel_box(dc, r->left, r->top + tp, r->right, r->top + tp + thumb);
        else bevel_box(dc, r->left + tp, r->top, r->left + tp + thumb, r->bottom);
    }
    if (pressed == 2 || pressed == 4) {
        RECT part;
        int code = pressed;
        RECT rr = *r;
        /* invert the page area being auto-repeated */
        int y = 0, x = 0;
        int tp = a - 1 + thumb_pos(s, track, thumb);
        if (vert) {
            if (code == 2) { y = r->top + a; SetRect(&part, r->left + 1, y, r->right - 1, r->top + tp); }
            else SetRect(&part, r->left + 1, r->top + tp + thumb, r->right - 1, r->bottom - a);
        } else {
            if (code == 2) { x = r->left + a; SetRect(&part, x, r->top + 1, r->left + tp, r->bottom - 1); }
            else SetRect(&part, r->left + tp + thumb, r->top + 1, r->right - a, r->bottom - 1);
        }
        (void)rr;
        int L = part.left, T = part.top, R = part.right, B = part.bottom;
        w16_lp_to_dp(dc, &L, &T); w16_lp_to_dp(dc, &R, &B);
        w16_invert_dev(dc, &(RECT){L, T, R, B});
    }
}

void w16_draw_sb(HWND h, HDC wdc, int bar, int pressed)
{
    RECT r;
    w16_get_sb_rect(h, bar, &r);
    /* a disabled window keeps its own scroll bars live (SND.CPL's disabled Files list on 3.11) */
    w16_draw_sb_ctl(wdc, &r, bar == SB_VERT, &h->sb[bar], pressed, 1, h);
}

/* ------------------------------------------------------------------ WM_NCPAINT */
void w16_nc_paint(HWND h, int active)
{
    if (!w16_window_visible(h)) return;
    if (h->style & WS_MINIMIZE) return;
    HDC dc = GetWindowDC(h);
    /* exclude client area */
    RECT c = h->rc;
    rgn_sub(&dc->vis, &c);
    draw_frame(h, dc, active);
    w16_draw_caption(h, dc, active);
    if (is_top(h) && h->menu && !(h->style & WS_CHILD)) w16_draw_menubar(h, dc);
    if (h->style & WS_VSCROLL) w16_draw_sb(h, dc, SB_VERT, 0);
    if (h->style & WS_HSCROLL) w16_draw_sb(h, dc, SB_HORZ, 0);
    if ((h->style & WS_VSCROLL) && (h->style & WS_HSCROLL)) {
        RECT v, hz;
        w16_get_sb_rect(h, SB_VERT, &v);
        w16_get_sb_rect(h, SB_HORZ, &hz);
        fill(dc, v.left + 1, hz.top + 1, v.right - 1, hz.bottom - 1, GetSysColor(COLOR_SCROLLBAR));
    }
    ReleaseDC(h, dc);
    h->active_frame = active;
}

/* ------------------------------------------------------------------ hit testing */
int w16_nc_hittest(HWND h, int x, int y)
{
    POINT p = {x, y};
    if (!PtInRect(&h->rw, p)) return HTNOWHERE;
    if (h->style & WS_MINIMIZE) return HTCAPTION;
    if (PtInRect(&h->rc, p)) return HTCLIENT;
    int wx = x - h->rw.left, wy = y - h->rw.top;
    int w = h->rw.right - h->rw.left, ht = h->rw.bottom - h->rw.top;
    if (h->style & WS_THICKFRAME) {
        int f = CXFRAME, n = f + GetSystemMetrics(SM_CXSIZE);
        if (wx < f || wy < f || wx >= w - f || wy >= ht - f) {
            if (wy < n) return wx < n ? HTTOPLEFT : wx >= w - n ? HTTOPRIGHT : HTTOP;
            if (wy >= ht - n) return wx < n ? HTBOTTOMLEFT : wx >= w - n ? HTBOTTOMRIGHT : HTBOTTOM;
            if (wx < f) return wy < n ? HTTOPLEFT : wy >= ht - n ? HTBOTTOMLEFT : HTLEFT;
            return wy < n ? HTTOPRIGHT : wy >= ht - n ? HTBOTTOMRIGHT : HTRIGHT;
        }
    }
    if (w16_has_caption(h->style)) {
        RECT c, sm, mn, mx;
        caption_rect(h, &c);
        c.bottom += 1;
        if (PtInRect(&c, (POINT){wx, wy})) {
            sysmenu_rect(h, &sm);
            button_rects(h, &mn, &mx);
            if (PtInRect(&sm, (POINT){wx, wy})) return HTSYSMENU;
            if (PtInRect(&mn, (POINT){wx, wy})) return HTREDUCE;
            if (PtInRect(&mx, (POINT){wx, wy})) return HTZOOM;
            return HTCAPTION;
        }
    }
    if (is_top(h) && h->menu && !(h->style & WS_CHILD)) {
        RECT c;
        caption_rect(h, &c);
        int mt = w16_has_caption(h->style) ? c.bottom + 1 : c.top;
        int mb = h->rc.top - h->rw.top;
        if (wy >= mt && wy < mb) return HTMENU;
    }
    RECT sb;
    if (h->style & WS_VSCROLL) {
        w16_get_sb_rect(h, SB_VERT, &sb);
        if (PtInRect(&sb, (POINT){wx, wy})) {
            if ((h->style & WS_HSCROLL) && wy >= sb.bottom - 1) return HTSIZE;
            return HTVSCROLL;
        }
    }
    if (h->style & WS_HSCROLL) {
        w16_get_sb_rect(h, SB_HORZ, &sb);
        if (PtInRect(&sb, (POINT){wx, wy})) return HTHSCROLL;
        if ((h->style & WS_VSCROLL) && wy >= sb.top && wx >= sb.right) return HTSIZE;
    }
    return HTBORDER;
}

/* ------------------------------------------------------------------ min / max / restore */
/* An icon window is the icon and a 2-border margin around it (USER seg3:242F: 4 * cxBorder + cxIcon,
 * 36 x 36 on VGA); the whole of it is the client area (real 3.11's Clock paints its minimised face
 * from GetClientRect: a 36-pixel ring). */
static int icon_wnd_cx(void) { return GetSystemMetrics(SM_CXICON) + 4 * GetSystemMetrics(SM_CXBORDER); }
static int icon_wnd_cy(void) { return GetSystemMetrics(SM_CYICON) + 4 * GetSystemMetrics(SM_CYBORDER); }

/* USER seg4:0000, the slot of a window minimised for the first time: slots of SM_CXICONSPACING x
 * SM_CYICONSPACING fill the parent's client area from its bottom-left corner, left to right, then
 * upwards; a slot is taken when it overlaps the slot of another visible icon. The icon window goes
 * to the slot's top, centred across: spacing / 2 - cxIcon / 2 (measured on real 3.11: x 21 with the
 * default spacing of 75, 34 with WIN.INI IconSpacing=100; y 408 = 480 - 72). */
static void icon_slot_rect(int i, int cols, int ph, RECT *s)
{
    int sx = GetSystemMetrics(SM_CXICONSPACING), sy = GetSystemMetrics(SM_CYICONSPACING);
    s->left = (i % cols) * sx;
    s->right = s->left + sx;
    s->top = ph - (i / cols + 1) * sy;
    s->bottom = s->top + sy;
}

static void icon_slot(HWND h, POINT *pt)
{
    if (h->has_iconpos) { *pt = h->iconpos; return; }
    int sx = GetSystemMetrics(SM_CXICONSPACING), sy = GetSystemMetrics(SM_CYICONSPACING);
    int half = GetSystemMetrics(SM_CXICON) / 2;
    int cols = max(1, w16_screen.w / sx);
    RECT s;
    for (int i = 0; i < 1000; i++) {
        icon_slot_rect(i, cols, w16_screen.h, &s);
        int used = 0;
        for (HWND c = w16_desktop->child; c && !used; c = c->next) {
            if (c == h || !(c->style & WS_MINIMIZE) || !(c->style & WS_VISIBLE)) continue;
            RECT o = {c->rw.left - sx / 2 + half, c->rw.top, 0, 0}, x;
            o.right = o.left + sx;
            o.bottom = o.top + sy;
            used = IntersectRect(&x, &o, &s);
        }
        if (!used) break;
    }
    pt->x = s.left + sx / 2 - half;
    pt->y = s.top;
}

/* USER seg1:6B43 / 6C14: an icon's title window. The text is measured with DrawText(DT_CALCRECT |
 * DT_WORDBREAK | DT_CENTER | DT_NOPREFIX) in SM_CXICONSPACING - 2 cxBorder (one line when WIN.INI
 * [desktop] IconTitleWrap=0) in the icon title font; the window is that wide plus 4 cxBorder, as high
 * as the text, centred under the icon window: x = icon x + width/2 - text/2 - 2 cxBorder. */
static HFONT icon_title_font(void)
{
    static HFONT f;
    if (!f) {
        LOGFONT lf;
        SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof lf, &lf, 0);
        f = CreateFontIndirect(&lf);
    }
    return f;
}

static UINT icon_title_dt(void)
{
    return DT_WORDBREAK | DT_CENTER | DT_NOPREFIX | (GetProfileInt("desktop", "IconTitleWrap", 1) ? 0 : DT_SINGLELINE);
}

int w16_icon_title_rect(HWND h, RECT *r)
{
    SetRectEmpty(r);
    if (!w16_valid(h) || !(h->style & WS_MINIMIZE) || h->parent != w16_desktop) return 0;
    int cxb = GetSystemMetrics(SM_CXBORDER);
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, icon_title_font());
    RECT t = {0, 0, GetSystemMetrics(SM_CXICONSPACING) - 2 * cxb, 2};
    char buf[80];
    snprintf(buf, sizeof buf, "%s", h->text ? h->text : "");
    int n = strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    if (n) DrawText(dc, buf, n, &t, DT_CALCRECT | icon_title_dt());
    else t.bottom = t.top + HIWORD(GetTextExtent(dc, " ", 1));
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
    int w = t.right - t.left;
    r->left = h->rw.left + icon_wnd_cx() / 2 - w / 2 - 2 * cxb;
    r->top = h->rw.top + icon_wnd_cy();
    r->right = r->left + w + 4 * cxb;
    r->bottom = r->top + (t.bottom - t.top);
    return 1;
}

void w16_invalidate_icon_title(HWND h)
{
    RECT r;
    if (w16_icon_title_rect(h, &r)) w16_invalidate_screen_rect(&r);
}

/* USER seg1:6984, painting an icon title: the active icon's on the active caption colour in the
 * caption text colour; an inactive one on the desktop's colour, its text black when that colour is
 * light (R + G + B > 381), else white. Text in the title rectangle less cxBorder at the left and
 * 2 cxBorder at the right. Called by the desktop's WM_PAINT for every visible icon. */
void w16_paint_icon_titles(HDC dc)
{
    for (HWND c = w16_desktop->child; c; c = c->next) {
        if (!(c->style & WS_VISIBLE) || !(c->style & WS_MINIMIZE)) continue;
        RECT r;
        if (!w16_icon_title_rect(c, &r) || !RectVisible(dc, &r)) continue;
        int active = c == w16_active;
        COLORREF bk = GetSysColor(active ? COLOR_ACTIVECAPTION : COLOR_BACKGROUND), fg;
        if (active) fg = GetSysColor(COLOR_CAPTIONTEXT);
        else fg = GetRValue(bk) + GetGValue(bk) + GetBValue(bk) > 0x17D ? RGB(0, 0, 0) : RGB(255, 255, 255);
        FillRect(dc, &r, w16_sys_brush(active ? COLOR_ACTIVECAPTION : COLOR_BACKGROUND));
        HGDIOBJ of = SelectObject(dc, icon_title_font());
        SetTextColor(dc, fg);
        SetBkMode(dc, TRANSPARENT);
        int cxb = GetSystemMetrics(SM_CXBORDER);
        RECT t = {r.left + cxb, r.top, r.right - 2 * cxb, r.bottom};
        DrawText(dc, c->text ? c->text : "", -1, &t, icon_title_dt());
        SelectObject(dc, of);
    }
}

/* USER seg6:1A72 (MinMaximize) with SW_MINIMIZE / SW_SHOWMINIMIZED: the window becomes an icon window
 * at its icon slot and gets WM_SIZE with the icon window's size (its client area) */
void w16_minimize(HWND h)
{
    if (h->style & WS_MINIMIZE) return;
    if (!(h->style & WS_MAXIMIZE)) h->restore = h->rw;
    RECT old = h->rw;
    POINT p;
    icon_slot(h, &p);
    h->style |= WS_MINIMIZE;
    h->style &= ~WS_MAXIMIZE;
    h->rw = (RECT){p.x, p.y, p.x + icon_wnd_cx(), p.y + icon_wnd_cy()};
    w16_nc_calc(h, &h->rw, &h->rc);
    if (w16_focus && (w16_focus == h || IsChild(h, w16_focus))) SetFocus(h);
    w16_invalidate_screen_rect(&old);
    w16_invalidate_window(h, NULL, 1, 1);
    w16_invalidate_icon_title(h);
    SendMessage(h, WM_SIZE, SIZE_MINIMIZED, MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
    SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rw.left, h->rw.top));
}

void w16_maximize(HWND h)
{
    if (!(h->style & (WS_MINIMIZE | WS_MAXIMIZE))) h->restore = h->rw;
    RECT old = h->rw;
    int f = (h->style & WS_THICKFRAME) ? CXFRAME : 0;
    h->style &= ~WS_MINIMIZE;
    h->style |= WS_MAXIMIZE;
    RECT nr = {-f, -f, w16_screen.w + f, w16_screen.h + f};
    w16_invalidate_screen_rect(&old);
    w16_set_window_rect(h, &nr, 0);
    w16_invalidate_window(h, NULL, 1, 1);
    SendMessage(h, WM_SIZE, SIZE_MAXIMIZED, MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
    SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left, h->rc.top));
}

/* Restoring does not make the icon's place stick: USER's MinMaximize (seg6:1BC2) forgets the icon
 * position and takes a free slot (seg4:0000) on the next minimize unless the checkpoint says the icon
 * was moved (flag set at seg6:123B when a move ends on an icon: track_rect below). */
void w16_restore(HWND h)
{
    RECT old = h->rw, title;
    int was_min = (h->style & WS_MINIMIZE) != 0;
    w16_icon_title_rect(h, &title);
    h->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
    w16_invalidate_screen_rect(&old);
    if (was_min) w16_invalidate_screen_rect(&title);
    h->rw = (RECT){0, 0, 0, 0};
    w16_set_window_rect(h, &h->restore, 0);
    w16_invalidate_window(h, NULL, 1, 1);
    SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
    SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left, h->rc.top));
}

/* a minimised window of a class with an icon shows the icon inside its 2-border margin (title drawn
 * by the desktop, w16_paint_icon_titles); without a class icon the program paints its icon window
 * itself (Clock) - WM_QUERYDRAGICON only gives the icon dragged with the mouse. UNTESTED against
 * real 3.11: the class-icon position */
void w16_iconic_paint(HWND h)
{
    HICON ic = h->cls->wc.hIcon;
    if (!ic) return;
    HDC dc = GetWindowDC(h);
    DrawIcon(dc, 2 * GetSystemMetrics(SM_CXBORDER), 2 * GetSystemMetrics(SM_CYBORDER), ic);
    ReleaseDC(h, dc);
}

/* ------------------------------------------------------------------ move / size tracking */
void w16_xor_frame_rect(const RECT *r, int t)
{
    /* gray XOR outline like USER's DrawDragRect (pattern = 50% gray) */
    HDC dc = w16_make_dc(NULL, 1);
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++) {
            int edge = x < r->left + t || x >= r->right - t || y < r->top + t || y >= r->bottom - t;
            if (!edge || x < 0 || y < 0 || x >= w16_screen.w || y >= w16_screen.h) continue;
            if (((x + y) & 1) == 0) w16_screen.px[y * w16_screen.w + x] = w16_invert_px(w16_screen.px[y * w16_screen.w + x]);
        }
    w16_screen_dirty = 1;
    DeleteDC(dc);
}

static void track_rect(HWND h, int hit, POINT start)
{
    RECT r = h->rw, orig = h->rw;
    int t = (h->style & WS_THICKFRAME) ? CXFRAME : 1;
    w16_xor_frame_rect(&r, t);
    SetCapture(h);
    MSG m;
    int done = 0, cancel = 0;
    while (!done) {
        if (!GetMessage(&m, NULL, 0, 0)) { PostQuitMessage(m.wParam); break; }
        POINT p = m.pt;
        RECT nr = orig;
        int dx = p.x - start.x, dy = p.y - start.y;
        switch (m.message) {
        case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_LBUTTONUP: case WM_NCLBUTTONUP:
            if (hit == HTCAPTION) OffsetRect(&nr, dx, dy);
            else {
                if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT) nr.left += dx;
                if (hit == HTRIGHT || hit == HTTOPRIGHT || hit == HTBOTTOMRIGHT) nr.right += dx;
                if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT) nr.top += dy;
                if (hit == HTBOTTOM || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT) nr.bottom += dy;
                int mw = GetSystemMetrics(SM_CXMINTRACK), mh = GetSystemMetrics(SM_CYMINTRACK);
                if (nr.right - nr.left < mw) { if (nr.left != orig.left) nr.left = nr.right - mw; else nr.right = nr.left + mw; }
                if (nr.bottom - nr.top < mh) { if (nr.top != orig.top) nr.top = nr.bottom - mh; else nr.bottom = nr.top + mh; }
            }
            if (!EqualRect(&nr, &r)) {
                w16_xor_frame_rect(&r, t);
                r = nr;
                w16_xor_frame_rect(&r, t);
            }
            if (m.message == WM_LBUTTONUP || m.message == WM_NCLBUTTONUP) done = 1;
            break;
        case WM_KEYDOWN:
            if (m.wParam == VK_ESCAPE) { done = cancel = 1; }
            else if (m.wParam == VK_RETURN) done = 1;
            break;
        default:
            if (m.message == WM_PAINT || m.message == WM_TIMER) DispatchMessage(&m);
            break;
        }
        w16_present();
    }
    w16_xor_frame_rect(&r, t);
    ReleaseCapture();
    if (!cancel && !EqualRect(&r, &orig)) {
        RECT pr = {0, 0, 0, 0};
        if (h->parent && h->parent != w16_desktop) pr = h->parent->rc;
        SetWindowPos(h, NULL, r.left - pr.left, r.top - pr.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        /* an icon moved by the user keeps its place (USER seg6:123B sets the checkpoint flag) */
        if (IsIconic(h)) h->iconpos = (POINT){r.left, r.top}, h->has_iconpos = 1;
    }
}

/* press-and-hold on a caption button: draws the pressed bitmap while the mouse is inside */
static int track_button(HWND h, int hit)
{
    RECT mn, mx;
    button_rects(h, &mn, &mx);
    RECT br = hit == HTREDUCE ? mn : mx;
    int up = hit == HTREDUCE ? OBM_REDUCE : (IsZoomed(h) ? OBM_RESTORE : OBM_ZOOM);
    int dn = hit == HTREDUCE ? OBM_REDUCED : (IsZoomed(h) ? OBM_RESTORED : OBM_ZOOMD);
    int down = 1;
    HDC dc = GetWindowDC(h);
    obm(dc, dn, br.left, br.top, 0, -1, -1);
    SetCapture(h);
    MSG m;
    for (;;) {
        w16_present();
        if (!GetMessage(&m, NULL, 0, 0)) break;
        if (m.message == WM_MOUSEMOVE || m.message == WM_NCMOUSEMOVE || m.message == WM_LBUTTONUP || m.message == WM_NCLBUTTONUP) {
            POINT p = {m.pt.x - h->rw.left, m.pt.y - h->rw.top};
            int in = PtInRect(&br, p);
            if (in != down) { down = in; obm(dc, down ? dn : up, br.left, br.top, 0, -1, -1); }
            if (m.message == WM_LBUTTONUP || m.message == WM_NCLBUTTONUP) break;
        } else if (m.message == WM_PAINT || m.message == WM_TIMER)
            DispatchMessage(&m);
    }
    ReleaseCapture();
    if (down) obm(dc, up, br.left, br.top, 0, -1, -1);
    ReleaseDC(h, dc);
    return down;
}

LRESULT w16_nc_lbuttondown(HWND h, int hit, int x, int y)
{
    POINT pt = {x, y};
    switch (hit) {
    case HTCAPTION:
        if (h->parent == w16_desktop || (h->style & WS_CHILD)) {
            w16_activate(h, WA_CLICKACTIVE);
            /* 3.1 starts a move only once the mouse actually moves */
            SendMessage(h, WM_SYSCOMMAND, SC_MOVE + HTCAPTION, MAKELPARAM(x, y));
        }
        return 0;
    case HTSYSMENU:
        if (h->style & WS_SYSMENU) SendMessage(h, WM_SYSCOMMAND, SC_MOUSEMENU + HTSYSMENU, MAKELPARAM(x, y));
        return 0;
    case HTMENU:
        SendMessage(h, WM_SYSCOMMAND, SC_MOUSEMENU, MAKELPARAM(x, y));
        return 0;
    case HTREDUCE:
        if (track_button(h, hit)) SendMessage(h, WM_SYSCOMMAND, SC_MINIMIZE, MAKELPARAM(x, y));
        return 0;
    case HTZOOM:
        if (track_button(h, hit)) SendMessage(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, MAKELPARAM(x, y));
        return 0;
    case HTVSCROLL:
    case HTHSCROLL:
        w16_track_sb(h, h, hit == HTVSCROLL ? SB_VERT : SB_HORZ, x, y, 0);
        return 0;
    case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
    case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
        if (!IsZoomed(h)) SendMessage(h, WM_SYSCOMMAND, SC_SIZE + hit - HTLEFT + 1, MAKELPARAM(x, y));
        return 0;
    }
    (void)pt;
    return 0;
}

void w16_sys_command(HWND h, UINT cmd, int x, int y)
{
    switch (cmd & 0xFFF0) {
    case SC_MOVE: {
        if (IsZoomed(h)) return;
        POINT start = {x, y};
        if ((cmd & 0xF) == HTCAPTION) {
            /* wait for a drag beyond the double-click rectangle */
            SetCapture(h);
            MSG m;
            int moved = 0;
            for (;;) {
                if (!GetMessage(&m, NULL, 0, 0)) break;
                if (m.message == WM_MOUSEMOVE || m.message == WM_NCMOUSEMOVE) {
                    if (abs(m.pt.x - x) > 0 || abs(m.pt.y - y) > 0) { moved = 1; break; }
                } else if (m.message == WM_LBUTTONUP || m.message == WM_NCLBUTTONUP) break;
                else if (m.message == WM_PAINT || m.message == WM_TIMER) DispatchMessage(&m);
            }
            ReleaseCapture();
            if (!moved) return;
            track_rect(h, HTCAPTION, start);
        } else {
            /* keyboard move: start with the cursor at the caption centre */
            start = (POINT){(h->rw.left + h->rw.right) / 2, h->rw.top + 10};
            SetCursorPos(start.x, start.y);
            track_rect(h, HTCAPTION, start);
        }
        return;
    }
    case SC_SIZE: {
        int hit = (cmd & 0xF) ? (int)(cmd & 0xF) + HTLEFT - 1 : HTBOTTOMRIGHT;
        track_rect(h, hit, (POINT){x, y});
        return;
    }
    case SC_MINIMIZE: ShowWindow(h, SW_MINIMIZE); return;
    case SC_MAXIMIZE: ShowWindow(h, SW_SHOWMAXIMIZED); return;
    case SC_RESTORE: ShowWindow(h, SW_RESTORE); return;
    case SC_CLOSE: SendMessage(h, WM_CLOSE, 0, 0); return;
    case SC_MOUSEMENU:
        if ((cmd & 0xF) == HTSYSMENU) w16_menu_track_sys(h, 0);
        else w16_menu_track_bar(h, x, y, 0, 0);
        return;
    case SC_KEYMENU:
        if (x == ' ' || (x == '-' && (h->style & WS_CHILD))) w16_menu_track_sys(h, 1);
        else if (h->menu && !(h->style & WS_CHILD)) w16_menu_track_bar(h, 0, 0, x, 1);
        else if (h->style & WS_SYSMENU && x == 0) w16_menu_track_sys(h, 1);
        else MessageBeep(0);
        return;
    case SC_NEXTWINDOW:
    case SC_PREVWINDOW: {
        HWND last = NULL;
        for (HWND c = w16_desktop->child; c; c = c->next)
            if ((c->style & WS_VISIBLE) && c != h) last = c;
        if (last) w16_activate(last, WA_ACTIVE);
        return;
    }
    case SC_TASKLIST:
        /* Task List: launched as the separate taskman port when available */
        return;
    }
}

/* window-relative rectangle of the system-menu box (used by menu tracking) */
void w16_sysbox_rect(HWND h, RECT *r) { sysmenu_rect(h, r); }
