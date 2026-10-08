/* EDIT control (single and multi-line), 3.1 behaviour and look.
 * Geometry as USER's edit code has it (seg26-seg30): a WS_BORDER edit takes the style off and draws
 * its own one-pixel frame, the formatting rect is inset from that frame (SLSize/MLSize) and drawing
 * is clipped as ECSetEditClip clips it; the single-line control keeps 3.1's first-visible-character
 * scrolling, extents, hit testing and caret. Multi-line: caret 2 px wide and one line tall; tab
 * stops every 8 average characters. The text buffer is a local-memory handle (EM_GETHANDLE /
 * EM_SETHANDLE work as in Win16). */
#include "w16int.h"
#include <ctype.h>

typedef struct {
    HLOCAL hbuf;
    int len;
    int anchor, caret;        /* selection = [min, max) */
    int limit;
    int multi, wrap;
    int *ls;                  /* line starts */
    int nl, capl;
    int top, xoff;            /* first visible line; horizontal pixel offset (multi-line) */
    int scr;                  /* single-line: first visible character (ichScreenStart) */
    W16Font *f;
    int lh, avgw;
    int avew, cxsys, cysys;   /* USER's average width of the font; the system font's width, height */
    int overhang, fixed;      /* tmOverhang; a fixed-pitch font */
    int border;               /* created with WS_BORDER: draws its own frame */
    int nofmt;                /* multi-line: too small to format (caret hidden) */
    int pww;                  /* password character width */
    HWND combo;               /* the combo box this is the edit of (ES_COMBOBOX) */
    RECT fmt;
    int modified;
    HLOCAL undo;              /* snapshot for one-level undo */
    int undo_len, undo_a, undo_c, undo_run;
    int ntabs;
    int tabs[64];             /* pixels */
    int focus, mdown;
    char pw;
    int maxw;
    int nohidesel;
    int quiet;                /* no EN_ notifications (initial text in WM_CREATE) */
    UINT_PTR_W16 timer;
} Edit;

static Edit *ed(HWND h) { return (Edit *)h->ctl; }
static char *txt(Edit *e) { return (char *)LocalLock(e->hbuf); }
static void untxt(Edit *e) { LocalUnlock(e->hbuf); }
static int smin(Edit *e) { return min(e->anchor, e->caret); }
static int smax(Edit *e) { return max(e->anchor, e->caret); }

static void notify(HWND h, int code) { Edit *e = h->ctl; if (!e || !e->quiet) w16_notify_parent(h, code); }

/* ------------------------------------------------------------------ measuring */
static int tab_stop(Edit *e, int x)
{
    if (e->ntabs == 0) { int t = 8 * e->avgw; return (x / t + 1) * t; }
    if (e->ntabs == 1) { int t = e->tabs[0] > 0 ? e->tabs[0] : 8 * e->avgw; return (x / t + 1) * t; }
    for (int i = 0; i < e->ntabs; i++) if (e->tabs[i] > x) return e->tabs[i];
    return x + e->avgw;
}

static int ch_w(Edit *e, char c, int x)
{
    if (e->pw) return e->f->widths[(unsigned char)e->pw];
    if (c == '\t' && e->multi) return tab_stop(e, x) - x;
    return e->f->widths[(unsigned char)c];
}

static int seg_width(Edit *e, const char *s, int from, int to)
{
    int x = 0;
    for (int i = from; i < to; i++) x += ch_w(e, s[i], x);
    return x;
}

static int line_end(Edit *e, const char *t, int i)
{
    /* end of line i, excluding its line break */
    int s = e->ls[i], end = (i + 1 < e->nl) ? e->ls[i + 1] : e->len;
    while (end > s && (t[end - 1] == '\n' || t[end - 1] == '\r')) end--;
    return end;
}

static void push_line(Edit *e, int start)
{
    if (e->nl == e->capl) { e->capl = e->capl ? e->capl * 2 : 64; e->ls = realloc(e->ls, sizeof(int) * e->capl); }
    e->ls[e->nl++] = start;
}

static void build_lines(HWND h)
{
    Edit *e = ed(h);
    char *t = txt(e);
    e->nl = 0;
    e->maxw = 0;
    push_line(e, 0);
    if (!e->multi) { e->maxw = seg_width(e, t, 0, e->len); untxt(e); return; }
    int width = e->fmt.right - e->fmt.left;
    int i = 0, start = 0, x = 0, last_space = -1;
    while (i < e->len) {
        char c = t[i];
        if (c == '\r' && i + 1 < e->len && t[i + 1] == '\n') {
            e->maxw = max(e->maxw, x);
            i += 2;
            start = i; x = 0; last_space = -1;
            push_line(e, start);
            continue;
        }
        if (c == '\r' && i + 2 < e->len && t[i + 1] == '\r' && t[i + 2] == '\n') { /* soft break (EM_FMTLINES) */
            i += 3;
            start = i; x = 0; last_space = -1;
            push_line(e, start);
            continue;
        }
        int w = ch_w(e, c, x);
        if (e->wrap && x + w > width && i > start && c != ' ') {
            int brk = last_space >= start ? last_space + 1 : i;
            e->maxw = max(e->maxw, x);
            start = brk;
            i = brk;
            x = 0; last_space = -1;
            push_line(e, start);
            continue;
        }
        if (c == ' ' || c == '\t') last_space = i;
        x += w;
        i++;
    }
    e->maxw = max(e->maxw, x);
    untxt(e);
}

static int line_of(Edit *e, int pos)
{
    int lo = 0, hi = e->nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (e->ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int vis_lines(Edit *e) { return max(1, (e->fmt.bottom - e->fmt.top) / e->lh); }

/* ES_CENTER / ES_RIGHT: in 3.1 they align each line of a multiline edit (single-line edits ignore
 * them; MAIN.CPL's Date & Time fields are multiline for that reason). The line is measured like
 * GetTextExtent, the font's overhang included. */
static int line_indent(HWND h, Edit *e, const char *t, int l)
{
    if (!e->multi || !(h->style & (ES_CENTER | ES_RIGHT))) return 0;
    int end = line_end(e, t, l);
    int w = seg_width(e, t, e->ls[l], end);
    if (end > e->ls[l]) w += e->f ? e->f->bold_sim : 0;
    int fw = e->fmt.right - e->fmt.left;
    /* measured on the Date & Time fields: two digits sit 2 px into an 18-px field, one digit 5 */
    return (h->style & ES_CENTER) ? (fw - w + 1) / 2 : fw - w;
}

/* ------------------------------------------------------------------ single-line geometry (seg28) */
/* GetTextExtent in the edit's font: the advance widths plus the overhang, once per string */
static int text_ext(Edit *e, const char *s, int n)
{
    return n > 0 ? w16_text_width(e->f, s, n) + e->overhang : 0;
}

/* the single-line caret (seg28:1224): one pixel wide when the font's average character is
 * narrower than the system font's, else two; one pixel taller than the line */
static int sl_caret_w(Edit *e) { return e->avew < e->cxsys ? 1 : 2; }

/* ECCchInWidth (seg26:025C): how many characters of s[0..n) fit in w pixels, counted from the
 * start or, backward, from the end */
static int cch_in_width(Edit *e, const char *s, int n, int w, int forward)
{
    if (w <= 0 || n <= 0) return 0;
    if (e->fixed) return min(n, w / e->avew);
    if (e->pw) return min(n, w / e->pww);
    int lo = 0, hi = n + 1;
    while (hi - 1 > lo) {
        int mid = lo + max(1, (hi - lo) / 2);
        if (text_ext(e, forward ? s : s + n - mid, mid) > w) hi = mid;
        else lo = mid;
    }
    return lo;
}

/* SLIchToLeftXPos (seg28:0047): the x of a character position; the text starts at the formatting
 * rect's left edge less the overhang, so positions after the first are measured with
 * GetTextExtent */
static int sl_x(Edit *e, const char *t, int ich)
{
    if (ich >= e->scr && ich - e->scr > 1000) return 30000;
    if (ich < e->scr && e->scr - ich > 1000) return -30000;
    if (e->fixed) return (ich - e->scr) * e->avew + e->fmt.left;
    if (e->pw) return (ich - e->scr) * e->pww + e->fmt.left;
    int w;
    if (ich >= e->scr) {
        w = text_ext(e, t + e->scr, ich - e->scr);
        if (w < 0 || w > 31000) w = 30000;
    } else
        w = -text_ext(e, t + ich, e->scr - ich);
    return e->fmt.left - e->overhang + w;
}

/* SLMouseToIch (seg28:0EEE): left of the formatting rect, the character before the first visible
 * one; right of it, the one after the last; else the last position whose extent less half an
 * average character is left of x */
static int sl_hit(Edit *e, const char *t, int x)
{
    if (x <= e->fmt.left) return e->scr ? e->scr - 1 : 0;
    if (x > e->fmt.right) {
        int i = e->scr + cch_in_width(e, t + e->scr, e->len - e->scr, e->fmt.right - e->fmt.left, 1);
        return e->len <= i ? e->len : i + 1;
    }
    if (e->pw) { /* sic: counted from the start of the text, not from the first visible character */
        int n = (x - e->fmt.left) / e->pww;
        return n > e->len ? e->len : n;
    }
    if (e->len == 0) return 0;
    int lo = 0, hi = e->len - e->scr + 1;
    while (hi - 1 > lo) {
        int mid = lo + max(1, (hi - lo) / 2);
        if (text_ext(e, t + e->scr, mid) - e->avew / 2 > x - e->fmt.left) hi = mid;
        else lo = mid;
    }
    return lo + e->scr;
}

/* SLScrollText (seg28:061D), ES_AUTOHSCROLL only: a caret at or before the first visible character
 * brings a quarter of the width of the text before it into view; a caret past the right edge
 * scrolls three quarters of a width in, but never so far that the text ends before the edge */
static int sl_scroll(HWND h)
{
    Edit *e = ed(h);
    if (!(h->style & ES_AUTOHSCROLL)) return 0;
    char *t = txt(e);
    int fw = e->fmt.right - e->fmt.left, ns = e->scr;
    if (e->caret <= e->scr)
        ns = e->caret - cch_in_width(e, t, e->caret, fw / 4, 0);
    else {
        int n = cch_in_width(e, t + e->scr, e->caret - e->scr, fw, 0);
        if (e->caret - e->scr > n) {
            ns = e->caret - 3 * n / 4;
            int m = cch_in_width(e, t + e->scr, e->len - e->scr, fw, 0);
            if (e->len - m < ns) ns = e->len - m;
        }
    }
    untxt(e);
    if (ns == e->scr) return 0;
    e->scr = ns;
    return 1;
}

/* client coordinates of a character position */
static void pos_xy(HWND h, int pos, int *x, int *y)
{
    Edit *e = ed(h);
    char *t = txt(e);
    if (!e->multi) {
        *x = sl_x(e, t, pos);
        *y = e->fmt.top;
        untxt(e);
        return;
    }
    int l = line_of(e, pos);
    int w = seg_width(e, t, e->ls[l], pos) + line_indent(h, e, t, l);
    untxt(e);
    *x = e->fmt.left + w - e->xoff;
    *y = e->fmt.top + (l - e->top) * e->lh;
}

static int xy_pos(HWND h, int x, int y)
{
    Edit *e = ed(h);
    if (!e->multi) {
        char *t = txt(e);
        int p = sl_hit(e, t, x);
        untxt(e);
        return p;
    }
    int l = e->top + (y - e->fmt.top) / e->lh;
    if (y < e->fmt.top && e->multi) l = e->top - 1;
    if (l < 0) l = 0;
    if (l >= e->nl) l = e->nl - 1;
    char *t = txt(e);
    int s = e->ls[l], end = line_end(e, t, l);
    int target = x - e->fmt.left + e->xoff - line_indent(h, e, t, l), cx = 0, p = s;
    while (p < end) {
        int w = ch_w(e, t[p], cx);
        if (cx + w / 2 >= target) break;
        cx += w;
        p++;
    }
    untxt(e);
    return p;
}

/* ------------------------------------------------------------------ scroll bars */
static void update_sb(HWND h)
{
    Edit *e = ed(h);
    if (h->style & WS_VSCROLL) {
        int maxl = max(1, e->nl - 1);
        SetScrollRange(h, SB_VERT, 0, 100, FALSE);
        SetScrollPos(h, SB_VERT, e->nl <= 1 ? 0 : e->top * 100 / maxl, TRUE);
    }
    if (h->style & WS_HSCROLL) {
        int range = max(1, e->maxw);
        SetScrollRange(h, SB_HORZ, 0, 100, FALSE);
        SetScrollPos(h, SB_HORZ, min(100, e->xoff * 100 / range), TRUE);
    }
}

/* ------------------------------------------------------------------ painting */
static void draw_line(HWND h, HDC dc, int l, HBRUSH bg, char *t)
{
    Edit *e = ed(h);
    int y = e->fmt.top + (l - e->top) * e->lh;
    RECT lr = {e->fmt.left, y, e->fmt.right, y + e->lh};
    if (y >= e->fmt.bottom) return;
    if (lr.bottom > e->fmt.bottom && e->multi) lr.bottom = e->fmt.bottom;
    FillRect(dc, &lr, bg);
    if (l >= e->nl) return;
    int s = e->ls[l], end = line_end(e, t, l);
    int ss = smin(e), se = smax(e);
    int showsel = (e->focus || e->nohidesel) && ss != se;
    int x0 = e->fmt.left - e->xoff + line_indent(h, e, t, l);
    int sv = SaveDC(dc);
    IntersectClipRect(dc, lr.left, lr.top, lr.right, lr.bottom);
    COLORREF fg = GetTextColor(dc);
    int x = 0;
    for (int i = s; i < end;) {
        int sel = showsel && i >= ss && i < se;
        int j = i;
        while (j < end && (showsel && j >= ss && j < se) == sel) j++;
        /* draw run [i, j) */
        int rx = x;
        for (int k = i; k < j; k++) {
            int w = ch_w(e, t[k], x);
            if (sel) {
                RECT hr = {x0 + x, y, x0 + x + w, y + e->lh};
                FillRect(dc, &hr, w16_sys_brush(COLOR_HIGHLIGHT));
            }
            x += w;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : fg);
        int cx = rx;
        int st = i;
        for (int k = i; k <= j; k++) {
            if (k == j || (t[k] == '\t' && e->multi)) {
                if (k > st) {
                    if (e->pw) {
                        char buf[256];
                        int n = min(k - st, 255);
                        memset(buf, e->pw, n);
                        TextOut(dc, x0 + cx, y, buf, n);
                    } else
                        TextOut(dc, x0 + cx, y, t + st, k - st);
                    cx += seg_width(e, t, st, k);
                }
                if (k < j) cx = tab_stop(e, cx);
                st = k + 1;
            }
        }
        i = j;
    }
    /* selected line break: 3.1 highlights nothing past the text */
    SetTextColor(dc, fg);
    RestoreDC(dc, sv);
}

/* DrawFrame(hdc, rc, 1, DF_WINDOWFRAME): the edit's own border, in the window-frame colour */
static void draw_frame(HDC dc, const RECT *r)
{
    int cx = GetSystemMetrics(SM_CXBORDER), cy = GetSystemMetrics(SM_CYBORDER);
    HBRUSH b = w16_sys_brush(COLOR_WINDOWFRAME);
    RECT s;
    SetRect(&s, r->left, r->top, r->right, r->top + cy); FillRect(dc, &s, b);
    SetRect(&s, r->left, r->bottom - cy, r->right, r->bottom); FillRect(dc, &s, b);
    SetRect(&s, r->left, r->top + cy, r->left + cx, r->bottom - cy); FillRect(dc, &s, b);
    SetRect(&s, r->right - cx, r->top + cy, r->right, r->bottom - cy); FillRect(dc, &s, b);
}

/* ECSetEditClip (seg26:0A36): the client rect, inset as a single-line formatting rect is when the
 * edit has a border; a multi-line edit's is also cut to its formatting rect */
static void edit_clip(HWND h, HDC dc)
{
    Edit *e = ed(h);
    RECT r;
    GetClientRect(h, &r);
    if (e->border) InflateRect(&r, -(min(e->avew, e->cxsys) / 2), -(min(e->lh, e->cysys) / 4));
    if (e->multi) IntersectRect(&r, &r, &e->fmt);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
}

/* SLDrawLine (seg28:0280): characters [ich, ich + n). The run's rect starts at the formatting
 * rect's left edge plus the extent of the visible text before it less the overhang, spans the
 * run's own extent (overhang included) and is filled a pixel taller above and below (the clip trims
 * it) before the text is drawn opaque at its top left. */
static void sl_draw_line(HWND h, HDC dc, const char *t, int ich, int n, int sel)
{
    Edit *e = ed(h);
    if (ich < e->scr) {
        if (ich + n < e->scr) return;
        n -= e->scr - ich;
        ich = e->scr;
    }
    RECT r = e->fmt;
    if (ich > e->scr) r.left += e->pw ? e->pww * (ich - e->scr) : text_ext(e, t + e->scr, ich - e->scr) - e->overhang;
    r.right = r.left + (e->pw ? e->pww * n : text_ext(e, t + ich, n));
    SetBkMode(dc, OPAQUE);
    HBRUSH br;
    COLORREF otext = 0, obk = 0;
    int restore = 0;
    if (sel) {
        br = w16_sys_brush(COLOR_HIGHLIGHT);
        obk = SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        otext = SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        restore = 1;
    } else
        br = w16_ctl_color(h, dc, CTLCOLOR_EDIT);
    if (h->style & WS_DISABLED) {
        COLORREF g = GetSysColor(COLOR_GRAYTEXT);
        if (g) { otext = SetTextColor(dc, g); restore = 1; }
    }
    InflateRect(&r, 0, 1);
    FillRect(dc, &r, br);
    InflateRect(&r, 0, -1);
    if (e->pw)
        for (int i = 0; i < n; i++) TextOut(dc, r.left + i * e->pww, r.top, &e->pw, 1);
    else
        TextOut(dc, r.left, r.top, t + ich, n);
    if (restore) SetTextColor(dc, otext);
    if (sel) SetBkColor(dc, obk);
}

/* SLDrawText (seg28:04E1): the characters that fit, in runs of one selection state (no selection
 * shows without the focus unless ES_NOHIDESEL), then the rest of the formatting rect, a pixel
 * taller above and below, in the control's brush */
static void sl_draw_text(HWND h, HDC dc)
{
    Edit *e = ed(h);
    if (!w16_window_visible(h)) return;
    int sv = SaveDC(dc);
    edit_clip(h, dc);
    char *t = txt(e);
    int n = cch_in_width(e, t + e->scr, e->len - e->scr, e->fmt.right - e->fmt.left, 1);
    int end = e->scr + n, ss = smin(e), se = smax(e);
    int nosel = ss == se || (!e->focus && !e->nohidesel);
    for (int i = e->scr; i < end;) {
        int sel = 0, k = end;
        if (!nosel) {
            if (i < ss) k = min(ss, end);
            else if (i < se) { sel = 1; k = min(se, end); }
        }
        sl_draw_line(h, dc, t, i, k - i, sel);
        i = k;
    }
    RECT r = e->fmt;
    if (n) r.left += e->pw ? e->pww * n : text_ext(e, t + e->scr, n);
    untxt(e);
    if (r.right > r.left) {
        SetBkMode(dc, OPAQUE);
        InflateRect(&r, 0, 1);
        FillRect(dc, &r, w16_ctl_color(h, dc, CTLCOLOR_EDIT));
    }
    RestoreDC(dc, sv);
}

/* SLPaint (seg28:1151): the client in the control's brush, the frame, the text */
static void sl_paint(HWND h, HDC dc)
{
    Edit *e = ed(h);
    HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
    RECT r;
    GetClientRect(h, &r);
    FillRect(dc, &r, w16_ctl_color(h, dc, CTLCOLOR_EDIT));
    if (e->border) draw_frame(dc, &r);
    sl_draw_text(h, dc);
    SelectObject(dc, of);
}

static void ml_paint(HWND h, HDC dc)
{
    Edit *e = ed(h);
    HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
    HBRUSH bg = w16_ctl_color(h, dc, CTLCOLOR_EDIT);
    RECT r;
    GetClientRect(h, &r);
    /* margins around the formatting rectangle */
    RECT m;
    SetRect(&m, r.left, r.top, r.right, e->fmt.top); FillRect(dc, &m, bg);
    SetRect(&m, r.left, e->fmt.top, e->fmt.left, r.bottom); FillRect(dc, &m, bg);
    SetRect(&m, e->fmt.right, e->fmt.top, r.right, r.bottom); FillRect(dc, &m, bg);
    SetRect(&m, e->fmt.left, e->fmt.bottom, e->fmt.right, r.bottom); FillRect(dc, &m, bg);
    /* MLPaint (seg30:0ED8) frames the window rect, so scroll bars cover the frame's edge */
    if (e->border) {
        RECT w = {0, 0, h->rw.right - h->rw.left, h->rw.bottom - h->rw.top};
        draw_frame(dc, &w);
    }
    int sv = SaveDC(dc);
    edit_clip(h, dc);
    if (h->style & WS_DISABLED) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    char *t = txt(e);
    int n = vis_lines(e) + 1;
    for (int i = 0; i < n; i++) draw_line(h, dc, e->top + i, bg, t);
    untxt(e);
    RestoreDC(dc, sv);
    SelectObject(dc, of);
}

static void paint(HWND h, HDC dc)
{
    if (ed(h)->multi) ml_paint(h, dc);
    else sl_paint(h, dc);
}

static void redraw(HWND h)
{
    if (!w16_window_visible(h) || h->redraw_off) return;
    HideCaret(h);
    HDC dc = GetDC(h);
    paint(h, dc);
    ReleaseDC(h, dc);
    ShowCaret(h);
}

static void place_caret(HWND h)
{
    Edit *e = ed(h);
    if (!e->focus) return;
    int x, y;
    if (e->multi && e->nofmt) { SetCaretPos(-20000, -20000); return; }
    pos_xy(h, e->caret, &x, &y);
    /* SLSetCaretPosition (seg28:0000): never past the formatting rect's right edge */
    if (!e->multi) x = min(x, e->fmt.right - sl_caret_w(e));
    SetCaretPos(x, y);
}

/* make the caret visible by scrolling */
static int ensure_visible(HWND h)
{
    Edit *e = ed(h);
    if (!e->multi) return sl_scroll(h);
    int changed = 0;
    int l = line_of(e, e->caret);
    if (e->multi) {
        int vl = vis_lines(e);
        if (l < e->top) { e->top = l; changed = 1; }
        else if (l >= e->top + vl) { e->top = l - vl + 1; changed = 1; }
    }
    int fw = e->fmt.right - e->fmt.left;
    if (!e->wrap && (h->style & ES_AUTOHSCROLL || e->multi)) {
        char *t = txt(e);
        int cx = seg_width(e, t, e->ls[l], e->caret);
        untxt(e);
        if (cx < e->xoff) { e->xoff = max(0, cx - fw / 3); changed = 1; }
        else if (cx > e->xoff + fw - 1) { e->xoff = cx - fw + fw / 3; changed = 1; }
        if (e->xoff < 0) e->xoff = 0;
    }
    if (changed) {
        if (e->multi) notify(h, EN_VSCROLL);
        update_sb(h);
    }
    return changed;
}

static void refresh(HWND h, int text_changed)
{
    Edit *e = ed(h);
    if (text_changed) build_lines(h);
    if (e->top > max(0, e->nl - 1)) e->top = max(0, e->nl - 1);
    ensure_visible(h);
    if (text_changed) notify(h, EN_UPDATE);
    redraw(h);
    place_caret(h);
    update_sb(h);
    if (text_changed) notify(h, EN_CHANGE);
}

/* ------------------------------------------------------------------ editing */
static void save_undo(Edit *e)
{
    char *t = txt(e);
    if (!e->undo) e->undo = LocalAlloc(LMEM_MOVEABLE, e->len + 1);
    else e->undo = LocalReAlloc(e->undo, e->len + 1, LMEM_MOVEABLE);
    memcpy(LocalLock(e->undo), t, e->len + 1);
    LocalUnlock(e->undo);
    untxt(e);
    e->undo_len = e->len;
    e->undo_a = e->anchor;
    e->undo_c = e->caret;
}

static void do_undo(HWND h)
{
    Edit *e = ed(h);
    if (!e->undo) return;
    HLOCAL cur = e->hbuf;
    int len = e->len, a = e->anchor, c = e->caret;
    e->hbuf = e->undo;
    e->len = e->undo_len;
    e->anchor = e->undo_a;
    e->caret = e->undo_c;
    e->undo = cur;
    e->undo_len = len;
    e->undo_a = a;
    e->undo_c = c;
    e->undo_run = 0;
    e->modified = 1;
    refresh(h, 1);
}

/* replace selection with s[0..n); returns 0 if the limit was hit */
static int replace_sel(HWND h, const char *s, int n, int undoable)
{
    Edit *e = ed(h);
    int a = smin(e), b = smax(e), want = n;
    if (e->len - (b - a) + n > e->limit) {
        n = e->limit - (e->len - (b - a));
        if (n < 0) n = 0;
    }
    if (!e->multi && !(h->style & ES_AUTOHSCROLL) && n) {
        /* SLInsertText (seg28:0719): without ES_AUTOHSCROLL, only what fits in the formatting rect
         * beside the rest of the text goes in */
        char *t = txt(e);
        int rest = e->len - (b - a), w = 0;
        if (e->pw) w = rest * e->pww;
        else if (rest) w = w16_text_width(e->f, t, a) + w16_text_width(e->f, t + b, e->len - b) + e->overhang;
        untxt(e);
        n = cch_in_width(e, s, n, e->fmt.right - e->fmt.left - w, 1);
    }
    if (n < want) {
        notify(h, EN_MAXTEXT);
        if (n == 0 && a == b) { MessageBeep(0); return 0; }
    }
    if (undoable) save_undo(e);
    int nl = e->len - (b - a) + n;
    if ((UINT)(nl + 1) > LocalSize(e->hbuf)) {
        HLOCAL nh = LocalReAlloc(e->hbuf, nl + 256, LMEM_MOVEABLE);
        if (!nh) { notify(h, EN_ERRSPACE); return 0; }
        e->hbuf = nh;
    }
    char *t = txt(e);
    memmove(t + a + n, t + b, e->len - b + 1);
    memcpy(t + a, s, n);
    if (h->style & ES_UPPERCASE) for (int i = 0; i < n; i++) t[a + i] = (char)(uintptr_t)AnsiUpper((LPSTR)(uintptr_t)(unsigned char)t[a + i]);
    if (h->style & ES_LOWERCASE) for (int i = 0; i < n; i++) t[a + i] = (char)(uintptr_t)AnsiLower((LPSTR)(uintptr_t)(unsigned char)t[a + i]);
    untxt(e);
    e->len = nl;
    e->anchor = e->caret = a + n;
    e->modified = 1;
    refresh(h, 1);
    return 1;
}

static void set_text(HWND h, const char *s)
{
    Edit *e = ed(h);
    int n = s ? strlen(s) : 0;
    if (n > e->limit && !e->multi) n = e->limit;
    if ((UINT)(n + 1) > LocalSize(e->hbuf)) e->hbuf = LocalReAlloc(e->hbuf, n + 256, LMEM_MOVEABLE);
    char *t = txt(e);
    memcpy(t, s ? s : "", n);
    t[n] = 0;
    untxt(e);
    e->len = n;
    e->anchor = e->caret = 0;
    e->top = 0;
    e->xoff = e->scr = 0;
    e->modified = 0;
    if (e->undo) { LocalFree(e->undo); e->undo = NULL; }
    refresh(h, 1);
}

static int is_word(char c) { return IsCharAlphaNumeric(c) || c == '_' || (unsigned char)c >= 0x80; }

static int word_left(Edit *e, const char *t, int p)
{
    if (p > 0) p--;
    while (p > 0 && !is_word(t[p])) p--;
    while (p > 0 && is_word(t[p - 1])) p--;
    (void)e;
    return p;
}
static int word_right(Edit *e, const char *t, int p)
{
    while (p < e->len && is_word(t[p])) p++;
    while (p < e->len && !is_word(t[p]) && t[p] != '\r') p++;
    if (p < e->len && t[p] == '\r' && p + 1 < e->len && t[p + 1] == '\n' && p == e->caret) p += 2;
    return p;
}

static void move_caret(HWND h, int pos, int extend)
{
    Edit *e = ed(h);
    if (pos < 0) pos = 0;
    if (pos > e->len) pos = e->len;
    /* never land inside a CRLF pair */
    char *t = txt(e);
    if (pos > 0 && pos < e->len && t[pos - 1] == '\r' && t[pos] == '\n') pos--;
    untxt(e);
    int had_sel = e->anchor != e->caret;
    e->caret = pos;
    if (!extend) e->anchor = pos;
    e->undo_run = 0;
    int scrolled = ensure_visible(h);
    if (scrolled || had_sel || extend) redraw(h);
    place_caret(h);
}

static void copy_sel(HWND h)
{
    Edit *e = ed(h);
    int a = smin(e), b = smax(e);
    if (a == b || e->pw) return;
    HGLOBAL g = GlobalAlloc(GHND, b - a + 1);
    char *p = GlobalLock(g);
    char *t = txt(e);
    memcpy(p, t + a, b - a);
    untxt(e);
    GlobalUnlock(g);
    if (OpenClipboard(h)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, g);
        CloseClipboard();
    }
}

static void paste(HWND h)
{
    if (!OpenClipboard(h)) return;
    HANDLE g = GetClipboardData(CF_TEXT);
    if (g) {
        char *p = GlobalLock(g);
        if (p) {
            int n = strlen(p);
            if (!ed(h)->multi) { const char *cr = strpbrk(p, "\r\n"); if (cr) n = cr - p; }
            replace_sel(h, p, n, 1);
        }
        GlobalUnlock(g);
    }
    CloseClipboard();
}

/* ------------------------------------------------------------------ geometry */
/* ECSetFont (seg27:02C9): line height, USER's average width, overhang and pitch of the font; the
 * system font (hFont 0, as every edit gets at creation) also sets the system metrics the border
 * insets use */
static void set_font(HWND h, HFONT f)
{
    Edit *e = ed(h);
    h->font = f;
    HDC dc = GetDC(h);
    SelectObject(dc, f ? f : GetStockObject(SYSTEM_FONT));
    e->f = w16_dc_font(dc);
    TEXTMETRIC tm;
    e->avew = w16_ave_char_width(dc, &tm);
    ReleaseDC(h, dc);
    e->lh = tm.tmHeight;
    e->overhang = tm.tmOverhang;
    e->fixed = !(tm.tmPitchAndFamily & 1);
    if (!f) { e->cxsys = e->avew; e->cysys = e->lh; }
    e->avgw = tm.tmAveCharWidth - (e->f->bold_sim ? 1 : 0);
    if (e->avgw < 1) e->avgw = 1;
    /* ECSetPasswordChar: the extent of the character, at least 1 */
    if (e->pw) e->pww = max(1, text_ext(e, &e->pw, 1));
}

/* SLSize (seg29:0000) and MLSize (seg30:1FF9): the client rect, inset when the edit has a border
 * by half an average character and a quarter of a line - for a single-line edit no more than the
 * system font's, for a multi-line edit the system font's own. A single-line rect is at most one
 * line tall; a multi-line rect is a whole number of lines, and one too small for a character and
 * a line stays as it was with the caret hidden. A borderless multi-line edit (Notepad's) formats
 * right at its client edge. */
static void calc_fmt(HWND h)
{
    Edit *e = ed(h);
    RECT r;
    GetClientRect(h, &r);
    if (r.left == r.right || r.top == r.bottom) {
        if (e->fmt.left != e->fmt.right) {
            if (e->multi) e->nofmt = 1;
            return;
        }
        SetRect(&r, 0, 0, e->multi ? e->avew * 10 : 10, e->multi ? e->lh : 10);
    }
    if (e->multi) {
        if (e->border) InflateRect(&r, -(e->cxsys / 2), -(e->cysys / 4));
        int n = (r.bottom - r.top) / e->lh;
        if (r.right - r.left < e->avew || n == 0) { e->nofmt = 1; return; }
        e->nofmt = 0;
        r.bottom = r.top + n * e->lh;
    } else {
        if (e->border) InflateRect(&r, -(min(e->avew, e->cxsys) / 2), -(min(e->lh, e->cysys) / 4));
        r.bottom = min(r.top + e->lh, r.bottom);
    }
    e->fmt = r;
}

/* ------------------------------------------------------------------ window procedure */
static UINT edit_timer_id = 0x7EED;

LRESULT w16_edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Edit *e = ed(h);
    switch (m) {
    case WM_NCCREATE: {
        e = calloc(1, sizeof *e);
        h->ctl = e;
        e->multi = (h->style & ES_MULTILINE) != 0;
        e->wrap = e->multi && !(h->style & ES_AUTOHSCROLL) && !(h->style & WS_HSCROLL);
        e->limit = e->multi ? 0x7FFFFFFF : 30000;
        e->hbuf = LocalAlloc(LMEM_MOVEABLE | LMEM_ZEROINIT, 256);
        e->pw = (h->style & ES_PASSWORD) && !e->multi ? '*' : 0;
        e->nohidesel = (h->style & ES_NOHIDESEL) != 0;
        /* seg27:0056: a WS_BORDER edit takes the style off and draws its own frame inside its
         * client area, so its insets count from the window's edge */
        if (h->style & WS_BORDER) { e->border = 1; h->style &= ~WS_BORDER; }
        if (!e->multi && (h->style & W16_ES_COMBOBOX)) e->combo = h->parent;
        set_font(h, NULL);
        DefWindowProc(h, m, wp, lp);
        return TRUE;
    }
    case WM_CREATE: {
        calc_fmt(h);
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        e->quiet = 1; /* the creation text is not a change the parent hears about */
        set_text(h, cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName) ? cs->lpszName : "");
        e->quiet = 0;
        if (e->multi) {
            if (h->style & WS_VSCROLL) SetScrollRange(h, SB_VERT, 0, 100, FALSE);
            if (h->style & WS_HSCROLL) SetScrollRange(h, SB_HORZ, 0, 100, FALSE);
        }
        return 0;
    }
    case WM_DESTROY:
        if (e->focus) DestroyCaret();
        return 0;
    case WM_NCDESTROY:
        if (e) {
            LocalFree(e->hbuf);
            if (e->undo) LocalFree(e->undo);
            free(e->ls);
            free(e);
            h->ctl = NULL;
        }
        return 0;
    case WM_SIZE:
        calc_fmt(h);
        build_lines(h);
        update_sb(h);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT:
        set_font(h, (HFONT)wp);
        calc_fmt(h);
        build_lines(h);
        /* ECSetFont: a focused edit gets a 2-px caret one line tall, single-line ones too */
        if (e->focus) { DestroyCaret(); CreateCaret(h, NULL, 2, e->lh); place_caret(h); ShowCaret(h); }
        if (lp) InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_SETTEXT: set_text(h, (const char *)lp); return TRUE;
    case WM_GETTEXT: {
        int n = (int)wp;
        if (n <= 0) return 0;
        char *t = txt(e);
        int c = min(n - 1, e->len);
        memcpy((char *)lp, t, c);
        ((char *)lp)[c] = 0;
        untxt(e);
        return c;
    }
    case WM_GETTEXTLENGTH: return e->len;
    case WM_GETDLGCODE: {
        LRESULT c = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
        if (e->multi && (h->style & ES_WANTRETURN)) c |= DLGC_WANTALLKEYS;
        if (lp && e->multi) {
            MSG *msg = (MSG *)lp;
            if (msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN && (w16_keystate[VK_CONTROL] & 0x80)) c |= DLGC_WANTALLKEYS;
        }
        return c;
    }
    case WM_SETFOCUS:
        e->focus = 1;
        if (e->multi) CreateCaret(h, NULL, 2, e->lh);              /* seg30:1DBF */
        else CreateCaret(h, NULL, sl_caret_w(e), e->lh + 1);      /* seg28:1224 */
        place_caret(h);
        ShowCaret(h);
        if (e->anchor != e->caret && !e->nohidesel) redraw(h);
        notify(h, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        e->focus = 0;
        HideCaret(h);
        DestroyCaret();
        if (e->anchor != e->caret && !e->nohidesel) redraw(h);
        notify(h, EN_KILLFOCUS);
        return 0;
    case WM_ENABLE: InvalidateRect(h, NULL, TRUE); return 0;
    case WM_LBUTTONDOWN: {
        if (!e->focus) SetFocus(h);
        int p = xy_pos(h, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
        move_caret(h, p, (wp & MK_SHIFT) != 0);
        e->mdown = 1;
        SetCapture(h);
        e->timer = SetTimer(h, edit_timer_id, 100, NULL);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        char *t = txt(e);
        int p = xy_pos(h, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
        int a = p, b = p;
        if (p < e->len && is_word(t[p])) { while (a > 0 && is_word(t[a - 1])) a--; while (b < e->len && is_word(t[b])) b++; while (b < e->len && t[b] == ' ') b++; }
        untxt(e);
        e->anchor = a;
        e->caret = b;
        ensure_visible(h);
        redraw(h);
        place_caret(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->mdown) {
            int p = xy_pos(h, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
            if (p != e->caret) move_caret(h, p, 1);
        }
        return 0;
    case WM_TIMER:
        if (wp == edit_timer_id && e->mdown) {
            POINT p = w16_mouse;
            ScreenToClient(h, &p);
            RECT r;
            GetClientRect(h, &r);
            if (!PtInRect(&r, p)) {
                int np = xy_pos(h, p.x, p.y);
                if (e->multi && p.y >= r.bottom) np = e->ls[min(e->nl - 1, line_of(e, e->caret) + 1)];
                if (e->multi && p.y < r.top) np = e->ls[max(0, line_of(e, e->caret) - 1)];
                move_caret(h, np, 1);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->mdown) { e->mdown = 0; ReleaseCapture(); KillTimer(h, edit_timer_id); }
        return 0;
    case WM_KEYDOWN: {
        int shift = (w16_keystate[VK_SHIFT] & 0x80) != 0, ctrl = (w16_keystate[VK_CONTROL] & 0x80) != 0;
        if (!e->multi) {
            /* SLKeyDown (seg28:0A93): a combo box's edit hands F4, Page Up/Down and Up/Down to the
             * combo's list; elsewhere Up and Down move as Left and Right, Page Up/Down do nothing */
            if (e->combo && (wp == VK_F4 || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN))
                return SendMessage(e->combo, WM_KEYDOWN, wp, 0);
            if (wp == VK_UP) wp = VK_LEFT;
            else if (wp == VK_DOWN) wp = VK_RIGHT;
            else if (wp == VK_PRIOR || wp == VK_NEXT) return 0;
        }
        char *t = txt(e);
        int l = line_of(e, e->caret);
        int np = -1;
        switch (wp) {
        case VK_LEFT: np = ctrl ? word_left(e, t, e->caret) : (!shift && e->anchor != e->caret ? smin(e) : e->caret - 1);
            if (!ctrl && np > 0 && t[np] == '\n' && t[np - 1] == '\r') np--;
            break;
        case VK_RIGHT: np = ctrl ? word_right(e, t, e->caret) : (!shift && e->anchor != e->caret ? smax(e) : e->caret + 1);
            if (!ctrl && np < e->len && np > 0 && t[np - 1] == '\r' && t[np] == '\n') np++;
            break;
        case VK_HOME: np = ctrl ? 0 : e->ls[l]; break;
        case VK_END: np = ctrl ? e->len : line_end(e, t, l); break;
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
            if (!e->multi) { if (wp == VK_UP || wp == VK_PRIOR) np = 0; else np = e->len; break; }
            {
                int x, y;
                untxt(e);
                pos_xy(h, e->caret, &x, &y);
                t = txt(e);
                int dl = wp == VK_UP ? -1 : wp == VK_DOWN ? 1 : wp == VK_PRIOR ? -vis_lines(e) : vis_lines(e);
                int nl2 = l + dl;
                if (nl2 < 0) nl2 = 0;
                if (nl2 >= e->nl) nl2 = e->nl - 1;
                if (wp == VK_PRIOR || wp == VK_NEXT) {
                    e->top += nl2 - l;
                    if (e->top > e->nl - vis_lines(e)) e->top = max(0, e->nl - vis_lines(e));
                    if (e->top < 0) e->top = 0;
                }
                untxt(e);
                np = xy_pos(h, x, e->fmt.top + (nl2 - e->top) * e->lh);
                t = txt(e);
            }
            break;
        case VK_DELETE:
            untxt(e);
            if (h->style & ES_READONLY) return 0;
            if (shift) { copy_sel(h); replace_sel(h, "", 0, 1); return 0; }
            if (e->anchor == e->caret) {
                if (e->caret >= e->len) return 0;
                t = txt(e);
                int n = (t[e->caret] == '\r' && e->caret + 1 < e->len && t[e->caret + 1] == '\n') ? 2 : 1;
                untxt(e);
                e->anchor = e->caret + n;
            }
            replace_sel(h, "", 0, 1);
            return 0;
        case VK_INSERT:
            untxt(e);
            if (ctrl) copy_sel(h);
            else if (shift && !(h->style & ES_READONLY)) paste(h);
            return 0;
        default:
            untxt(e);
            return 0;
        }
        untxt(e);
        if (np >= 0) move_caret(h, np, shift);
        return 0;
    }
    case WM_CHAR: {
        int c = (int)wp;
        if (h->style & ES_READONLY) { if (c == 3) copy_sel(h); return 0; }
        switch (c) {
        case 3: copy_sel(h); return 0;                                    /* ^C */
        case 22: paste(h); return 0;                                      /* ^V */
        case 24: copy_sel(h); replace_sel(h, "", 0, 1); return 0;         /* ^X */
        case 26: do_undo(h); return 0;                                    /* ^Z */
        case 8:
            if (e->anchor == e->caret) {
                if (e->caret == 0) { MessageBeep(0); return 0; }
                char *t = txt(e);
                int n = (e->caret >= 2 && t[e->caret - 1] == '\n' && t[e->caret - 2] == '\r') ? 2 : 1;
                untxt(e);
                e->anchor = e->caret - n;
            }
            if (!e->undo_run) save_undo(e);
            e->undo_run = 1;
            replace_sel(h, "", 0, 0);
            return 0;
        case '\r': case '\n':
            if (!e->multi) return 0;
            if (!e->undo_run) save_undo(e);
            e->undo_run = 1;
            replace_sel(h, "\r\n", 2, 0);
            return 0;
        case '\t':
            if (!e->multi) return 0;
            /* fall through */
        default:
            if (c < 32 && c != '\t') return 0;
            {
                char ch = (char)c;
                if (h->style & ES_OEMCONVERT) { /* round-trip through the OEM code page: identity here */ }
                if (!e->undo_run) save_undo(e);
                e->undo_run = 1;
                replace_sel(h, &ch, 1, 0);
            }
            return 0;
        }
    }
    case WM_VSCROLL: {
        if (!e->multi) return 0;
        int vl = vis_lines(e), ot = e->top;
        switch (LOWORD(wp)) {
        case SB_LINEUP: e->top--; break;
        case SB_LINEDOWN: e->top++; break;
        case SB_PAGEUP: e->top -= max(1, vl - 1); break;
        case SB_PAGEDOWN: e->top += max(1, vl - 1); break;
        case SB_TOP: e->top = 0; break;
        case SB_BOTTOM: e->top = e->nl - vl; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK:
            e->top = (int)((long)LOWORD(lp) * max(0, e->nl - 1) / 100);
            break;
        default: return 0;
        }
        if (e->top > e->nl - 1) e->top = e->nl - 1;
        if (e->top < 0) e->top = 0;
        if (e->top != ot) {
            HideCaret(h);
            ScrollWindow(h, 0, (ot - e->top) * e->lh, &e->fmt, &e->fmt);
            UpdateWindow(h);
            place_caret(h);
            ShowCaret(h);
            notify(h, EN_VSCROLL);
        }
        if (LOWORD(wp) != SB_THUMBTRACK) update_sb(h);
        return 0;
    }
    case WM_HSCROLL: {
        int fw = e->fmt.right - e->fmt.left, ox = e->xoff;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: e->xoff -= e->avgw; break;
        case SB_LINERIGHT: e->xoff += e->avgw; break;
        case SB_PAGELEFT: e->xoff -= fw / 3 * 2; break;
        case SB_PAGERIGHT: e->xoff += fw / 3 * 2; break;
        case SB_LEFT: e->xoff = 0; break;
        case SB_RIGHT: e->xoff = e->maxw; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: e->xoff = (int)((long)LOWORD(lp) * e->maxw / 100); break;
        default: return 0;
        }
        if (e->xoff > e->maxw) e->xoff = e->maxw;
        if (e->xoff < 0) e->xoff = 0;
        if (ox != e->xoff) { redraw(h); place_caret(h); notify(h, EN_HSCROLL); }
        if (LOWORD(wp) != SB_THUMBTRACK) update_sb(h);
        return 0;
    }
    case WM_CUT: copy_sel(h); replace_sel(h, "", 0, 1); return 0;
    case WM_COPY: copy_sel(h); return 0;
    case WM_PASTE: paste(h); return 0;
    case WM_CLEAR: replace_sel(h, "", 0, 1); return 0;
    case WM_UNDO: case EM_UNDO: do_undo(h); return TRUE;
    case EM_CANUNDO: return e->undo != NULL;
    case EM_EMPTYUNDOBUFFER: if (e->undo) LocalFree(e->undo); e->undo = NULL; return 0;
    case EM_GETSEL: return MAKELONG(smin(e), smax(e));
    case EM_SETSEL: {
        int a = (SHORT)LOWORD(lp), b = (SHORT)HIWORD(lp);
        if (a < 0 || a > e->len) a = e->len;
        if (b < 0 || b > e->len) b = e->len;
        if (LOWORD(lp) == 0 && HIWORD(lp) == 0x7FFF) b = e->len;
        e->anchor = a;
        e->caret = b;
        if (!wp) ensure_visible(h);
        redraw(h);
        place_caret(h);
        return TRUE;
    }
    case EM_REPLACESEL: {
        const char *s = (const char *)lp;
        replace_sel(h, s ? s : "", s ? strlen(s) : 0, 1);
        return 0;
    }
    case EM_GETMODIFY: return e->modified;
    case EM_SETMODIFY: e->modified = (int)wp; return 0;
    case EM_GETLINECOUNT: return e->nl;
    case EM_LINEINDEX: {
        int l = (int)(SHORT)wp;
        if (l < 0) l = line_of(e, e->caret);
        return l < e->nl ? e->ls[l] : -1;
    }
    case EM_LINEFROMCHAR: {
        int p = (int)(SHORT)wp;
        if (p < 0) p = smin(e);
        return line_of(e, p);
    }
    case EM_LINELENGTH: {
        int p = (int)(SHORT)wp;
        char *t = txt(e);
        int r;
        if (p < 0) { int a = line_of(e, smin(e)), b = line_of(e, smax(e)); r = (smin(e) - e->ls[a]) + (line_end(e, t, b) - smax(e)); }
        else { int l = line_of(e, p); r = line_end(e, t, l) - e->ls[l]; }
        untxt(e);
        return r;
    }
    case EM_GETLINE: {
        int l = (int)wp;
        if (l >= e->nl) return 0;
        char *t = txt(e);
        char *buf = (char *)lp;
        int cb = *(WORD *)buf;
        int n = min(cb, line_end(e, t, l) - e->ls[l]);
        memcpy(buf, t + e->ls[l], n);
        untxt(e);
        return n;
    }
    case EM_LIMITTEXT: e->limit = wp ? (int)wp : (e->multi ? 0x7FFFFFFF : 30000); return 0;
    case EM_GETHANDLE: return (LRESULT)e->hbuf;
    case EM_SETHANDLE: {
        HLOCAL nh = (HLOCAL)wp;
        if (!nh) return 0;
        e->hbuf = nh;
        char *t = txt(e);
        e->len = strlen(t);
        untxt(e);
        e->anchor = e->caret = 0;
        e->top = 0;
        e->xoff = e->scr = 0;
        e->modified = 0;
        if (e->undo) { LocalFree(e->undo); e->undo = NULL; }
        refresh(h, 1);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case EM_FMTLINES: {
        /* wp TRUE: insert soft breaks (\r\r\n) at wrap points; FALSE: remove them */
        char *t = txt(e);
        int n = 0;
        char *out = malloc(e->len + e->nl * 3 + 1);
        for (int i = 0; i < e->len; i++) {
            if (t[i] == '\r' && i + 2 < e->len && t[i + 1] == '\r' && t[i + 2] == '\n') { i += 2; continue; }
            out[n++] = t[i];
        }
        if (wp && e->wrap) {
            /* rebuild with soft breaks at each wrapped line end */
            char *o2 = malloc(n + e->nl * 3 + 1);
            int k = 0;
            for (int l = 0; l < e->nl; l++) {
                int s = e->ls[l], end = (l + 1 < e->nl) ? e->ls[l + 1] : e->len;
                for (int i = s; i < end; i++) {
                    if (t[i] == '\r' && i + 2 < e->len && t[i + 1] == '\r' && t[i + 2] == '\n') { i += 2; continue; }
                    o2[k++] = t[i];
                }
                int hard = end >= 2 && t[end - 1] == '\n' && t[end - 2] == '\r';
                if (l + 1 < e->nl && !hard) { o2[k++] = '\r'; o2[k++] = '\r'; o2[k++] = '\n'; }
            }
            free(out);
            out = o2;
            n = k;
        }
        untxt(e);
        if ((UINT)(n + 1) > LocalSize(e->hbuf)) e->hbuf = LocalReAlloc(e->hbuf, n + 256, LMEM_MOVEABLE);
        t = txt(e);
        memcpy(t, out, n);
        t[n] = 0;
        untxt(e);
        free(out);
        e->len = n;
        if (e->caret > n) e->caret = n;
        if (e->anchor > n) e->anchor = n;
        build_lines(h);
        return wp;
    }
    case EM_GETRECT: *(RECT *)lp = e->fmt; return 0;
    case EM_SETRECT: case EM_SETRECTNP:
        e->fmt = *(RECT *)lp;
        build_lines(h);
        if (m == EM_SETRECT) InvalidateRect(h, NULL, TRUE);
        return 0;
    case EM_SCROLL: SendMessage(h, WM_VSCROLL, wp, 0); return 0;
    case EM_LINESCROLL: {
        int dl = (SHORT)LOWORD(lp), dc2 = (SHORT)HIWORD(lp);
        e->top = max(0, min(e->nl - 1, e->top + dl));
        e->xoff = max(0, e->xoff + dc2 * e->avgw);
        redraw(h);
        place_caret(h);
        update_sb(h);
        return TRUE;
    }
    case EM_GETFIRSTVISIBLELINE: return e->top;
    case EM_SETREADONLY:
        if (wp) h->style |= ES_READONLY; else h->style &= ~ES_READONLY;
        return TRUE;
    case EM_SETTABSTOPS: {
        int n = (int)wp;
        const int *v = (const int *)lp;
        HWND p = h->parent;
        W16Dialog *d = p ? w16_dlg(p) : NULL;
        int cx = d ? d->cxchar : e->avgw;
        if (n == 0) { e->ntabs = 0; }
        else {
            e->ntabs = min(n, 64);
            for (int i = 0; i < e->ntabs; i++) e->tabs[i] = v[i] * cx / 4;
        }
        build_lines(h);
        return TRUE;
    }
    case EM_SETPASSWORDCHAR:
        e->pw = (char)wp;
        if (e->pw) e->pww = max(1, text_ext(e, &e->pw, 1));
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case EM_GETPASSWORDCHAR: return (unsigned char)e->pw;
    case EM_GETTHUMB: return GetScrollPos(h, SB_VERT);
    case EM_SETWORDBREAK: case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC: return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}
