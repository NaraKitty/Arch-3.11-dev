/* EDIT control (single and multi-line), 3.1 behaviour and look.
 * Both halves follow USER's own edit code (seg26-seg32). A WS_BORDER edit takes the style off and
 * draws its own one-pixel frame, the formatting rect is inset from that frame (SLSize/MLSize) and
 * drawing is clipped as ECSetEditClip clips it.
 * Single-line (seg28/29): 3.1's first-visible-character scrolling, extents, hit testing and caret.
 * Multi-line (seg30-32, ported routine by routine, each named with its seg:offset): line starts
 * built and word-wrapped by MLBuildchLines, tabs expanded by ECTabTheTextOut from the font's width
 * table, drawing by MLDrawText (selection runs in the highlight colours, opaque text cells), caret,
 * hit testing (MLMouseToIch), scrolling (MLScrollHandler / MLEnsureCaretVisible), keyboard
 * (MLKeyDown / MLChar: Up/Down/Page keys are simulated mouse clicks as in USER), mouse, the undo
 * buffer of ECInsertText / ECDeleteText / MLUndo and the EM_ messages.
 * The text buffer is a local-memory handle (EM_GETHANDLE / EM_SETHANDLE work as in Win16). */
#include "w16int.h"
#include <ctype.h>

typedef struct {
    HLOCAL hbuf;
    int len;                  /* cch */
    int anchor, caret;        /* single-line selection = [min, max); multi-line: caret = ichCaret */
    int mn, mx;               /* multi-line ichMinSel, ichMaxSel */
    int limit;                /* cchTextMax */
    int multi, wrap;          /* fWrap (multi-line) */
    int *ls;                  /* line starts (chLines) */
    int nl, capl;             /* cLines */
    int top, xoff;            /* multi-line: first visible line (ichScreenStart), xOffset in pixels */
    int scr;                  /* single-line: first visible character (ichScreenStart) */
    W16Font *f;
    int lh, avgw;
    int avew, cxsys, cysys;   /* USER's average width of the font; the system font's width, height */
    int overhang, fixed;      /* tmOverhang; a fixed-pitch font */
    int border;               /* created with WS_BORDER: draws its own frame */
    int nofmt;                /* multi-line fCaretHidden: too small to format */
    int pww;                  /* password character width */
    HWND combo;               /* the combo box this is the edit of (ES_COMBOBOX) */
    RECT fmt;                 /* rcFmt */
    int modified;             /* fDirty */
    HLOCAL undo;              /* single-line: snapshot for one-level undo */
    int undo_len, undo_a, undo_c, undo_run;
    int focus, mdown;
    char pw;
    int maxw;                 /* single-line text width; multi-line maxPixelWidth */
    int nohidesel;
    int quiet;                /* no EN_ notifications (single-line creation text) */
    UINT_PTR_W16 timer;
    /* multi-line (ped fields of USER seg30) */
    int cline;                /* iCaretLine */
    int lines;                /* ichLinesOnScreen */
    int format;               /* ES_LEFT 0, ES_CENTER 1, ES_RIGHT 2 */
    int autovs, autohs;       /* fAutoVScroll, fAutoHScroll */
    int indlg;                /* fInDialogBox: WM_GETDLGCODE came with a message */
    int win31;                /* the program expects Windows 3.10 (every arch311 port does) */
    int cw[256];              /* charWidthBuffer: GetCharWidth less the overhang */
    int *pts;                 /* pTabStops: [0] = count, then the stops in pixels */
    int undo_type, ich_del, cch_del, ins_start, ins_end;
    char *del;                /* hDeletedText */
    int msx, msy, msk;        /* the last mouse position and keys (WM_SYSTIMER repeats them) */
    COLORREF hibk, hitx;      /* the highlight colours, read at creation */
} Edit;

static Edit *ed(HWND h) { return (Edit *)h->ctl; }
static char *txt(Edit *e) { return (char *)LocalLock(e->hbuf); }
static void untxt(Edit *e) { LocalUnlock(e->hbuf); }
static int smin(Edit *e) { return min(e->anchor, e->caret); }
static int smax(Edit *e) { return max(e->anchor, e->caret); }

static void notify(HWND h, int code) { Edit *e = h->ctl; if (!e || !e->quiet) w16_notify_parent(h, code); }

/* the edit's private timer stands for USER's system timer 1 (SetSystemTimer / WM_SYSTIMER) */
static UINT edit_timer_id = 0x7EED;

/* ------------------------------------------------------------------ measuring (single-line) */
static int ch_w(Edit *e, char c)
{
    if (e->pw) return e->f->widths[(unsigned char)e->pw];
    return e->f->widths[(unsigned char)c];
}

static int seg_width(Edit *e, const char *s, int from, int to)
{
    int x = 0;
    for (int i = from; i < to; i++) x += ch_w(e, s[i]);
    return x;
}

static int line_end(Edit *e, const char *t, int i)
{
    /* end of line i, excluding its line break */
    int s = e->ls[i], end = (i + 1 < e->nl) ? e->ls[i + 1] : e->len;
    while (end > s && (t[end - 1] == '\n' || t[end - 1] == '\r')) end--;
    return end;
}

static void build_lines(HWND h)
{
    /* a single-line edit has one line; its width serves the scroll bar of an edit created with one */
    Edit *e = ed(h);
    char *t = txt(e);
    e->nl = 1;
    e->ls[0] = 0;
    e->maxw = seg_width(e, t, 0, e->len);
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

/* ------------------------------------------------------------------ single-line geometry (seg28) */
/* GetTextExtent in the edit's font: the advance widths plus the overhang, once per string */
static int text_ext(Edit *e, const char *s, int n)
{
    return n > 0 ? w16_text_width(e->f, s, n) + e->overhang : 0;
}

/* the single-line caret (seg28:1224): one pixel wide when the font's average character is
 * narrower than the system font's, else two; one pixel taller than the line */
static int sl_caret_w(Edit *e) { return e->avew < e->cxsys ? 1 : 2; }

static int ml_extent(Edit *e, const char *s, int n);

/* ECCchInWidth (seg26:025C): how many characters of s[0..n) fit in w pixels, counted from the
 * start or, backward, from the end. A single-line edit in a fixed-pitch font divides; a multi-line
 * edit measures with ECTabTheTextOut (tabs expanded) */
static int cch_in_width(Edit *e, const char *s, int n, int w, int forward)
{
    if (w <= 0 || n <= 0) return 0;
    if (e->fixed && !e->multi) return min(n, w / e->avew);
    if (e->pw) return min(n, w / e->pww);
    int lo = 0, hi = n + 1;
    while (hi - 1 > lo) {
        int mid = lo + max(1, (hi - lo) / 2);
        const char *p = forward ? s : s + n - mid;
        if ((e->multi ? ml_extent(e, p, mid) : text_ext(e, p, mid)) > w) hi = mid;
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

/* ------------------------------------------------------------------ scroll bars (single-line) */
static void update_sb(HWND h)
{
    Edit *e = ed(h);
    if (h->style & WS_VSCROLL) {
        SetScrollRange(h, SB_VERT, 0, 100, FALSE);
        SetScrollPos(h, SB_VERT, 0, TRUE);
    }
    if (h->style & WS_HSCROLL) {
        int range = max(1, e->maxw);
        SetScrollRange(h, SB_HORZ, 0, 100, FALSE);
        SetScrollPos(h, SB_HORZ, min(100, e->xoff * 100 / range), TRUE);
    }
}

/* ------------------------------------------------------------------ shared drawing helpers */
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

/* ------------------------------------------------------------------ single-line painting */
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

static void redraw(HWND h)
{
    if (!w16_window_visible(h) || h->redraw_off) return;
    HideCaret(h);
    HDC dc = GetDC(h);
    sl_paint(h, dc);
    ReleaseDC(h, dc);
    ShowCaret(h);
}

static void place_caret(HWND h)
{
    Edit *e = ed(h);
    if (!e->focus) return;
    char *t = txt(e);
    int x = sl_x(e, t, e->caret);
    untxt(e);
    /* SLSetCaretPosition (seg28:0000): never past the formatting rect's right edge */
    x = min(x, e->fmt.right - sl_caret_w(e));
    SetCaretPos(x, e->fmt.top);
}

static void refresh(HWND h, int text_changed)
{
    if (text_changed) build_lines(h);
    sl_scroll(h);
    if (text_changed) notify(h, EN_UPDATE);
    redraw(h);
    place_caret(h);
    update_sb(h);
    if (text_changed) notify(h, EN_CHANGE);
}

/* ------------------------------------------------------------------ single-line editing */
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
    if (!(h->style & ES_AUTOHSCROLL) && n) {
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
    if (n > e->limit) n = e->limit;
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
    int scrolled = sl_scroll(h);
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
            const char *cr = strpbrk(p, "\r\n");
            if (cr) n = cr - p;
            replace_sel(h, p, n, 1);
        }
        GlobalUnlock(g);
    }
    CloseClipboard();
}

/* ------------------------------------------------------------------ geometry */
/* ECSetFont (seg27:02C9): line height, USER's average width, overhang and pitch of the font; the
 * system font (hFont 0, as every edit gets at creation) also sets the system metrics the border
 * insets use. A multi-line edit keeps the font's GetCharWidth table less the overhang
 * (charWidthBuffer, measured from by ECTabTheTextOut). */
static void set_font(HWND h, HFONT f)
{
    Edit *e = ed(h);
    h->font = f;
    HDC dc = GetDC(h);
    SelectObject(dc, f ? f : GetStockObject(SYSTEM_FONT));
    e->f = w16_dc_font(dc);
    TEXTMETRIC tm;
    e->avew = w16_ave_char_width(dc, &tm);
    if (e->multi) {
        GetCharWidth(dc, 0, 255, e->cw);
        for (int i = 0; i < 256; i++) e->cw[i] -= tm.tmOverhang;
    }
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

/* SLSize (seg29:0000): the client rect, inset when the edit has a border by half an average
 * character and a quarter of a line, no more than the system font's; at most one line tall */
static void calc_fmt(HWND h)
{
    Edit *e = ed(h);
    RECT r;
    GetClientRect(h, &r);
    if (r.left == r.right || r.top == r.bottom) {
        if (e->fmt.left != e->fmt.right) return;
        SetRect(&r, 0, 0, 10, 10);
    }
    if (e->border) InflateRect(&r, -(min(e->avew, e->cxsys) / 2), -(min(e->lh, e->cysys) / 4));
    r.bottom = min(r.top + e->lh, r.bottom);
    e->fmt = r;
}

/* ==================================================================================== multi-line */
/* IsWindowVisible as the edit sees it: in 3.1 WM_SETREDRAW FALSE takes WS_VISIBLE off */
static int vis(HWND h) { return w16_window_visible(h) && !h->redraw_off; }

/* MulDiv of USER (seg1:39AB): (a * b + c / 2) / c, a when c is 0 */
static int user_muldiv(int a, int b, int c)
{
    if (!c) return a;
    long p = (long)a * b + ((unsigned)(WORD)c >> 1);
    return (int)(p / c);
}

static int is_space_tab(char c) { return c == ' ' || c == '\t'; } /* seg30:0000, seg26:0361 */

/* the text buffer holds at least cch + 4 bytes, zero after the text (USER reads the byte after a
 * line's end; its stale contents are not reproduced) */
static int ec_reserve(Edit *e, int n)
{
    if ((UINT)(n + 4) <= LocalSize(e->hbuf)) return 1;
    return LocalReAlloc(e->hbuf, n + 0x20, LMEM_MOVEABLE | LMEM_ZEROINIT) != NULL;
}

static void ls_reserve(Edit *e, int n)
{
    if (n <= e->capl) return;
    while (e->capl < n) e->capl = e->capl ? e->capl * 2 : 64;
    e->ls = realloc(e->ls, sizeof(int) * e->capl);
}

/* ECGetEditDC (seg26:0AB2): the caret hidden unless fast, clipped by ECSetEditClip, the edit's
 * font; ECReleaseEditDC (seg26:0AF5) undoes it */
static HDC ec_get_dc(HWND h, int fast)
{
    if (!fast) HideCaret(h);
    HDC dc = GetDC(h);
    edit_clip(h, dc);
    if (h->font) SelectObject(dc, h->font);
    return dc;
}
static void ec_release_dc(HWND h, HDC dc, int fast)
{
    if (h->font) SelectObject(dc, GetStockObject(SYSTEM_FONT));
    ReleaseDC(h, dc);
    if (!fast) ShowCaret(h);
}

/* ECTabTheTextOut (seg26:0020): the text in runs between tabs, measured from the width table and
 * drawn (fDraw) with ExtTextOut, opaque over [end of the previous run, end of this run] when the
 * DC's background mode is OPAQUE; tab stops are pixel positions from org (EM_SETTABSTOPS), one
 * stop is an interval, none or past the last every 8 average characters. Returns the width. */
static int ec_tab_text_out(Edit *e, HDC dc, int x, int y, const char *s, int n, int org, int draw)
{
    int xs = x, tabw = 0;
    int opaque = draw && GetBkMode(dc) == OPAQUE;
    if (!s || !n) return 0;
    int nt = e->pts ? e->pts[0] : 0;
    const int *tabs = e->pts ? e->pts + 1 : NULL;
    if (nt == 1) { tabw = tabs[0]; if (!tabw) tabw = 1; }
    RECT r = {x, y, 0, y + e->lh};
    while (n) {
        int run = 0, w = 0;
        while (run < n && s[run] != '\t') w += e->cw[(unsigned char)s[run++]];
        n -= run;
        if (draw) {
            r.right = x + w;
            ExtTextOut(dc, x, y, opaque ? ETO_OPAQUE : 0, &r, s, run, NULL);
            r.left = r.right;
        }
        if (!n) return x + w - xs;
        if (tabw) x = (int)((unsigned)(x - org + w) / (unsigned)tabw + 1) * tabw + org;
        else {
            x += w;
            int i = 0;
            while (i < nt && tabs[i] + org <= x) i++;
            if (i < nt) x = tabs[i] + org;
            else { tabw = e->avew * 8; x = ((x - org) / tabw + 1) * tabw + org; }
        }
        s += run + 1;
        if (--n == 0 && draw) {
            /* a tab at the end: its gap is filled in the background colour */
            r.right = x;
            ExtTextOut(dc, r.left, r.top, ETO_OPAQUE, &r, "", 0, NULL);
        }
    }
    return x - xs;
}

/* MLTabExtent (seg30:000F): the width of n characters, tabs from 0 */
static int ml_extent(Edit *e, const char *s, int n) { return ec_tab_text_out(e, NULL, 0, 0, s, n, 0, 0); }

/* MLLineLength (seg30:01D8): a line's characters without its CR LF (or soft CR CR LF) */
static int ml_line_len(Edit *e, const char *t, int line)
{
    if ((unsigned)line >= (unsigned)e->nl) return 0;
    if (line == e->nl - 1) return e->len - e->ls[line];
    int n = e->ls[line + 1] - e->ls[line];
    if (n > 1) {
        int p = e->ls[line + 1] - 2;
        if (t[p] == '\r' && t[p + 1] == '\n') {
            n -= 2;
            if (n && t[p - 1] == '\r') n--;
        }
    }
    return n;
}

/* MLIchToLine (seg30:0243): the line holding ich (-1: the selection's start) */
static int ml_ich_to_line(Edit *e, int ich)
{
    if ((WORD)ich == 0xFFFF) ich = e->mn;
    int l = e->nl - 1;
    while (l && (unsigned)e->ls[l] > (unsigned)(WORD)ich) l--;
    return l;
}

/* MLCalcXOffset (seg30:0030): how far ES_CENTER / ES_RIGHT move a line in from the left edge */
static int ml_calc_xoffset(Edit *e, int line)
{
    if (!e->format) return 0;
    char *t = txt(e);
    int n = ml_line_len(e, t, line), w = n ? ml_extent(e, t + e->ls[line], n) : 0;
    untxt(e);
    int x = e->fmt.right - e->fmt.left - w;
    if (x < 0) x = 0;
    if (e->format == 1) return x >> 1;
    if (e->format == 2 && --x < 0) return 0;
    return x;
}

/* MLAdjustIch (seg30:00BE): one character left or right, never into a CR LF / CR CR LF */
static int ml_adjust_ich(Edit *e, int ich, int left)
{
    char *t = txt(e);
    if (left && ich) {
        if (--ich && t[ich - 1] == '\r' && t[ich] == '\n') {
            if (--ich && t[ich - 1] == '\r') ich--;
        }
    } else if (!left && e->len > ich) {
        ich++;
        if (e->len > ich) {
            if (t[ich - 1] == '\r' && t[ich] == '\n') ich++;
            else if (ich && t[ich] == '\r' && t[ich + 1] == '\n' && t[ich - 1] == '\r') ich += 2;
        }
    }
    untxt(e);
    return ich;
}

/* MLIchToXYPos (seg30:0277): the client point of a character position. fPrevLine puts a position
 * at a soft line break at the end of the line before. Formatted lines are offset, others scroll by
 * xOffset. */
static void ml_ich_to_xy(Edit *e, int ich, int prev, int *px, int *py)
{
    int line = ml_ich_to_line(e, ich);
    int y = (line - e->top) * e->lh + e->fmt.top;
    char *t = txt(e);
    int start, n;
    if (prev && line && e->ls[line] == ich && !(ich >= 2 && t[ich - 2] == '\r' && t[ich - 1] == '\n')) {
        y -= e->lh;
        line--;
        start = e->ls[line];
        n = ml_line_len(e, t, line);
    } else {
        start = e->ls[line];
        int q = ich;
        if (ich < e->len && ich && t[ich - 1] == '\r' && t[ich] == '\n') {
            q--;
            if (ich > 2 && t[q - 1] == '\r') q--;
        }
        n = q - start;
    }
    int xo = e->format ? ml_calc_xoffset(e, line) : -e->xoff;
    *px = ml_extent(e, t + start, n) + e->fmt.left + xo;
    *py = y;
    untxt(e);
}

/* MLMouseToIch (seg30:0366): the character position (and line) nearest a client point. Above the
 * formatting rect is the line before the first visible one, below it the line after the last;
 * left of the text half an average character rounds, right of the rect is the character after
 * the last visible one. */
static int ml_mouse_to_ich(Edit *e, int x, int y, int *pline)
{
    int line;
    if (y <= e->fmt.top) {
        line = e->top - 1;
        if (line < 0) line = 0;
    } else {
        line = y >= e->fmt.bottom ? e->lines + e->top : (y - e->fmt.top) / e->lh + e->top;
        if (line > e->nl - 1) line = e->nl - 1;
    }
    char *t = txt(e);
    const char *pl = t + e->ls[line];
    int len = ml_line_len(e, t, line), ich;
    if (e->format) x -= ml_calc_xoffset(e, line);
    if (x >= e->fmt.right) {
        int n = cch_in_width(e, pl, len, e->xoff - e->fmt.left + e->fmt.right, 1);
        ich = e->ls[line] + min(n + 1, len);
    } else if (x <= e->fmt.left + e->avew / 2) {
        int n = cch_in_width(e, pl, len, e->xoff, 1);
        if (n) n--;
        ich = e->ls[line] + n;
    } else {
        int xp = x + e->xoff, hi = len + 1, lo = 0, mid = 0, xm = xp;
        while ((unsigned)(hi - 1) > (unsigned)lo) {
            int d = (unsigned)(hi - lo) >> 1;
            mid = lo + (d < 1 ? 1 : d);
            xm = ml_extent(e, pl, mid) + e->avew / 2 + e->fmt.left;
            if (xm > xp) hi = mid;
            else lo = mid;
        }
        /* the last probe decides: its position, or the next one when it was left of the point */
        if (xm - xp < xp - xm) mid++;
        if ((unsigned)mid > (unsigned)len) mid = len;
        ich = e->ls[line] + mid;
    }
    untxt(e);
    if (pline) *pline = line;
    return ich;
}

/* MLSetCaretPosition (seg30:0134): the caret at the caret position when the edit has the focus;
 * off-screen while its line is out of view, or past the right / bottom edge (word wrap: past the
 * last whole line); never more than 2 pixels short of the right edge */
static void ml_set_caret_pos(HWND h)
{
    Edit *e = ed(h);
    if (!e->focus || !vis(h)) return;
    int x = -20000, y = -20000;
    if (!e->nofmt && (unsigned)e->cline >= (unsigned)e->top && (unsigned)(e->lines + e->top) >= (unsigned)e->cline) {
        int prev = e->nl - e->cline - 1 != 0 && e->ls[e->cline + 1] == e->caret;
        int cx, cy, ok;
        ml_ich_to_xy(e, e->caret, prev, &cx, &cy);
        if (e->wrap) ok = !(e->fmt.bottom - e->lh < cy);
        else ok = !(e->fmt.right < cx) && !(e->fmt.bottom < cy);
        if (ok) { x = min(e->fmt.right - 2, cx); y = cy; }
    }
    SetCaretPos(x, y);
}

/* MLUpdateiCaretLine (seg30:0604): a caret at a soft line break belongs to the line before */
static void ml_update_caret_line(Edit *e)
{
    e->cline = ml_ich_to_line(e, e->caret);
    char *t = txt(e);
    if (e->cline && e->ls[e->cline] == e->caret && !(e->caret >= 2 && t[e->caret - 2] == '\r' && t[e->caret - 1] == '\n'))
        e->cline--;
    untxt(e);
}

/* MLDrawText (seg30:2578): characters ichStart..ichEnd of the visible lines, each line in runs of
 * one selection state (the selection shows with the focus or ES_NOHIDESEL), then the rest of the
 * line to the right edge (and the indent of a centred / right-aligned line) in the background
 * colour. The control's colours come from WM_CTLCOLOR. */
static void ml_draw_text(HWND h, HDC dc, int ichStart, int ichEnd)
{
    Edit *e = ed(h);
    int again = 0, sel = 0, left = 1, x0 = 0, y0 = 0;
    COLORREF obk = 0, otx = 0;
    if (!e->lines) return;
    w16_ctl_color(h, dc, CTLCOLOR_EDIT); /* GetControlBrush: sets the DC's colours */
    if (e->ls[e->top] > ichStart) {
        ichStart = e->ls[e->top];
        if (ichStart > ichEnd) return;
    }
    int last = min(e->top + e->lines, e->nl - 1);
    char *t = txt(e);
    int lend = ml_line_len(e, t, last) + e->ls[last];
    if (lend <= ichEnd) ichEnd = lend;
    int line = ml_ich_to_line(e, ichStart);
    if (e->format) { ichStart = e->ls[line]; left = 0; }
    HideCaret(h);
    while (ichStart <= ichEnd) {
        int di = ichStart;
        for (;;) {
            int len = ml_line_len(e, t, line);
            if (di - e->ls[line] > len) break;
            int cch = e->ls[line] - di + len, x, y;
            ml_ich_to_xy(e, di, 0, &x, &y);
            if (!left && e->ls[line] == di) { x0 = x; y0 = y; }
            int xo = e->format ? ml_calc_xoffset(e, line) : -e->xoff;
            if (e->mn != e->mx && e->mx > di && e->mn <= ichEnd && (e->nohidesel || e->focus)) {
                int c2;
                if (e->mn > di) { sel = 0; c2 = min(di + cch, e->mn) - di; }
                else {
                    sel = 1;
                    c2 = min(di + cch, e->mx) - di;
                    obk = SetBkColor(dc, e->hibk);
                    otx = SetTextColor(dc, e->hitx);
                }
                again = c2 != cch;
                cch = c2;
            }
            int w = ec_tab_text_out(e, dc, x, y, t + di, cch, e->fmt.left + xo, 1);
            if (sel) { sel = 0; SetBkColor(dc, obk); SetTextColor(dc, otx); }
            if (again) { again = 0; di += cch; continue; }
            RECT r;
            SetRect(&r, x + w, y, 0x7FFC, y + e->lh);
            ExtTextOut(dc, r.left, r.top, ETO_OPAQUE, &r, "", 0, NULL);
            if (!left) {
                SetRect(&r, e->fmt.left, y0, x0, y0 + e->lh);
                ExtTextOut(dc, r.left, r.top, ETO_OPAQUE, &r, "", 0, NULL);
            }
            break;
        }
        line++;
        ichStart = line < e->nl ? e->ls[line] : ichEnd + 1;
    }
    untxt(e);
    ShowCaret(h);
    ml_set_caret_pos(h);
}

/* ECCalcChangeSelection (seg26:0FC2, with seg26:0F02): the two blocks to redraw when the selection
 * goes from old[] to new[]; returns 0 when nothing changes */
static int ec_calc_change_sel(Edit *e, int omin, int omax, int *old, int *nw)
{
    int b[4] = {-1, -1, -1, -1}, n = 0;
    if (omin != omax) { b[0] = old[0]; b[1] = old[1]; n = 1; }
    if (e->mn != e->mx) { b[2] = nw[0]; b[3] = nw[1]; n++; }
    if (n == 2) {
        if (e->mn == omin) {
            if (e->mx == omax) return 0;
            b[0] = min(nw[1], old[1]);
            b[1] = max(nw[1], old[1]);
            b[2] = -1;
        } else if (e->mx == omax) {
            b[0] = min(nw[0], old[0]);
            b[1] = max(nw[0], old[0]);
            b[2] = -1;
        } else {
            if (nw[0] <= old[0]) { b[0] = nw[0]; b[1] = min(nw[1], old[0]); }
            else { b[0] = old[0]; b[1] = min(old[1], nw[0]); }
            if (nw[1] >= old[1]) { b[2] = max(old[1], nw[0]); b[3] = nw[1]; }
            else { b[2] = max(nw[1], old[0]); b[3] = old[1]; }
        }
    }
    old[0] = b[0]; old[1] = b[1];
    nw[0] = b[2]; nw[1] = b[3];
    return 1;
}

/* seg30:0505: redraw what changed between the old selection and the new one */
static void ml_draw_changed(HWND h, HDC dc, int omin, int omax)
{
    Edit *e = ed(h);
    int blk[4] = {omin, omax, e->mn, e->mx};
    if (!vis(h) || !ec_calc_change_sel(e, omin, omax, blk, blk + 2)) return;
    for (int i = 0; i < 2; i++)
        if (blk[2 * i] != -1) ml_draw_text(h, dc, blk[2 * i], blk[2 * i + 1]);
}

/* MLChangeSelection (seg30:0587) */
static void ml_change_selection(HWND h, HDC dc, int nmin, int nmax)
{
    Edit *e = ed(h);
    if ((unsigned)nmax < (unsigned)nmin) { int t = nmin; nmin = nmax; nmax = t; }
    int omin = e->mn, omax = e->mx;
    e->mn = min(e->len, nmin);
    e->mx = min(e->len, nmax);
    if (vis(h) && (e->focus || e->nohidesel)) {
        ml_draw_changed(h, dc, omin, omax);
        ml_set_caret_pos(h);
    }
}

/* ------------------------------------------------------------------ multi-line lines */
/* MLShiftchLines (seg30:0B39) */
static void ml_shift_lines(Edit *e, int line, int delta)
{
    for (int i = line; i < e->nl; i++) e->ls[i] += delta;
}

/* MLInsertchLine (seg30:0AB5): overwrite a line start while typing, else insert one */
static int ml_insert_line(Edit *e, int line, int ich, int typing)
{
    if (typing && line < e->nl) { e->ls[line] = ich; return 1; }
    ls_reserve(e, e->nl + 2);
    memmove(e->ls + line + 1, e->ls + line, (e->nl - line) * sizeof(int));
    e->nl++;
    e->ls[line] = ich;
    return 1;
}

/* ECWord (seg26:0426): the word around ich - fLeft: the one starting at or before ich, else the one
 * holding ich - as [start, end), the end past the spaces after it; a line break ends a word */
static void ec_word(Edit *e, int ich, int fleft, int *ps, int *pe)
{
    if ((ich == 0 && fleft) || (e->len == ich && !fleft)) { *ps = *pe = 0; return; }
    char *t = txt(e);
    int i = ich, punct = 0, sp = 0;
    if (!fleft && (is_space_tab(t[i]) || t[i] == '\r')) {
        while ((is_space_tab(t[i]) || t[i] == '\n') && e->len > i) i++;
    } else {
        while (0 < i) {
            char c = t[i - 1];
            if ((is_space_tab(c) || c == '\n') && punct) break;
            if (!fleft && (is_space_tab(c) || c == '\n')) break;
            i--;
            if (is_space_tab(t[i]) || t[i] == '\n') continue;
            punct = 1;
            if (t[i] == '\r') break;
        }
    }
    int end = min(e->len, i + 1), start = i;
    if (t[i] == '\r') {
        if (0 < i && t[i - 1] == '\r') start = --i;
        else if (t[i + 1] == '\r') end++;
    }
    if (is_space_tab(t[end])) sp = 1;
    while (e->len > end) {
        if (sp && !is_space_tab(t[end])) break;
        if (t[end] == '\r') break;
        end++;
        if (is_space_tab(t[end])) sp = 1;
        if (t[end - 1] == '\n') break;
    }
    untxt(e);
    *ps = start;
    *pe = end;
}

/* MLBuildchLines (seg30:0B63): the line starts from line iLine on - hard breaks at CR LF (or soft
 * CR CR LF), lines of at most 1024 characters, with word wrap the characters that fit, broken
 * after the last space (ECWord) unless one word fills the line; one space at a break stays on the
 * line. While typing it stops where the old line starts agree again. Returns in *pll / *phl the
 * first and last character positions that moved; without wrap it keeps maxPixelWidth. */
static void ml_build_lines(HWND h, int line, int delta, int typing, int *pll, int *phl)
{
    Edit *e = ed(h);
    int broken = 0, ll, hl;
    if (e->len == 0) {
        e->maxw = 0;
        e->xoff = 0;
        e->top = 0;
        e->nl = 1;
        e->ls[0] = 0;
        if (pll) *pll = 1;
        if (phl) *phl = 0;
        return;
    }
    if (typing && delta) ml_shift_lines(e, line + 1, delta);
    if (line == 0 && delta == 0 && !typing) { e->maxw = 0; e->nl = 1; }
    ll = hl = delta ? e->ls[line] : 0;
    char *t = txt(e);
    int start = e->ls[line], crlf = start, end = start;
    while (start < e->len) {
        if (crlf <= start) {
            crlf = start;
            while (crlf < e->len && !(t[crlf] == '\r' && (t[crlf + 1] == '\n' || (t[crlf + 1] == '\r' && t[crlf + 2] == '\n'))))
                crlf++;
        }
        if (!e->wrap) {
            int n = min(crlf - start, 0x400);
            end = start + n;
            int w = ml_extent(e, t + start, n);
            if (w >= e->maxw) e->maxw = w;
        } else {
            int fw = e->fmt.right - e->fmt.left;
            end = fw > 0 ? start + cch_in_width(e, t + start, crlf - start, fw, 1) : start;
            if (end == start && crlf != start) end++;
            if (end != crlf) {
                /* (an application's word-break procedure is not supported: EM_SETWORDBREAKPROC) */
                int brk = is_space_tab(t[end]) || is_space_tab(t[end - 1]);
                if (!brk || t[end] == '\r') {
                    int ws, we;
                    untxt(e);
                    ec_word(e, end, 1, &ws, &we);
                    t = txt(e);
                    if (ws > start) end = ws;
                }
            }
        }
        if (is_space_tab(t[end]) && !(end > 0 && is_space_tab(t[end - 1]))) end++;
        int cr = end;
        if (t[end] == '\r') end += 2;
        if (t[end] == '\n') end++;
        line++;
        if (typing && line <= e->nl - 1 && e->ls[line] == end) {
            hl = e->ls[line];
            untxt(e);
            goto done;
        }
        if (!broken) {
            broken = 1;
            ll = hl = end == cr ? (end ? end - 1 : 0) : cr;
        }
        if ((unsigned)end > (unsigned)hl) hl = end;
        ml_insert_line(e, line, end, delta != 0);
        start = end;
    }
    if (e->nl != line) {
        e->nl = line;
        ls_reserve(e, line + 1);
        e->ls[line] = 0;
    }
    if (e->len && t[e->len - 1] == '\n' && e->ls[e->nl - 1] < e->len) {
        if (!broken) ll = e->len - 1;
        if ((unsigned)end > (unsigned)hl) hl = end;
        ml_insert_line(e, line, e->len, 0);
    }
    untxt(e);
done:
    if (pll) *pll = ll;
    if (phl) *phl = hl;
}

/* ------------------------------------------------------------------ multi-line scrolling */
/* seg30:1A84: scroll by lines (or average characters for WM_HSCROLL); returns the pixels moved,
 * at most a page */
static int ml_scroll_lines(Edit *e, int delta, UINT msg)
{
    if (msg != WM_HSCROLL) {
        int old = e->top, n = old + delta;
        if (n < 0) n = 0;
        if (n > e->nl - 1) n = e->nl - 1;
        e->top = n;
        int d = old - n;
        d = d < 0 ? -min(e->lines, -d) : min(e->lines, d);
        return d * e->lh;
    }
    if (e->format) return 0;
    int old = e->xoff, n = old + delta * e->avew;
    if (n < 0) n = 0;
    if (n > e->maxw) n = e->maxw;
    e->xoff = n;
    int d = old - n, fw = e->fmt.right - e->fmt.left + 1;
    return d < 0 ? -min(fw, -d) : min(fw, d);
}

/* seg30:1B40: the first line (or xOffset) for a thumb position 0..100 */
static int ml_thumb_scroll(Edit *e, int pos, int vert)
{
    if (vert) {
        int old = e->top, n = user_muldiv(e->nl - 1, pos, 100);
        if (n > e->nl - 1) n = e->nl - 1;
        e->top = n;
        return (old - n) * e->lh;
    }
    if (e->format) return 0;
    int old = e->xoff;
    e->xoff = user_muldiv(e->maxw - e->avew, pos, 100);
    return old - e->xoff;
}

/* MLThumbPos (seg30:1BBD): the scroll-bar position 0..100 (EM_GETTHUMB) */
static int ml_thumb_pos(Edit *e, int vert)
{
    if (vert) return e->nl < 2 ? 0 : user_muldiv(e->top, 100, e->nl - 1);
    if (e->avew * 2 > e->maxw) return 0;
    return user_muldiv(e->xoff, 100, e->maxw);
}

/* MLScrollHandler (seg30:1BFE): WM_VSCROLL / WM_HSCROLL / EM_SCROLL / EM_LINESCROLL. A page is a
 * screen less one line; SB_TOP and SB_BOTTOM are not handled. The text moves with ScrollDC, the
 * uncovered strip is invalidated (erased only when the last line is in view) and painted at once. */
static LRESULT ml_scroll(HWND h, UINT msg, int cmd, int pos)
{
    Edit *e = ed(h);
    UpdateWindow(h);
    int vert = msg != WM_HSCROLL, dy = 0, dx = 0, n;
    switch (cmd) {
    case SB_LINEUP: n = -1; break;
    case SB_LINEDOWN: n = 1; break;
    case SB_PAGEUP: n = 1 - e->lines; break;
    case SB_PAGEDOWN: n = e->lines - 1; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: {
        int d = ml_thumb_scroll(e, pos, vert);
        n = -(d / (msg == WM_VSCROLL ? e->lh : e->avew));
        if (vert) dy = d;
        else dx = d;
        break;
    }
    case EM_LINESCROLL: n = pos; break;
    case EM_GETTHUMB: return ml_thumb_pos(e, vert);
    default: return 0;
    }
    RECT rc, upd;
    GetClientRect(h, &rc);
    IntersectRect(&rc, &rc, &e->fmt);
    rc.bottom++;
    if (cmd != SB_THUMBPOSITION && cmd != SB_THUMBTRACK) {
        if (msg == WM_VSCROLL) { dx = 0; dy = ml_scroll_lines(e, n, msg); }
        else if (msg == WM_HSCROLL) { dx = ml_scroll_lines(e, n, msg); dy = 0; }
    }
    SetScrollPos(h, vert ? SB_VERT : SB_HORZ, ml_thumb_pos(e, vert), TRUE);
    if (cmd != SB_THUMBTRACK) notify(h, vert ? EN_VSCROLL : EN_HSCROLL);
    if (vis(h)) {
        HDC dc = ec_get_dc(h, 0);
        ScrollDC(dc, dx, dy, &rc, &rc, NULL, &upd);
        ml_set_caret_pos(h);
        ec_release_dc(h, dc, 0);
        InvalidateRect(h, &upd, e->lines + e->top >= e->nl);
        UpdateWindow(h);
    }
    return MAKELONG(n, 1);
}

/* MLEnsureCaretVisible (seg30:1E9B): ES_AUTOVSCROLL brings the caret's line into view, ES_AUTOHSCROLL
 * (when the widest line is wider than the rect) scrolls a caret left of 0 or right of the edge a
 * third of the width in; the scroll bars follow. Returns whether it scrolled vertically. */
static int ml_ensure_caret_visible(HWND h)
{
    Edit *e = ed(h);
    int scrolled = 0;
    if (vis(h)) {
        if (e->autovs) {
            WORD last = (WORD)(e->lines + e->top - 1);
            if (last < (WORD)e->cline) { scrolled = 1; ml_scroll(h, WM_VSCROLL, EM_LINESCROLL, e->cline - last); }
            else if ((WORD)e->cline < (WORD)e->top) { scrolled = 1; ml_scroll(h, WM_VSCROLL, EM_LINESCROLL, e->cline - e->top); }
        }
        if (e->autohs && e->fmt.right - e->fmt.left < e->maxw) {
            int prev = e->nl - e->cline - 1 != 0 && e->ls[e->cline + 1] == e->caret, x, y;
            ml_ich_to_xy(e, e->caret, prev, &x, &y);
            if (x < 0) ml_scroll(h, WM_HSCROLL, EM_LINESCROLL, ((e->fmt.left - e->fmt.right) / 3 + x) / e->avew);
            else if (e->fmt.right < x)
                ml_scroll(h, WM_HSCROLL, EM_LINESCROLL, ((e->fmt.right - e->fmt.left) / 3 - e->fmt.right + x) / e->avew);
        }
    }
    int p = ml_thumb_pos(e, 1);
    if (GetScrollPos(h, SB_VERT) != p) SetScrollPos(h, SB_VERT, p, TRUE);
    p = ml_thumb_pos(e, 0);
    if (GetScrollPos(h, SB_HORZ) != p) SetScrollPos(h, SB_HORZ, p, TRUE);
    return scrolled;
}

/* ------------------------------------------------------------------ multi-line text changes */
enum { UNDO_INSERT = 1, UNDO_DELETE = 2 };

/* ECEmptyUndo (seg26:059F) */
static void ec_empty_undo(Edit *e)
{
    e->undo_type = 0;
    free(e->del);
    e->del = NULL;
}

/* ECInsertText (seg26:05C4): n characters in at the caret (case and OEM conversions of the style),
 * recorded for undo - a run of typing extends the insertion, deleted text right there stays with
 * it; the caret and an empty selection follow */
static int ec_insert_text(HWND h, const char *s, int n)
{
    Edit *e = ed(h);
    if (!n) return 1;
    if (!ec_reserve(e, e->len + n)) return 0;
    char *t = txt(e), *d = t + e->caret;
    if (e->len != e->caret) memmove(d + n, d, e->len - e->caret);
    memcpy(d, s, n);
    t[e->len + n] = 0;
    if (h->style & ES_LOWERCASE) AnsiLowerBuff(d, n);
    else if (h->style & ES_UPPERCASE) AnsiUpperBuff(d, n);
    /* ES_OEMCONVERT: each character through the OEM code page and back (identity in libw16) */
    untxt(e);
    if (e->undo_type == 0) {
        e->undo_type = UNDO_INSERT;
        e->ins_start = e->caret;
        e->ins_end = e->caret + n;
    } else if ((e->undo_type & UNDO_INSERT) && e->ins_end == e->caret)
        e->ins_end += n;
    else if ((e->undo_type & UNDO_INSERT) || e->undo_type == UNDO_DELETE) {
        if (e->ich_del != e->caret) {
            free(e->del);
            e->del = NULL;
            e->ich_del = -1;
            e->undo_type &= ~UNDO_DELETE;
        }
        e->ins_start = e->caret;
        e->ins_end = e->caret + n;
        e->undo_type |= UNDO_INSERT;
    }
    e->len += n;
    e->caret += n;
    e->mx = e->mn = e->caret;
    e->modified = 1;
    return 1;
}

/* ECDeleteText (seg26:0841): the selection out, kept for undo - Backspace runs add in front,
 * Delete runs behind; returns how many characters went */
static int ec_delete_text(HWND h)
{
    Edit *e = ed(h);
    int n = e->mx - e->mn;
    if (!n) return 0;
    char *t = txt(e);
    if (e->undo_type == UNDO_DELETE && (e->mx == e->ich_del || e->mn == e->ich_del)) {
        char *p = realloc(e->del, e->cch_del + n + 1);
        if (p) {
            e->del = p;
            if (e->mx == e->ich_del) {
                memmove(p + n, p, e->cch_del);
                memcpy(p, t + e->mn, n);
                e->ich_del = e->mn;
            } else
                memcpy(p + e->cch_del, t + e->mn, n);
            e->cch_del += n;
            p[e->cch_del] = 0;
            goto out;
        }
    }
    if (e->undo_type != 0 && e->undo_type != UNDO_DELETE && !(e->undo_type & UNDO_INSERT)) goto out;
    if (e->undo_type != 0) {
        e->undo_type = 0;
        e->ins_start = e->ins_end = -1;
        free(e->del);
        e->del = NULL;
        e->ich_del = -1;
        e->cch_del = 0;
    }
    e->del = malloc(n + 1);
    if (e->del) {
        e->undo_type = UNDO_DELETE;
        e->ich_del = e->mn;
        e->cch_del = n;
        memcpy(e->del, t + e->mn, n);
        e->del[n] = 0;
    }
out:
    if (e->len != e->mx) memmove(t + e->mn, t + e->mx, e->len - e->mx);
    e->len -= n;
    t[e->len] = 0;
    untxt(e);
    e->mx = e->caret = e->mn;
    e->modified = 1;
    return n;
}

/* ECSetText (seg26:0B34): the whole text replaced (no limit applies); everything at the start */
static int ec_set_text(HWND h, const char *s)
{
    Edit *e = ed(h);
    int olen = e->len, ocaret = e->caret;
    e->caret = e->len = 0;
    char *t = txt(e);
    t[0] = 0;
    untxt(e);
    if (s) {
        int n = strlen(s);
        if (n && !ec_insert_text(h, s, n)) {
            e->len = olen;
            e->caret = ocaret;
            notify(h, EN_ERRSPACE);
            return 0;
        }
    }
    e->cline = e->top = e->caret = e->mn = e->mx = 0;
    e->nl = 1;
    e->ls[0] = 0;
    return 1;
}

static int ml_char(HWND h, int ch, int mods);
static int ml_set_selection(HWND h, int noscroll, int a, int b);
static int ml_insert_text(HWND h, const char *s, int n, int typing);

/* MLUndo (seg32:0477): an insertion selected and deleted (which makes it the undo text), then
 * deleted text put back and selected */
static int ml_undo(HWND h)
{
    Edit *e = ed(h);
    char *del = e->del;
    int had_del = e->undo_type & UNDO_DELETE, cdel = e->cch_del, idel = e->ich_del;
    if (!e->undo_type) return 0;
    e->del = NULL;
    e->cch_del = 0;
    e->ich_del = -1;
    e->undo_type &= ~UNDO_DELETE;
    if (e->undo_type == UNDO_INSERT) {
        e->undo_type = 0;
        ml_set_selection(h, 0, e->ins_start, e->ins_end);
        e->ins_end = e->ins_start = -1;
        ml_char(h, VK_BACK, 4);
    }
    if (had_del) {
        ml_set_selection(h, 0, idel, idel);
        ml_insert_text(h, del ? del : "", cdel, 0);
        free(del);
        ml_set_selection(h, 0, idel, idel + cdel);
    }
    return 1;
}

/* MLInsertText (seg30:0641): text in at the caret within the limit (EN_MAXTEXT; never between a CR
 * and its LF), lines rebuilt from the caret's line, the changed part drawn, then scrolled into
 * view. Without ES_AUTOVSCROLL text that needs more lines than the edit shows is taken back. */
static int ml_insert_text(HWND h, const char *s, int n, int typing)
{
    Edit *e = ed(h);
    int ocaret = e->caret, oline = e->cline, crlf = 0, stype = 0, sins_s = 0, sins_e = 0, sidel = 0, scdel = 0;
    char *sdel = NULL;
    if (!n) return 0;
    if ((unsigned)e->limit <= (unsigned)e->len) { notify(h, EN_MAXTEXT); return 0; }
    int cnt = min(e->limit - e->len, n);
    if (cnt && s[cnt - 1] == '\r' && s[cnt] == '\n') cnt--;
    if (!cnt) { notify(h, EN_MAXTEXT); return 0; }
    if (cnt == 2 && s[0] == '\r' && s[1] == '\n') crlf = 1;
    if (!e->autovs && (e->undo_type == UNDO_INSERT || e->undo_type == UNDO_DELETE)) {
        stype = e->undo_type; sdel = e->del; sidel = e->ich_del; scdel = e->cch_del;
        sins_s = e->ins_start; sins_e = e->ins_end;
        e->del = NULL;
        e->undo_type = e->ins_end = e->ins_start = e->cch_del = e->ich_del = 0;
    }
    HDC dc = ec_get_dc(h, 0);
    int ox = 0, oy = 0, nx = 0, ny = 0, ll, hl;
    if (e->len) ml_ich_to_xy(e, e->len - 1, 0, &ox, &oy);
    if (!ec_insert_text(h, s, cnt)) {
        ec_release_dc(h, dc, 0);
        notify(h, EN_ERRSPACE);
        return 0;
    }
    ml_build_lines(h, oline, cnt, crlf ? 0 : typing, &ll, &hl);
    if (e->len) ml_ich_to_xy(e, e->len - 1, 0, &nx, &ny);
    if ((WORD)ny < (WORD)oy && (unsigned)(e->lines + e->top) >= (unsigned)(e->nl - 1)) {
        RECT r = e->fmt;
        r.top = ny + e->lh;
        InvalidateRect(h, &r, TRUE);
    }
    if (!e->autovs) {
        if (e->lines < e->nl) {
            ml_undo(h);
            ec_empty_undo(e);
            if (stype == UNDO_INSERT || stype == UNDO_DELETE) {
                e->undo_type = stype; e->del = sdel; e->ich_del = sidel; e->cch_del = scdel;
                e->ins_start = sins_s; e->ins_end = sins_e;
            } else
                free(sdel);
            MessageBeep(0);
            ec_release_dc(h, dc, 0);
            notify(h, EN_MAXTEXT);
            return 0;
        }
        free(sdel);
    }
    if (typing && e->wrap && (unsigned)ll < (unsigned)ocaret) ocaret = ll;
    ml_update_caret_line(e);
    notify(h, EN_UPDATE);
    if (!IsWindow(h)) return 0;
    if (vis(h)) {
        if (!crlf && typing) ml_draw_text(h, dc, ocaret, max(e->caret, hl));
        else ml_draw_text(h, dc, typing ? ocaret : 0, e->len);
    }
    ec_release_dc(h, dc, 0);
    ml_ensure_caret_visible(h);
    e->modified = 1;
    notify(h, EN_CHANGE);
    if (cnt < n) notify(h, EN_MAXTEXT);
    if (!IsWindow(h)) return 0;
    return cnt;
}

/* MLDeleteText (seg30:0943): the selection out, lines rebuilt (one character inside a line of an
 * ES_AUTOVSCROLL edit only shifts the following line starts), the rest of the text redrawn and
 * the space below it erased */
static int ml_delete_text(HWND h)
{
    Edit *e = ed(h);
    int maxsel = e->mx, fast = 0;
    int minline = ml_ich_to_line(e, e->mn), maxline = ml_ich_to_line(e, maxsel);
    if (maxsel - e->mn == 1 && minline == maxline && e->ls[minline] != e->mn) fast = e->autovs;
    int n = ec_delete_text(h);
    if (!n) return 0;
    int ll = 0, hl = 0;
    if (fast) {
        ml_shift_lines(e, minline + 1, -2);
        ml_build_lines(h, minline, 1, 1, &ll, &hl);
    } else
        ml_build_lines(h, max(minline - 1, 0), -n, 0, NULL, NULL);
    ml_update_caret_line(e);
    notify(h, EN_UPDATE);
    if (vis(h)) {
        HDC dc = ec_get_dc(h, 0);
        int l = max(minline - 1, 0);
        ml_draw_text(h, dc, e->ls[l], fast ? hl : e->len);
        if (e->len) {
            int x, y;
            ml_ich_to_xy(e, e->len, 0, &x, &y);
            RECT r = e->fmt;
            r.top = y + e->lh;
            InvalidateRect(h, &r, TRUE);
        } else
            InvalidateRect(h, &e->fmt, TRUE);
        ec_release_dc(h, dc, 0);
        ml_ensure_caret_visible(h);
    }
    e->modified = 1;
    notify(h, EN_CHANGE);
    return n;
}

/* MLSetSelection (seg32:0340): EM_SETSEL - the caret at the second position (-1 first: an empty
 * selection at the caret), scrolled into view unless a 3.1 program asked not to */
static int ml_set_selection(HWND h, int noscroll, int a, int b)
{
    Edit *e = ed(h);
    if ((WORD)a == 0xFFFF) a = b = e->caret;
    a = (unsigned)e->len < (unsigned)(WORD)a ? e->len : (WORD)a;
    b = (unsigned)e->len < (unsigned)(WORD)b ? e->len : (WORD)b;
    e->caret = b;
    e->cline = ml_ich_to_line(e, b);
    HDC dc = ec_get_dc(h, 0);
    ml_change_selection(h, dc, a, b);
    ml_set_caret_pos(h);
    ec_release_dc(h, dc, 0);
    if (!noscroll) ml_ensure_caret_visible(h);
    return 1;
}

/* ECCopy (seg26:0BE7): the selection to the clipboard as CF_TEXT (not from a password edit) */
static int ec_copy(HWND h)
{
    Edit *e = ed(h);
    if (e->pw) { MessageBeep(0); return 0; }
    int n = e->mx - e->mn;
    if (!n || !OpenClipboard(h)) return 0;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GHND, n + 1);
    if (!g) { CloseClipboard(); return 0; }
    char *p = GlobalLock(g), *t = txt(e);
    memcpy(p, t + e->mn, n);
    p[n] = 0;
    untxt(e);
    GlobalUnlock(g);
    SetClipboardData(CF_TEXT, g);
    CloseClipboard();
    return n;
}

/* MLPasteText (seg30:17F2): the selection replaced by the clipboard's text (at most 0xFA00) */
static int ml_paste(HWND h)
{
    Edit *e = ed(h);
    int n = 0;
    if (!e->autovs) ec_empty_undo(e);
    ml_delete_text(h);
    HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (OpenClipboard(h)) {
        HANDLE g = GetClipboardData(CF_TEXT);
        if (g) {
            char *p = GlobalLock(g);
            if (p) {
                n = strlen(p);
                if (n >= 0xFA00) n = 0xFA00;
                n = ml_insert_text(h, p, n, 0);
            }
            GlobalUnlock(g);
        }
        CloseClipboard();
    }
    if (oc) SetCursor(oc);
    return n;
}

/* ------------------------------------------------------------------ multi-line input */
/* MLMouseMotion (seg30:18A9): a click puts the caret (Shift extends from the end it is not at), a
 * drag with the button down moves the caret end of the selection - a timer repeats the last move
 * so a drag outside the rect keeps scrolling, faster the farther out - and a double click selects
 * ECWord's word (with its spaces) */
static void ml_mouse_motion(HWND h, UINT msg, int keys, int x, int y)
{
    Edit *e = ed(h);
    int changed = 0, line, nmax = e->mx, nmin = e->mn;
    HDC dc = ec_get_dc(h, 1);
    int ich = ml_mouse_to_ich(e, x, y, &line);
    e->msx = x; e->msy = y; e->msk = keys;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (e->mdown) {
            int dist = y >= 0 ? y - e->fmt.bottom : -y, ms = (25 - dist) * 16;
            if (ms < 100) ms = 100;
            SetTimer(h, edit_timer_id, ms, NULL);
            changed = 1;
            if (e->mn == e->caret && e->mn != e->mx) { e->caret = nmin = ich; nmax = e->mx; }
            else e->caret = nmax = ich;
            e->cline = line;
        }
        break;
    case WM_LBUTTONDOWN:
        e->mdown = 1;
        SetCapture(h);
        changed = 1;
        if (!(keys & MK_SHIFT)) e->caret = nmax = nmin = ich;
        else if (e->mn == e->caret) e->caret = nmin = ich;
        else e->caret = nmax = ich;
        e->cline = line;
        SetTimer(h, edit_timer_id, 400, NULL);
        break;
    case WM_LBUTTONUP:
        if (e->mdown) {
            KillTimer(h, edit_timer_id);
            ReleaseCapture();
            ml_set_caret_pos(h);
            e->mdown = 0;
        }
        break;
    case WM_LBUTTONDBLCLK: {
        int s, en;
        ec_word(e, e->caret, e->ls[e->cline] != e->caret, &s, &en);
        nmin = s;
        e->caret = nmax = en;
        e->cline = ml_ich_to_line(e, en);
        changed = 1;
        e->mdown = 0;
        break;
    }
    }
    if (changed) {
        ml_change_selection(h, dc, nmin, nmax);
        ml_ensure_caret_visible(h);
    }
    ec_release_dc(h, dc, 1);
    if (!e->focus && msg == WM_LBUTTONDOWN) SetFocus(h);
}

/* MLKeyDown (seg30:1022): caret keys (Shift extends from the end the caret is at, Ctrl moves by
 * ECWord words / to the text's ends); Up/Down and Page Up/Down click at the caret's x a line away
 * (after a page scroll); Delete goes through Backspace; in a dialog Tab, Enter and Esc move the
 * focus, press the default button, close. mods: 0 = read the keyboard, 1 Ctrl, 2 Shift, 4 none. */
static void ml_key_down(HWND h, int vk, int mods)
{
    Edit *e = ed(h);
    int nmin = e->mn, nmax = e->mx;
    int nosel = e->mn == e->mx, atmin = e->mn == e->caret, atmax = e->mx == e->caret;
    int change = 0, sc = 0, prev = 0, s, en;
    if (e->mdown) return;
    if (mods == 0)
        sc = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) + ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 0);
    else if (mods != 4)
        sc = mods;
    switch (vk) {
    case VK_ESCAPE:
        if (e->indlg) PostMessage(h->parent, WM_CLOSE, 0, 0);
        return;
    case VK_RETURN:
        if (!e->indlg) break;
        if (sc == 1 || (h->style & ES_WANTRETURN)) return;
        {
            int id = LOWORD(SendMessage(h->parent, DM_GETDEFID, 0, 0));
            HWND d = id ? GetDlgItem(h->parent, id) : NULL;
            if (!d) return;
            SendMessage(h->parent, WM_NEXTDLGCTL, (WPARAM)d, 1);
            if (!e->focus) PostMessage(d, WM_KEYDOWN, VK_RETURN, 0);
        }
        return;
    case VK_TAB:
        if (sc == 1) { ml_char(h, vk, mods); return; }
        if (e->indlg) SendMessage(h->parent, WM_NEXTDLGCTL, sc == 2, 0);
        return;
    case VK_LEFT:
        if (e->caret) {
            switch (sc) {
            case 0: e->caret = nmin = nmax = ml_adjust_ich(e, e->caret, 1); break;
            case 1: ec_word(e, e->caret, 1, &s, &en); e->caret = nmin = nmax = s; break;
            case 2:
                e->caret = ml_adjust_ich(e, e->caret, 1);
                if (atmax && !nosel) nmax = e->caret; else nmin = e->caret;
                break;
            case 3:
                ec_word(e, e->caret, 1, &s, &en);
                e->caret = s;
                if (atmax && !nosel) { nmin = e->mn; nmax = e->caret; } else nmin = e->caret;
                break;
            }
            change = 1;
        } else if (!nosel && (sc == 0 || sc == 1)) { change = 1; nmin = nmax = e->caret; }
        break;
    case VK_RIGHT:
        if ((unsigned)e->len > (unsigned)e->caret) {
            switch (sc) {
            case 0: e->caret = nmin = nmax = ml_adjust_ich(e, e->caret, 0); break;
            case 1: ec_word(e, e->caret, 0, &s, &en); e->caret = nmin = nmax = en; break;
            case 2:
                e->caret = ml_adjust_ich(e, e->caret, 0);
                if (atmin && !nosel) nmin = e->caret; else nmax = e->caret;
                break;
            case 3:
                ec_word(e, e->caret, 0, &s, &en);
                e->caret = en;
                if (atmin && !nosel) { nmin = en; nmax = e->mx; } else nmax = en;
                break;
            }
            change = 1;
        } else if (!nosel && (sc == 0 || sc == 1)) { change = 1; nmin = nmax = e->caret; }
        break;
    case VK_UP:
    case VK_DOWN: {
        prev = e->nl - e->cline - 1 != 0 && e->ls[e->cline + 1] == e->caret;
        int x, y;
        ml_ich_to_xy(e, e->caret, prev, &x, &y);
        y += (vk == VK_UP ? -e->lh : e->lh) + 1;
        if (sc == 0 || sc == 2) {
            ml_mouse_motion(h, WM_LBUTTONDOWN, sc ? MK_SHIFT : 0, x, y);
            ml_mouse_motion(h, WM_LBUTTONUP, sc ? MK_SHIFT : 0, x, y);
        }
        break;
    }
    case VK_HOME:
        switch (sc) {
        case 0: e->caret = nmin = nmax = e->ls[e->cline]; break;
        case 1: e->caret = nmin = nmax = 0; break;
        case 2:
            e->caret = e->ls[e->cline];
            if (atmax && !nosel) { nmin = e->mn; nmax = e->caret; } else nmin = e->caret;
            break;
        case 3:
            nmin = e->caret = 0;
            if (atmax && !nosel) nmax = e->mn;
            break;
        }
        change = 1;
        break;
    case VK_END: {
        char *t = txt(e);
        int le = e->ls[e->cline] + ml_line_len(e, t, e->cline);
        untxt(e);
        switch (sc) {
        case 0: e->caret = nmin = nmax = le; break;
        case 1: e->caret = nmin = nmax = e->len; break;
        case 2:
            e->caret = le;
            if (atmin && !nosel) { nmin = le; nmax = e->mx; } else nmax = le;
            break;
        case 3:
            nmax = e->caret = e->len;
            if (atmin && !nosel) nmin = e->mx;
            break;
        }
        change = 1;
        break;
    }
    case VK_PRIOR:
    case VK_NEXT:
        if (sc == 0 || sc == 2) {
            /* (USER passes an uninitialised fPrevLine here; FALSE) */
            int x, y;
            ml_ich_to_xy(e, e->caret, 0, &x, &y);
            SendMessage(h, WM_VSCROLL, vk == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN, 0);
            ml_mouse_motion(h, WM_LBUTTONDOWN, sc ? MK_SHIFT : 0, x, y + 1);
            ml_mouse_motion(h, WM_LBUTTONUP, sc ? MK_SHIFT : 0, x, y + 1);
        } else if (sc == 1) {
            int n = (e->fmt.right - e->fmt.left) / e->avew - 1;
            SendMessage(h, WM_HSCROLL, EM_LINESCROLL, MAKELONG(vk == VK_PRIOR ? -n : n, 0));
        }
        break;
    case VK_DELETE:
        if (h->style & ES_READONLY) break;
        switch (sc) {
        case 0:
            if ((unsigned)e->len > (unsigned)e->mx && e->mn == e->mx) {
                e->caret = e->mn = e->mx = ml_adjust_ich(e, e->caret, 0);
                SendMessage(h, WM_CHAR, VK_BACK, 0);
            }
            if (e->mn != e->mx) SendMessage(h, WM_CHAR, VK_BACK, 0);
            break;
        case 1:
            if ((unsigned)e->len > (unsigned)e->mx && e->mn == e->mx) {
                char *t = txt(e);
                e->caret = e->mx = e->ls[e->cline] + ml_line_len(e, t, e->cline);
                untxt(e);
            }
            if (e->mn != e->mx) SendMessage(h, WM_CHAR, VK_BACK, 0);
            break;
        case 2:
            /* cut: what WM_COPY took goes; with nothing selected the character before goes */
            if (SendMessage(h, WM_COPY, 0, 0) || e->mn == e->mx) SendMessage(h, WM_CHAR, VK_BACK, 0);
            break;
        }
        break;
    case VK_INSERT:
        if (sc == 1) SendMessage(h, WM_COPY, 0, 0);
        else if (sc == 2 && !(h->style & ES_READONLY)) SendMessage(h, WM_PASTE, 0, 0);
        break;
    }
    if (change) {
        HDC dc = ec_get_dc(h, 0);
        ml_change_selection(h, dc, nmin, nmax);
        e->cline = ml_ich_to_line(e, e->caret);
        if (vk == VK_END && (unsigned)e->len > (unsigned)e->caret && e->wrap && e->cline) {
            char *t = txt(e);
            int p = e->ls[e->cline];
            if (!(p >= 2 && t[p - 2] == '\r' && t[p - 1] == '\n')) e->cline--;
            untxt(e);
        }
        ml_set_caret_pos(h);
        ec_release_dc(h, dc, 0);
        ml_ensure_caret_visible(h);
    }
}

/* MLChar (seg30:1682): a character replaces the selection (Enter inserts CR LF); Backspace deletes
 * the selection or the character before the caret; ^Z undo, ^C copy, ^V paste, ^X cut; other
 * control characters beep. In a dialog Tab and Enter (without Ctrl, without ES_WANTRETURN) are
 * left to the dialog; a read-only edit takes only ^C. */
static int ml_char(HWND h, int ch, int mods)
{
    Edit *e = ed(h);
    unsigned char c = (unsigned char)ch;
    int deleted = 0, sc;
    if (e->mdown || c == 27) return 0;
    if (mods == 0) sc = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
    else if (mods == 4) sc = 0;
    else sc = mods;
    if (e->indlg && c == '\t' && sc != 1) return 0;
    if (e->indlg && c == '\r' && sc != 1 && !(h->style & ES_WANTRETURN)) return 0;
    if ((h->style & ES_READONLY) && (c != 3 || sc != 1)) return 0;
    if (c == '\n') c = '\r';
    if (c == '\t' || c == '\r' || c == 8 || c >= 0x20)
        if (ml_delete_text(h)) deleted = 1;
    switch (c) {
    case 0x1A: SendMessage(h, EM_UNDO, 0, 0); return 0;
    case 3: ml_key_down(h, VK_INSERT, 1); return 0;
    case 8:
        if (!deleted && e->mn) {
            e->mn = ml_adjust_ich(e, e->caret, 1);
            ml_delete_text(h);
        }
        return 0;
    case 0x16: ml_key_down(h, VK_INSERT, 2); return 0;
    case 0x18:
        if (e->mn != e->mx) ml_key_down(h, VK_DELETE, 2);
        else MessageBeep(0);
        return 0;
    }
    if (c >= 0x20 || c == '\r' || c == '\t') {
        char buf[3] = {(char)c, 0, 0};
        if (c == '\r') buf[1] = '\n';
        ml_insert_text(h, buf, c == '\r' ? 2 : 1, 1);
    } else
        MessageBeep(0);
    return 0;
}

/* ------------------------------------------------------------------ multi-line set-up and paint */
/* MLSize (seg30:1FF9): the formatting rect from a rect (the client rect, EM_SETRECT's, or 10
 * average characters by a line when empty), inset by half the system font's average width and a
 * quarter of its height when bordered, a whole number of lines tall; one too small for a character
 * and a line hides the caret and keeps the old rect. With word wrap the lines are rebuilt. */
static void ml_size(HWND h, const RECT *prc)
{
    Edit *e = ed(h);
    RECT r = *prc;
    if (r.right == r.left || r.bottom == r.top) {
        if (e->fmt.right != e->fmt.left) goto hide;
        SetRect(&r, 0, 0, e->avew * 10, e->lh);
    }
    if (e->border) InflateRect(&r, -(e->cxsys / 2), -(e->cysys / 4));
    if (r.right - r.left < e->avew) goto hide;
    int n = (r.bottom - r.top) / e->lh;
    if (n == 0) goto hide;
    e->nofmt = 0;
    e->lines = n;
    e->fmt = r;
    e->fmt.bottom = r.top + n * e->lh;
    if (e->wrap) {
        ml_build_lines(h, 0, 0, 0, NULL, NULL);
        ml_update_caret_line(e);
    }
    return;
hide:
    e->nofmt = 1;
    SetCaretPos(-20000, -20000);
}

/* the window rect in client coordinates (the frame drawn by MLPaint and invalidated with it) */
static void window_rect_client(HWND h, RECT *r)
{
    GetWindowRect(h, r);
    ScreenToClient(h, (POINT *)&r->left);
    ScreenToClient(h, (POINT *)&r->right);
}

/* seg31:0000 (WM_SIZE): MLSize from the client rect, the whole window repainted */
static void ml_size_handler(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    ml_size(h, &r);
    window_rect_client(h, &r);
    InvalidateRect(h, &r, TRUE);
}

/* MLSetText (seg31:0067): WM_SETTEXT and the creation text - no EN_ notifications */
static int ml_set_text(HWND h, const char *s)
{
    Edit *e = ed(h);
    int r = ec_set_text(h, s);
    if (r) {
        if (!e->wrap || e->fmt.right - e->fmt.left > 0) ml_build_lines(h, 0, 0, 0, NULL, NULL);
        e->mx = e->mn = e->top = e->caret = e->xoff = e->cline = 0;
        e->modified = 0;
    } else if (!IsWindow(h))
        return 0;
    ec_empty_undo(e);
    SetScrollPos(h, SB_VERT, 0, TRUE);
    SetScrollPos(h, SB_HORZ, 0, TRUE);
    InvalidateRect(h, NULL, TRUE);
    if (!e->win31) UpdateWindow(h);
    return r;
}

/* MLPaint (seg30:0ED8): the frame round the window rect (so scroll bars cover its edge), then
 * MLDrawText over the characters under the update rect; WM_ERASEBKGND has filled the background */
static void ml_paint(HWND h, HDC given)
{
    Edit *e = ed(h);
    PAINTSTRUCT ps;
    HDC dc = given ? given : BeginPaint(h, &ps);
    if (vis(h)) {
        if (e->border) {
            RECT r;
            GetWindowRect(h, &r);
            OffsetRect(&r, -r.left, -r.top);
            if (h->style & WS_THICKFRAME)
                InflateRect(&r, GetSystemMetrics(SM_CXBORDER) - GetSystemMetrics(SM_CXFRAME),
                            GetSystemMetrics(SM_CYBORDER) - GetSystemMetrics(SM_CYFRAME));
            draw_frame(dc, &r);
        }
        edit_clip(h, dc);
        HGDIOBJ of = h->font ? SelectObject(dc, h->font) : NULL;
        if (!given) {
            int a = ml_mouse_to_ich(e, ps.rcPaint.left, ps.rcPaint.top, NULL) - 1;
            if (a == -1) a = 0;
            int b = ml_mouse_to_ich(e, ps.rcPaint.right, ps.rcPaint.bottom, NULL) + 1;
            ml_draw_text(h, dc, a, b);
        } else
            ml_draw_text(h, dc, 0, e->len);
        if (h->font && of) SelectObject(dc, of);
    }
    if (!given) EndPaint(h, &ps);
}

/* MLSetHandle (seg32:018F): a new text buffer from the application */
static void ml_set_handle(HWND h, HLOCAL nh)
{
    Edit *e = ed(h);
    e->hbuf = nh;
    e->len = 0;
    if (LocalSize(nh)) {
        e->len = strlen((char *)LocalLock(nh));
        LocalUnlock(nh);
    }
    ec_empty_undo(e);
    ec_reserve(e, e->len);
    e->modified = 0;
    ml_build_lines(h, 0, 0, 0, NULL, NULL);
    e->mx = e->mn = e->top = e->xoff = e->caret = e->cline = 0;
    SetScrollPos(h, SB_VERT, 0, TRUE);
    SetScrollPos(h, SB_HORZ, 0, TRUE);
    InvalidateRect(h, NULL, TRUE);
}

/* MLInsertCrCrLf (seg32:0000): EM_FMTLINES TRUE - a soft break (CR CR LF) after every wrapped line */
static int ml_insert_crcrlf(HWND h)
{
    Edit *e = ed(h);
    if (!e->wrap || !e->len) return 1;
    int add = 3 * e->nl;
    if (!ec_reserve(e, e->len + add)) { notify(h, EN_ERRSPACE); return 0; }
    char *t = txt(e);
    memmove(t + add, t, e->len);
    int src = add, dst = 0, n = 0;
    for (int i = 0; i < e->nl - 1; i++) {
        int k = e->ls[i + 1] - e->ls[i];
        memmove(t + dst, t + src, k);
        dst += k;
        src += k;
        if (t[dst - 1] != '\n') {
            t[dst++] = '\r'; t[dst++] = '\r'; t[dst++] = '\n';
            n += 3;
        }
    }
    memmove(t + dst, t + src, e->len - e->ls[e->nl - 1]);
    e->len += n;
    t[e->len] = 0;
    untxt(e);
    return n != 0;
}

/* MLStripCrCrLf (seg32:010C): EM_FMTLINES FALSE - the soft breaks out again */
static void ml_strip_crcrlf(Edit *e)
{
    if (!e->len) return;
    char *t = txt(e);
    int s = 0, d = 0, end = e->len;
    while (s < end) {
        if (t[s] == '\r' && t[s + 1] == '\r' && t[s + 2] == '\n') { s += 3; e->len -= 3; }
        else t[d++] = t[s++];
    }
    t[e->len] = 0;
    untxt(e);
    if ((unsigned)e->len < (unsigned)e->caret) e->caret = e->len;
    if ((unsigned)e->len < (unsigned)e->mn) e->mn = e->len;
    if ((unsigned)e->len < (unsigned)e->mx) e->mx = e->len;
}

/* MLCreate (seg31:010D) with ECCreate (seg27:0172): ES_AUTOVSCROLL or WS_VSCROLL scroll vertically,
 * ES_AUTOHSCROLL or WS_HSCROLL horizontally, anything else wraps; ES_CENTER / ES_RIGHT lose the
 * horizontal scrolling */
static int ml_create(HWND h, CREATESTRUCT *cs)
{
    Edit *e = ed(h);
    if (h->style & ES_AUTOHSCROLL) e->autohs = 1;
    e->limit = 30000;
    e->ich_del = e->ins_start = e->ins_end = -1;
    e->hibk = GetSysColor(COLOR_HIGHLIGHT);
    e->hitx = GetSysColor(COLOR_HIGHLIGHTTEXT);
    e->nl = 1;
    e->ls[0] = 0;
    if ((h->style & ES_AUTOVSCROLL) || (h->style & WS_VSCROLL)) e->autovs = 1;
    e->format = h->style & 3;
    if (e->format) { h->style &= ~WS_HSCROLL; e->autohs = 0; }
    if (h->style & WS_HSCROLL) e->autohs = 1;
    e->wrap = !e->autohs && !(h->style & WS_HSCROLL);
    /* ECSetFont(ped, 0, FALSE) */
    set_font(h, NULL);
    ml_size_handler(h);
    if (!e->wrap) ml_build_lines(h, 0, 0, 0, NULL, NULL);
    SetScrollPos(h, SB_VERT, ml_thumb_pos(e, 1), FALSE);
    SetScrollPos(h, SB_HORZ, ml_thumb_pos(e, 0), FALSE);
    RECT r = {0, 0, e->avew * 10, e->lh};
    ml_size(h, &r);
    const char *s = cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName) ? cs->lpszName : NULL;
    if (s && !ml_set_text(h, s)) return -1;
    return 0;
}

/* MLSetTabStops (seg32:03C4): stops in dialog units of the edit's average width (MulDiv by 4); no
 * redraw */
static int ml_set_tab_stops(Edit *e, int n, const int *v)
{
    if (!n) { free(e->pts); e->pts = NULL; return 1; }
    int *p = realloc(e->pts, sizeof(int) * (n + 1));
    if (!p) return 0;
    e->pts = p;
    p[0] = n;
    for (int i = 0; i < n; i++) p[i + 1] = user_muldiv(v[i], e->avew, 4);
    return 1;
}

/* The multi-line window procedure (seg30:20EA), with the messages EditWndProc (seg26:0C96) handles
 * for both kinds */
static LRESULT ml_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Edit *e = ed(h);
    switch (m) {
    /* --- seg26:0C96, shared */
    case WM_ENABLE: return 0; /* (a multi-line edit does not repaint) */
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
    case WM_SETFONT: {
        /* ECSetFont (seg27:02C9) */
        set_font(h, (HFONT)wp);
        if (e->focus) { CreateCaret(h, NULL, 2, e->lh); ShowCaret(h); }
        ml_size_handler(h);
        if (!e->wrap) ml_build_lines(h, 0, 0, 0, NULL, NULL);
        SetScrollPos(h, SB_VERT, ml_thumb_pos(e, 1), (BOOL)lp);
        SetScrollPos(h, SB_HORZ, ml_thumb_pos(e, 0), (BOOL)lp);
        if (lp) {
            RECT r;
            window_rect_client(h, &r);
            InvalidateRect(h, &r, TRUE);
        }
        return 0;
    }
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_COPY: return ec_copy(h);
    case EM_GETSEL: return MAKELONG(e->mn, e->mx);
    case EM_GETRECT: *(RECT *)lp = e->fmt; return 1;
    case EM_GETMODIFY: return e->modified;
    case EM_SETMODIFY: e->modified = wp != 0; return 0;
    case EM_GETLINECOUNT: return e->nl;
    case EM_LIMITTEXT: e->limit = wp ? (int)(WORD)wp : 0xFFFF; return 0;
    case EM_CANUNDO: return e->undo_type != 0;
    case EM_SETPASSWORDCHAR:
        e->pw = (char)wp;
        if (e->pw) e->pww = max(1, text_ext(e, &e->pw, 1));
        return 0;
    case EM_EMPTYUNDOBUFFER: ec_empty_undo(e); return 0;
    case EM_SETREADONLY:
        if (wp) h->style |= ES_READONLY; else h->style &= ~ES_READONLY;
        return TRUE;
    case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC: return 0; /* (not supported) */
    case EM_GETPASSWORDCHAR: return (unsigned char)e->pw;
    case WM_DESTROY:
        if (e->focus) DestroyCaret();
        return DefWindowProc(h, m, wp, lp);

    /* --- seg30:20EA */
    case WM_CREATE: return ml_create(h, (CREATESTRUCT *)lp);
    case WM_SIZE: ml_size_handler(h); return 1;
    case WM_SETFOCUS: {
        /* MLSetFocus (seg30:1DBF) */
        if (!e->focus) {
            e->focus = 1;
            HDC dc = ec_get_dc(h, 1);
            CreateCaret(h, NULL, 2, e->lh);
            ShowCaret(h);
            ml_set_caret_pos(h);
            if (!e->nohidesel && e->mn != e->mx && vis(h)) ml_draw_text(h, dc, e->mn, e->mx);
            ec_release_dc(h, dc, 1);
        }
        notify(h, EN_SETFOCUS);
        return 1;
    }
    case WM_KILLFOCUS:
        /* MLKillFocus (seg30:1E35) */
        if (e->focus) {
            e->focus = 0;
            if (vis(h) && !e->nohidesel && e->mn != e->mx) {
                HDC dc = ec_get_dc(h, 0);
                ml_draw_text(h, dc, e->mn, e->mx);
                ec_release_dc(h, dc, 0);
            }
            HideCaret(h);
            DestroyCaret();
        }
        notify(h, EN_KILLFOCUS);
        return 1;
    case WM_SETREDRAW:
        DefWindowProc(h, m, wp, lp);
        if (wp) InvalidateRect(h, NULL, TRUE); /* RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_FRAME) */
        return 1;
    case WM_SETTEXT: return ml_set_text(h, (const char *)lp);
    case WM_PAINT: ml_paint(h, (HDC)wp); return 1;
    case WM_ERASEBKGND: {
        /* FillWindow(hwndParent, hwnd, hdc, CTLCOLOR_EDIT) */
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, w16_ctl_color(h, (HDC)wp, CTLCOLOR_EDIT));
        return 1;
    }
    case WM_GETDLGCODE:
        if (lp) e->indlg = 1;
        return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_KEYDOWN: ml_key_down(h, (int)wp, 0); return 1;
    case WM_CHAR: ml_char(h, (int)wp, 0); return 1;
    case WM_SYSKEYDOWN:
        if (wp == VK_BACK && (lp & 0x20000000)) { SendMessage(h, EM_UNDO, 0, 0); return 1; }
        break;
    case WM_SYSCHAR:
        if (wp == VK_BACK && (lp & 0x20000000)) return 1;
        break;
    case WM_HSCROLL: case WM_VSCROLL: return ml_scroll(h, m, (int)(WORD)wp, (SHORT)LOWORD(lp));
    case WM_TIMER:
        /* WM_SYSTIMER: the drag repeats its last move */
        if (wp == edit_timer_id) {
            if (e->mdown) ml_mouse_motion(h, WM_MOUSEMOVE, e->msk, e->msx, e->msy);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (e->mdown) ml_mouse_motion(h, m, (int)wp, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        ml_mouse_motion(h, m, (int)wp, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
        return 1;
    case WM_CUT:
        if (e->mn != e->mx && !(h->style & ES_READONLY)) ml_key_down(h, VK_DELETE, 2);
        return 1;
    case WM_PASTE:
        if (!(h->style & ES_READONLY)) ml_paste(h);
        return 1;
    case WM_CLEAR:
        if (e->mn != e->mx && !(h->style & ES_READONLY)) SendMessage(h, WM_CHAR, VK_BACK, 0);
        return 1;
    case WM_UNDO: case EM_UNDO: return ml_undo(h);
    case EM_SETSEL: return ml_set_selection(h, e->win31 && wp == 1, LOWORD(lp), HIWORD(lp));
    case EM_SETRECT: ml_size(h, (RECT *)lp); InvalidateRect(h, NULL, TRUE); return 1;
    case EM_SETRECTNP: ml_size(h, (RECT *)lp); return 1;
    case EM_SCROLL: return ml_scroll(h, WM_VSCROLL, (int)(WORD)wp, (SHORT)LOWORD(lp));
    case EM_LINESCROLL:
        ml_scroll(h, WM_VSCROLL, EM_LINESCROLL, (SHORT)LOWORD(lp));
        ml_scroll(h, WM_HSCROLL, EM_LINESCROLL, (SHORT)HIWORD(lp));
        return 1;
    case EM_LINEINDEX: {
        /* seg32:029F: -1 = the caret's line */
        int l = (WORD)wp == 0xFFFF ? e->cline : (int)(WORD)wp;
        return l >= e->nl ? -1 : e->ls[l];
    }
    case EM_SETHANDLE: ml_set_handle(h, (HLOCAL)wp); return 1;
    case EM_GETHANDLE: return (LRESULT)e->hbuf;
    case EM_LINELENGTH: {
        /* seg32:02CA: -1 = what the selection leaves of its lines */
        char *t = txt(e);
        int r;
        if ((WORD)wp != 0xFFFF) r = ml_line_len(e, t, ml_ich_to_line(e, (WORD)wp));
        else {
            int a = ml_ich_to_line(e, e->mn), b = ml_ich_to_line(e, e->mx);
            if (a == b) r = ml_line_len(e, t, a) - (e->mx - e->mn);
            else r = ml_line_len(e, t, b) + e->ls[b] - e->ls[a] - e->mx + e->mn;
        }
        untxt(e);
        return r;
    }
    case EM_REPLACESEL:
        /* not undoable */
        ec_empty_undo(e);
        ml_delete_text(h);
        ec_empty_undo(e);
        if (lp) ml_insert_text(h, (const char *)lp, strlen((const char *)lp), 0);
        ec_empty_undo(e);
        return 1;
    case EM_GETLINE: {
        /* seg32:0234: no terminating zero */
        int l = (int)(WORD)wp;
        if (e->nl - 1 < l) return 0;
        char *t = txt(e), *buf = (char *)lp;
        int n = min((int)*(WORD *)buf, ml_line_len(e, t, l));
        if (n) memcpy(buf, t + e->ls[l], n);
        untxt(e);
        return n;
    }
    case EM_FMTLINES:
        if (wp) ml_insert_crcrlf(h);
        else ml_strip_crcrlf(e);
        ml_build_lines(h, 0, 0, 0, NULL, NULL);
        return wp != 0;
    case EM_LINEFROMCHAR: return ml_ich_to_line(e, (int)(WORD)wp);
    case EM_SETTABSTOPS: return ml_set_tab_stops(e, (int)wp, (const int *)lp);
    case EM_GETFIRSTVISIBLELINE: return e->top;
    case EM_GETTHUMB: return 0; /* (a scroll command: WM_VSCROLL / WM_HSCROLL with wParam EM_GETTHUMB) */
    }
    return DefWindowProc(h, m, wp, lp);
}

/* ------------------------------------------------------------------ window procedure */
LRESULT w16_edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Edit *e = ed(h);
    switch (m) {
    case WM_NCCREATE: {
        e = calloc(1, sizeof *e);
        h->ctl = e;
        e->multi = (h->style & ES_MULTILINE) != 0;
        e->limit = 30000;
        e->hbuf = LocalAlloc(LMEM_MOVEABLE | LMEM_ZEROINIT, 0x20);
        e->pw = (h->style & ES_PASSWORD) && !e->multi ? '*' : 0;
        e->nohidesel = (h->style & ES_NOHIDESEL) != 0;
        e->win31 = 1; /* GetExpWinVer >= 0x30A: every port is a 3.1 program */
        ls_reserve(e, 4);
        e->nl = 1;
        /* seg27:0056: a WS_BORDER edit takes the style off and draws its own frame inside its
         * client area, so its insets count from the window's edge */
        if (h->style & WS_BORDER) { e->border = 1; h->style &= ~WS_BORDER; }
        if (!e->multi && (h->style & W16_ES_COMBOBOX)) e->combo = h->parent;
        if (!e->multi) set_font(h, NULL);
        DefWindowProc(h, m, wp, lp);
        return TRUE;
    }
    case WM_NCDESTROY:
        if (e) {
            if (e->timer || e->mdown) KillTimer(h, edit_timer_id);
            LocalFree(e->hbuf);
            if (e->undo) LocalFree(e->undo);
            free(e->ls);
            free(e->pts);
            free(e->del);
            free(e);
            h->ctl = NULL;
        }
        return 0;
    }
    if (!e) return DefWindowProc(h, m, wp, lp);
    if (e->multi) return ml_proc(h, m, wp, lp);

    /* single-line */
    switch (m) {
    case WM_CREATE: {
        calc_fmt(h);
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        e->quiet = 1; /* the creation text is not a change the parent hears about */
        set_text(h, cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName) ? cs->lpszName : "");
        e->quiet = 0;
        return 0;
    }
    case WM_DESTROY:
        if (e->focus) DestroyCaret();
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
        sl_paint(h, dc);
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
    case WM_GETDLGCODE: return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
    case WM_SETFOCUS:
        e->focus = 1;
        CreateCaret(h, NULL, sl_caret_w(e), e->lh + 1);      /* seg28:1224 */
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
    case WM_ENABLE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        if (!e->focus) SetFocus(h);
        char *t = txt(e);
        int p = sl_hit(e, t, (SHORT)LOWORD(lp));
        untxt(e);
        move_caret(h, p, (wp & MK_SHIFT) != 0);
        e->mdown = 1;
        SetCapture(h);
        e->timer = SetTimer(h, edit_timer_id, 100, NULL);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        char *t = txt(e);
        int p = sl_hit(e, t, (SHORT)LOWORD(lp));
        int a = p, b = p;
        if (p < e->len && is_word(t[p])) { while (a > 0 && is_word(t[a - 1])) a--; while (b < e->len && is_word(t[b])) b++; while (b < e->len && t[b] == ' ') b++; }
        untxt(e);
        e->anchor = a;
        e->caret = b;
        sl_scroll(h);
        redraw(h);
        place_caret(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->mdown) {
            char *t = txt(e);
            int p = sl_hit(e, t, (SHORT)LOWORD(lp));
            untxt(e);
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
                char *t = txt(e);
                int np = sl_hit(e, t, p.x);
                untxt(e);
                move_caret(h, np, 1);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->mdown) { e->mdown = 0; ReleaseCapture(); KillTimer(h, edit_timer_id); e->timer = 0; }
        return 0;
    case WM_KEYDOWN: {
        int shift = (w16_keystate[VK_SHIFT] & 0x80) != 0, ctrl = (w16_keystate[VK_CONTROL] & 0x80) != 0;
        /* SLKeyDown (seg28:0A93): a combo box's edit hands F4, Page Up/Down and Up/Down to the
         * combo's list; elsewhere Up and Down move as Left and Right, Page Up/Down do nothing */
        if (e->combo && (wp == VK_F4 || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN))
            return SendMessage(e->combo, WM_KEYDOWN, wp, 0);
        if (wp == VK_UP) wp = VK_LEFT;
        else if (wp == VK_DOWN) wp = VK_RIGHT;
        else if (wp == VK_PRIOR || wp == VK_NEXT) return 0;
        char *t = txt(e);
        int np = -1;
        switch (wp) {
        case VK_LEFT: np = ctrl ? word_left(e, t, e->caret) : (!shift && e->anchor != e->caret ? smin(e) : e->caret - 1);
            if (!ctrl && np > 0 && t[np] == '\n' && t[np - 1] == '\r') np--;
            break;
        case VK_RIGHT: np = ctrl ? word_right(e, t, e->caret) : (!shift && e->anchor != e->caret ? smax(e) : e->caret + 1);
            if (!ctrl && np < e->len && np > 0 && t[np - 1] == '\r' && t[np] == '\n') np++;
            break;
        case VK_HOME: np = 0; break;
        case VK_END: np = ctrl ? e->len : line_end(e, t, 0); break;
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
        case '\r': case '\n': case '\t':
            return 0;
        default:
            if (c < 32) return 0;
            {
                char ch = (char)c;
                if (!e->undo_run) save_undo(e);
                e->undo_run = 1;
                replace_sel(h, &ch, 1, 0);
            }
            return 0;
        }
    }
    case WM_VSCROLL: return 0;
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
        if (!wp) sl_scroll(h);
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
    case EM_LIMITTEXT: e->limit = wp ? (int)wp : 30000; return 0;
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
        if (e->multi) {
            /* a multi-line edit rebuilds its lines without EN_UPDATE / EN_CHANGE (measured: real
             * SYSEDIT, which sets its changed flag on EN_CHANGE, loads its files with EM_SETHANDLE
             * and closes them without asking to save) */
            build_lines(h);
            ensure_visible(h);
            redraw(h);
            place_caret(h);
            update_sb(h);
        } else
            refresh(h, 1);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case EM_FMTLINES: {
        /* (single-line text keeps no soft breaks; FALSE takes out any it was given) */
        char *t = txt(e);
        int n = 0;
        for (int i = 0; i < e->len; i++) {
            if (t[i] == '\r' && i + 2 < e->len && t[i + 1] == '\r' && t[i + 2] == '\n') { i += 2; continue; }
            t[n++] = t[i];
        }
        t[n] = 0;
        untxt(e);
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
    case EM_SCROLL: return 0;
    case EM_LINESCROLL:
        e->xoff = max(0, e->xoff + (SHORT)HIWORD(lp) * e->avgw);
        redraw(h);
        place_caret(h);
        update_sb(h);
        return TRUE;
    case EM_GETFIRSTVISIBLELINE: return 0;
    case EM_SETTABSTOPS: return TRUE;
    case EM_GETTHUMB: return GetScrollPos(h, SB_VERT);
    case EM_SETREADONLY:
        if (wp) h->style |= ES_READONLY; else h->style &= ~ES_READONLY;
        return TRUE;
    case EM_SETPASSWORDCHAR:
        e->pw = (char)wp;
        if (e->pw) e->pww = max(1, text_ext(e, &e->pw, 1));
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case EM_GETPASSWORDCHAR: return (unsigned char)e->pw;
    case EM_SETWORDBREAK: case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC: return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}
