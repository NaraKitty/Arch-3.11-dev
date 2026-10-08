/* USER: the multiple-document interface - the MDI client window class ("MDIClient"), DefFrameProc,
 * DefMDIChildProc, TranslateMDISysAccel, the WM_MDI* messages, the Window menu list ("More
 * Windows..." past nine children), a maximised child's system menu and restore box in the frame's
 * menu bar, CascadeChildWindows, TileChildWindows, ArrangeIconicWindows, CalcChildScroll and
 * ScrollChildren. Ported function by function from USER.EXE 3.11 (seg15 and seg20; the icon
 * arrangement is seg4); each function names its segment:offset. The parameter-validation layer of
 * the exported entry points is not ported.
 *
 * The client's private data (USER keeps it in the 16 extra bytes at WND+3C..4B) lives in the window's
 * control data here. Win16 messages that packed two handles into lParam are described in w16.h. */
#include "w16int.h"

#define WM_MDICALCCHILDSCROLL 0x10AC /* USER's own: recalculate the client's scroll bars */

typedef struct {
    int ckids;          /* 3C number of children */
    HWND maxed;         /* 3E the maximised child */
    HWND active;        /* 40 the active child */
    HMENU winmenu;      /* 42 the Window menu */
    UINT idfirst;       /* 44 id of the first child */
    UINT flags;         /* 46: low byte set while tiling, cascading, arranging or scrolling (no scroll
                           bar recalculation); 0x0100 / 0x0200 the client was created with WS_VSCROLL /
                           WS_HSCROLL; 0x0400 a child is being maximised; 0x0800 a recalculation is
                           posted */
    char *frametitle;   /* 48 the frame's own title */
    int cascade;        /* 4A next cascade position for CW_USEDEFAULT children */
} MDIClient;

static MDIClient *mdi(HWND client)
{
    return w16_valid(client) && client->proc && client->cls && client->cls->wc.lpfnWndProc == w16_mdiclient_proc
               ? (MDIClient *)client->ctl : NULL;
}

/* seg13:0ABE RedrawFrame: the frame recalculated and drawn again */
static void redraw_frame(HWND h)
{
    if (w16_valid(h))
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* ------------------------------------------------------------------ frame title */
/* seg15:0000 MDISetFrameTitle: the frame shows its own title, or "title - [child title]" while a child
 * is maximised (at most 159 characters). lpch: a new frame title; 1 redraws the whole frame; 2 only
 * stores the text; anything else draws the caption */
static void set_frame_title(HWND frame, HWND client, const char *lpch)
{
    MDIClient *c = mdi(client);
    if (!c || !w16_valid(frame)) return;
    uintptr_t code = (uintptr_t)lpch;
    if (code > 3 || code == 0) {
        free(c->frametitle);
        c->frametitle = lpch ? strdup(lpch) : NULL;
    }
    char sz[0xa0];
    sz[0] = 0;
    if (c->frametitle) {
        if (w16_valid(c->maxed) && c->maxed->text && c->maxed->text[0]) {
            snprintf(sz, sizeof sz, "%s", c->frametitle);
            int lt = strlen(c->frametitle);
            if (lt + 5 < 0xa0) {
                strcat(sz, " - [");
                const char *ch = c->maxed->text;
                if ((int)strlen(ch) + lt + 5 < 0xa0)
                    strcat(sz, ch);
                else {
                    int n = strlen(sz);
                    while (n < 0x9e && *ch) sz[n++] = *ch++;
                    sz[n] = 0;
                }
                strcat(sz, "]");
            }
        } else
            snprintf(sz, sizeof sz, "%s", c->frametitle);
    }
    free(frame->text);
    frame->text = strdup(sz);
    if (code == 1) { redraw_frame(frame); return; }
    if (code == 2) return;
    if (w16_has_caption(frame->style)) {
        if (w16_window_visible(frame)) {
            HDC dc = GetWindowDC(frame);
            w16_draw_caption(frame, dc, w16_caption_active(frame));
            ReleaseDC(frame, dc);
        }
    } else
        redraw_frame(frame);
}

/* ------------------------------------------------------------------ the Window menu (seg20) */
/* seg20:0000: the client's child with this id that is not an icon title */
static HWND child_by_id(HWND client, UINT id)
{
    for (HWND w = client->child; w; w = w->next)
        if (!w->owner && w->id == id) return w;
    return NULL;
}

/* seg20:0025: "&n title" with any '&' of the title doubled (159 characters at most) */
static void window_menu_text(HWND child, MDIClient *c, char *out, size_t cb)
{
    int n = child->id - c->idfirst + 1;
    if (child->text && child->text[0]) {
        char buf[0xa0];
        int i = 0;
        for (const char *p = child->text; *p && i < 0x9f; p++) {
            buf[i++] = *p;
            if (*p == '&') buf[i++] = '&';
        }
        buf[i] = 0;
        snprintf(out, cb, "&%d %s", n, buf);
    } else
        snprintf(out, cb, "&%d ", n);
}

/* seg20:00B1: the child's line of the Window menu written again (checked when it is active) */
static void update_window_menu_item(HWND child)
{
    HWND client = child->parent;
    MDIClient *c = mdi(client);
    if (!c || child->id > c->idfirst + 8) return;
    char buf[0xc8];
    window_menu_text(child, c, buf, sizeof buf);
    HWND frame = client->parent;
    ModifyMenu(frame->menu, child->id, MF_BYCOMMAND | MF_STRING | (c->active == child ? MF_CHECKED : 0), child->id, buf);
}

/* seg20:00FE: a maximised child's system menu goes to the left end of the frame's menu bar (USER's
 * bitmap 1) and its restore box to the right end (bitmap 2, MF_HELP, SC_RESTORE); the child loses
 * WS_SYSMENU meanwhile */
static int add_sys_menu(HWND frame, HWND client, HWND child)
{
    (void)client;
    if (!frame->menu || !child->sysmenu) return 0;
    if (!InsertMenu(frame->menu, 0, MF_BYPOSITION | MF_POPUP | MF_BITMAP, (UINT_PTR_W16)child->sysmenu, (LPCSTR)1))
        return 0;
    if (!AppendMenu(frame->menu, MF_HELP | MF_BITMAP, SC_RESTORE, (LPCSTR)2)) {
        RemoveMenu(frame->menu, 0, MF_BYPOSITION);
        return 0;
    }
    w16_set_sysmenu(child);
    child->style &= ~WS_SYSMENU;
    redraw_frame(child);
    return 1;
}

/* seg20:0176: the two taken out again when the bar ends with the restore box */
static int remove_sys_menu(HWND frame, HWND client, HWND child)
{
    (void)client;
    if (!frame->menu) return 0;
    int n = GetMenuItemCount(frame->menu) - 1;
    if (n < 0 || GetMenuItemID(frame->menu, n) != SC_RESTORE) return 0;
    child->style |= WS_SYSMENU;
    RemoveMenu(frame->menu, 0, MF_BYPOSITION);
    DeleteMenu(frame->menu, n - 1, MF_BYPOSITION);
    redraw_frame(child);
    return 1;
}

/* seg20:01D0: the child's line appended to the Window menu (a separator before the first; the tenth
 * child gets "More Windows...", USER string 63; none after that) */
static int append_window_menu_item(HWND client, HWND child)
{
    MDIClient *c = mdi(client);
    int k = (int)child->id - (int)c->idfirst;
    if (!c->winmenu || k >= 10) return 1;
    if (k == 0 && !AppendMenu(c->winmenu, MF_STRING, 0, NULL)) return 0;
    char buf[0xa8];
    if (k == 9) {
        HINSTANCE user = w16_system_module("USER.EXE");
        if (!user || !LoadString(user, 63, buf, 0xa5)) snprintf(buf, sizeof buf, "&More Windows...");
    } else
        window_menu_text(child, c, buf, sizeof buf);
    return AppendMenu(c->winmenu, MF_STRING, child->id, buf);
}

/* seg20:0252: the old Window menu replaced by the new one in the frame's menu bar, title kept */
static int replace_window_menu(HMENU bar, HMENU old, HMENU nw)
{
    if (old == nw) return 1;
    int i;
    HMENU s = NULL;
    for (i = 0; (s = GetSubMenu(bar, i)) != NULL && s != old; i++)
        ;
    if (!s) return 0;
    char buf[0x80];
    GetMenuString(bar, i, buf, sizeof buf, MF_BYPOSITION);
    if (!RemoveMenu(bar, i, MF_BYPOSITION)) return 0;
    return InsertMenu(bar, i, MF_BYPOSITION | MF_POPUP, (UINT_PTR_W16)nw, buf);
}

/* seg20:02C1: the children after this one move up an id; this one takes the last */
static void renumber(HWND client, HWND child)
{
    MDIClient *c = mdi(client);
    for (HWND w = client->child; w; w = w->next)
        if (!w->owner && w->id > child->id) w->id--;
    child->id = c->ckids + c->idfirst - 1;
}

/* seg20:02F9 MDISetMenu: a new frame menu (the maximised child's boxes move with it) and / or a new
 * Window menu; the children's lines are taken off the old Window menu (from its last separator, when
 * the line after it is the first child's) and written to the new one (the first ten visible, enabled
 * children; the others move to the end of the numbering) */
static void set_menu(HWND client, BOOL refresh, HMENU *pframe, HMENU *pwin)
{
    MDIClient *c = mdi(client);
    HWND frame = client->parent;
    HMENU oldf = frame->menu, oldw = c->winmenu;
    HMENU nf = *pframe, nw = *pwin;
    if (refresh) { nf = oldf; nw = oldw; }
    if (nf && nf != oldf) {
        if (c->maxed) remove_sys_menu(frame, client, c->maxed);
        frame->menu = nf;
        nf->owner = frame;
        if (c->maxed) add_sys_menu(frame, client, c->maxed);
    } else
        nf = oldf;
    *pframe = oldf;
    *pwin = oldw;
    if (!refresh && nw == oldw) return;
    if (oldw) {
        int cnt = GetMenuItemCount(oldw), i = cnt - 1;
        while (i >= 0 && !(GetMenuState(oldw, i, MF_BYPOSITION) & MF_SEPARATOR)) i--;
        if (i >= 0 && GetMenuItemID(oldw, i + 1) == c->idfirst)
            for (int k = i; k < cnt; k++) DeleteMenu(oldw, i, MF_BYPOSITION);
    }
    c->winmenu = nw;
    if (nw) {
        int k = 0;
        for (int i = 0; i < c->ckids; i++) {
            if (k >= 10) break;
            HWND w = child_by_id(client, c->idfirst + k);
            if (!w) break;
            if ((!(w->style & WS_VISIBLE) && (client->style & MDIS_ALLCHILDSTYLES)) || (w->style & WS_DISABLED))
                renumber(client, w);
            else {
                append_window_menu_item(client, w);
                k++;
            }
        }
        if (c->active) CheckMenuItem(nw, c->active->id, MF_CHECKED);
    }
    replace_window_menu(nf, oldw, nw);
}

/* seg20:045D / 04EC: "Select Window" (USER dialog 9): the visible, enabled children in id order, the
 * tenth selected; OK or a double click ends with the list index, Cancel with -2 */
static BOOL more_windows_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND client = (HWND)lp;
        MDIClient *c = mdi(client);
        for (int i = 0; c && i < c->ckids; i++) {
            HWND w = child_by_id(client, c->idfirst + i);
            if (w && (w->style & WS_VISIBLE) && !(w->style & WS_DISABLED)) {
                char buf[0x50];
                GetWindowText(w, buf, sizeof buf);
                SendDlgItemMessage(dlg, 100, LB_ADDSTRING, 0, (LPARAM)buf);
            }
        }
        SendDlgItemMessage(dlg, 100, LB_SETTOPINDEX, 9, 0);
        SendDlgItemMessage(dlg, 100, LB_SETCURSEL, 9, 0);
        SetFocus(GetDlgItem(dlg, 100));
        return FALSE;
    }
    case WM_COMMAND: {
        int r = -2;
        switch (wp) {
        case 100:
            if (HIWORD(lp) != LBN_DBLCLK) return TRUE;
            /* fall through */
        case IDOK:
            r = (int)SendDlgItemMessage(dlg, 100, LB_GETCURSEL, 0, 0);
            /* fall through */
        case IDCANCEL:
            EndDialog(dlg, r);
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ scroll bars (seg15) */
/* seg15:06B3: a recalculation posted unless one is pending or the client is busy arranging */
static void check_scroll(HWND client)
{
    MDIClient *c = mdi(client);
    if (!c || (c->flags & 0x08FF)) return;
    if (PostMessage(client, WM_MDICALCCHILDSCROLL, 0, 0)) c->flags |= 0x0800;
}

/* seg15:0276 CalcChildScroll: the range of a window's scroll bars covers the union of its client area
 * and its visible children (none while one is maximised); a bar is shown only when that union is
 * larger, and a bar that appears takes room from the other direction. Positions and ranges are in
 * screen coordinates as USER keeps them. bar: SB_HORZ, SB_VERT or SB_BOTH */
void CalcChildScroll(HWND h, UINT bar)
{
    if (!w16_valid(h) || (h->style & WS_MINIMIZE)) return;
    int cxv = GetSystemMetrics(SM_CXVSCROLL), cyh = GetSystemMetrics(SM_CYHSCROLL);
    if (h->style & WS_BORDER) { cxv -= GetSystemMetrics(SM_CXBORDER); cyh -= GetSystemMetrics(SM_CYBORDER); }
    int fv = 0, fh = 0;
    if (bar == SB_HORZ) fh = 1;
    else if (bar == SB_VERT) fv = 1;
    else if (bar == SB_BOTH) fv = fh = 1;
    else return;
    RECT rc = h->rc, all = {0, 0, 0, 0}, nr = {0, 0, 0, 0}, t;
    if (fv && (h->style & WS_VSCROLL)) rc.right += cxv;
    if (fh && (h->style & WS_HSCROLL)) rc.bottom += cyh;
    int need = 0;
    for (HWND w = h->child; w; w = w->next) {
        if (!(w->style & WS_VISIBLE)) continue;
        if (w->style & WS_MAXIMIZE) { need = 0; break; }
        UnionRect(&all, &all, &w->rw);
        if (!w->owner) {
            UnionRect(&t, &rc, &w->rw);
            if (!EqualRect(&rc, &t)) need = 1;
        }
    }
    int vadded = 0, hadded = 0;
    if (need) {
        do {
            t = nr;
            UnionRect(&nr, &all, &rc);
            nr.right += rc.left - rc.right;
            nr.bottom += rc.top - rc.bottom;
            if (fv && nr.top < nr.bottom && !vadded) { vadded = 1; rc.right -= cxv; }
            if (fh && nr.right > nr.left && !hadded) { hadded = 1; rc.bottom -= cyh; }
        } while (!EqualRect(&nr, &t));
    }
    int change = 0;
    if (fv) {
        if (nr.top >= nr.bottom) {
            nr.top = nr.bottom = rc.top = 0;
            if (h->style & WS_VSCROLL) change = 1;
        } else if (!(h->style & WS_VSCROLL))
            change = 1;
    }
    if (fh) {
        if (nr.right <= nr.left) {
            nr.left = nr.right = rc.left = 0;
            if (h->style & WS_HSCROLL) change = 1;
        } else if (!(h->style & WS_HSCROLL))
            change = 1;
    }
    if (change) {
        h->style &= ~(WS_VSCROLL | WS_HSCROLL);
        if (nr.top != nr.bottom) h->style |= WS_VSCROLL;
        if (nr.left != nr.right) h->style |= WS_HSCROLL;
        h->sb[SB_HORZ].pos = rc.left;
        h->sb[SB_HORZ].min = nr.left;
        h->sb[SB_HORZ].max = nr.right;
        h->sb[SB_VERT].pos = rc.top;
        h->sb[SB_VERT].min = nr.top;
        h->sb[SB_VERT].max = nr.bottom;
        redraw_frame(h);
        return;
    }
    if (fv) {
        SetScrollRange(h, SB_VERT, nr.top, nr.bottom, FALSE);
        SetScrollPos(h, SB_VERT, rc.top, TRUE);
    }
    if (fh) {
        SetScrollRange(h, SB_HORZ, nr.left, nr.right, FALSE);
        SetScrollPos(h, SB_HORZ, rc.left, TRUE);
    }
}

/* seg15:053F ScrollChildren: a scroll bar message moves the children: a line is SM_CXSIZE / SM_CYSIZE,
 * a page half the client area. With the application workspace background the distance is kept a
 * multiple of 8 (rounded away from zero) so its pattern lines up. SB_ENDSCROLL recalculates the bar */
void ScrollChildren(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w16_valid(h)) return;
    int vert = msg != WM_HSCROLL, mn, mx, pos, line, page, np;
    GetScrollRange(h, vert ? SB_VERT : SB_HORZ, &mn, &mx);
    pos = GetScrollPos(h, vert ? SB_VERT : SB_HORZ);
    if (!vert) {
        line = GetSystemMetrics(SM_CXSIZE);
        page = (h->rc.right - h->rc.left) / 2;
    } else {
        line = GetSystemMetrics(SM_CYSIZE);
        page = (h->rc.bottom - h->rc.top) / 2;
    }
    switch (wp) {
    case SB_LINEUP: np = pos - line; break;
    case SB_LINEDOWN: np = line + pos; break;
    case SB_PAGEUP: np = pos - page; break;
    case SB_PAGEDOWN: np = page + pos; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: np = (SHORT)LOWORD(lp); break;
    case SB_TOP: np = mn; break;
    case SB_BOTTOM: np = mx; break;
    case SB_ENDSCROLL: CalcChildScroll(h, vert ? SB_VERT : SB_HORZ); return;
    default: return;
    }
    if (mn > np) np = mn;
    else if (mx < np) np = mx;
    if ((uintptr_t)h->cls->wc.hbrBackground == COLOR_APPWORKSPACE + 1) {
        int d = pos - np;
        if (d & 7) d += d > 0 ? 8 : -8;
        np = pos - (d & ~7);
    }
    SetScrollPos(h, vert ? SB_VERT : SB_HORZ, np, TRUE);
    if (vert) ScrollWindow(h, 0, pos - np, NULL, NULL);
    else ScrollWindow(h, pos - np, 0, NULL, NULL);
}

/* ------------------------------------------------------------------ arranging (seg15, seg4) */
/* seg15:06DE: windows that tiling and cascading move: visible, neither minimised nor maximised, not
 * owned (icon titles), and enabled with MDITILE_SKIPDISABLED */
static int tile_filter(HWND w, UINT flags)
{
    return w && !(w->style & (WS_MINIMIZE | WS_MAXIMIZE)) && !w->owner && (w->style & WS_VISIBLE) &&
           !((flags & MDITILE_SKIPDISABLED) && (w->style & WS_DISABLED));
}

/* seg15:071D */
static int count_tiled(HWND parent, UINT flags)
{
    int n = 0;
    for (HWND w = parent->child; w; w = w->next)
        if (tile_filter(w, flags)) n++;
    return n;
}

/* seg15:0746 GetCascadeWindowPos: steps of cxFrame + cxSize across and cyFrame + cySize down, as many
 * as fit in a third of the height left above the icons; the size takes the rest */
static void cascade_pos(HWND parent, int i, int cyicons, RECT *r)
{
    RECT rc;
    GetClientRect(parent, &rc);
    int hgt = rc.bottom - rc.top - cyicons;
    if (hgt < 0) hgt = 0;
    int dy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYSIZE);
    int dx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXSIZE);
    int n = hgt / (3 * dy);
    r->right = rc.right - n * dx - rc.left; /* cx */
    r->bottom = hgt - dy * n;               /* cy */
    int k = i % (n + 1);
    r->left = k * dx;
    r->top = k * dy;
}

/* seg4:01AF ArrangeIconicWindows: the visible icons packed into the slots from the bottom-left corner
 * of the parent's client area, in their order (rows from the bottom, then left to right; each icon's
 * row is the nearest), their titles with them. Windows that are not minimised forget their icon
 * place. Returns the number of icons */
UINT ArrangeIconicWindows(HWND parent)
{
    if (!parent) parent = w16_desktop;
    if (!w16_valid(parent) && parent != w16_desktop) return 0;
    int sx = GetSystemMetrics(SM_CXICONSPACING), sy = GetSystemMetrics(SM_CYICONSPACING);
    int hx = GetSystemMetrics(SM_CXICON) / 2;
    int iw = GetSystemMetrics(SM_CXICON) + 4 * GetSystemMetrics(SM_CXBORDER);
    int ih = GetSystemMetrics(SM_CYICON) + 4 * GetSystemMetrics(SM_CYBORDER);
    RECT pc = parent == w16_desktop ? (RECT){0, 0, w16_screen.w, w16_screen.h} : parent->rc;
    int ph = pc.bottom - pc.top;
    HWND list[512];
    int n = 0;
    for (HWND w = parent->child; w && n < 512; w = w->next) {
        if (!(w->style & WS_VISIBLE)) continue;
        if (!(w->style & WS_MINIMIZE)) { w->has_iconpos = 0; continue; }
        /* the icon's row: its distance from the bottom rounded to the nearest whole row */
        w->has_iconpos = 0;
        w->iconpos.x = w->rw.left - pc.left;
        int y = ph - (w->rw.top - pc.top);
        y += (UINT)sy / 2;
        y -= (UINT)y % (UINT)sy;
        w->iconpos.y = ph - y;
        list[n++] = w;
    }
    if (!n) return 0;
    /* insertion sort: rows from the bottom up, then x */
    for (int i = 1; i < n; i++) {
        HWND e = list[i];
        int j = 0;
        while (j < i && !(list[j]->iconpos.y < e->iconpos.y ||
                          (list[j]->iconpos.y == e->iconpos.y && list[j]->iconpos.x > e->iconpos.x)))
            j++;
        memmove(&list[j + 1], &list[j], sizeof list[0] * (i - j));
        list[j] = e;
    }
    int cols = (UINT)(pc.right - pc.left) / (UINT)sx;
    if (cols < 1) cols = 1;
    for (int k = 0; k < n; k++) {
        HWND e = list[k];
        e->iconpos.y = ph - (k / cols) * sy - sy;
        e->iconpos.x = (k % cols) * sx + sx / 2 - hx;
    }
    HDWP d = BeginDeferWindowPos(n * 2);
    for (int k = 0; k < n; k++) {
        HWND e = list[k];
        if (!(e->style & WS_CHILD)) w16_invalidate_icon_title(e);
        d = DeferWindowPos(d, e, NULL, e->iconpos.x, e->iconpos.y, iw, ih, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS);
        if (w16_valid(e->icon_title)) {
            RECT r;
            w16_icon_title_rect_at(e, e->iconpos.x, e->iconpos.y, &r);
            d = DeferWindowPos(d, e->icon_title, NULL, r.left, r.top, r.right, r.bottom,
                               SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS);
        }
    }
    EndDeferWindowPos(d);
    for (int k = 0; k < n; k++)
        if (w16_valid(list[k]) && !(list[k]->style & WS_CHILD)) w16_invalidate_icon_title(list[k]);
    return n;
}

/* seg15:0875 CascadeChildWindows: from the bottom of the z-order up, each window at the next cascade
 * position above the icons (a window without a sizing frame keeps its size). The desktop's own
 * variant (seg15:07C6) is not ported */
BOOL CascadeChildWindows(HWND parent, UINT flags)
{
    if (!w16_valid(parent)) return FALSE;
    int cyicons = ArrangeIconicWindows(parent) ? GetSystemMetrics(SM_CYICONSPACING) : 0;
    int n = count_tiled(parent, flags);
    if (!n) return TRUE;
    HWND w = GetWindow(parent->child, GW_HWNDLAST);
    HDWP d = BeginDeferWindowPos(n);
    if (!d) return FALSE;
    for (int i = 0; i < n; i++) {
        while (w && !tile_filter(w, flags)) w = GetWindow(w, GW_HWNDPREV);
        if (!w) break;
        RECT r;
        cascade_pos(parent, i, cyicons, &r);
        UINT swp = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | ((w->style & WS_THICKFRAME) ? 0 : SWP_NOSIZE);
        d = DeferWindowPos(d, w, NULL, r.left, r.top, r.right, r.bottom, swp);
        w = GetWindow(w, GW_HWNDPREV);
    }
    EndDeferWindowPos(d);
    return TRUE;
}

/* seg15:0956 TileChildWindows: a grid of k - 1 rows (MDITILE_VERTICAL) or columns, k the smallest
 * number whose square is over the count; the windows from the top of the z-order down, column by
 * column, the last columns taking the remainder one more each. A maximised child is restored first.
 * The desktop's variant (seg15:07C6) is not ported */
BOOL TileChildWindows(HWND parent, UINT flags)
{
    if (!w16_valid(parent)) return FALSE;
    MDIClient *c = mdi(parent);
    if (c && c->maxed) ShowWindow(c->maxed, SW_SHOWNORMAL);
    int cyicons = ArrangeIconicWindows(parent) ? GetSystemMetrics(SM_CYICONSPACING) : 0;
    int n = count_tiled(parent, flags);
    if (!n) return TRUE;
    int k = 2;
    while (k * k <= n) k++;
    int cols, rows;
    if (flags & MDITILE_HORIZONTAL) { cols = k - 1; rows = n / (k - 1); }
    else { rows = k - 1; cols = n / (k - 1); }
    int rem = n % (k - 1);
    RECT rc;
    GetClientRect(parent, &rc);
    int hgt = rc.bottom - rc.top - cyicons, wid = rc.right - rc.left;
    if (wid <= 0 || hgt <= 0) return FALSE;
    HWND w = parent->child;
    HDWP d = BeginDeferWindowPos(n);
    if (!d) return FALSE;
    for (int col = 0; col < cols && w; col++) {
        if (cols - col <= rem) rows++;
        for (int row = 0; row < rows; row++) {
            int cxw = wid / cols, cyw = hgt / rows;
            while (w && !tile_filter(w, flags)) w = w->next;
            if (!w) break;
            UINT swp = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | ((w->style & WS_THICKFRAME) ? 0 : SWP_NOSIZE);
            d = DeferWindowPos(d, w, NULL, col * cxw, row * cyw, cxw, cyw, swp);
            w = w->next;
            if (!w) break;
        }
        if (cols - col <= rem) { rows--; rem--; }
    }
    EndDeferWindowPos(d);
    return TRUE;
}

/* ------------------------------------------------------------------ activation (seg15) */
/* seg15:0B01 ActivateMDIChild: the old child is deactivated (WM_NCACTIVATE, WM_MDIACTIVATE FALSE,
 * unchecked), a maximised state passes to the new child, the new child comes to the top, is checked
 * in the Window menu (taking the ninth line when its own is past it), drawn active and focused while
 * the frame is active, and told (WM_MDIACTIVATE TRUE) */
static void activate_child(HWND client, HWND child)
{
    MDIClient *c = mdi(client);
    if (!c || c->active == child) return;
    if (child && (child->style & WS_DISABLED)) return;
    HWND old = c->active;
    int fframe = client->parent == w16_active;
    if (old) {
        /* (every window counts as a 3.1 program's) */
        if (!SendMessage(old, WM_NCACTIVATE, FALSE, 0) && fframe) return;
        SendMessage(old, WM_MDIACTIVATE, FALSE, MAKELPARAM(w16_hwnd16(child), w16_hwnd16(old)));
        if (c->winmenu && w16_valid(old)) CheckMenuItem(c->winmenu, old->id, MF_UNCHECKED);
    }
    if (c->maxed && c->maxed != child) {
        if (child) {
            c->active = child;
            ShowWindow(child, SW_SHOWMAXIMIZED);
        } else
            ShowWindow(c->maxed, SW_SHOWNORMAL);
    }
    c->active = child;
    if (!child) {
        if (fframe) SetFocus(client);
        return;
    }
    if (c->winmenu) {
        if ((UINT)(child->id - c->idfirst) < 9)
            CheckMenuItem(c->winmenu, child->id, MF_CHECKED);
        else {
            HWND o = child_by_id(client, c->idfirst + 8);
            if (o) o->id = child->id;
            child->id = c->idfirst + 8;
            update_window_menu_item(child);
        }
    }
    SetWindowPos(child, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    if (!w16_valid(child)) return;
    if (fframe) {
        SendMessage(child, WM_NCACTIVATE, TRUE, 0);
        if (w16_focus == client && old)
            SendMessage(client, WM_SETFOCUS, (WPARAM)client, 0);
        else
            SetFocus(client);
    }
    if (w16_valid(child)) SendMessage(child, WM_MDIACTIVATE, TRUE, MAKELPARAM(w16_hwnd16(child), w16_hwnd16(old)));
}

/* seg15:0C7F MDINext: the next (or previous) visible, enabled, unowned child after hwnd in the z-order
 * (around the end) comes to the top and is activated; going forward hwnd goes to the bottom. With a
 * maximised child the client is hidden meanwhile */
static void mdi_next(HWND client, HWND h, BOOL prev)
{
    MDIClient *c = mdi(client);
    if (!c) return;
    if (!h) {
        if (!c->active) return;
        h = c->active;
    }
    HWND w = prev ? GetWindow(h, GW_HWNDPREV) : h->next;
    for (;;) {
        if (!w)
            w = prev ? GetWindow(client->child, GW_HWNDLAST) : client->child;
        else if (!w->owner && !(w->style & WS_DISABLED) && (w->style & WS_VISIBLE))
            break;
        else
            w = prev ? GetWindow(w, GW_HWNDPREV) : w->next;
        if (w == h) break;
    }
    if (w == h || !w) return;
    int hid = 0;
    if (c->maxed) {
        client->style &= ~WS_VISIBLE;
        hid = 1;
    }
    HDWP d = BeginDeferWindowPos(2);
    d = DeferWindowPos(d, w, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    if (!prev) d = DeferWindowPos(d, h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    EndDeferWindowPos(d);
    if (hid) ShowWindow(client, SW_SHOW);
}

/* ------------------------------------------------------------------ creating and destroying children */
/* seg15:0D72: CW_USEDEFAULT (or 0) sizes and positions from the next cascade position */
static void check_cascade_rect(HWND client, int *x, int *y, int *cx, int *cy)
{
    MDIClient *c = mdi(client);
    RECT r;
    cascade_pos(client, c->cascade, 0, &r);
    if (*cx == CW_USEDEFAULT || *cx == 0) *cx = r.right;
    if (*cy == CW_USEDEFAULT || *cy == 0) *cy = r.bottom;
    if (*x == CW_USEDEFAULT) {
        *x = r.left;
        *y = r.top;
    }
}

/* seg15:0DDB MDICreateChild: the child gets WS_CHILD | WS_CLIPSIBLINGS and (without
 * MDIS_ALLCHILDSTYLES) the full frame with caption and boxes, the next id, USER's MDI system menu
 * (menu 2), a line in the Window menu, and is shown and activated - a maximised child is restored
 * first, unless the new one comes maximised */
static HWND create_child(HWND client, const MDICREATESTRUCT *mcs)
{
    MDIClient *c = mdi(client);
    if (!c || !mcs) return NULL;
    DWORD style = mcs->style | WS_CHILD | WS_CLIPSIBLINGS;
    if (!(client->style & MDIS_ALLCHILDSTYLES)) {
        style &= 0x2B30FFFFu;
        style |= 0x54CF0000u;
    }
    int vis = (style & WS_VISIBLE) != 0;
    int x = mcs->x, y = mcs->y, cx = mcs->cx, cy = mcs->cy;
    check_cascade_rect(client, &x, &y, &cx, &cy);
    HWND m = c->maxed;
    if (vis && w16_valid(m)) {
        if (style & WS_MAXIMIZE) SendMessage(m, WM_SETREDRAW, FALSE, 0);
        w16_min_maximize(m, SW_SHOWNORMAL, 1);
        if (style & WS_MAXIMIZE) SendMessage(m, WM_SETREDRAW, TRUE, 0);
    }
    HWND h = CreateWindowEx(0, mcs->szClass, mcs->szTitle, style, x, y, cx, cy, client,
                            (HMENU)(uintptr_t)(c->idfirst + c->ckids), mcs->hOwner, (void *)mcs);
    if (!h) {
        if (w16_valid(m)) ShowWindow(m, SW_SHOWMAXIMIZED);
        return NULL;
    }
    c->ckids++;
    if (++c->cascade > 0x7FFE) c->cascade = 0;
    int hadsys = h->sysmenu != NULL;
    if (vis && !(style & WS_DISABLED) && c->ckids <= 10) SendMessage(client, WM_MDISETMENU, TRUE, 0);
    if ((style & WS_SYSMENU) && !hadsys && w16_valid(h)) h->sysmenu = w16_default_sysmenu(h);
    if (vis && w16_valid(h)) {
        if ((h->style & WS_MINIMIZE) && c->active)
            ShowWindow(h, SW_SHOWMINNOACTIVE);
        else {
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
            if (w16_valid(h) && (h->style & WS_MAXIMIZE) && !hadsys) {
                add_sys_menu(client->parent, client, h);
                redraw_frame(client->parent);
            }
        }
    }
    return w16_valid(h) ? h : NULL;
}

/* seg15:0FD7 MDIDestroyChild: the next child is activated first (the last one hidden; a maximised one
 * takes its boxes and title out of the frame), the numbering closes up, the Window menu is written
 * again and the scroll bars recalculated */
static void destroy_child(HWND client, HWND h)
{
    MDIClient *c = mdi(client);
    if (!c || !w16_valid(h)) return;
    renumber(client, h);
    if (c->active == h) {
        mdi_next(client, h, FALSE);
        if (c->active == h) {
            ShowWindow(h, SW_HIDE);
            if (c->maxed) {
                remove_sys_menu(client->parent, client, c->maxed);
                c->maxed = NULL;
                set_frame_title(client->parent, client, (const char *)1);
                if (client->parent->style & WS_VISIBLE) redraw_frame(client->parent);
            }
            activate_child(client, NULL);
        }
    }
    c->ckids--;
    SendMessage(client, WM_MDISETMENU, TRUE, 0);
    DestroyWindow(h);
    check_scroll(client);
}

/* ------------------------------------------------------------------ the MDI client window (seg15:1062) */
LRESULT w16_mdiclient_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    MDIClient *c = mdi(h);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        const CLIENTCREATESTRUCT *ccs = cs ? (const CLIENTCREATESTRUCT *)cs->lpCreateParams : NULL;
        c = calloc(1, sizeof *c);
        if (!c) return -1;
        h->ctl = c;
        if (ccs) {
            c->winmenu = ccs->hWindowMenu;
            c->idfirst = ccs->idFirstChild;
        }
        HWND frame = h->parent;
        /* the frame's title moves into the client's keeping */
        c->frametitle = frame->text;
        frame->text = NULL;
        set_frame_title(frame, h, (const char *)2);
        if (h->style & WS_VSCROLL) c->flags |= 0x0100;
        if (h->style & WS_HSCROLL) c->flags |= 0x0200;
        h->style &= ~(WS_VSCROLL | WS_HSCROLL);
        GetSystemMenu(frame, FALSE);
        w16_nc_calc(h, &h->rw, &h->rc);
        return 0;
    }
    case WM_DESTROY:
        if (!c) break;
        if (c->maxed) remove_sys_menu(h->parent, h, c->maxed);
        free(c->frametitle);
        c->frametitle = NULL;
        if (c->ckids++ && c->winmenu) {
            int n = GetMenuItemCount(c->winmenu);
            while (c->ckids-- && n > 0) DeleteMenu(c->winmenu, --n, MF_BYPOSITION);
        }
        return 0;
    case WM_NCDESTROY:
        free(c);
        h->ctl = NULL;
        break;
    case WM_SIZE:
        if (!c) break;
        if (c->active && (c->active->style & WS_MAXIMIZE)) {
            RECT r = {0, 0, LOWORD(lp), HIWORD(lp)};
            AdjustWindowRectEx(&r, c->active->style, FALSE, c->active->exstyle);
            MoveWindow(c->active, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        } else
            check_scroll(h);
        break;
    case WM_SETFOCUS:
        if (c && c->active && !(c->active->style & WS_MINIMIZE)) SetFocus(c->active);
        return 0;
    case WM_NCACTIVATE:
        if (c && c->active) SendMessage(c->active, WM_NCACTIVATE, wp, lp);
        break;
    case WM_HSCROLL:
    case WM_VSCROLL:
        if (!c) break;
        c->flags |= 3;
        ScrollChildren(h, msg, wp, lp);
        c->flags &= ~0xFFu;
        return 0;
    case WM_PARENTNOTIFY:
        if (c && wp == WM_LBUTTONDOWN) {
            POINT pt = {(SHORT)LOWORD(lp), (SHORT)HIWORD(lp)};
            HWND w = ChildWindowFromPoint(h, pt);
            if (w && w != h) {
                if (w->owner) w = w->owner;
                if (c->active != w) SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
            }
        }
        return 0;
    case WM_MDICREATE:
        return (LRESULT)create_child(h, (const MDICREATESTRUCT *)lp);
    case WM_MDIDESTROY:
        destroy_child(h, (HWND)wp);
        return 0;
    case WM_MDIACTIVATE:
        if (c && w16_valid((HWND)wp) && c->active != (HWND)wp)
            SetWindowPos((HWND)wp, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        return 0;
    case WM_MDIRESTORE:
        ShowWindow((HWND)wp, SW_SHOWNORMAL);
        return 0;
    case WM_MDINEXT:
        mdi_next(h, (HWND)wp, lp != 0);
        return 0;
    case WM_MDIMAXIMIZE:
        ShowWindow((HWND)wp, SW_SHOWMAXIMIZED);
        return 0;
    case WM_MDITILE:
    case WM_MDICASCADE: {
        if (!c) break;
        c->flags |= 3;
        ShowScrollBar(h, SB_BOTH, FALSE);
        if (c->maxed) ShowWindow(c->maxed, SW_SHOWNORMAL);
        BOOL r = msg == WM_MDITILE ? TileChildWindows(h, (UINT)wp) : CascadeChildWindows(h, (UINT)wp);
        c->flags &= ~0xFFu;
        return r;
    }
    case WM_MDIICONARRANGE:
        if (!c) break;
        c->flags |= 3;
        ArrangeIconicWindows(h);
        c->flags &= ~0xFFu;
        check_scroll(h);
        return 0;
    case WM_MDIGETACTIVE:
        if (!c) return 0;
        if (lp) *(BOOL *)lp = c->maxed != NULL;
        return (LRESULT)c->active;
    case WM_MDISETMENU: {
        if (!c) return 0;
        W16MDISETMENU *s = (W16MDISETMENU *)lp, tmp = {NULL, NULL};
        if (!s) s = &tmp;
        set_menu(h, wp != 0, &s->hmenuFrame, &s->hmenuWindow);
        return (LRESULT)s->hmenuFrame;
    }
    case WM_MDICALCCHILDSCROLL: {
        if (!c || (c->flags & 0xFF)) return 0;
        int bar = 0;
        if (c->flags & 0x0100) bar = SB_VERT;
        if (c->flags & 0x0200) bar = bar == SB_VERT ? SB_BOTH : 0; /* a client with only WS_HSCROLL never
                                                                       recalculates: as USER */
        if (bar) {
            CalcChildScroll(h, bar);
            c->flags &= ~0x0800u;
        }
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ DefFrameProc (seg15:147C) */
LRESULT DefFrameProc(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp)
{
    MDIClient *c = mdi(client);
    if (!c) return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            MoveWindow(client, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        else {
            /* minimised: the client keeps the size the restored frame's client area will have */
            int m = GetSystemMetrics((h->style & WS_THICKFRAME) ? SM_CYFRAME : SM_CYBORDER);
            RECT n = h->restore;
            MoveWindow(client, 0, 0, n.right - n.left - 2 * m,
                       n.bottom - n.top - 2 * m - GetSystemMetrics(SM_CYCAPTION) - GetSystemMetrics(SM_CYMENU), TRUE);
        }
        break;
    case WM_SETFOCUS:
        SetFocus(client);
        return 0;
    case WM_SETTEXT:
        set_frame_title(h, client, lp ? (const char *)lp : NULL);
        return 0;
    case WM_NCACTIVATE:
        SendMessage(client, WM_NCACTIVATE, wp, lp);
        break;
    case WM_COMMAND: {
        UINT id = (UINT)wp;
        if (id == c->idfirst + 9) {
            /* "More Windows...": USER dialog 9 */
            HINSTANCE user = w16_system_module("USER.EXE");
            int r = user ? DialogBoxParam(user, MAKEINTRESOURCE(9), h, more_windows_proc, (LPARAM)client) : -2;
            if (r < 0) goto sc;
            id = r + c->idfirst;
            goto act;
        }
        if (c->idfirst <= id && id < (UINT)c->ckids + c->idfirst) {
        act: {
            HWND w = child_by_id(client, id);
            SendMessage(client, WM_MDIACTIVATE, (WPARAM)w, 0);
            if (w16_valid(w) && (w->style & WS_MINIMIZE)) ShowWindow(w, SW_SHOWNORMAL);
            return 0;
        }
        }
    sc:
        /* a maximised child's system menu sits in the frame's menu bar: its commands go to the child */
        switch (wp & 0xFFF0) {
        case SC_SIZE: case SC_MOVE: case SC_MINIMIZE: case SC_MAXIMIZE: case SC_NEXTWINDOW:
        case SC_PREVWINDOW: case SC_CLOSE: case SC_RESTORE:
            if (c->maxed) return SendMessage(c->maxed, WM_SYSCOMMAND, wp, lp);
        }
        break;
    }
    case WM_MENUCHAR:
        /* '-' in the frame's menu: the maximised child's system menu (the bar's first item), else the
         * active child's own */
        if (!(h->style & WS_MINIMIZE) && wp == '-') {
            if (c->maxed) return MAKELONG(0, 2);
            if (c->active) {
                PostMessage(c->active, WM_SYSCOMMAND, SC_KEYMENU, '-');
                return MAKELONG(0, 1);
            }
        }
        break;
    case WM_NEXTMENU: {
        /* left from the menu bar or right from the system menu: the active child's system menu */
        MDINEXTMENU *nm = (MDINEXTMENU *)lp;
        if ((h->style & WS_MINIMIZE) || !c->active || c->maxed || !nm) return 0;
        if ((wp == VK_LEFT && nm->hmenuIn == h->menu) || (wp == VK_RIGHT && nm->hmenuIn == GetSystemMenu(h, FALSE))) {
            nm->hmenuNext = GetSystemMenu(c->active, FALSE);
            nm->hwndNext = c->active;
        }
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ DefMDIChildProc (seg15:187A) */
/* seg15:1772 MDIChildSize: the system menu follows the state; a child leaving the maximised state
 * takes its boxes and title out of the frame, one becoming maximised restores the old maximised child
 * (without redrawing it) and brings its own; a minimised child activates the top window; the scroll
 * bars are recalculated */
static void child_size(HWND h, WPARAM type)
{
    HWND client = h->parent, frame = client->parent;
    MDIClient *c = mdi(client);
    w16_set_sysmenu(h);
    if (c->maxed == h && type != SIZE_MAXIMIZED && !(c->flags & 0x0400)) {
        c->maxed = NULL;
        remove_sys_menu(frame, client, h);
        set_frame_title(frame, client, (const char *)1);
    }
    if (type == SIZE_MAXIMIZED) {
        if (c->maxed == h) return;
        c->flags |= 0x04FF;
        HWND old = c->maxed;
        if (old) {
            SendMessage(old, WM_SETREDRAW, FALSE, 0);
            remove_sys_menu(frame, client, old);
            w16_min_maximize(old, 0xCC, 0);
            SendMessage(old, WM_SETREDRAW, TRUE, 0);
        }
        c->maxed = h;
        add_sys_menu(frame, client, h);
        set_frame_title(frame, client, (const char *)1);
        c->flags &= 0xFB00;
    }
    if (type == SIZE_MINIMIZED) {
        HWND w = client->child;
        while (w && (w->owner || !(w->style & WS_VISIBLE))) w = w->next;
        if (w && w16_active && IsChild(w16_active, client)) SendMessage(w, WM_CHILDACTIVATE, 0, 0);
    }
    if (!(c->flags & 0xFF)) check_scroll(client);
}

/* seg15:16EF: a child maximises to its client's client area, its frame outside it */
static void child_minmax(HWND h, MINMAXINFO *mm)
{
    RECT r;
    GetClientRect(h->parent, &r);
    AdjustWindowRectEx(&r, h->style, FALSE, h->exstyle);
    mm->ptMaxPosition = (POINT){r.left, r.top};
    mm->ptMaxSize = (POINT){r.right - r.left, r.bottom - r.top};
}

LRESULT DefMDIChildProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w16_valid(h) || !w16_valid(h->parent)) return DefWindowProc(h, msg, wp, lp);
    HWND client = h->parent;
    MDIClient *c = mdi(client);
    if (!c) return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_MOVE:
        if (!(h->style & WS_MAXIMIZE)) check_scroll(client);
        break;
    case WM_SIZE:
        child_size(h, wp);
        break;
    case WM_SETFOCUS:
        if (c->active != h) activate_child(client, h);
        break;
    case WM_SETTEXT:
        DefWindowProc(h, msg, wp, lp);
        if (c->winmenu) update_window_menu_item(h);
        if (h->style & WS_MAXIMIZE) set_frame_title(client->parent, client, (const char *)3);
        return 0;
    case WM_CLOSE:
        SendMessage(GetParent(h), WM_MDIDESTROY, (WPARAM)h, 0);
        return 0;
    case WM_CHILDACTIVATE:
        activate_child(client, h);
        return 0;
    case WM_GETMINMAXINFO:
        child_minmax(h, (MINMAXINFO *)lp);
        return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_SIZE:
        case SC_MOVE:
            if (c->maxed == h) return 0;
            break;
        case SC_MAXIMIZE:
            if (c->maxed == h) return SendMessage(client->parent, WM_SYSCOMMAND, SC_MAXIMIZE, lp);
            break;
        case SC_NEXTWINDOW:
            SendMessage(GetParent(h), WM_MDINEXT, (WPARAM)h, 0);
            return 0;
        case SC_PREVWINDOW:
            SendMessage(GetParent(h), WM_MDINEXT, (WPARAM)h, 1);
            return 0;
        }
        break;
    case WM_MENUCHAR:
        /* any other character of a child's system menu goes to the frame's menu */
        PostMessage(GetParent(GetParent(h)), WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)(WORD)wp);
        return MAKELONG(0, 1);
    case WM_NEXTMENU: {
        MDINEXTMENU *nm = (MDINEXTMENU *)lp;
        HWND frame = client->parent;
        if (nm) {
            nm->hmenuNext = wp == VK_LEFT ? GetSystemMenu(frame, FALSE) : frame->menu;
            nm->hwndNext = frame;
        }
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ TranslateMDISysAccel (seg15:01D1) */
/* Ctrl+F6 / Ctrl+Tab (with Shift: backwards) and Ctrl+F4 (without Alt) become the active child's
 * SC_NEXTWINDOW / SC_PREVWINDOW / SC_CLOSE */
BOOL TranslateMDISysAccel(HWND client, LPMSG m)
{
    if (!m || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return FALSE;
    MDIClient *c = mdi(client);
    if (!c || !c->active || (c->active->style & WS_DISABLED)) return FALSE;
    if ((SHORT)GetKeyState(VK_CONTROL) >= 0 || (SHORT)GetKeyState(VK_MENU) < 0) return FALSE;
    UINT sc;
    switch (m->wParam) {
    case VK_F6:
    case VK_TAB:
        sc = (SHORT)GetKeyState(VK_SHIFT) < 0 ? SC_PREVWINDOW : SC_NEXTWINDOW;
        break;
    case VK_F4:
        sc = SC_CLOSE;
        break;
    default:
        return FALSE;
    }
    SendMessage(c->active, WM_SYSCOMMAND, sc, MAKELPARAM(m->wParam, 0));
    return TRUE;
}
