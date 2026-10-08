/* LISTBOX and COMBOBOX controls */
#include "w16int.h"
#include <ctype.h>

WORD w16_cmd_slot(HWND h);

typedef struct { char *s; ULONG_PTR data; int sel; int h; } LbItem;
typedef struct {
    LbItem *it;
    int n, cap;
    int top, caret, anchor, cursel;
    int ih;           /* item height */
    int colw;         /* multicolumn */
    int focus, mdown;
    int hext;
    int ntabs;
    int tabs[32];
    HWND combo;       /* owning combobox, if any */
    int redraw_off;
    int want_h;       /* height asked for inside the border; whole items of it are shown */
    int fitting;      /* fit_height is resizing the window */
} Lb;

static Lb *lbd(HWND h) { return (Lb *)h->ctl; }
static int has_strings(HWND h) { return !(h->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (h->style & LBS_HASSTRINGS); }
static int multisel(HWND h) { return (h->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }
static int ownerdraw(HWND h) { return (h->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0; }

static int visible_items(HWND h)
{
    Lb *l = lbd(h);
    RECT r;
    GetClientRect(h, &r);
    return max(1, (r.bottom - r.top) / l->ih);
}

static void lb_notify(HWND h, int code)
{
    Lb *l = lbd(h);
    if (l->combo) { SendMessage(l->combo, WM_USER + 0x500, code, 0); return; }
    if (h->style & LBS_NOTIFY) w16_notify_parent(h, code);
}

static void update_sb(HWND h)
{
    Lb *l = lbd(h);
    if (!(h->style & WS_VSCROLL)) {
        if (!(h->style & LBS_DISABLENOSCROLL) && l->n > visible_items(h) && (h->style & 0x00200000)) {}
        return;
    }
    int vis = visible_items(h);
    int mx = max(0, l->n - vis);
    if (mx == 0 && !(h->style & LBS_DISABLENOSCROLL)) {
        if (h->sb[1].max != 0 || h->sb[1].min != 0) SetScrollRange(h, SB_VERT, 0, 0, TRUE);
        return;
    }
    /* LBS_DISABLENOSCROLL keeps the bar, with both arrows disabled while everything fits */
    if ((h->style & LBS_DISABLENOSCROLL) && h->sb[1].disabled != (mx ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH))
        EnableScrollBar(h, SB_VERT, mx ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH);
    SetScrollRange(h, SB_VERT, 0, max(mx, 1), FALSE);
    SetScrollPos(h, SB_VERT, l->top, TRUE);
}

static void draw_item(HWND h, HDC dc, int i, HBRUSH bg)
{
    Lb *l = lbd(h);
    RECT r;
    GetClientRect(h, &r);
    int y = (i - l->top) * l->ih;
    RECT ir = {0, y, r.right, y + l->ih};
    if (y >= r.bottom || i < l->top) return;
    int sel = i < l->n && (multisel(h) ? l->it[i].sel : i == l->cursel);
    if (ownerdraw(h) && i < l->n) {
        DRAWITEMSTRUCT di = {ODT_LISTBOX, h->id, i, ODA_DRAWENTIRE, (sel ? ODS_SELECTED : 0) | (l->focus && i == l->caret ? ODS_FOCUS : 0),
                             h, dc, ir, l->it[i].data};
        HWND notify = l->combo ? l->combo->parent : h->parent;
        SendMessage(notify, WM_DRAWITEM, h->id, (LPARAM)&di);
        return;
    }
    FillRect(dc, &ir, sel ? w16_sys_brush(COLOR_HIGHLIGHT) : bg);
    if (i >= l->n) return;
    SetBkMode(dc, TRANSPARENT);
    /* a disabled list grays every item, the selected one too (on the highlight) */
    COLORREF old = SetTextColor(dc, (h->style & WS_DISABLED) ? GetSysColor(COLOR_GRAYTEXT) : sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetTextColor(dc));
    const char *s = l->it[i].s ? l->it[i].s : "";
    if (h->style & LBS_USETABSTOPS) TabbedTextOut(dc, 2, y, s, strlen(s), l->ntabs, l->ntabs ? l->tabs : NULL, 2);
    else TextOut(dc, 2, y, s, strlen(s));
    SetTextColor(dc, old);
    if (l->focus && i == l->caret) DrawFocusRect(dc, &ir);
}

static void paint(HWND h, HDC dc)
{
    Lb *l = lbd(h);
    HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
    HBRUSH bg = w16_ctl_color(l->combo ? l->combo : h, dc, CTLCOLOR_LISTBOX);
    RECT r;
    GetClientRect(h, &r);
    int vis = visible_items(h) + 1;
    for (int i = l->top; i < l->top + vis; i++) draw_item(h, dc, i, bg);
    int bottom = (vis) * l->ih;
    if (bottom < r.bottom) { RECT rr = {0, bottom, r.right, r.bottom}; FillRect(dc, &rr, bg); }
    if (l->n == 0 && l->focus) {
        RECT fr = {0, 0, r.right, l->ih};
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, of);
}

static void redraw(HWND h)
{
    if (!w16_window_visible(h) || lbd(h)->redraw_off) return;
    HDC dc = GetDC(h);
    paint(h, dc);
    ReleaseDC(h, dc);
}

static void ensure_visible(HWND h, int i)
{
    Lb *l = lbd(h);
    int vis = visible_items(h);
    if (i < l->top) l->top = i;
    else if (i >= l->top + vis) l->top = i - vis + 1;
    if (l->top < 0) l->top = 0;
    update_sb(h);
}

static int insert(HWND h, int pos, const char *s, ULONG_PTR data)
{
    Lb *l = lbd(h);
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->it = realloc(l->it, sizeof(LbItem) * l->cap);
    }
    if (pos < 0 || pos > l->n) pos = l->n;
    memmove(&l->it[pos + 1], &l->it[pos], sizeof(LbItem) * (l->n - pos));
    l->it[pos].s = has_strings(h) && s ? strdup(s) : NULL;
    l->it[pos].data = has_strings(h) ? 0 : data;
    l->it[pos].sel = 0;
    l->it[pos].h = l->ih;
    l->n++;
    if (l->cursel >= pos) l->cursel++;
    if (l->caret >= pos && l->n > 1) l->caret++;
    update_sb(h);
    redraw(h);
    return pos;
}

static int sorted_pos(HWND h, const char *s, ULONG_PTR data)
{
    Lb *l = lbd(h);
    for (int i = 0; i < l->n; i++) {
        int c;
        if (has_strings(h)) c = lstrcmpi(s, l->it[i].s ? l->it[i].s : "");
        else {
            COMPAREITEMSTRUCT ci = {ODT_LISTBOX, h->id, h, (UINT)-1, data, i, l->it[i].data};
            c = (int)SendMessage(h->parent, WM_COMPAREITEM, h->id, (LPARAM)&ci);
        }
        if (c < 0) return i;
    }
    return l->n;
}

static int find_string(HWND h, int start, const char *s, int exact)
{
    Lb *l = lbd(h);
    if (!l->n) return LB_ERR;
    int n = strlen(s);
    for (int k = 1; k <= l->n; k++) {
        int i = (start + k) % l->n;
        if (start < 0) i = k - 1;
        const char *t = l->it[i].s ? l->it[i].s : "";
        if (exact ? !lstrcmpi(t, s) : !strncasecmp(t, s, n)) return i;
    }
    return LB_ERR;
}

static void set_cur(HWND h, int i, int notify)
{
    Lb *l = lbd(h);
    if (i >= l->n) i = l->n - 1;
    int old = l->cursel;
    l->cursel = i;
    l->caret = i < 0 ? 0 : i;
    if (i >= 0) ensure_visible(h, i);
    redraw(h);
    if (notify && old != i) lb_notify(h, LBN_SELCHANGE);
}

static int item_at(HWND h, int y)
{
    Lb *l = lbd(h);
    int i = l->top + y / l->ih;
    if (y < 0) i = l->top - 1;
    return i;
}

/* unless LBS_NOINTEGRALHEIGHT, a list box shows whole items only: the height it was given is cut
 * down to whole items of the current font, again whenever the font or item height changes (on
 * 3.11, SND.CPL's lists come out 8 items of 13 px high from a 114 px template height) */
static void fit_height(HWND h)
{
    Lb *l = lbd(h);
    if ((h->style & (LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE)) || l->combo || l->ih <= 0) return;
    RECT r;
    GetClientRect(h, &r);
    int want = l->want_h / l->ih * l->ih;
    if (want <= 0 || want == r.bottom) return;
    RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
    l->fitting = 1;
    SetWindowPos(h, NULL, h->rw.left - pr.left, h->rw.top - pr.top, h->rw.right - h->rw.left,
                 h->rw.bottom - h->rw.top - (r.bottom - want), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    l->fitting = 0;
}

LRESULT w16_listbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Lb *l = lbd(h);
    switch (m) {
    case WM_NCCREATE: {
        l = calloc(1, sizeof *l);
        h->ctl = l;
        l->cursel = -1;
        HDC dc = GetDC(NULL);
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        TEXTMETRIC tm;
        GetTextMetrics(dc, &tm);
        ReleaseDC(NULL, dc);
        l->ih = tm.tmHeight;
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        if (cs && cs->lpCreateParams && !strcasecmp(h->cls->name, "COMBOLBOX")) l->combo = (HWND)cs->lpCreateParams;
        return DefWindowProc(h, m, wp, lp);
    }
    case WM_CREATE:
        if (ownerdraw(h)) {
            MEASUREITEMSTRUCT mi = {ODT_LISTBOX, h->id, 0, 0, l->ih, 0};
            SendMessage(l->combo ? l->combo->parent : h->parent, WM_MEASUREITEM, h->id, (LPARAM)&mi);
            if (mi.itemHeight) l->ih = mi.itemHeight;
        }
        if ((h->style & WS_BORDER) && !l->combo) {
            /* 3.1 puts a list box's border around the rectangle it was given, which stays the area
             * inside (measured on 3.11: SND.CPL's lists are a border wider on every side than
             * their template rectangles) */
            RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
            int bx = GetSystemMetrics(SM_CXBORDER), by = GetSystemMetrics(SM_CYBORDER);
            l->fitting = 1;
            SetWindowPos(h, NULL, h->rw.left - pr.left - bx, h->rw.top - pr.top - by, h->rw.right - h->rw.left + 2 * bx,
                         h->rw.bottom - h->rw.top + 2 * by, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            l->fitting = 0;
        }
        {
            RECT r;
            GetClientRect(h, &r);
            l->want_h = r.bottom;
        }
        fit_height(h);
        if (h->style & WS_VSCROLL) {
            if (h->style & LBS_DISABLENOSCROLL) SetScrollRange(h, SB_VERT, 0, 1, FALSE);
            else SetScrollRange(h, SB_VERT, 0, 0, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        if (l) {
            for (int i = 0; i < l->n; i++) free(l->it[i].s);
            free(l->it);
            free(l);
            h->ctl = NULL;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT: {
        h->font = (HFONT)wp;
        HDC dc = GetDC(h);
        SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
        TEXTMETRIC tm;
        GetTextMetrics(dc, &tm);
        ReleaseDC(h, dc);
        if (!ownerdraw(h)) l->ih = tm.tmHeight;
        fit_height(h);
        if (lp) InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_SIZE:
        if (!l->fitting) {
            /* resized from outside: that height is the one to fit */
            RECT r;
            GetClientRect(h, &r);
            l->want_h = r.bottom;
            fit_height(h);
        }
        update_sb(h);
        return 0;
    case WM_SETREDRAW: l->redraw_off = !wp; if (wp) { update_sb(h); InvalidateRect(h, NULL, TRUE); } return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: l->focus = 1; redraw(h); lb_notify(h, LBN_SETFOCUS); return 0;
    case WM_KILLFOCUS: l->focus = 0; redraw(h); lb_notify(h, LBN_KILLFOCUS); return 0;
    case WM_ENABLE: InvalidateRect(h, NULL, TRUE); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (!l->combo) SetFocus(h);
        int i = item_at(h, (SHORT)HIWORD(lp));
        if (i < 0 || i >= l->n) { if (m == WM_LBUTTONDOWN) { l->mdown = 1; SetCapture(h); } return 0; }
        if (m == WM_LBUTTONDBLCLK) { lb_notify(h, LBN_DBLCLK); return 0; }
        if (h->style & LBS_MULTIPLESEL) {
            l->it[i].sel = !l->it[i].sel;
            l->caret = i;
            redraw(h);
            lb_notify(h, LBN_SELCHANGE);
        } else if (h->style & LBS_EXTENDEDSEL) {
            if (wp & MK_SHIFT) {
                for (int k = 0; k < l->n; k++) l->it[k].sel = (k >= min(l->anchor, i) && k <= max(l->anchor, i)) || ((wp & MK_CONTROL) && l->it[k].sel);
            } else if (wp & MK_CONTROL) { l->it[i].sel = !l->it[i].sel; l->anchor = i; }
            else { for (int k = 0; k < l->n; k++) l->it[k].sel = k == i; l->anchor = i; }
            l->caret = i;
            redraw(h);
            lb_notify(h, LBN_SELCHANGE);
        } else
            set_cur(h, i, 1);
        l->mdown = 1;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (l->mdown && !(h->style & LBS_MULTIPLESEL)) {
            RECT r;
            GetClientRect(h, &r);
            int y = (SHORT)HIWORD(lp);
            int i = item_at(h, y);
            if (y >= r.bottom) i = l->top + visible_items(h);
            if (i < 0) i = 0;
            if (i >= l->n) i = l->n - 1;
            if (i >= 0 && i != l->cursel && !(h->style & LBS_EXTENDEDSEL)) set_cur(h, i, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (l->mdown) {
            l->mdown = 0;
            ReleaseCapture();
            if (l->combo) SendMessage(l->combo, WM_USER + 0x501, 0, 0); /* close dropdown */
        }
        return 0;
    case WM_KEYDOWN: {
        if (h->style & LBS_WANTKEYBOARDINPUT) {
            LRESULT r = SendMessage(h->parent, WM_VKEYTOITEM, wp, MAKELPARAM(l->caret, w16_cmd_slot(h)));
            if (r == -2) return 0;
            if (r >= 0) { set_cur(h, (int)r, 1); return 0; }
        }
        int i = l->caret, vis = visible_items(h);
        switch (wp) {
        case VK_UP: case VK_LEFT: i--; break;
        case VK_DOWN: case VK_RIGHT: i++; break;
        case VK_PRIOR: i -= vis - 1; break;
        case VK_NEXT: i += vis - 1; break;
        case VK_HOME: i = 0; break;
        case VK_END: i = l->n - 1; break;
        case VK_SPACE:
            if (h->style & LBS_MULTIPLESEL && l->caret < l->n) { l->it[l->caret].sel = !l->it[l->caret].sel; redraw(h); lb_notify(h, LBN_SELCHANGE); }
            return 0;
        case VK_RETURN:
            if (l->combo) SendMessage(l->combo, WM_USER + 0x501, 0, 0);
            return 0;
        default: return 0;
        }
        if (l->n == 0) return 0;
        if (i < 0) i = 0;
        if (i >= l->n) i = l->n - 1;
        if (multisel(h)) {
            l->caret = i;
            if (h->style & LBS_EXTENDEDSEL) {
                for (int k = 0; k < l->n; k++) l->it[k].sel = k == i;
                l->anchor = i;
                lb_notify(h, LBN_SELCHANGE);
            }
            ensure_visible(h, i);
            redraw(h);
        } else
            set_cur(h, i, 1);
        return 0;
    }
    case WM_CHAR: {
        if (h->style & LBS_WANTKEYBOARDINPUT) {
            LRESULT r = SendMessage(h->parent, WM_CHARTOITEM, wp, MAKELPARAM(l->caret, w16_cmd_slot(h)));
            if (r == -2) return 0;
            if (r >= 0) { set_cur(h, (int)r, 1); return 0; }
        }
        if (!has_strings(h) || wp < 32) return 0;
        char s[2] = {(char)wp, 0};
        int i = find_string(h, l->caret, s, 0);
        if (i >= 0) {
            if (multisel(h)) { l->caret = i; ensure_visible(h, i); redraw(h); }
            else set_cur(h, i, 1);
        }
        return 0;
    }
    case WM_VSCROLL: {
        int vis = visible_items(h), ot = l->top;
        switch (LOWORD(wp)) {
        case SB_LINEUP: l->top--; break;
        case SB_LINEDOWN: l->top++; break;
        case SB_PAGEUP: l->top -= vis - 1; break;
        case SB_PAGEDOWN: l->top += vis - 1; break;
        case SB_TOP: l->top = 0; break;
        case SB_BOTTOM: l->top = l->n; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: l->top = LOWORD(lp); break;
        default: return 0;
        }
        if (l->top > l->n - vis) l->top = l->n - vis;
        if (l->top < 0) l->top = 0;
        if (ot != l->top) redraw(h);
        update_sb(h);
        return 0;
    }
    case LB_ADDSTRING: {
        const char *s = (const char *)lp;
        int pos = (h->style & LBS_SORT) ? sorted_pos(h, has_strings(h) ? s : "", (ULONG_PTR)lp) : l->n;
        return insert(h, pos, has_strings(h) ? s : NULL, (ULONG_PTR)lp);
    }
    case LB_INSERTSTRING: return insert(h, (int)(SHORT)wp, has_strings(h) ? (const char *)lp : NULL, (ULONG_PTR)lp);
    case LB_DELETESTRING: {
        int i = (int)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        free(l->it[i].s);
        memmove(&l->it[i], &l->it[i + 1], sizeof(LbItem) * (l->n - i - 1));
        l->n--;
        if (l->cursel == i) l->cursel = -1;
        else if (l->cursel > i) l->cursel--;
        if (l->caret >= l->n) l->caret = max(0, l->n - 1);
        if (l->top > max(0, l->n - visible_items(h))) l->top = max(0, l->n - visible_items(h));
        update_sb(h);
        redraw(h);
        return l->n;
    }
    case LB_RESETCONTENT:
        for (int i = 0; i < l->n; i++) free(l->it[i].s);
        l->n = 0; l->top = 0; l->cursel = -1; l->caret = 0;
        update_sb(h);
        redraw(h);
        return 0;
    case LB_GETCOUNT: return l->n;
    case LB_GETCURSEL: return multisel(h) ? l->caret : l->cursel;
    case LB_SETCURSEL:
        if (multisel(h)) return LB_ERR;
        if ((int)(SHORT)wp < 0 || (int)wp >= l->n) { set_cur(h, -1, 0); return LB_ERR; }
        set_cur(h, (int)wp, 0);
        return wp;
    case LB_GETSEL: return ((int)wp >= 0 && (int)wp < l->n) ? (multisel(h) ? l->it[wp].sel : (int)wp == l->cursel) : LB_ERR;
    case LB_SETSEL: {
        int i = (int)(SHORT)LOWORD(lp);
        if (!multisel(h)) return LB_ERR;
        if (i == -1) { for (int k = 0; k < l->n; k++) l->it[k].sel = wp != 0; }
        else if (i >= 0 && i < l->n) { l->it[i].sel = wp != 0; l->caret = i; }
        redraw(h);
        return 0;
    }
    case LB_SELITEMRANGE: {
        int a = LOWORD(lp), b = HIWORD(lp);
        for (int k = a; k <= b && k < l->n; k++) l->it[k].sel = wp != 0;
        redraw(h);
        return 0;
    }
    case LB_GETSELCOUNT: { if (!multisel(h)) return LB_ERR; int c = 0; for (int k = 0; k < l->n; k++) c += l->it[k].sel; return c; }
    case LB_GETSELITEMS: {
        int *out = (int *)lp, c = 0;
        for (int k = 0; k < l->n && c < (int)wp; k++) if (l->it[k].sel) out[c++] = k;
        return c;
    }
    case LB_GETTEXT: {
        int i = (int)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        if (!has_strings(h)) { *(DWORD *)lp = (DWORD)l->it[i].data; return 4; }
        strcpy((char *)lp, l->it[i].s ? l->it[i].s : "");
        return strlen((char *)lp);
    }
    case LB_GETTEXTLEN: { int i = (int)wp; if (i < 0 || i >= l->n) return LB_ERR; return has_strings(h) ? (int)strlen(l->it[i].s ? l->it[i].s : "") : 4; }
    case LB_GETITEMDATA: { int i = (int)wp; if (i < 0 || i >= l->n) return LB_ERR; return l->it[i].data; }
    case LB_SETITEMDATA: { int i = (int)wp; if (i < 0 || i >= l->n) return LB_ERR; l->it[i].data = (ULONG_PTR)lp; return 0; }
    case LB_FINDSTRING: return find_string(h, (int)(SHORT)wp, (const char *)lp, 0);
    case LB_FINDSTRINGEXACT: return find_string(h, (int)(SHORT)wp, (const char *)lp, 1);
    case LB_SELECTSTRING: {
        int i = find_string(h, (int)(SHORT)wp, (const char *)lp, 0);
        if (i >= 0 && !multisel(h)) set_cur(h, i, 0);
        return i;
    }
    case LB_GETTOPINDEX: return l->top;
    case LB_SETTOPINDEX: l->top = max(0, min((int)wp, l->n - 1)); update_sb(h); redraw(h); return 0;
    case LB_GETITEMRECT: {
        RECT r;
        GetClientRect(h, &r);
        int i = (int)wp;
        SetRect((RECT *)lp, 0, (i - l->top) * l->ih, r.right, (i - l->top + 1) * l->ih);
        return 1;
    }
    case LB_SETITEMHEIGHT: l->ih = max(1, (int)LOWORD(lp)); fit_height(h); update_sb(h); InvalidateRect(h, NULL, TRUE); return 0;
    case LB_GETITEMHEIGHT: return l->ih;
    case LB_SETTABSTOPS: {
        W16Dialog *d = w16_dlg(h->parent);
        int cx = d ? d->cxchar : 8;
        l->ntabs = min((int)wp, 32);
        for (int i = 0; i < l->ntabs; i++) l->tabs[i] = ((int *)lp)[i] * cx / 4;
        return TRUE;
    }
    case LB_SETCARETINDEX: l->caret = (int)wp; ensure_visible(h, l->caret); redraw(h); return 0;
    case LB_GETCARETINDEX: return l->caret;
    case LB_SETCOLUMNWIDTH: l->colw = (int)wp; return 0;
    case LB_GETHORIZONTALEXTENT: return l->hext;
    case LB_SETHORIZONTALEXTENT: l->hext = (int)wp; return 0;
    case LB_DIR: return w16_dir_add(h, (UINT)wp, (LPCSTR)lp, 0);
    }
    return DefWindowProc(h, m, wp, lp);
}

LRESULT w16_combolbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return w16_listbox_proc(h, m, wp, lp);
}

/* ------------------------------------------------------------------ COMBOBOX */
typedef struct {
    HWND edit;      /* CBS_SIMPLE / CBS_DROPDOWN */
    HWND list;
    int dropped;
    int btn_down;
    RECT btn;
    RECT field;       /* the edit or static part */
    RECT droprc;      /* where the list goes, in the combo's coordinates */
    int full_h;       /* the height the combo was created with (field + list) */
    int ih;
    int drop_h;
    int focus;
} Cb;

static Cb *cbd(HWND h) { return (Cb *)h->ctl; }
static int cbtype(HWND h) { return h->style & 3; }

static void cb_text_from_list(HWND h)
{
    Cb *c = cbd(h);
    int i = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
    char buf[512] = "";
    if (i >= 0) SendMessage(c->list, LB_GETTEXT, i, (LPARAM)buf);
    if (c->edit) { SetWindowText(c->edit, buf); SendMessage(c->edit, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF)); }
    else { free(h->text); h->text = strdup(buf); InvalidateRect(h, NULL, FALSE); }
}

/* USER seg34:02AC, at creation and again on WM_SETFONT: the field is the font's height + min(that,
 * the system font's height) / 4 + 4 borders high; a drop-down's button is the right SM_CXVSCROLL
 * pixels; a drop-down list's field reaches under the button's left border, a drop-down edit stops a
 * system-font character width (8 on VGA) short of it; the list starts on the field's bottom line,
 * indented by that width except under a drop-down list. On 3.11 the Ports settings combos (8 pt Helv)
 * come out 20 px high, edits 53 and lists 61 px wide in a 77 px combo. */
static void cb_layout(HWND h)
{
    Cb *c = cbd(h);
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
    TEXTMETRIC tm, sys;
    GetTextMetrics(dc, &tm);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    GetTextMetrics(dc, &sys);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER), cxsys = LOWORD(GetDialogBaseUnits());
    int w = h->rc.right - h->rc.left;
    int eh = tm.tmHeight + min(tm.tmHeight, sys.tmHeight) / 4 + 4 * cyb, fw = w;
    c->ih = tm.tmHeight;
    if (cbtype(h) == CBS_SIMPLE)
        SetRectEmpty(&c->btn);
    else {
        int bw = GetSystemMetrics(SM_CXVSCROLL);
        SetRect(&c->btn, w - bw, 0, w, eh);
        fw = max(0, w - bw + cxb);
        if (cbtype(h) == CBS_DROPDOWN) fw = max(0, fw - cxsys);
    }
    SetRect(&c->field, 0, 0, fw, eh);
    SetRect(&c->droprc, cbtype(h) == CBS_DROPDOWNLIST ? 0 : cxsys, eh - cyb, w, c->full_h - cyb);
    c->drop_h = c->droprc.bottom - c->droprc.top;
    if (c->edit) SetWindowPos(c->edit, NULL, 0, 0, fw, eh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (cbtype(h) == CBS_SIMPLE)
        SetWindowPos(c->list, NULL, c->droprc.left, c->droprc.top, c->droprc.right - c->droprc.left, c->drop_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    else if (h->rc.bottom - h->rc.top != eh) {
        /* the combo window itself is only as tall as its field */
        RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
        SetWindowPos(h, NULL, h->rw.left - pr.left, h->rw.top - pr.top, h->rw.right - h->rw.left, eh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
}

static void cb_show(HWND h, int show)
{
    Cb *c = cbd(h);
    if (cbtype(h) == CBS_SIMPLE || c->dropped == show) return;
    c->dropped = show;
    if (show) {
        w16_notify_parent(h, CBN_DROPDOWN);
        RECT r = h->rw;
        int w = r.right - r.left;
        int n = (int)SendMessage(c->list, LB_GETCOUNT, 0, 0);
        int maxh = c->drop_h > 0 ? c->drop_h : c->ih * 8 + 2;
        int hh = min(maxh, max(1, n) * c->ih + 2);
        /* UNTESTED against 3.11: the dropped list's height (sized to its items here) */
        SetWindowPos(c->list, HWND_TOP, r.left + c->droprc.left, r.top + c->droprc.top, w - c->droprc.left, hh,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        int sel = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessage(c->list, LB_SETTOPINDEX, sel, 0);
        SetCapture(c->list);
        lbd(c->list)->mdown = 1;
    } else {
        ShowWindow(c->list, SW_HIDE);
        if (GetCapture() == c->list) ReleaseCapture();
        w16_notify_parent(h, CBN_CLOSEUP);
    }
    InvalidateRect(h, NULL, FALSE);
}

static void cb_paint(HWND h, HDC dc)
{
    Cb *c = cbd(h);
    RECT r;
    GetClientRect(h, &r);
    HGDIOBJ of = SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
    HBRUSH bg = w16_ctl_color(h, dc, CTLCOLOR_LISTBOX);
    if (cbtype(h) != CBS_SIMPLE) {
        /* button: black frame + 3D face + OBM_COMBO arrow (3.1 look) */
        RECT b = c->btn;
        FrameRect(dc, &b, w16_sys_brush(COLOR_WINDOWFRAME));
        InflateRect(&b, -1, -1);
        FillRect(dc, &b, w16_sys_brush(COLOR_BTNFACE));
        HBRUSH hi = w16_sys_brush(COLOR_BTNHIGHLIGHT), sh = w16_sys_brush(COLOR_BTNSHADOW);
        if (!c->btn_down) {
            /* 2 px shadow, then the highlight over it: the inner shadow line starts a pixel in (3.11) */
            RECT t;
            SetRect(&t, b.left, b.bottom - 2, b.right, b.bottom); FillRect(dc, &t, sh);
            SetRect(&t, b.right - 2, b.top, b.right, b.bottom); FillRect(dc, &t, sh);
            SetRect(&t, b.left, b.top, b.right - 1, b.top + 1); FillRect(dc, &t, hi);
            SetRect(&t, b.left, b.top, b.left + 1, b.bottom - 1); FillRect(dc, &t, hi);
        } else {
            RECT t;
            SetRect(&t, b.left, b.top, b.right, b.top + 1); FillRect(dc, &t, sh);
            SetRect(&t, b.left, b.top, b.left + 1, b.bottom); FillRect(dc, &t, sh);
        }
        W16Bitmap *ar = w16_obm(OBM_COMBO);
        if (ar) {
            int ax = b.left + (b.right - b.left - ar->w) / 2 + (c->btn_down ? 1 : 0), ay = b.top + (b.bottom - b.top - ar->h) / 2 + (c->btn_down ? 1 : 0);
            int dx = ax, dy = ay;
            w16_lp_to_dp(dc, &dx, &dy);
            Region e;
            w16_dc_clip_iter_begin(dc, &e);
            for (int yy = 0; yy < ar->h; yy++)
                for (int xx = 0; xx < ar->w; xx++)
                    if (!ar->px[yy * ar->w + xx] && rgn_contains(&e, dx + xx, dy + yy))
                        w16_screen.px[(dy + yy) * w16_screen.w + dx + xx] = w16_rgb(GetSysColor(COLOR_BTNTEXT));
            rgn_free(&e);
            w16_screen_dirty = 1;
        }
    }
    if (cbtype(h) == CBS_DROPDOWNLIST) {
        /* USER seg33:0E77: inside the frame the control colour, then 1 px in the text cell: opaque
         * text 1 px in from its corner, in the highlight colours with a focus rectangle around it
         * while the box has the focus and is not dropped */
        RECT t = c->field;
        int sel = c->focus && !c->dropped;
        FrameRect(dc, &t, w16_sys_brush(COLOR_WINDOWFRAME));
        InflateRect(&t, -GetSystemMetrics(SM_CXBORDER), -GetSystemMetrics(SM_CYBORDER));
        FillRect(dc, &t, bg);
        InflateRect(&t, -1, -1);
        if (sel) {
            FillRect(dc, &t, w16_sys_brush(COLOR_HIGHLIGHT));
            SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
            SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        } else if (h->style & WS_DISABLED)
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        SetBkMode(dc, OPAQUE);
        ExtTextOut(dc, t.left + 1, t.top + 1, ETO_CLIPPED | ETO_OPAQUE, &t, h->text, strlen(h->text), NULL);
        if (sel) DrawFocusRect(dc, &t);
    }
    SelectObject(dc, of);
}

LRESULT w16_combobox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Cb *c = cbd(h);
    switch (m) {
    case WM_NCCREATE: {
        c = calloc(1, sizeof *c);
        h->ctl = c;
        /* WS_VSCROLL in a template means the drop-down list scrolls; the combo itself has
         * no scroll bars and draws its own frame */
        h->style &= ~(WS_VSCROLL | WS_HSCROLL | WS_BORDER);
        return DefWindowProc(h, m, wp, lp);
    }
    case WM_CREATE: {
        RECT r;
        GetClientRect(h, &r);
        c->full_h = r.bottom;
        DWORD ls = WS_BORDER | WS_VSCROLL | LBS_NOTIFY | (h->style & CBS_SORT ? LBS_SORT : 0) |
                   (h->style & CBS_OWNERDRAWFIXED ? LBS_OWNERDRAWFIXED : 0) | (h->style & CBS_HASSTRINGS ? LBS_HASSTRINGS : 0) | LBS_NOINTEGRALHEIGHT;
        if (cbtype(h) != CBS_DROPDOWNLIST)
            c->edit = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, r.right, 1, h, (HMENU)1001, NULL, NULL);
        if (cbtype(h) == CBS_SIMPLE)
            c->list = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | ls, 0, 0, r.right, 1, h, (HMENU)1000, NULL, NULL);
        else
            c->list = CreateWindowEx(WS_EX_TOPMOST, "COMBOLBOX", "", WS_POPUP | ls, 0, 0, r.right, 100, h, NULL, NULL, h);
        lbd(c->list)->combo = h;
        cb_layout(h);
        return 0;
    }
    case WM_DESTROY:
        if (c->list && cbtype(h) != CBS_SIMPLE) DestroyWindow(c->list);
        return 0;
    case WM_NCDESTROY: free(c); h->ctl = NULL; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        cb_paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, w16_ctl_color(h, (HDC)wp, CTLCOLOR_DLG));
        return 1;
    }
    case WM_SETFONT:
        h->font = (HFONT)wp;
        if (c->edit) SendMessage(c->edit, WM_SETFONT, wp, lp);
        SendMessage(c->list, WM_SETFONT, wp, lp);
        cb_layout(h);
        if (lp) InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_GETDLGCODE: return DLGC_WANTCHARS | DLGC_WANTARROWS;
    case WM_SETFOCUS:
        c->focus = 1;
        if (c->edit) { SetFocus(c->edit); SendMessage(c->edit, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF)); }
        else InvalidateRect(h, NULL, FALSE);
        w16_notify_parent(h, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        if ((HWND)wp == c->edit || (HWND)wp == c->list) return 0;
        c->focus = 0;
        cb_show(h, 0);
        InvalidateRect(h, NULL, FALSE);
        w16_notify_parent(h, CBN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = {(SHORT)LOWORD(lp), (SHORT)HIWORD(lp)};
        if (cbtype(h) == CBS_SIMPLE) return 0;
        if (!c->edit || PtInRect(&c->btn, p)) {
            SetFocus(h);
            c->btn_down = PtInRect(&c->btn, p);
            cb_show(h, !c->dropped);
            c->btn_down = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F4 || ((wp == VK_DOWN || wp == VK_UP) && (w16_keystate[VK_MENU] & 0x80))) { cb_show(h, !c->dropped); return 0; }
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_HOME || wp == VK_END) {
            int old = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            SendMessage(c->list, WM_KEYDOWN, wp, lp);
            int now = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            if (old != now) { cb_text_from_list(h); w16_notify_parent(h, CBN_SELCHANGE); }
            return 0;
        }
        if ((wp == VK_RETURN || wp == VK_ESCAPE) && c->dropped) { cb_show(h, 0); return 0; }
        return 0;
    case WM_CHAR:
        if (!c->edit) {
            int old = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            SendMessage(c->list, WM_CHAR, wp, lp);
            int now = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            if (old != now) { cb_text_from_list(h); w16_notify_parent(h, CBN_SELCHANGE); }
        }
        return 0;
    case WM_COMMAND:
        if (c->edit && (HWND)W16_CMD_HWND(lp) == c->edit) {
            if (HIWORD(lp) == EN_CHANGE) w16_notify_parent(h, CBN_EDITCHANGE);
            else if (HIWORD(lp) == EN_UPDATE) w16_notify_parent(h, CBN_EDITUPDATE);
            else if (HIWORD(lp) == EN_KILLFOCUS && w16_focus != h && w16_focus != c->list) { c->focus = 0; w16_notify_parent(h, CBN_KILLFOCUS); }
        }
        if (cbtype(h) == CBS_SIMPLE && W16_CMD_HWND(lp) == c->list) {
            if (HIWORD(lp) == LBN_SELCHANGE) { cb_text_from_list(h); w16_notify_parent(h, CBN_SELCHANGE); }
            if (HIWORD(lp) == LBN_DBLCLK) w16_notify_parent(h, CBN_DBLCLK);
        }
        return 0;
    case WM_USER + 0x500: /* from the drop-down list */
        if (wp == LBN_SELCHANGE) { cb_text_from_list(h); w16_notify_parent(h, CBN_SELCHANGE); }
        if (wp == LBN_DBLCLK) w16_notify_parent(h, CBN_DBLCLK);
        return 0;
    case WM_USER + 0x501: cb_show(h, 0); return 0;
    case WM_SETTEXT:
        if (c->edit) return SendMessage(c->edit, WM_SETTEXT, wp, lp);
        return DefWindowProc(h, m, wp, lp);
    case WM_GETTEXT:
        if (c->edit) return SendMessage(c->edit, WM_GETTEXT, wp, lp);
        return DefWindowProc(h, m, wp, lp);
    case WM_GETTEXTLENGTH:
        if (c->edit) return SendMessage(c->edit, WM_GETTEXTLENGTH, wp, lp);
        return DefWindowProc(h, m, wp, lp);
    case WM_ENABLE:
        if (c->edit) EnableWindow(c->edit, wp != 0);
        EnableWindow(c->list, wp != 0);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case CB_ADDSTRING: return SendMessage(c->list, LB_ADDSTRING, wp, lp);
    case CB_INSERTSTRING: return SendMessage(c->list, LB_INSERTSTRING, wp, lp);
    case CB_DELETESTRING: return SendMessage(c->list, LB_DELETESTRING, wp, lp);
    case CB_RESETCONTENT: SendMessage(c->list, LB_RESETCONTENT, 0, 0); if (!c->edit) { free(h->text); h->text = strdup(""); InvalidateRect(h, NULL, FALSE); } return 0;
    case CB_GETCOUNT: return SendMessage(c->list, LB_GETCOUNT, 0, 0);
    case CB_DIR: return w16_dir_add(c->list, (UINT)wp, (LPCSTR)lp, 0);
    case CB_GETCURSEL: return SendMessage(c->list, LB_GETCURSEL, 0, 0);
    case CB_SETCURSEL: { LRESULT r = SendMessage(c->list, LB_SETCURSEL, wp, 0); cb_text_from_list(h); return r; }
    case CB_GETLBTEXT: return SendMessage(c->list, LB_GETTEXT, wp, lp);
    case CB_GETLBTEXTLEN: return SendMessage(c->list, LB_GETTEXTLEN, wp, lp);
    case CB_FINDSTRING: return SendMessage(c->list, LB_FINDSTRING, wp, lp);
    case CB_FINDSTRINGEXACT: return SendMessage(c->list, LB_FINDSTRINGEXACT, wp, lp);
    case CB_SELECTSTRING: { LRESULT r = SendMessage(c->list, LB_SELECTSTRING, wp, lp); if (r >= 0) cb_text_from_list(h); return r; }
    case CB_GETITEMDATA: return SendMessage(c->list, LB_GETITEMDATA, wp, lp);
    case CB_SETITEMDATA: return SendMessage(c->list, LB_SETITEMDATA, wp, lp);
    case CB_SHOWDROPDOWN: cb_show(h, wp != 0); return TRUE;
    case CB_GETDROPPEDSTATE: return c->dropped;
    case CB_LIMITTEXT: if (c->edit) SendMessage(c->edit, EM_LIMITTEXT, wp, 0); return TRUE;
    case CB_GETEDITSEL: return c->edit ? SendMessage(c->edit, EM_GETSEL, 0, 0) : CB_ERR;
    case CB_SETEDITSEL: return c->edit ? SendMessage(c->edit, EM_SETSEL, 0, lp) : CB_ERR;
    case CB_GETDROPPEDCONTROLRECT: GetWindowRect(c->list, (RECT *)lp); return 0;
    case CB_SETITEMHEIGHT: return SendMessage(c->list, LB_SETITEMHEIGHT, wp, lp);
    case CB_GETITEMHEIGHT: return SendMessage(c->list, LB_GETITEMHEIGHT, wp, lp);
    case CB_SETEXTENDEDUI: case CB_GETEXTENDEDUI: return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}
