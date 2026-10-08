/* LISTBOX and COMBOBOX controls.
 * The list box is a function-by-function port of USER.EXE's (Windows for Workgroups 3.11) list box:
 * the window procedure seg35:0100, its helpers in seg35, the multicolumn code in seg36, creation /
 * size / font / item height in seg38, and the item store and scroll bars in seg43. Each function
 * names the routine it ports. Not ported: LBS_OWNERDRAWVARIABLE's per-item heights (treated as
 * fixed), the WM_DRAGSELECT / WM_DRAGLOOP item lookup and WS_EX_DRAGOBJECT's DragDetect (seg35:15D5),
 * GrayString for a disabled list when COLOR_GRAYTEXT is black (seg35:075F), Windows 2.x programs'
 * differences (fWin2App). */
#include "w16int.h"
#include <ctype.h>

WORD w16_cmd_slot(HWND h);

/* one item; sel is USER's selection byte of a multiple-selection list: the low nibble says
 * selected, the high nibble hilited (seg35:0964 / seg35:0F25) */
typedef struct { char *s; ULONG_PTR data; BYTE sel; } LbItem;
typedef struct {
    LbItem *it;
    int n, cap;        /* cMac (+0C) */
    int top;           /* iTop (+04) */
    int cursel;        /* iSel (+06) */
    int caret;         /* iSelBase (+08): the caret item */
    int cifm;          /* cItemFullMax (+0A): whole items in the window */
    int lastsel;       /* iLastSelection (+2E): a combo's selection when the mouse went down */
    int anchor;        /* iMouseDown (+30): the anchor */
    int lastmove;      /* iLastMouseMove (+32) */
    int cxchar;        /* average character width of the font (+18) */
    int ih;            /* item height, cyChar (+1A) */
    int colw;          /* cxColumn (+1C) */
    int ipc, ncols;    /* itemsPerColumn (+1E), numberOfColumns (+20) */
    POINT ptprev;      /* ptPrev (+22) */
    int ownerdraw;     /* OwnerDraw (+26): 0, 1 fixed, 2 variable */
    int multiple;      /* wMultiple (+2A): 0 single, 1 LBS_MULTIPLESEL, 2 LBS_EXTENDEDSEL */
    int *tabs, ntabs;  /* (+34) */
    int xorg, hext;    /* xOrigin (+38), maxWidth (+3A) */
    /* flag bytes +28 / +2C / +2D */
    unsigned redraw : 1, defer : 1, sort : 1, notify : 1, mdown : 1, focus : 1, dblclk : 1, caret_drawn : 1,
        addsel : 1, strings : 1, newstate : 1, tabstops : 1, multicol : 1, nointegral : 1, wantkb : 1, scroll : 1,
        disnoscroll : 1;
    HWND combo;        /* owning combobox, if any (pcbox +3E) */
    int caret_on;      /* the combo turned the dropped list's caret on (LBCB_CARETON) */
    int cbtrack;       /* the combo dropped the list with the mouse captured */
    int want_h;        /* height asked for inside the border; whole items of it are shown */
    int fitting;       /* fit_height is resizing the window */
    int fit_h;         /* the client height fit_height gave it last */
} Lb;

/* USER's system timers on the list (SetSystemTimer ids 1 and 2) */
#define LB_TIMER_TRACK 0xFFF1 /* auto-scroll while the mouse is down (seg35:12A8) */
#define LB_TIMER_CARET 0xFFF2 /* the blinking caret of Shift+F8's add mode (seg35:1B6C) */
#define LB_KEYDOWN_ITEM 0x401 /* seg35:18A8's private "move to item" message */

static Lb *lbd(HWND h) { return (Lb *)h->ctl; }

/* where a list box sends WM_MEASUREITEM, WM_DRAWITEM, WM_COMPAREITEM, WM_DELETEITEM and its
 * notifications: its parent, or the combo box it belongs to, which hands them on to its own parent
 * as the combo's (USER seg33:073F) */
static HWND lb_owner(HWND h)
{
    Lb *l = lbd(h);
    return l && l->combo ? l->combo : h->parent;
}

/* fRedraw && IsWindowVisible (seg1:1C5F), the test before every drawing */
static int lb_visible(HWND h) { return lbd(h)->redraw && w16_window_visible(h); }
/* fCaret: the list has the focus (or its combo turned the caret on) */
static int lb_fcaret(Lb *l) { return l->focus || l->caret_on; }

/* seg35:11BD: WM_COMMAND to the parent (a combo's list tells the combo) */
static void lb_notify(HWND h, int code)
{
    Lb *l = lbd(h);
    if (l->combo) { SendMessage(l->combo, WM_USER + 0x500, code, 0); return; }
    w16_notify_parent(h, code);
}

static HBRUSH lb_brush(HWND h, HDC dc)
{
    Lb *l = lbd(h);
    return w16_ctl_color(l->combo ? l->combo : h, dc, CTLCOLOR_LISTBOX);
}

/* seg35:04BA: a DC with the list's font, clipped to the client area, origin at xOrigin */
static HDC lb_getdc(HWND h)
{
    Lb *l = lbd(h);
    HDC dc = GetDC(h);
    if (h->font) SelectObject(dc, h->font);
    if (h->style & WS_VISIBLE) {
        RECT r;
        GetClientRect(h, &r);
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    } else
        IntersectClipRect(dc, 0, 0, 0, 0);
    SetWindowOrg(dc, l->xorg, 0);
    return dc;
}
/* seg35:0520 */
static void lb_releasedc(HWND h, HDC dc)
{
    if (h->font) SelectObject(dc, GetStockObject(SYSTEM_FONT));
    ReleaseDC(h, dc);
}

/* seg35:09D2 CItemInWindow: the items the window shows, partly visible ones too if fPartial */
static int lb_cinwin(HWND h, int partial)
{
    Lb *l = lbd(h);
    if (l->multicol) return (l->ncols + (partial ? 1 : 0)) * l->ipc;
    RECT r;
    GetClientRect(h, &r);
    return r.bottom / l->ih + (partial && r.bottom % l->ih ? 1 : 0);
}

/* seg38:0000 */
static void lb_set_cifm(HWND h) { lbd(h)->cifm = lb_cinwin(h, 0); }

/* seg35:0FAB: the last whole item shown */
static int lb_last_full(HWND h)
{
    Lb *l = lbd(h);
    int i = l->top + l->cifm - 1;
    return i > l->n - 1 ? l->n - 1 : i;
}

/* seg35:054A GetItemRect: LB_ERR for a bad index (item 0 is always good), else whether the item is
 * (partly) visible */
static int lb_item_rect(HWND h, int i, RECT *r)
{
    Lb *l = lbd(h);
    if (i && i >= l->n) return LB_ERR;
    GetClientRect(h, r);
    if (l->multicol) {
        int col = i / l->ipc;
        r->top = (i - col * l->ipc) * l->ih;
        r->bottom = r->top + l->ih;
        r->left += (col - l->top / l->ipc) * l->colw;
        r->right = r->left + l->colw;
    } else {
        r->right += l->xorg;
        r->top = (i - l->top) * l->ih;
        r->bottom = r->top + l->ih;
    }
    return l->top <= i && lb_cinwin(h, 1) + l->top > i;
}

/* seg35:0964 IsSelected: -1 for a bad index; wOpFlags 1 = the hilite state, else the selection */
static int lb_is_sel(Lb *l, int i, int hilite)
{
    if ((unsigned)i >= (unsigned)l->n) return -1;
    if (!l->multiple) return l->cursel == i;
    return hilite ? l->it[i].sel >> 4 : l->it[i].sel & 15;
}

/* seg35:0F25 SetSelected: op 1 = hilite only, 2 = selection only, 3 = both */
static void lb_set_sel(Lb *l, int i, int fsel, int op)
{
    if (i >= l->n || i < 0) return;
    if (!l->multiple) {
        if (fsel) l->cursel = i;
        return;
    }
    BYTE v = (BYTE)fsel, mask = 0;
    if (op == 1) { v <<= 4; mask = 0x0F; }
    else if (op == 2) mask = 0xF0;
    else if (op == 3) { v |= (BYTE)(v << 4); mask = 0; }
    l->it[i].sel = (BYTE)((l->it[i].sel & mask) | v);
}

static ULONG_PTR lb_item_data(Lb *l, int i)
{
    /* seg43:01D4 */
    if ((unsigned)i >= (unsigned)l->n) return (ULONG_PTR)(LONG)LB_ERR;
    return l->it[i].data;
}

/* seg35:23E1: WM_DRAWITEM to the owner */
static void lb_draw_od(HWND h, int i, HDC dc, UINT action, UINT state, const RECT *r)
{
    Lb *l = lbd(h);
    DRAWITEMSTRUCT di;
    di.CtlType = ODT_LISTBOX;
    di.CtlID = h->id;
    di.itemID = l->n > i ? (UINT)i : (UINT)-1;
    di.itemAction = action;
    di.itemState = state | ((h->style & WS_DISABLED) ? ODS_DISABLED : 0);
    di.hwndItem = h;
    di.hDC = dc;
    di.rcItem = *r;
    di.itemData = l->n ? lb_item_data(l, i) : 0;
    SendMessage(lb_owner(h), WM_DRAWITEM, h->id, (LPARAM)&di);
}

/* seg35:085C CaretOn: the focus rectangle (ODA_FOCUS for owner-draw lists) on the caret item */
static void lb_caret_on(HWND h)
{
    Lb *l = lbd(h);
    if (!lb_fcaret(l) || l->caret_drawn || !lb_visible(h)) return;
    HDC dc = lb_getdc(h);
    RECT r;
    lb_item_rect(h, l->caret, &r);
    r.right += l->xorg;
    if (l->ownerdraw)
        lb_draw_od(h, l->caret, dc, ODA_FOCUS, lb_is_sel(l, l->caret, 1) ? ODS_FOCUS | ODS_SELECTED : ODS_FOCUS, &r);
    else
        DrawFocusRect(dc, &r);
    lb_releasedc(h, dc);
    l->caret_drawn = 1;
}

/* seg35:08E1 CaretOff */
static void lb_caret_off(HWND h)
{
    Lb *l = lbd(h);
    if (!lb_fcaret(l) || !l->caret_drawn) return;
    if (lb_visible(h)) {
        HDC dc = lb_getdc(h);
        RECT r;
        lb_item_rect(h, l->caret, &r);
        r.right += l->xorg;
        if (l->ownerdraw)
            lb_draw_od(h, l->caret, dc, ODA_FOCUS, lb_is_sel(l, l->caret, 1) ? ODS_SELECTED : 0, &r);
        else
            DrawFocusRect(dc, &r);
        lb_releasedc(h, dc);
    }
    l->caret_drawn = 0;
}

/* seg35:11EA SetISelBase: move the caret */
static void lb_set_caret(HWND h, int i)
{
    lb_caret_off(h);
    lbd(h)->caret = i;
    lb_caret_on(h);
}

static const char *lb_str(Lb *l, int i) { return i >= 0 && i < l->n && l->it[i].s ? l->it[i].s : ""; }

/* seg35:069A: a text item. Its background is filled only when hilited, for the caret item and in
 * tab-stop lists; the rest relies on WM_ERASEBKGND or the opaque text of multiple-selection lists. */
static void lb_draw_text(HWND h, int i, HDC dc, const RECT *r, int hilite)
{
    Lb *l = lbd(h);
    COLORREF otext = 0, obk = 0;
    if (hilite) {
        FillRect(dc, r, w16_sys_brush(COLOR_HIGHLIGHT));
        otext = SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        obk = SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
    } else if (i == l->caret || l->tabstops)
        FillRect(dc, r, lb_brush(h, dc));
    const char *s = lb_str(l, i);
    int x = l->multicol ? r->left : 2, len = (int)strlen(s);
    /* a disabled list grays every item, the selected one too (on the highlight) */
    if (h->style & WS_DISABLED) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    /* seg35:24A6 measures the tab stops from the client edge (tab origin 0), not from the text
     * (measured: MAIN.CPL Connect's port list puts "Local Port" 56 px in) */
    if (l->tabstops) TabbedTextOut(dc, x, r->top, s, len, l->ntabs, l->ntabs ? l->tabs : NULL, 0);
    else if (l->multiple) ExtTextOut(dc, x, r->top, ETO_OPAQUE, r, s, len, NULL);
    else if (l->multicol) ExtTextOut(dc, x, r->top, ETO_CLIPPED, r, s, len, NULL);
    else TextOut(dc, x, r->top, s, len);
    if (hilite) {
        SetTextColor(dc, otext);
        SetBkColor(dc, obk);
    }
}

/* seg35:0C4E: paint the items that meet the paint rectangle */
static void lb_paint(HWND h, HDC dc, const RECT *paint)
{
    Lb *l = lbd(h);
    if (!lb_visible(h)) {
        IntersectClipRect(dc, 0, 0, 0, 0);
        lb_caret_off(h);
        return;
    }
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    SetBkMode(dc, OPAQUE);
    HBRUSH br = lb_brush(h, dc);
    HGDIOBJ obr = SelectObject(dc, br), ofont = h->font ? SelectObject(dc, h->font) : NULL;
    RECT rc;
    GetClientRect(h, &rc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    SetWindowOrg(dc, l->xorg, 0);
    rc.right += l->xorg;
    int last = lb_cinwin(h, 1) + l->top;
    if (last >= l->n - 1) last = l->n - 1;
    if (last == -1) FillRect(dc, &rc, br);
    for (int i = l->top; i <= last; i++) {
        rc.bottom = rc.top + l->ih;
        if ((unsigned)i < (unsigned)l->n) {
            if (l->multicol) lb_item_rect(h, i, &rc);
            RECT t;
            if (IntersectRect(&t, paint, &rc)) {
                int sel = lb_is_sel(l, i, 1);
                if (l->ownerdraw) lb_draw_od(h, i, dc, ODA_DRAWENTIRE, sel ? ODS_SELECTED : 0, &rc);
                else lb_draw_text(h, i, dc, &rc, sel);
            }
        }
        rc.top = rc.bottom;
    }
    if (obr) SelectObject(dc, obr);
    if (ofont) SelectObject(dc, ofont);
    if (was) lb_caret_on(h);
}

/* seg35:0FF1 InvertItem: redraw one visible item hilited or not (ODA_SELECT) */
static void lb_invert(HWND h, int i, int hilite)
{
    Lb *l = lbd(h);
    if (l->top > i || lb_cinwin(h, 1) + l->top <= i || !lb_visible(h)) return;
    RECT r;
    lb_item_rect(h, i, &r);
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    HDC dc = lb_getdc(h);
    SetBkMode(dc, OPAQUE);
    HBRUSH br = lb_brush(h, dc);
    HGDIOBJ ob = br ? SelectObject(dc, br) : NULL;
    if (!l->ownerdraw) {
        if (!hilite) FillRect(dc, &r, br);
        lb_draw_text(h, i, dc, &r, hilite);
    } else
        lb_draw_od(h, i, dc, ODA_SELECT, hilite ? ODS_SELECTED : 0, &r);
    if (ob) SelectObject(dc, ob);
    lb_releasedc(h, dc);
    if (was) lb_caret_on(h);
}

/* seg35:10F2 ResetWorld: everything outside a..b gets fsel (a single-selection list drops its
 * selection unless it lies in a..b) */
static void lb_reset_world(HWND h, int a, int b, int fsel)
{
    Lb *l = lbd(h);
    if (b < a) { int t = a; a = b; b = t; }
    if (!l->multiple) {
        if (l->cursel == -1 || (l->cursel >= a && l->cursel <= b)) return;
        lb_invert(h, l->cursel, fsel);
        l->cursel = -1;
        return;
    }
    int lastvis = lb_cinwin(h, 1) + l->top;
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    for (int i = 0; i < l->n; i++) {
        if (a <= i && i <= b) continue;
        if (l->top <= i && i <= lastvis && lb_is_sel(l, i, 1) != fsel) lb_invert(h, i, fsel);
        lb_set_sel(l, i, fsel, 3);
    }
    if (was) lb_caret_on(h);
}

/* seg35:2592 AlterHilite: the items between i (left out) and j get fhilite, or with fselstatus their
 * own selection state, as hilite (op & 1) and / or selection (op & 2) */
static void lb_alter_hilite(HWND h, int i, int j, int fhilite, int op, int fselstatus)
{
    Lb *l = lbd(h);
    int lastvis = lb_cinwin(h, 1) + l->top;
    if (lastvis >= l->n - 1) lastvis = l->n - 1;
    int end = (j >= i ? j : i) + 1;
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    for (int k = j <= i ? j : i; k < end; k++) {
        if (k == i) continue;
        if (op & 1) {
            int want = fselstatus ? lb_is_sel(l, k, 0) : fhilite;
            if (lb_is_sel(l, k, 1) != want) {
                if (l->top <= k && k <= lastvis) lb_invert(h, k, want);
                lb_set_sel(l, k, want, 1);
            }
        }
        if (op & 2) lb_set_sel(l, k, fhilite, 2);
    }
    if (was) lb_caret_on(h);
}

/* seg35:24FB BlockHilite: extend or shrink the anchor..last range to inew (the keyboard selects
 * as it goes, the mouse only hilites until the button comes up) */
static void lb_block_hilite(HWND h, int inew, int fkb)
{
    Lb *l = lbd(h);
    int op, fsel, fh;
    if (fkb) { op = 3; fsel = 0; fh = 0; }
    else { op = 1; fsel = 1; fh = l->newstate; }
    int a = l->anchor - l->lastmove, c = l->anchor - inew;
    if ((SHORT)(a * c) >= 0) {
        if (abs(a) < abs(c)) lb_alter_hilite(h, l->lastmove, inew, l->newstate, op, 0);
        else lb_alter_hilite(h, inew, l->lastmove, fh, op, fsel);
    } else {
        lb_alter_hilite(h, l->anchor, l->lastmove, fh, op, fsel);
        lb_alter_hilite(h, l->anchor, inew, l->newstate, op, 0);
    }
}

/* seg43:0000 SetScrollbars: the bars come and go (or are disabled with LBS_DISABLENOSCROLL) with
 * what there is to scroll; their range stays 0..100 and the position is scaled to it */
static void lb_set_scrollbars(HWND h)
{
    Lb *l = lbd(h);
    int vrange = l->n - l->cifm, hrange = 0, fv, fh;
    if (vrange < 0) vrange = 0;
    if (!l->scroll || !l->redraw) return;
    if (l->multicol) {
        fv = 0;
        fh = l->top != 0 || vrange != 0;
    } else {
        RECT r;
        GetClientRect(h, &r);
        hrange = l->hext - (r.right - r.left);
        if (hrange < 0) hrange = 0;
        fh = l->xorg != 0 || hrange != 0;
        fv = l->top != 0 || vrange != 0;
    }
    if (l->disnoscroll) {
        if ((h->style & WS_VSCROLL) && h->sb[1].disabled != (fv ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH))
            EnableScrollBar(h, SB_VERT, fv ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH);
        if ((h->style & WS_HSCROLL) && h->sb[0].disabled != (fh ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH))
            EnableScrollBar(h, SB_HORZ, fh ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH);
    } else {
        DWORD ns = h->style;
        ns = fv ? ns | WS_VSCROLL : ns & ~WS_VSCROLL;
        ns = fh ? ns | WS_HSCROLL : ns & ~WS_HSCROLL;
        if (ns != h->style) {
            /* seg13:0ABE: the frame is redrawn and the client area recalculated */
            h->style = ns;
            RECT rc;
            w16_nc_calc(h, &h->rw, &rc);
            h->rc = rc;
            w16_invalidate_window(h, NULL, 1, 1);
            SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        }
    }
    if (fv) {
        int pos = l->top ? (vrange ? MulDiv(l->top, 100, vrange) : 100) : 0;
        if (GetScrollPos(h, SB_VERT) != pos) SetScrollPos(h, SB_VERT, pos, l->redraw);
    }
    if (fh) {
        int pos;
        if (l->multicol) pos = MulDiv(l->top / l->ipc, 100, (l->n - 1) / l->ipc - l->ncols + 1);
        else pos = l->xorg ? (hrange ? MulDiv(l->xorg, 100, hrange) : 100) : 0;
        if (GetScrollPos(h, SB_HORZ) != pos) SetScrollPos(h, SB_HORZ, pos, l->redraw);
    }
}

/* seg36:02EB: a multicolumn list's new first item - always the top of a column, and the last
 * columns fill the window */
static void lb_new_top_multi(HWND h, int t)
{
    Lb *l = lbd(h);
    if (l->n) {
        int lastcol = l->n - (l->n - 1) % l->ipc - 1;
        int v = t - t % l->ipc;
        if (v < 0) v = 0;
        if (v > lastcol) v = lastcol;
        int cols = (l->n - v + l->ipc - 1) / l->ipc;
        if (cols < l->ncols) {
            int back = (l->ncols - cols) * l->ipc;
            if (back > v) back = v;
            v -= back;
        }
        t = v;
    } else
        t = 0;
    if (l->top == t) return;
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    int d = t / l->ipc - l->top / l->ipc;
    if (d < 0) d = -(t / l->ipc + l->top / l->ipc); /* 3.1's sign slip (seg36:0391), kept */
    int dx = l->ncols < d ? 32000 : (l->top - t) / l->ipc * l->colw;
    l->top = t;
    SetScrollPos(h, SB_HORZ, MulDiv(t / l->ipc, 100, (l->n - 1) / l->ipc - l->ncols + 1), l->redraw);
    if (lb_visible(h)) {
        RECT r;
        GetClientRect(h, &r);
        ScrollWindow(h, dx, 0, NULL, &r);
        UpdateWindow(h);
        if (was) lb_caret_on(h);
    }
}

/* seg35:16F9 NewITop: scroll to a new first item */
static void lb_new_top(HWND h, int t)
{
    Lb *l = lbd(h);
    if (l->multicol) { lb_new_top_multi(h, t); return; }
    int mx = l->n - l->cifm;
    if (mx < 0) mx = 0;
    if (t < 0) t = 0;
    if (t > mx) t = mx;
    if (t == l->top) return;
    RECT r;
    GetClientRect(h, &r);
    int was = l->caret_drawn;
    if (was) lb_caret_off(h);
    int dy = (l->top - t) * l->ih;
    l->top = t;
    SetScrollPos(h, SB_VERT, t ? (mx ? MulDiv(t, 100, mx) : 100) : 0, l->redraw);
    if (lb_visible(h)) {
        ScrollWindow(h, 0, dy, NULL, &r);
        UpdateWindow(h);
        if (was) lb_caret_on(h);
    }
}

/* seg35:17F7 InsureVisible: scroll item i into view (whole, or partly if fPartial) */
static void lb_insure_visible(HWND h, int i, int partial)
{
    Lb *l = lbd(h);
    if (i < l->top) { lb_new_top(h, i); return; }
    int last = partial ? l->top + lb_cinwin(h, 1) - 1 : lb_last_full(h);
    if (last >= i) return;
    if (l->multicol) lb_new_top(h, (i / l->ipc - (l->ncols - 1 > 0 ? l->ncols - 1 : 0)) * l->ipc);
    else lb_new_top(h, l->top - last + i > 0 ? l->top - last + i : 0);
}

/* seg36:01CA: rows and columns of a multicolumn list from its client size */
static void lb_calc_rows_cols(HWND h)
{
    Lb *l = lbd(h);
    RECT r;
    GetClientRect(h, &r);
    if (r.bottom == r.top || r.right == r.left) return;
    l->ipc = (r.bottom - r.top) / l->ih;
    if (l->ipc < 1) l->ipc = 1;
    l->ncols = (r.right - r.left) / (l->colw > 0 ? l->colw : 1);
    if (l->ncols < 1) l->ncols = 1;
    l->cifm = l->ipc * l->ncols;
    lb_new_top_multi(h, l->top);
}

/* seg35:0E27 ISelFromPt: the item under a client point, and whether the point is outside the items */
static int lb_sel_from_pt(HWND h, int x, int y, int *outside)
{
    Lb *l = lbd(h);
    RECT r;
    GetClientRect(h, &r);
    int out = 0, i;
    if (y < 0) { *outside = 1; return l->top; }
    if (y > r.bottom) { y = r.bottom; out = 1; }
    if (x < 0 || x > r.right) out = 1;
    if (l->multicol) {
        if (l->ih * l->ipc > y) i = y / l->ih + x / l->colw * l->ipc + l->top;
        else { out = 1; i = (x / l->colw + 1) * l->ipc + l->top - 1; }
    } else
        i = y / l->ih + l->top;
    if (l->n - 1 < i) { out = 1; i = l->n - 1; }
    *outside = out;
    return i;
}

/* seg35:221A: repaint the list after a change at item i (unless check and i lies below the window) */
static void lb_inval_from(HWND h, int check, int i)
{
    Lb *l = lbd(h);
    if (check && l->n && lb_cinwin(h, 1) + l->top < i) return;
    if (lb_visible(h)) InvalidateRect(h, NULL, TRUE);
    else if (!l->redraw) l->defer = 1;
}

/* invalidate one item's rectangle as seg35:22C8 / seg35:20AC do */
static void lb_inval_item(HWND h, int i, BOOL erase)
{
    Lb *l = lbd(h);
    RECT r;
    if (!lb_item_rect(h, i, &r)) return;
    if (lb_visible(h)) InvalidateRect(h, &r, erase);
    else if (!l->redraw) l->defer = 1;
}

/* seg35:0A33: WM_VSCROLL */
static void lb_vscroll(HWND h, int cmd, int pos)
{
    Lb *l = lbd(h);
    if (l->multicol) return;
    int page = l->cifm;
    if (page > 1) page--;
    if (!l->n) return;
    int t = l->top;
    switch (cmd) {
    case SB_LINEUP: t--; break;
    case SB_LINEDOWN: t++; break;
    case SB_PAGEUP: t -= page; break;
    case SB_PAGEDOWN: t += page; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: {
        int v = MulDiv(l->n - page, pos, 100);
        t = v > 0 ? v : 0;
        break;
    }
    case SB_TOP: t = 0; break;
    case SB_BOTTOM: t = l->n - 1; break;
    case SB_ENDSCROLL:
        lb_caret_off(h);
        lb_set_scrollbars(h);
        lb_caret_on(h);
        return;
    default: break;
    }
    lb_caret_off(h);
    lb_new_top(h, t);
    lb_caret_on(h);
}

/* seg36:022C: a multicolumn list's WM_HSCROLL, by whole columns */
static void lb_hscroll_multi(HWND h, int cmd, int pos)
{
    Lb *l = lbd(h);
    int t = l->top;
    if (!l->n) return;
    switch (cmd) {
    case SB_LINEUP: t -= l->ipc; break;
    case SB_LINEDOWN: t += l->ipc; break;
    case SB_PAGEUP: t -= l->ipc * l->ncols; break;
    case SB_PAGEDOWN: t += l->ipc * l->ncols; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: t = MulDiv((l->n - 1) / l->ipc, pos, 100) * l->ipc; break;
    case SB_TOP: t = 0; break;
    case SB_BOTTOM: t = l->n - (l->n - 1) % l->ipc - 1; break;
    case SB_ENDSCROLL: lb_set_scrollbars(h); break;
    default: break;
    }
    int lastcol = l->n - (l->n - 1) % l->ipc - 1;
    if (t > lastcol) t = lastcol;
    if (t < 0) t = 0;
    if (l->top != t) lb_new_top_multi(h, t);
}

/* seg35:0B04: WM_HSCROLL - by pixels up to the horizontal extent */
static void lb_hscroll(HWND h, int cmd, int pos)
{
    Lb *l = lbd(h);
    int old = l->xorg, x = old;
    if (l->multicol) { lb_hscroll_multi(h, cmd, pos); return; }
    RECT r;
    GetClientRect(h, &r);
    int w = r.right - r.left;
    if (!l->n) return;
    switch (cmd) {
    case SB_LINEUP: x -= l->cxchar; break;
    case SB_LINEDOWN: x += l->cxchar; break;
    case SB_PAGEUP: x += 2 * (w / -3); break;
    case SB_PAGEDOWN: x += 2 * (w / 3); break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: x = MulDiv(l->hext - w > 0 ? l->hext - w : 0, pos, 100); break;
    case SB_TOP: x = 0; break;
    case SB_BOTTOM: x = l->hext; break;
    case SB_ENDSCROLL:
        lb_caret_off(h);
        lb_set_scrollbars(h);
        lb_caret_on(h);
        return;
    default: break;
    }
    if (l->hext - w < x) x = l->hext - w;
    if (x < 0) x = 0;
    int range = l->hext - w > 0 ? l->hext - w : 0;
    SetScrollPos(h, SB_HORZ, x ? (range ? MulDiv(x, 100, range) : 100) : 0, l->redraw);
    lb_caret_off(h);
    l->xorg = x;
    ScrollWindow(h, old - x, 0, NULL, &r);
    UpdateWindow(h);
    lb_caret_on(h);
}

/* seg35:1206 TrackMouse: button down / up, mouse moves and the auto-scroll timer */
static void lb_track_mouse(HWND h, UINT msg, int x, int y)
{
    Lb *l = lbd(h);
    int keys = 0, outside, i = lb_sel_from_pt(h, x, y, &outside);
    int inrow = !(outside && l->combo);
    RECT rc;
    GetClientRect(h, &rc);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!l->mdown && !l->cbtrack) return;
        l->ptprev.x = x;
        l->ptprev.y = y;
        if (y < 0 || rc.bottom - 1 <= y) {
            /* outside: scroll by a line, the faster the farther out (seg35:128B) */
            int el = 200 - abs(y) * 16;
            if (el < 1) el = 1;
            SetTimer(h, LB_TIMER_TRACK, el, NULL);
            if ((y >= 0 || l->top != 0) && (rc.bottom - 1 > y || l->n - l->top - 1 != 0)) {
                lb_vscroll(h, y >= 0 ? SB_LINEDOWN : SB_LINEUP, 0);
                if (y < 0) i = l->top;
                else i = l->n - 1 < i + 1 ? l->n - 1 : i + 1;
            }
        }
        if (l->multiple == 0) goto select;
        if (l->multiple > 2 || l->caret == i) return;
        lb_set_caret(h, i);
        if (l->multiple == 2) lb_block_hilite(h, i, 0);
        l->lastmove = i;
        return;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!l->combo) {
            SetFocus(h);
            if (!l->focus) return;
        }
        if (l->addsel) {
            KillTimer(h, LB_TIMER_CARET);
            lb_caret_on(h);
            l->addsel = 0;
        }
        if (!l->n || outside) return;
        l->dblclk = msg == WM_LBUTTONDBLCLK;
        if (!l->dblclk) {
            /* WM_LBTRACKPOINT (seg35:13A9): the parent may take the click over - any non-zero answer
             * ends it here, 2 also keeps the next click from making a double click */
            LRESULT r = SendMessage(lb_owner(h), WM_LBTRACKPOINT, i, MAKELPARAM(x + l->xorg, y));
            if (r) {
                if (r == 2) w16_cancel_dblclk();
                return;
            }
        }
        l->mdown = 1;
        if (l->combo) l->lastsel = l->cursel;
        l->ptprev.x = x;
        l->ptprev.y = y;
        SetCapture(h);
        if (l->dblclk) {
            lb_track_mouse(h, WM_LBUTTONUP, x, y);
            return;
        }
        SetTimer(h, LB_TIMER_TRACK, 400, NULL);
        if (l->multiple == 2) {
            if (GetKeyState(VK_SHIFT) < 0) keys = 1;
            if (GetKeyState(VK_CONTROL) < 0) keys += 2;
        }
        switch (keys) {
        case 0: goto select;
        case 1: /* Shift: hilite from the anchor */
            l->cursel = i;
            l->lastmove = i;
            if (l->anchor == -1) {
                l->anchor = i;
                lb_reset_world(h, i, i, 0);
                lb_set_sel(l, l->anchor, 1, 3);
                lb_invert(h, l->anchor, 1);
            } else {
                lb_reset_world(h, l->anchor, l->anchor, 0);
                lb_alter_hilite(h, l->anchor, i, 1, 1, 0);
            }
            l->newstate = 1;
            break;
        case 2: /* Ctrl: toggle the item, which becomes the anchor */
            l->cursel = i;
            l->lastmove = i;
            l->anchor = i;
            l->newstate = !lb_is_sel(l, i, 1);
            lb_set_sel(l, i, l->newstate, 3);
            lb_invert(h, i, l->newstate);
            break;
        case 3: /* Shift+Ctrl: the anchor's state over anchor..item */
            if (l->newstate) lb_alter_hilite(h, l->anchor, l->lastmove, 0, 3, 0);
            l->cursel = i;
            l->lastmove = i;
            if (l->anchor == -1) l->anchor = i;
            l->newstate = lb_is_sel(l, l->anchor, 1) & 1;
            lb_alter_hilite(h, l->anchor, i, l->newstate, 1, 0);
            break;
        }
        goto caret;
    case WM_LBUTTONUP:
        if (!l->mdown && !l->cbtrack) return;
        if (l->multiple == 2) lb_alter_hilite(h, l->anchor, l->lastmove, l->newstate, 2, 0);
        if (l->combo && outside) {
            if (l->cursel >= 0) lb_invert(h, l->cursel, 0);
            l->cursel = l->lastsel;
            lb_invert(h, l->cursel, 1);
        }
        KillTimer(h, LB_TIMER_TRACK);
        l->mdown = 0;
        l->cbtrack = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (l->top > l->caret || lb_cinwin(h, 1) + l->top < l->caret) lb_insure_visible(h, l->caret, 0);
        if (l->notify) {
            /* a 3.x program's list sends only LBN_DBLCLK for the second click */
            if (inrow) lb_notify(h, l->dblclk ? LBN_DBLCLK : LBN_SELCHANGE);
            else lb_notify(h, LBN_SELCANCEL);
        }
        if (l->combo) SendMessage(l->combo, WM_USER + 0x501, 0, 0); /* close the drop-down */
        return;
    default: return;
    }
select: { /* seg35:153D */
        if (l->caret != i) lb_caret_off(h);
        int si = inrow ? i : -1;
        if (l->multiple != 1) lb_reset_world(h, si, si, 0);
        int st = lb_is_sel(l, si, 1);
        if (si != -1 && ((l->multiple == 1 && msg != WM_LBUTTONDBLCLK) || !st)) {
            st = !st;
            lb_set_sel(l, si, st, 3);
            lb_invert(h, si, st);
        }
        l->cursel = si;
        l->anchor = i;
        l->lastmove = i;
        l->newstate = st & 1;
    }
caret:
    if (l->caret != i) lb_set_caret(h, i);
}

/* seg35:2127: LB_SETCURSEL and LB_SELECTSTRING's selection */
static int lb_set_cursel(HWND h, int i)
{
    Lb *l = lbd(h);
    if (l->multiple || i < -1 || i >= l->n) return LB_ERR;
    lb_caret_off(h);
    if (l->cursel != -1) {
        if (i != -1) lb_insure_visible(h, i, 0);
        lb_invert(h, l->cursel, 0);
    }
    if (i != -1) {
        lb_insure_visible(h, i, 0);
        l->cursel = l->caret = i;
        lb_invert(h, i, 1);
    } else {
        l->cursel = -1;
        l->caret = l->n ? (l->n - 1 < l->caret ? l->n - 1 : l->caret) : 0;
    }
    lb_caret_on(h);
    return l->cursel;
}

/* seg35:1D38: compare the typed / searched text with an item, letter by letter in upper case:
 * 0 equal, 1 the text is a prefix of the item, 2 less, 3 greater */
static int lb_cmp_prefix(const char *s, const char *t)
{
    char a[2] = {1, 0}, b[2] = {1, 0};
    while (a[0] == b[0]) {
        if (!a[0]) return 0;
        a[0] = *s++;
        b[0] = *t++;
        AnsiUpper(a);
        AnsiUpper(b);
    }
    if (!a[0]) return 1;
    int r = lstrcmpi(a, b);
    return r == -1 ? 2 : r == 1 ? 3 : 0;
}

/* seg35:1DCE SearchString: from the item after start, wrapping if asked; type 1 takes a prefix,
 * 0 only the whole string. A list without strings compares item data: through WM_COMPAREITEM if it
 * is sorted, else for equality. */
static int lb_search(HWND h, ULONG_PTR key, int start, int type, int wrap)
{
    Lb *l = lbd(h);
    const char *s = (const char *)key;
    if (l->strings && (!s || !*s)) return LB_ERR;
    int i = start + 1;
    if (i >= l->n) i = wrap ? 0 : l->n - 1;
    int stop = wrap ? i : 0;
    if (l->n - 1 <= start && !wrap) return LB_ERR;
    if (l->n < 1) return LB_ERR;
    for (;;) {
        if (l->strings) {
            const char *t = l->it[i].s ? l->it[i].s : "";
            /* a typed letter finds directories and drives too: "[" and "[-" are skipped */
            if (type == 1 && *s != '[' && *t == '[') {
                t++;
                if (*t == '-') t++;
            }
            if (lb_cmp_prefix(s, t) <= type) return i;
        } else if (l->sort) {
            COMPAREITEMSTRUCT ci = {ODT_LISTBOX, h->id, h, (UINT)-1, key, (UINT)i, l->it[i].data};
            LRESULT c = SendMessage(lb_owner(h), WM_COMPAREITEM, h->id, (LPARAM)&ci);
            if ((c == -1 ? 2 : c == 1 ? 3 : 0) <= type) return i;
        } else if (l->it[i].data == key)
            return i;
        if (++i == l->n) i = 0;
        if (i == stop) return LB_ERR;
    }
}

/* seg35:18A8: WM_KEYDOWN (and LB_KEYDOWN_ITEM: go to item vk) */
static void lb_keydown(HWND h, UINT msg, int vk)
{
    Lb *l = lbd(h);
    int hbar = (h->style & WS_HSCROLL) != 0, keys = 0, space = 0, di, ni;
    if (l->mdown) return;
    if (!l->n && vk != VK_F4) return;
    if (l->multiple == 2) {
        if (GetKeyState(VK_SHIFT) < 0) keys = 1;
        if (GetKeyState(VK_CONTROL) < 0) keys += 2;
    }
    if (msg == LB_KEYDOWN_ITEM) { ni = vk; goto go; }
    di = l->cifm;
    if (di > 1) di--;
    if (l->wantkb) {
        /* LBS_WANTKEYBOARDINPUT: -2 done, -1 go on, else that item */
        LRESULT r = SendMessage(lb_owner(h), WM_VKEYTOITEM, vk, W16_CMD_LPARAM(h, l->caret));
        if (r == -2) return;
        if (r != -1) { ni = (int)r; goto go; }
    }
    switch (vk) {
    case VK_SPACE: di = 0; space = 1; break;
    case VK_PRIOR: di = -di; break;
    case VK_NEXT: break;
    case VK_END: di = 30000; break;
    case VK_HOME: di = -30000; break;
    case VK_LEFT:
        if (l->multicol) { di = l->caret / l->ipc == 0 ? 0 : -l->ipc; break; }
        if (hbar) { SendMessage(h, WM_HSCROLL, SB_LINEUP, 0); return; }
        vk = VK_UP;
        di = -1;
        break;
    case VK_UP: di = -1; break;
    case VK_RIGHT:
        if (l->multicol) { di = l->caret / l->ipc == l->n / l->ipc ? 0 : l->ipc; break; }
        if (hbar) { SendMessage(h, WM_HSCROLL, SB_LINEDOWN, 0); return; }
        vk = VK_DOWN;
        di = 1;
        break;
    case VK_DOWN: di = 1; break;
    case VK_DIVIDE:
    case 0xBF: /* Ctrl+/ selects everything */
        if (!(keys & 2) || !l->multiple) return;
        lb_caret_off(h);
        lb_reset_world(h, -1, -1, 1);
        lb_caret_on(h);
        lb_notify(h, LBN_SELCHANGE);
        return;
    case 0xDC: /* Ctrl+\ keeps only the caret item */
        if (!(keys & 2) || !l->multiple) return;
        lb_caret_off(h);
        lb_reset_world(h, l->caret, l->caret, 0);
        lb_set_sel(l, l->caret, 1, 3);
        lb_invert(h, l->caret, 1);
        lb_caret_on(h);
        lb_notify(h, LBN_SELCHANGE);
        return;
    case VK_F8: /* Shift+F8: add mode, the caret blinks and moves without selecting */
        if (!l->multiple || keys != 1) return;
        if (l->addsel) {
            KillTimer(h, LB_TIMER_CARET);
            lb_caret_on(h);
        } else
            SetTimer(h, LB_TIMER_CARET, GetCaretBlinkTime(), NULL);
        l->addsel = !l->addsel;
        return;
    default: return; /* F4 too: the combo box handles it */
    }
    ni = (SHORT)(l->caret + di);
    if (ni < 0) ni = 0;
    if (l->n <= ni) ni = l->n - 1;
    if (!l->multiple) {
        if (l->cursel == ni) return;
        /* nothing selected yet: Up / Down select the caret item itself */
        if ((vk == VK_UP || vk == VK_DOWN) && !lb_is_sel(l, l->caret, 1)) ni = l->caret;
    }
go:
    lb_set_caret(h, ni);
    lb_caret_off(h);
    if (keys == 0 || keys == 2) {
        if (!l->addsel && l->multiple != 1) {
            lb_reset_world(h, ni, ni, 0);
            lb_set_sel(l, ni, 1, 3);
            lb_invert(h, ni, 1);
            l->anchor = ni;
            l->lastmove = ni;
        } else if (space) {
            l->newstate = !lb_is_sel(l, ni, 0);
            lb_set_sel(l, ni, l->newstate, 3);
            lb_invert(h, ni, l->newstate);
            l->anchor = ni;
            l->lastmove = ni;
        }
    } else {
        if (l->anchor == -1) l->anchor = ni;
        if (l->lastmove == -1) l->lastmove = ni;
        if (l->addsel) {
            if (!l->newstate) l->lastmove = l->anchor;
        } else
            lb_reset_world(h, l->anchor, l->lastmove, 0);
        l->newstate = lb_is_sel(l, l->anchor, 0) & 1;
        lb_block_hilite(h, ni, 1);
        l->lastmove = ni;
    }
    lb_insure_visible(h, ni, 0);
    lb_set_scrollbars(h);
    lb_caret_on(h);
    if (l->notify) lb_notify(h, LBN_SELCHANGE);
}

/* seg35:1F6A: WM_CHAR - the next item starting with the letter, or the owner's WM_CHARTOITEM answer
 * for a list without strings */
static void lb_char(HWND h, int ch)
{
    Lb *l = lbd(h);
    if (!l->n || l->mdown) return;
    int ctrl = GetKeyState(VK_CONTROL) < 0, r;
    if (ch == ' ') return;
    if (l->strings) {
        if (ctrl && ch < 0x20) ch += 0x40;
        char s[2] = {(char)ch, 0};
        r = lb_search(h, (ULONG_PTR)s, l->caret, 1, 1);
        if (r == -1) return;
    } else {
        r = (int)SendMessage(lb_owner(h), WM_CHARTOITEM, ch, W16_CMD_LPARAM(h, l->caret));
        if (r == -1 || r == -2) return;
    }
    lb_keydown(h, LB_KEYDOWN_ITEM, r);
}

/* seg43:0AE6: WM_DELETEITEM for an owner-draw item */
static void lb_send_delete(HWND h, int i)
{
    Lb *l = lbd(h);
    DELETEITEMSTRUCT di = {ODT_LISTBOX, h->id, (UINT)i, h, l->strings ? 0 : l->it[i].data};
    SendMessage(lb_owner(h), WM_DELETEITEM, h->id, (LPARAM)&di);
}

/* seg43:032F InsertItem. 3.1 leaves the selection, caret and top index where they were, and a
 * string item inserted before others keeps the item data that was in its slot. */
static int lb_insert(HWND h, int pos, const char *s, ULONG_PTR data)
{
    Lb *l = lbd(h);
    if (pos == -1) pos = l->n;
    if ((unsigned)pos > (unsigned)l->n || l->n == 0x7FFF) return LB_ERR;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->it = realloc(l->it, sizeof(LbItem) * l->cap);
    }
    ULONG_PTR keep = pos < l->n ? l->it[pos].data : 0;
    memmove(&l->it[pos + 1], &l->it[pos], sizeof(LbItem) * (l->n - pos));
    l->it[pos].s = l->strings ? strdup(s ? s : "") : NULL;
    l->it[pos].data = l->strings ? keep : data;
    l->it[pos].sel = 0;
    l->n++;
    lb_set_scrollbars(h);
    lb_inval_from(h, 1, pos);
    return pos;
}

/* seg43:0614: the order of a sorted list: a string that starts with '[' (a directory or drive entry)
 * goes after every string that does not; otherwise lstrcmpi decides (measured: Drivers lists
 * "MIDI Mapper", "Timer", "[MCI] MIDI Sequencer", "[MCI] Sound") */
static int lb_sort_cmp(const char *item, const char *s)
{
    if (*item == '[') {
        if (*s != '[') return 1;
    } else if (*s == '[')
        return -1;
    return lstrcmpi(item, s);
}

/* seg43:0658: binary search for a sorted insert */
static int lb_sorted_pos(HWND h, ULONG_PTR key)
{
    Lb *l = lbd(h);
    int lo = 0, hi = l->n - 1;
    if (!l->n) return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c;
        if (l->strings) c = lb_sort_cmp(lb_str(l, mid), (const char *)key);
        else {
            COMPAREITEMSTRUCT ci = {ODT_LISTBOX, h->id, h, (UINT)mid, l->it[mid].data, (UINT)-1, key};
            c = (int)SendMessage(lb_owner(h), WM_COMPAREITEM, h->id, (LPARAM)&ci);
        }
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid - 1;
        else { lo = mid; break; }
    }
    return lo < 0 ? 0 : lo;
}

/* seg43:0771: LB_RESETCONTENT (the anchor stays) */
static void lb_reset(HWND h)
{
    Lb *l = lbd(h);
    if (!l->n) return;
    if (l->ownerdraw)
        for (int k = l->n - 1; k >= 0; k--) lb_send_delete(h, k);
    for (int k = 0; k < l->n; k++) free(l->it[k].s);
    lb_inval_from(h, 0, 0);
    l->caret = l->top = l->n = 0;
    l->xorg = 0;
    l->lastsel = 0;
    l->cursel = -1;
    lb_set_scrollbars(h);
}

/* seg43:0805: LB_DELETESTRING. Only a deleted selection is dropped (later items' indices are not
 * moved), as in 3.1. */
static int lb_delete(HWND h, int i)
{
    Lb *l = lbd(h);
    if ((unsigned)i >= (unsigned)l->n) return LB_ERR;
    if (l->n == 1)
        SendMessage(h, LB_RESETCONTENT, 0, 0);
    else {
        lb_inval_item(h, l->n - 1, TRUE);
        if (l->ownerdraw) lb_send_delete(h, i);
        free(l->it[i].s);
        memmove(&l->it[i], &l->it[i + 1], sizeof(LbItem) * (l->n - i - 1));
        l->n--;
        if (l->cursel == i || l->cursel >= l->n) l->cursel = -1;
        if (l->anchor != -1 && l->anchor >= i) l->anchor = -1;
        if (l->caret == i) l->caret--;
        l->caret = l->n ? (l->caret < l->n - 1 ? l->caret : l->n - 1) : 0;
        if (l->multiple == 2 && l->cursel == -1) l->cursel = l->caret;
        lb_insure_visible(h, l->top, 0);
    }
    lb_set_scrollbars(h);
    lb_inval_from(h, 1, i);
    lb_insure_visible(h, l->caret, 0);
    return l->n;
}

/* seg35:22C8: LB_SETSEL (index -1: every item) */
static int lb_setsel(HWND h, int fsel, int i)
{
    Lb *l = lbd(h);
    if (!l->multiple || i < -1 || i >= l->n) return LB_ERR;
    lb_caret_off(h);
    if (i == -1) {
        for (int k = 0; k < l->n; k++)
            if (lb_is_sel(l, k, 0) != fsel) {
                lb_set_sel(l, k, fsel, 3);
                lb_inval_item(h, k, TRUE);
            }
        lb_caret_on(h);
        return 0;
    }
    if (fsel) {
        lb_insure_visible(h, i, 1);
        l->cursel = l->caret = l->lastmove = l->anchor = i;
    }
    lb_set_sel(l, i, fsel, 3);
    if (!fsel && l->caret != i) lb_caret_on(h);
    else if (lb_fcaret(l)) l->caret_drawn = 1; /* the item's repaint brings it back */
    lb_inval_item(h, i, TRUE);
    return 0;
}

/* seg35:20AC: LB_SELITEMRANGE (lParam = MAKELONG(first, last)); caret and anchor stay */
static int lb_selitemrange(HWND h, int fsel, int a, int b)
{
    Lb *l = lbd(h);
    if (!l->multiple) return LB_ERR;
    if (b < a) { int t = a; a = b; b = t; }
    if (b >= l->n) b = l->n - 1;
    for (int k = a < 0 ? 0 : a; k <= b; k++)
        if (lb_is_sel(l, k, 0) != fsel) {
            lb_set_sel(l, k, fsel, 3);
            lb_inval_item(h, k, FALSE);
        }
    return 0;
}

/* seg35:2008: LB_GETSELCOUNT (out NULL) / LB_GETSELITEMS */
static int lb_getsel_items(Lb *l, int max, int *out)
{
    int c = 0;
    if (!l->multiple) return LB_ERR;
    for (int k = 0; k < l->n; k++)
        if (lb_is_sel(l, k, 0)) {
            if (out) {
                if (max <= c) return c;
                out[c] = k;
            }
            c++;
        }
    return c;
}

/* seg43:0234: LB_GETTEXT / LB_GETTEXTLEN. A list without strings answers its item data; it is
 * pointer-sized here (see ULONG_PTR in w16.h), 3.1's was a DWORD. */
static int lb_gettext(HWND h, int lenonly, int i, char *buf)
{
    Lb *l = lbd(h);
    if ((unsigned)i >= (unsigned)l->n) return LB_ERR;
    if (!l->strings && l->ownerdraw) {
        if (!lenonly) memcpy(buf, &l->it[i].data, sizeof(ULONG_PTR));
        return (int)sizeof(ULONG_PTR);
    }
    const char *s = lb_str(l, i);
    if (!lenonly) strcpy(buf, s);
    return (int)strlen(s);
}

/* seg38:05D5: LB_SETTABSTOPS in dialog units of the list's own font */
static int lb_settabs(HWND h, int n, const int *t)
{
    Lb *l = lbd(h);
    if (!l->tabstops) return 0;
    free(l->tabs);
    l->tabs = NULL;
    l->ntabs = 0;
    if (n > 0) {
        l->tabs = malloc(sizeof(int) * n);
        for (int k = 0; k < n; k++) l->tabs[k] = MulDiv(t[k], l->cxchar, 4);
        l->ntabs = n;
    }
    return 1;
}

/* unless LBS_NOINTEGRALHEIGHT, a list box shows whole items only, again whenever the font or item
 * height changes */
static void fit_height(HWND h)
{
    Lb *l = lbd(h);
    if ((h->style & (LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE)) || l->ih <= 0) return;
    /* USER seg38:0457 (the list box's WM_SIZE, again after a font change): unless the height less two
     * borders is whole items, the window becomes as many whole items as its full height holds, plus
     * two borders - so it can grow by a pixel or two. Measured on 3.11: COMMDLG's 16-pixel directory
     * list 113 -> 114 and its dropped drive list; from the system font's fit, MAIN.CPL Printers' lists
     * of 72 and 111 px end up 5 and 8 items of 13 px, SND.CPL's of 114 px 8 */
    RECT r;
    GetClientRect(h, &r);
    int H = h->rw.bottom - h->rw.top, cyb2 = 2 * GetSystemMetrics(SM_CYBORDER);
    l->fit_h = r.bottom;
    if ((H - cyb2) % l->ih == 0) return;
    int nh = H / l->ih * l->ih + cyb2;
    if (nh <= cyb2) return;
    l->fit_h = r.bottom + nh - H;
    RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
    l->fitting = 1;
    SetWindowPos(h, NULL, h->rw.left - pr.left, h->rw.top - pr.top, h->rw.right - h->rw.left, nh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    l->fitting = 0;
}

/* seg38:0457 after the integral-height fit: columns or horizontal origin, top index, repaint and
 * scroll bars for the new size */
static void lb_size(HWND h, int cx, int cy)
{
    Lb *l = lbd(h);
    if (l->multicol)
        lb_calc_rows_cols(h);
    else {
        RECT r;
        GetClientRect(h, &r);
        if (l->hext - l->xorg < r.right - r.left) l->xorg = l->hext - r.right + r.left > 0 ? l->hext - r.right + r.left : 0;
    }
    lb_set_cifm(h);
    int ot = l->top;
    lb_new_top(h, l->top);
    if (lb_visible(h)) {
        if (l->multicol && (cx || cy)) InvalidateRect(h, NULL, TRUE);
        else if (l->top != ot) InvalidateRect(h, NULL, TRUE);
        else if (l->caret >= 0) {
            RECT r;
            lb_item_rect(h, l->caret, &r);
            InvalidateRect(h, &r, FALSE);
        }
    } else if (!l->redraw)
        l->defer = 1;
    if (h->style & WS_VSCROLL) lb_vscroll(h, SB_ENDSCROLL, 0);
    lb_hscroll(h, SB_ENDSCROLL, 0);
    if (!l->n) lb_set_scrollbars(h);
}

LRESULT w16_listbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Lb *l = lbd(h);
    switch (m) {
    case WM_NCCREATE: {
        l = calloc(1, sizeof *l);
        h->ctl = l;
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        if (cs && cs->lpCreateParams && !strcasecmp(h->cls->name, "COMBOLBOX")) l->combo = (HWND)cs->lpCreateParams;
        return DefWindowProc(h, m, wp, lp);
    }
    case WM_CREATE: {
        /* seg38:0085 */
        DWORD st = h->style;
        l->redraw = !(st & LBS_NOREDRAW);
        l->notify = (st & LBS_NOTIFY) != 0;
        l->scroll = (st & (WS_VSCROLL | WS_HSCROLL)) != 0;
        l->disnoscroll = (st & LBS_DISABLENOSCROLL) != 0;
        l->multiple = (st & LBS_EXTENDEDSEL) ? 2 : (st & LBS_MULTIPLESEL) ? 1 : 0;
        l->nointegral = (st & LBS_NOINTEGRALHEIGHT) != 0;
        l->wantkb = (st & LBS_WANTKEYBOARDINPUT) != 0;
        l->tabstops = (st & LBS_USETABSTOPS) != 0;
        l->multicol = (st & LBS_MULTICOLUMN) != 0;
        l->lastsel = l->anchor = l->lastmove = -1;
        l->strings = 1;
        if (st & LBS_OWNERDRAWFIXED) l->ownerdraw = 1;
        else if ((st & LBS_OWNERDRAWVARIABLE) && !l->multicol) { l->ownerdraw = 2; l->nointegral = 1; }
        if (l->ownerdraw && !(st & LBS_HASSTRINGS)) l->strings = 0;
        l->sort = (st & LBS_SORT) != 0;
        l->cursel = -1;
        l->newstate = 1;
        HDC dc = GetDC(h);
        TEXTMETRIC tm;
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        l->cxchar = w16_ave_char_width(dc, &tm);
        ReleaseDC(h, dc);
        l->ih = tm.tmHeight;
        if (l->ownerdraw == 1) {
            MEASUREITEMSTRUCT mi = {ODT_LISTBOX, h->id, 0, 0, (UINT)tm.tmHeight, 0};
            SendMessage(lb_owner(h), WM_MEASUREITEM, h->id, (LPARAM)&mi);
            l->ih = (int)mi.itemHeight;
            if (l->multicol) l->colw = (int)mi.itemWidth;
        }
        if (l->ih <= 0) l->ih = 1;
        if (l->multicol) {
            if (l->colw <= 0) l->colw = 15 * l->cxchar;
            l->ipc = l->ncols = 1;
        }
        lb_set_cifm(h);
        if ((h->style & WS_BORDER) && !l->combo) {
            /* 3.1 puts a list box's border around the rectangle it was given, which stays the area
             * inside (seg38:02D5 MoveWindow; measured on 3.11: SND.CPL's lists are a border wider on
             * every side than their template rectangles) */
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
        if (!l->nointegral) SendMessage(h, WM_SIZE, 0, 0);
        return 0;
    }
    case WM_DESTROY:
        /* seg38:0351 */
        if (l && l->ownerdraw)
            for (int k = l->n - 1; k >= 0; k--) lb_send_delete(h, k);
        return 0;
    case WM_NCDESTROY:
        if (l) {
            for (int i = 0; i < l->n; i++) free(l->it[i].s);
            free(l->it);
            free(l->tabs);
            free(l);
            h->ctl = NULL;
        }
        return 0;
    case WM_PAINT: {
        if (wp) {
            RECT r;
            GetClientRect(h, &r);
            lb_paint(h, (HDC)wp, &r);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        lb_paint(h, dc, &ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        /* FillWindow(parent, list, hdc, CTLCOLOR_LISTBOX) */
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, lb_brush(h, (HDC)wp));
        return 1;
    }
    case WM_SETFONT: {
        /* seg38:03C3 */
        h->font = (HFONT)wp;
        HDC dc = GetDC(h);
        SelectObject(dc, h->font ? h->font : GetStockObject(SYSTEM_FONT));
        TEXTMETRIC tm;
        l->cxchar = w16_ave_char_width(dc, &tm);
        ReleaseDC(h, dc);
        if (!l->ownerdraw) l->ih = tm.tmHeight;
        if (l->multicol) lb_calc_rows_cols(h);
        lb_set_cifm(h);
        fit_height(h);
        if (lp) lb_inval_from(h, 0, 0);
        return 0;
    }
    case WM_GETFONT: return (LRESULT)h->font;
    case WM_SIZE:
        if (!l->fitting) {
            /* resized from outside: that height is the one to fit. CreateWindow's own WM_SIZE (and
             * the one a hidden window gets when shown) only reports the last fit, which was made
             * for the system font: the height asked for stays (3.11: MAIN.CPL Printers' lists of
             * 72 and 111 px show 5 and 8 items of 13 px, not 4 and 7) */
            RECT r;
            GetClientRect(h, &r);
            if (r.bottom != l->fit_h) {
                l->want_h = r.bottom;
                fit_height(h);
            }
        }
        lb_size(h, LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SETREDRAW: {
        /* seg35:2052 */
        int on = wp != 0;
        if (on == (int)l->redraw) return 0;
        l->redraw = on;
        if (on) {
            lb_caret_on(h);
            lb_set_scrollbars(h);
            if (l->defer) {
                l->defer = 0;
                InvalidateRect(h, NULL, TRUE);
            }
        }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        l->focus = 1;
        lb_caret_on(h);
        lb_notify(h, LBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        lb_caret_off(h);
        /* seg35:2287 */
        if (l->mdown) lb_track_mouse(h, WM_LBUTTONUP, 0, 0);
        if (l->addsel) {
            KillTimer(h, LB_TIMER_CARET);
            lb_caret_off(h);
            l->addsel = 0;
        }
        l->focus = 0;
        lb_notify(h, LBN_KILLFOCUS);
        return 0;
    case WM_ENABLE:
        /* seg38:059D */
        if (lb_visible(h)) InvalidateRect(h, NULL, !l->ownerdraw);
        return 0;
    case WM_TIMER:
        if (wp == LB_TIMER_TRACK) { lb_track_mouse(h, WM_MOUSEMOVE, l->ptprev.x, l->ptprev.y); return 0; }
        if (wp == LB_TIMER_CARET) {
            /* seg35:187A */
            if (l->caret_drawn) lb_caret_off(h);
            else lb_caret_on(h);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE: lb_track_mouse(h, m, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp)); return 0;
    case WM_KEYDOWN: lb_keydown(h, WM_KEYDOWN, (int)wp); return 0;
    case WM_CHAR: lb_char(h, (int)(BYTE)wp); return 0;
    case WM_VSCROLL: lb_vscroll(h, LOWORD(wp), (SHORT)LOWORD(lp)); return 0;
    case WM_HSCROLL: lb_hscroll(h, LOWORD(wp), (SHORT)LOWORD(lp)); return 0;
    case LB_ADDSTRING: {
        int pos = l->sort ? lb_sorted_pos(h, (ULONG_PTR)lp) : -1;
        return lb_insert(h, pos, l->strings ? (const char *)lp : NULL, (ULONG_PTR)lp);
    }
    case LB_INSERTSTRING: return lb_insert(h, (int)(SHORT)wp, l->strings ? (const char *)lp : NULL, (ULONG_PTR)lp);
    case LB_DELETESTRING: return lb_delete(h, (int)(SHORT)wp);
    case LB_RESETCONTENT: lb_reset(h); return TRUE;
    case LB_GETCOUNT: return l->n;
    case LB_GETCURSEL: return l->multiple ? l->caret : l->cursel;
    case LB_SETCURSEL: return lb_set_cursel(h, (int)(SHORT)wp);
    case LB_GETSEL: {
        int i = (int)(SHORT)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        return lb_is_sel(l, i, 0);
    }
    case LB_SETSEL: return lb_setsel(h, wp != 0, (int)(SHORT)LOWORD(lp));
    case LB_SELITEMRANGE: return lb_selitemrange(h, wp != 0, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
    case LB_GETSELCOUNT: return lb_getsel_items(l, 0, NULL);
    case LB_GETSELITEMS: return lb_getsel_items(l, (int)wp, (int *)lp);
    case LB_GETTEXT: return lb_gettext(h, 0, (int)(SHORT)wp, (char *)lp);
    case LB_GETTEXTLEN: return lb_gettext(h, 1, (int)(SHORT)wp, NULL);
    case LB_GETITEMDATA: return (LRESULT)lb_item_data(l, (int)(SHORT)wp);
    case LB_SETITEMDATA: {
        /* seg35:21B3 */
        int i = (int)(SHORT)wp;
        if ((unsigned)i >= (unsigned)l->n) return LB_ERR;
        l->it[i].data = (ULONG_PTR)lp;
        return TRUE;
    }
    case LB_FINDSTRING: return lb_search(h, (ULONG_PTR)lp, (int)(SHORT)wp, 1, 1);
    case LB_FINDSTRINGEXACT: return lb_search(h, (ULONG_PTR)lp, (int)(SHORT)wp, 0, 1);
    case LB_SELECTSTRING: {
        int i = lb_search(h, (ULONG_PTR)lp, (int)(SHORT)wp, 1, 1);
        if (i != LB_ERR) lb_set_cursel(h, i);
        return i;
    }
    case LB_GETTOPINDEX: return l->top;
    case LB_SETTOPINDEX:
        /* seg35:0142: as far as the scroll range goes - the last page stays full (on 3.11 COMMDLG's
         * three-item directory list stays at the top when it asks for item 1) */
        if ((int)(SHORT)wp >= l->n) return LB_ERR;
        lb_new_top(h, (int)(SHORT)wp);
        return 0;
    case LB_GETITEMRECT: return lb_item_rect(h, (int)(SHORT)wp, (RECT *)lp);
    case LB_SETITEMHEIGHT:
        /* seg38:073F: no repaint, no new window height */
        if (LOWORD(lp) == 0 || LOWORD(lp) > 255) return LB_ERR;
        l->ih = LOWORD(lp);
        if (l->multicol) lb_calc_rows_cols(h);
        lb_set_cifm(h);
        return 0;
    case LB_GETITEMHEIGHT: return l->ih;
    case LB_SETTABSTOPS: return lb_settabs(h, (int)wp, (const int *)lp);
    case LB_SETANCHORINDEX: l->anchor = l->lastmove = (int)(SHORT)wp; return 0;
    case LB_GETANCHORINDEX: return l->anchor;
    case LB_SETCARETINDEX: {
        /* seg35:0408: a single-selection list only while nothing is selected (3.1 tests iSel's low
         * byte); lParam TRUE accepts a partly visible item */
        int i = (int)(SHORT)wp;
        if ((l->cursel & 0xFF) == 0xFF || (l->multiple && i < l->n)) {
            lb_insure_visible(h, i, LOWORD(lp) != 0);
            lb_set_caret(h, i);
            return 0;
        }
        return LB_ERR;
    }
    case LB_GETCARETINDEX: return l->caret;
    case LB_SETCOLUMNWIDTH:
        /* seg35:03C7 */
        l->colw = (int)wp;
        lb_calc_rows_cols(h);
        if (lb_visible(h)) InvalidateRect(h, NULL, TRUE);
        lb_set_scrollbars(h);
        return 0;
    case LB_GETHORIZONTALEXTENT: return l->hext;
    case LB_SETHORIZONTALEXTENT: l->hext = (int)wp; lb_set_scrollbars(h); return 0;
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
    int extui;        /* CB_SETEXTENDEDUI */
} Cb;

static Cb *cbd(HWND h) { return (Cb *)h->ctl; }
static int cbtype(HWND h) { return h->style & 3; }

static void cb_text_from_list(HWND h)
{
    Cb *c = cbd(h);
    int i = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
    char buf[512] = "";
    if (i >= 0) SendMessage(c->list, LB_GETTEXT, i, (LPARAM)buf);
    if (c->edit) {
        SetWindowText(c->edit, buf);
        /* seg33:1053: the edit's text is selected only while the combo has the focus (on 3.11 the
         * Edit Pattern dialog's Add, pressed from OK, leaves the name unselected) */
        if (c->focus) SendMessage(c->edit, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
    }
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
    if (h->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
        /* owner-drawn: the field keeps the height its parent gave at the first layout, through
         * WM_MEASUREITEM for item -1 (itemHeight = field - 6); MAIN.CPL Color's scheme combo is 19 px */
        if (c->field.bottom > c->field.top) eh = c->field.bottom - c->field.top;
        else {
            MEASUREITEMSTRUCT mi = {ODT_COMBOBOX, h->id, (UINT)-1, 0, (UINT)(eh - 6), 0};
            SendMessage(h->parent, WM_MEASUREITEM, h->id, (LPARAM)&mi);
            eh = (int)mi.itemHeight + 6;
        }
    }
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
    lbd(c->list)->caret_on = 0;
    lbd(c->list)->caret_drawn = 0;
    if (show) {
        w16_notify_parent(h, CBN_DROPDOWN);
        RECT r = h->rw;
        int w = r.right - r.left;
        /* the list drops to the combo's full height whatever the number of items (measured on
         * 3.11: COMMDLG's drive list with three drives and its file type list with two) */
        int hh = c->drop_h > 0 ? c->drop_h : c->ih * 8 + 2;
        SetWindowPos(c->list, HWND_TOP, r.left + c->droprc.left, r.top + c->droprc.top, w - c->droprc.left, hh,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        int sel = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessage(c->list, LB_SETTOPINDEX, sel, 0);
        SetCapture(c->list);
        lbd(c->list)->cbtrack = 1;
        lbd(c->list)->lastsel = sel;
    } else {
        ShowWindow(c->list, SW_HIDE);
        lbd(c->list)->cbtrack = lbd(c->list)->mdown = 0;
        KillTimer(c->list, LB_TIMER_TRACK);
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
        if (h->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
            /* seg33:0F14: the parent draws the current item 3 px inside the field */
            int cur = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            DRAWITEMSTRUCT di = {ODT_COMBOBOX, h->id, (UINT)cur, ODA_DRAWENTIRE,
                                 (sel ? ODS_SELECTED | ODS_FOCUS : 0) | ((h->style & WS_DISABLED) ? ODS_DISABLED : 0),
                                 h, dc, c->field, (ULONG_PTR)SendMessage(c->list, LB_GETITEMDATA, cur, 0)};
            InflateRect(&di.rcItem, -3, -3);
            SendMessage(h->parent, WM_DRAWITEM, h->id, (LPARAM)&di);
        } else {
            ExtTextOut(dc, t.left + 1, t.top + 1, ETO_CLIPPED | ETO_OPAQUE, &t, h->text, strlen(h->text), NULL);
            if (sel) DrawFocusRect(dc, &t);
        }
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
        /* seg34:00DD: the list's height is whole items unless CBS_NOINTEGRALHEIGHT (measured on 3.11:
         * COMMDLG's drive list drops five 16-pixel items deep) */
        DWORD ls = WS_BORDER | WS_VSCROLL | LBS_NOTIFY | (h->style & CBS_SORT ? LBS_SORT : 0) |
                   (h->style & CBS_OWNERDRAWFIXED ? LBS_OWNERDRAWFIXED : 0) | (h->style & CBS_HASSTRINGS ? LBS_HASSTRINGS : 0) |
                   (h->style & CBS_NOINTEGRALHEIGHT ? LBS_NOINTEGRALHEIGHT : 0);
        if (cbtype(h) != CBS_DROPDOWNLIST)
            /* seg34: the selection stays drawn (the combo clears it when the focus leaves);
             * scrolling and OEM conversion only when the combo has them */
            c->edit = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NOHIDESEL | W16_ES_COMBOBOX |
                                   (h->style & CBS_AUTOHSCROLL ? ES_AUTOHSCROLL : 0) |
                                   (h->style & CBS_OEMCONVERT ? ES_OEMCONVERT : 0),
                                   0, 0, r.right, 1, h, (HMENU)1001, NULL, NULL);
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
        if (c->edit) SendMessage(c->edit, EM_SETSEL, 0, 0); /* CBKillFocusHelper (seg33) */
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
        if ((wp == VK_DOWN || wp == VK_UP) && (w16_keystate[VK_MENU] & 0x80)) { cb_show(h, !c->dropped); return 0; }
        /* USER seg35:1919: F4 drops or closes the list, except with the extended interface; with
         * it, Down drops the closed list and the other movement keys do nothing until it is open */
        if (wp == VK_F4) { if (cbtype(h) != CBS_SIMPLE && !c->extui) cb_show(h, !c->dropped); return 0; }
        if (c->extui && !c->dropped && cbtype(h) != CBS_SIMPLE &&
            (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_HOME || wp == VK_END)) {
            if (wp == VK_DOWN) cb_show(h, 1);
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_HOME || wp == VK_END) {
            int old = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            if (c->dropped) lbd(c->list)->caret_on = 1;
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
            else if (HIWORD(lp) == EN_SETFOCUS && !c->focus) {
                /* the focus went straight to the edit (a click): CBGetFocusHelper (seg33:115D) */
                c->focus = 1;
                SendMessage(c->edit, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                w16_notify_parent(h, CBN_SETFOCUS);
            }
            else if (HIWORD(lp) == EN_KILLFOCUS && w16_focus != h && w16_focus != c->list) {
                c->focus = 0;
                SendMessage(c->edit, EM_SETSEL, 0, 0); /* CBKillFocusHelper (seg33) */
                w16_notify_parent(h, CBN_KILLFOCUS);
            }
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
    case WM_MEASUREITEM: case WM_DRAWITEM: case WM_DELETEITEM: case WM_COMPAREITEM: {
        /* seg33:073F: from the combo's list, handed on to the parent as the combo's own */
        UINT *s = (UINT *)lp; /* CtlType, CtlID lead every one of these structures */
        s[0] = ODT_COMBOBOX;
        s[1] = h->id;
        if (m == WM_DRAWITEM) ((DRAWITEMSTRUCT *)lp)->hwndItem = h;
        else if (m == WM_DELETEITEM) ((DELETEITEMSTRUCT *)lp)->hwndItem = h;
        else if (m == WM_COMPAREITEM) ((COMPAREITEMSTRUCT *)lp)->hwndItem = h;
        return SendMessage(h->parent, m, h->id, lp);
    }
    case WM_SETTEXT:
    case WM_GETTEXTLENGTH:
        /* seg33:05AE: these go to the edit; a drop-down list has none and answers CB_ERR (the
         * Desktop's "Unlisted" pattern name never shows on 3.11) */
        if (c->edit) return SendMessage(c->edit, m, wp, lp);
        return CB_ERR;
    case WM_GETTEXT:
        if (c->edit) return SendMessage(c->edit, WM_GETTEXT, wp, lp);
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
    case CB_GETITEMHEIGHT:
        if ((int)(SHORT)wp == -1) return c->field.bottom - c->field.top; /* seg33:06BD: the field */
        return SendMessage(c->list, LB_GETITEMHEIGHT, wp, lp);
    /* seg33:0568: drop-down combos only */
    case CB_SETEXTENDEDUI:
        if (cbtype(h) == CBS_SIMPLE || wp > 1) return CB_ERR;
        c->extui = (int)wp;
        return 0;
    case CB_GETEXTENDEDUI: return cbtype(h) != CBS_SIMPLE && c->extui;
    }
    return DefWindowProc(h, m, wp, lp);
}
