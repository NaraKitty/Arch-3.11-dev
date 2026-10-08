/* USER: window classes, the window tree, creation/destruction, positions, painting state */
#include "w16int.h"

HWND w16_desktop, w16_focus, w16_active, w16_capture;
static W16Class *classes;
static int default_pos_count;

int w16_valid(HWND h) { return h && h->magic == W16_WND_MAGIC && !h->destroyed; }
BOOL IsWindow(HWND h) { return w16_valid(h); }

/* ------------------------------------------------------------------ classes */
W16Class *w16_find_class(LPCSTR name, HINSTANCE inst)
{
    (void)inst;
    if (IS_INTRESOURCE(name)) {
        char b[16];
        snprintf(b, sizeof b, "#%u", (unsigned)(uintptr_t)name);
        for (W16Class *c = classes; c; c = c->next)
            if (!strcasecmp(c->name, b)) return c;
        return NULL;
    }
    for (W16Class *c = classes; c; c = c->next)
        if (!strcasecmp(c->name, name)) return c;
    return NULL;
}

ATOM RegisterClass(const WNDCLASS *wc)
{
    char nm[64];
    if (IS_INTRESOURCE(wc->lpszClassName)) snprintf(nm, sizeof nm, "#%u", (unsigned)(uintptr_t)wc->lpszClassName);
    else snprintf(nm, sizeof nm, "%s", wc->lpszClassName);
    W16Class *old = w16_find_class(nm, wc->hInstance);
    if (old && !old->system) return 0;
    W16Class *c = calloc(1, sizeof *c);
    c->wc = *wc;
    snprintf(c->name, sizeof c->name, "%s", nm);
    c->wc.lpszClassName = c->name;
    if (wc->lpszMenuName && !IS_INTRESOURCE(wc->lpszMenuName)) c->wc.lpszMenuName = strdup(wc->lpszMenuName);
    c->next = classes;
    classes = c;
    static ATOM a = 0xC000;
    return a++;
}

BOOL UnregisterClass(LPCSTR name, HINSTANCE h)
{
    (void)h;
    for (W16Class **p = &classes; *p; p = &(*p)->next)
        if (!strcasecmp((*p)->name, name)) { *p = (*p)->next; return TRUE; }
    return FALSE;
}

BOOL GetClassInfo(HINSTANCE h, LPCSTR name, WNDCLASS *wc)
{
    W16Class *c = w16_find_class(name, h);
    if (!c) return FALSE;
    *wc = c->wc;
    return TRUE;
}

int GetClassName(HWND h, LPSTR buf, int cb)
{
    if (!w16_valid(h)) return 0;
    snprintf(buf, cb, "%s", h->cls->name);
    return strlen(buf);
}

static void sysclass(const char *name, WNDPROC p, UINT style, HCURSOR cur, int extra)
{
    WNDCLASS wc = {0};
    wc.style = style | CS_GLOBALCLASS;
    wc.lpfnWndProc = p;
    wc.lpszClassName = name;
    wc.hCursor = cur;
    wc.cbWndExtra = extra;
    RegisterClass(&wc);
    w16_find_class(name, NULL)->system = 1;
}

void w16_register_system_classes(void)
{
    HCURSOR arrow = LoadCursor(NULL, IDC_ARROW), ibeam = LoadCursor(NULL, IDC_IBEAM);
    sysclass("BUTTON", w16_button_proc, CS_DBLCLKS | CS_PARENTDC, arrow, 0);
    sysclass("EDIT", w16_edit_proc, CS_DBLCLKS, ibeam, 0);
    sysclass("STATIC", w16_static_proc, CS_PARENTDC, arrow, 0);
    sysclass("LISTBOX", w16_listbox_proc, CS_DBLCLKS | CS_PARENTDC, arrow, 0);
    sysclass("COMBOBOX", w16_combobox_proc, CS_DBLCLKS | CS_PARENTDC, arrow, 0);
    sysclass("COMBOLBOX", w16_combolbox_proc, CS_DBLCLKS | CS_SAVEBITS, arrow, 0);
    sysclass("SCROLLBAR", w16_scrollbar_proc, CS_DBLCLKS | CS_PARENTDC, arrow, 0);
    /* USER seg3:1547 registers the dialog class with style 0x2808 */
    sysclass("#32770", w16_dialog_wndproc, CS_DBLCLKS | CS_SAVEBITS | CS_BYTEALIGNWINDOW, arrow, 30);
    sysclass("#32769", w16_desktop_proc, 0, arrow, 0);
    /* USER seg3:1595: "MDIClient" (USER string 22), style 0, 16 extra bytes, the arrow, the
     * application workspace colour; the icon title class (atom 8004) has style 0 and no extra bytes */
    sysclass("MDIClient", w16_mdiclient_proc, 0, arrow, 16);
    w16_find_class("MDIClient", NULL)->wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_APPWORKSPACE + 1);
    sysclass("#32772", w16_icon_title_proc, 0, arrow, 0);
}

/* ------------------------------------------------------------------ helpers */
HWND w16_top_level(HWND h)
{
    while (h && h->parent && h->parent != w16_desktop) h = h->parent;
    return h;
}

int w16_window_visible(HWND h)
{
    for (; h && h != w16_desktop; h = h->parent)
        if (!(h->style & WS_VISIBLE)) return 0;
    return h != NULL;
}

static void link_top(HWND h)
{
    HWND p = h->parent;
    h->next = p->child;
    p->child = h;
}

static void unlink_w(HWND h)
{
    HWND p = h->parent;
    if (!p) return;
    for (HWND *q = &p->child; *q; q = &(*q)->next)
        if (*q == h) { *q = h->next; break; }
    h->next = NULL;
}

static void link_after(HWND h, HWND after)
{
    if (!after) { link_top(h); return; }
    if (after == HWND_BOTTOM) {
        HWND *q = &h->parent->child;
        while (*q) q = &(*q)->next;
        *q = h;
        h->next = NULL;
        return;
    }
    h->next = after->next;
    after->next = h;
}

/* bring h to the top of its siblings; owned top-level windows stay above their owner */
static void raise_w(HWND h)
{
    if (!h->parent) return;
    unlink_w(h);
    link_top(h);
    if (h->parent == w16_desktop) {
        /* keep owned popups above (collect, then relink on top in their relative order) */
        HWND owned[64];
        int n = 0;
        for (HWND c = h->parent->child; c && n < 64; c = c->next)
            if (c != h && c->owner == h) owned[n++] = c;
        for (int i = n - 1; i >= 0; i--) { unlink_w(owned[i]); link_top(owned[i]); }
    }
}

/* ------------------------------------------------------------------ visible regions */
/* USER's IsVisible (internal): the children of a minimised window are not shown - an MDI child's icon
 * shows its class icon, not its edit window (measured: SysEdit's children minimised on real 3.11) */
static int in_icon(HWND h)
{
    for (HWND p = h->parent; p && p != w16_desktop; p = p->parent)
        if (p->style & WS_MINIMIZE) return 1;
    return 0;
}

void w16_calc_visrgn(HWND h, int window, int clipchildren, Region *out)
{
    rgn_clear(out);
    if (!w16_window_visible(h) || in_icon(h)) return;
    rgn_set(out, window ? &h->rw : &h->rc);
    rgn_and(out, &(RECT){0, 0, w16_screen.w, w16_screen.h});
    for (HWND p = h->parent; p && p != w16_desktop; p = p->parent) rgn_and(out, &p->rc);
    for (HWND w = h; w && w != w16_desktop; w = w->parent) {
        int top = w->parent == w16_desktop;
        if (!top && !(w->style & WS_CLIPSIBLINGS)) continue;
        for (HWND s = w->parent->child; s && s != w; s = s->next)
            if (s->style & WS_VISIBLE) rgn_sub(out, &s->rw);
    }
    if (clipchildren)
        for (HWND c = h->child; c; c = c->next)
            if (c->style & WS_VISIBLE) rgn_sub(out, &c->rw);
}

/* ------------------------------------------------------------------ invalidation */
void w16_invalidate_window(HWND h, const RECT *sr, int erase, int nc)
{
    if (!w16_valid(h) || !w16_window_visible(h) || h->redraw_off || in_icon(h)) return;
    RECT r = sr ? *sr : h->rw, t;
    if (!IntersectRect(&t, &r, &h->rw)) return;
    /* parts outside the client area need WM_NCPAINT */
    if (nc && !(t.left >= h->rc.left && t.right <= h->rc.right && t.top >= h->rc.top && t.bottom <= h->rc.bottom))
        h->need_ncpaint = 1;
    RECT c;
    if (IntersectRect(&c, &t, &h->rc)) {
        rgn_add(&h->upd, &c);
        if (erase) h->need_erase = 1;
    }
    /* children inside the invalid area repaint too */
    for (HWND ch = h->child; ch; ch = ch->next)
        if ((ch->style & WS_VISIBLE)) {
            RECT cr;
            if (IntersectRect(&cr, &c, &ch->rw)) w16_invalidate_window(ch, &cr, erase, 1);
        }
}

void w16_invalidate_screen_rect(const RECT *r)
{
    if (!w16_desktop) return;
    RECT t;
    if (IntersectRect(&t, r, &w16_desktop->rw)) {
        rgn_add(&w16_desktop->upd, &t);
        w16_desktop->need_erase = 1;
    }
    for (HWND c = w16_desktop->child; c; c = c->next)
        if (c->style & WS_VISIBLE) w16_invalidate_window(c, r, 1, 1);
}

void InvalidateRect(HWND h, LPCRECT r, BOOL erase)
{
    if (!h) { w16_invalidate_screen_rect(&w16_desktop->rw); return; }
    if (!w16_valid(h)) return;
    RECT s;
    if (r) { s = *r; OffsetRect(&s, h->rc.left, h->rc.top); IntersectRect(&s, &s, &h->rc); }
    else s = h->rc;
    if (IsRectEmpty(&s)) return;
    w16_invalidate_window(h, &s, erase, 0);
}

void InvalidateRgn(HWND h, HRGN rgn, BOOL erase)
{
    if (!rgn) { InvalidateRect(h, NULL, erase); return; }
    for (int i = 0; i < rgn->u.rgn.n; i++) InvalidateRect(h, &rgn->u.rgn.r[i], erase);
}

void ValidateRect(HWND h, LPCRECT r)
{
    if (!w16_valid(h)) return;
    if (!r) { rgn_clear(&h->upd); h->need_erase = 0; return; }
    RECT s = *r;
    OffsetRect(&s, h->rc.left, h->rc.top);
    rgn_sub(&h->upd, &s);
}

BOOL GetUpdateRect(HWND h, LPRECT r, BOOL erase)
{
    if (!w16_valid(h)) return FALSE;
    RECT b;
    rgn_bounds(&h->upd, &b);
    if (!IsRectEmpty(&b)) OffsetRect(&b, -h->rc.left, -h->rc.top);
    if (r) *r = b;
    if (erase && h->need_erase && !rgn_empty(&h->upd)) {
        HDC dc = GetDC(h);
        if (SendMessage(h, WM_ERASEBKGND, (WPARAM)dc, 0)) h->need_erase = 0;
        ReleaseDC(h, dc);
    }
    return !IsRectEmpty(&b);
}

static int needs_paint(HWND h)
{
    return w16_window_visible(h) && (!rgn_empty(&h->upd) || h->need_ncpaint || h->internal_paint);
}

static HWND find_paint(HWND h)
{
    for (HWND c = h; c; c = c->next) {
        if (!(c->style & WS_VISIBLE)) continue;
        if (needs_paint(c)) return c;
        if (c->style & WS_MINIMIZE) continue; /* its children are not shown */
        HWND k = find_paint(c->child);
        if (k) return k;
    }
    return NULL;
}

HWND w16_next_to_paint(HWND root)
{
    (void)root;
    if (needs_paint(w16_desktop)) return w16_desktop;
    /* paint bottom-most top-level windows first is not required: regions do not overlap */
    return find_paint(w16_desktop->child);
}

int w16_any_paint_pending(void) { return w16_next_to_paint(NULL) != NULL; }

/* A minimized window whose class has an icon gets WM_PAINTICON instead of WM_PAINT: USER draws the
 * class icon itself (DefWindowProc), so a program that paints its client area in WM_PAINT (WINMINE)
 * still shows its icon. Without a class icon the program paints the icon in WM_PAINT. */
UINT w16_paint_msg(HWND h) { return IsIconic(h) && h->cls && h->cls->wc.hIcon ? WM_PAINTICON : WM_PAINT; }

void w16_send_paint_cascade(HWND h)
{
    if (!w16_valid(h)) return;
    if (needs_paint(h)) {
        if (!rgn_empty(&h->upd) || h->internal_paint) SendMessage(h, w16_paint_msg(h), 0, 0);
        else if (h->need_ncpaint) {
            h->need_ncpaint = 0;
            SendMessage(h, WM_NCPAINT, 1, 0);
        }
        if (w16_valid(h) && needs_paint(h) && rgn_empty(&h->upd)) h->need_ncpaint = 0;
    }
    if (w16_valid(h) && !(h->style & WS_MINIMIZE))
        for (HWND c = h->child; c; c = c->next) w16_send_paint_cascade(c);
}

BOOL UpdateWindow(HWND h)
{
    if (!w16_valid(h)) return FALSE;
    w16_send_paint_cascade(h);
    return TRUE;
}

/* ------------------------------------------------------------------ painting */
HDC BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    memset(ps, 0, sizeof *ps);
    if (!w16_valid(h)) return NULL;
    w16_caret_hide_for_paint(h);
    if (h->need_ncpaint) {
        h->need_ncpaint = 0;
        SendMessage(h, WM_NCPAINT, 1, 0);
    }
    HDC dc = w16_make_dc(h, 0);
    Region upd;
    rgn_init(&upd);
    rgn_copy(&upd, &h->upd);
    rgn_and(&upd, &h->rc);
    rgn_and_rgn(&dc->vis, &upd);
    /* Without WS_CLIPCHILDREN the parent paints (and erases) over its children, so as in
     * RedrawWindow's default the children under the update region repaint after it - even
     * one that an earlier UpdateWindow had already validated. */
    if (!(h->style & WS_CLIPCHILDREN))
        for (HWND c = h->child; c; c = c->next) {
            if (!(c->style & WS_VISIBLE)) continue;
            for (int i = 0; i < upd.n; i++) {
                RECT s;
                if (IntersectRect(&s, &upd.r[i], &c->rw)) w16_invalidate_window(c, &s, TRUE, 1);
            }
        }
    RECT b;
    rgn_bounds(&upd, &b);
    if (!IsRectEmpty(&b)) OffsetRect(&b, -h->rc.left, -h->rc.top);
    ps->rcPaint = b;
    rgn_free(&upd);
    rgn_clear(&h->upd);
    h->internal_paint = 0;
    ps->hdc = dc;
    if (h->need_erase) {
        h->need_erase = 0;
        /* 3.1 SDK: a minimized window gets WM_ICONERASEBKGND only if its class has an icon (the icon
         * USER draws for it, WM_PAINTICON); otherwise WM_ERASEBKGND. UNTESTED against real 3.11 with a
         * desktop colour other than the class brush. */
        if (IsIconic(h) && h->cls->wc.hIcon) ps->fErase = !SendMessage(h, WM_ICONERASEBKGND, (WPARAM)dc, 0);
        else ps->fErase = !SendMessage(h, WM_ERASEBKGND, (WPARAM)dc, 0);
    }
    return dc;
}

void EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    if (ps->hdc) ReleaseDC(h, ps->hdc);
    w16_caret_restore_after_paint(h);
}

HDC GetDC(HWND h)
{
    if (h && !w16_valid(h)) return NULL;
    if (!h) return w16_make_dc(NULL, 1);
    if (h->cls->wc.style & CS_PARENTDC && h->parent && h->parent != w16_desktop) {
        /* parent DC clipped to the parent client, origin at the child */
        HDC dc = w16_make_dc(h->parent, 0);
        dc->hwnd = h;
        dc->ox = h->rc.left;
        dc->oy = h->rc.top;
        Region own;
        rgn_init(&own);
        w16_calc_visrgn(h, 0, 0, &own);
        rgn_free(&dc->vis);
        dc->vis = own;
        return dc;
    }
    return w16_make_dc(h, 0);
}
HDC GetWindowDC(HWND h) { return w16_make_dc(h, 1); }
int ReleaseDC(HWND h, HDC dc) { (void)h; return DeleteDC(dc); }

/* ------------------------------------------------------------------ geometry */
void GetWindowRect(HWND h, LPRECT r)
{
    if (!h || h == w16_desktop) { SetRect(r, 0, 0, w16_screen.w, w16_screen.h); return; }
    if (w16_valid(h)) *r = h->rw;
}
void GetClientRect(HWND h, LPRECT r)
{
    if (!h || h == w16_desktop) { SetRect(r, 0, 0, w16_screen.w, w16_screen.h); return; }
    if (!w16_valid(h)) { SetRectEmpty(r); return; }
    SetRect(r, 0, 0, h->rc.right - h->rc.left, h->rc.bottom - h->rc.top);
}
void ClientToScreen(HWND h, LPPOINT p) { if (w16_valid(h)) { p->x += h->rc.left; p->y += h->rc.top; } }
void ScreenToClient(HWND h, LPPOINT p) { if (w16_valid(h)) { p->x -= h->rc.left; p->y -= h->rc.top; } }
void MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n)
{
    for (UINT i = 0; i < n; i++) {
        if (from && from != w16_desktop) ClientToScreen(from, &p[i]);
        if (to && to != w16_desktop) ScreenToClient(to, &p[i]);
    }
}

void AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex)
{
    /* inverse of w16_nc_calc for a window at r */
    struct W16Window fake;
    memset(&fake, 0, sizeof fake);
    fake.style = style;
    fake.exstyle = ex;
    RECT rw = {0, 0, 1000, 1000}, rc;
    fake.rw = rw;
    fake.parent = (style & WS_CHILD) ? (HWND)1 : w16_desktop;
    w16_nc_calc(&fake, &rw, &rc);
    r->left -= rc.left;
    r->top -= rc.top;
    r->right += 1000 - rc.right;
    r->bottom += 1000 - rc.bottom;
    if (menu) r->top -= GetSystemMetrics(SM_CYMENU) + 1;
}
void AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) { AdjustWindowRectEx(r, style, menu, 0); }

/* change rect, recompute client, move children, repaint */
void w16_set_window_rect(HWND h, const RECT *nr, UINT swp)
{
    RECT old = h->rw, oldc = h->rc;
    h->rw = *nr;
    RECT rc;
    w16_nc_calc(h, &h->rw, &rc);
    h->rc = rc;
    int dx = h->rc.left - oldc.left, dy = h->rc.top - oldc.top;
    if (dx || dy) {
        /* children are stored in screen coordinates: move the whole subtree */
        void move(HWND p, int ddx, int ddy);
        for (HWND c = h->child; c; c = c->next) move(c, dx, dy);
        rgn_offset(&h->upd, h->rw.left - old.left, h->rw.top - old.top);
    }
    if (!(swp & SWP_NOREDRAW) && w16_window_visible(h) && !EqualRect(&old, &h->rw)) {
        w16_invalidate_screen_rect(&old);
        int cls = h->cls->wc.style;
        int full = (cls & (CS_HREDRAW | CS_VREDRAW)) || dx || dy || 1;
        (void)full;
        w16_invalidate_window(h, &h->rw, 1, 1);
    }
}

void move(HWND p, int dx, int dy)
{
    OffsetRect(&p->rw, dx, dy);
    OffsetRect(&p->rc, dx, dy);
    rgn_offset(&p->upd, dx, dy);
    for (HWND c = p->child; c; c = c->next) move(c, dx, dy);
}

/* USER seg6:1A4F: the MINMAXINFO of a window - USER's defaults (seg6:18E0), the window's
 * WM_GETMINMAXINFO, then USER's corrections (seg6:19A8). The icon window is 4 borders around the icon
 * (seg3:242F); a window without a sizing frame maximizes 2 borders over each screen edge; tracking
 * goes up to the screen plus a sizing frame each side. UNTESTED against real 3.11: the corrections
 * (they only matter when WM_GETMINMAXINFO lowers the minimum tracking size; USER's caption button
 * width there, half of OBM_CLOSE, is SM_CXSIZE here), and USER takes ptMaxPosition from the
 * window's checkpoint once it has one, which libw16 does not keep. */
void w16_get_minmax_info(HWND h, MINMAXINFO *mm)
{
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    int cxf = GetSystemMetrics(SM_CXFRAME), cyf = GetSystemMetrics(SM_CYFRAME);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    POINT icon = {GetSystemMetrics(SM_CXICON) + 4 * cxb, GetSystemMetrics(SM_CYICON) + 4 * cyb};
    DWORD st = h->style;
    mm->ptReserved = icon;
    if (st & WS_THICKFRAME) {
        mm->ptMaxSize = (POINT){sw + 2 * cxf, sh + 2 * cyf};
        mm->ptMaxPosition = (POINT){-cxf, -cyf};
    } else {
        mm->ptMaxSize = (POINT){sw + 4 * cxb, sh + 4 * cyb};
        mm->ptMaxPosition = (POINT){-cxb, -cyb};
    }
    if (st & (WS_BORDER | WS_DLGFRAME))
        mm->ptMinTrackSize = (POINT){GetSystemMetrics(SM_CXMINTRACK), GetSystemMetrics(SM_CYMINTRACK)};
    else
        mm->ptMinTrackSize = (POINT){cxb, cyb};
    mm->ptMaxTrackSize = (POINT){sw + 2 * cxf, sh + 2 * cyf};
    SendMessage(h, WM_GETMINMAXINFO, 0, (LPARAM)mm);
    if (st & WS_MINIMIZEBOX) mm->ptReserved = icon;
    if ((st & WS_CAPTION) == WS_CAPTION) {
        /* room for the caption's boxes and the sizing frame */
        int n = !!(st & WS_SYSMENU) + !!(st & WS_MAXIMIZEBOX) + !!(st & WS_MINIMIZEBOX);
        mm->ptMinTrackSize.x = max(mm->ptMinTrackSize.x, n * GetSystemMetrics(SM_CXSIZE) + 2 * cxf);
        mm->ptMinTrackSize.y = max(mm->ptMinTrackSize.y, GetSystemMetrics(SM_CYMINTRACK));
    } else {
        int dx = (st & WS_THICKFRAME) ? cxf : cxb, dy = (st & WS_THICKFRAME) ? cyf : cyb;
        mm->ptMinTrackSize.x = max(mm->ptMinTrackSize.x, 2 * dx);
        mm->ptMinTrackSize.y = max(mm->ptMinTrackSize.y, 2 * dy);
    }
}

/* USER seg1:0000: a new window size kept within the window's MINMAXINFO - for an overlapped window or
 * one with a sizing frame; an icon between ptReserved and ptMaxSize, any other window between the
 * tracking sizes. CreateWindow (seg8:06D9, before WM_NCCREATE and with WS_MINIMIZE still in the style)
 * and DefWindowProc's WM_WINDOWPOSCHANGING (seg1:609A) use it. Measured on real 3.11: WINMINE's
 * 30 x 24 board asks for a window 491 high and, created minimized, gets 484 (480 + 4 borders). */
void w16_clamp_window_size(HWND h, int *cx, int *cy)
{
    if ((h->style & (WS_POPUP | WS_CHILD)) && !(h->style & WS_THICKFRAME)) return;
    MINMAXINFO mm;
    w16_get_minmax_info(h, &mm);
    POINT lo = mm.ptMinTrackSize, hi = mm.ptMaxTrackSize;
    if (h->style & WS_MINIMIZE) lo = mm.ptReserved, hi = mm.ptMaxSize;
    *cx = max(min(*cx, hi.x), lo.x);
    *cy = max(min(*cy, hi.y), lo.y);
}

BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    if (!w16_valid(h)) return FALSE;
    WINDOWPOS wp = {h, after, x, y, cx, cy, fl};
    RECT oldrc = h->rc, pr = {0, 0, 0, 0};
    if (h->parent && h->parent != w16_desktop) pr = h->parent->rc;
    if (fl & SWP_NOMOVE) { wp.x = h->rw.left - pr.left; wp.y = h->rw.top - pr.top; }
    if (fl & SWP_NOSIZE) { wp.cx = h->rw.right - h->rw.left; wp.cy = h->rw.bottom - h->rw.top; }
    SendMessage(h, WM_WINDOWPOSCHANGING, 0, (LPARAM)&wp);
    RECT nr = {pr.left + wp.x, pr.top + wp.y, pr.left + wp.x + wp.cx, pr.top + wp.y + wp.cy};
    int moved = nr.left != h->rw.left || nr.top != h->rw.top;
    int sized = (nr.right - nr.left) != (h->rw.right - h->rw.left) || (nr.bottom - nr.top) != (h->rw.bottom - h->rw.top);
    if (!(fl & SWP_NOZORDER) && h->parent) {
        /* the siblings above h before the move: those that end up above it but were below it get the
         * parts of h they now cover repainted (a window sent down the z-order, as MDI's Next does) */
        HWND above[64];
        int na = 0;
        for (HWND s = h->parent->child; s && s != h && na < 64; s = s->next) above[na++] = s;
        if (after == HWND_TOP || after == HWND_TOPMOST || after == NULL) raise_w(h);
        else if (after == HWND_BOTTOM) { unlink_w(h); link_after(h, HWND_BOTTOM); }
        else if (w16_valid(after) && after->parent == h->parent && after != h) { unlink_w(h); link_after(h, after); }
        if (w16_window_visible(h)) {
            w16_invalidate_window(h, NULL, 1, 1);
            for (HWND s = h->parent->child; s && s != h; s = s->next) {
                int was_above = 0;
                for (int i = 0; i < na; i++) was_above |= above[i] == s;
                if (!was_above && (s->style & WS_VISIBLE)) w16_invalidate_window(s, &h->rw, 1, 1);
            }
        }
    }
    if (fl & SWP_HIDEWINDOW) {
        if (h->style & WS_VISIBLE) {
            if (IsIconic(h)) w16_invalidate_icon_title(h);
            h->style &= ~WS_VISIBLE;
            w16_invalidate_screen_rect(&h->rw);
            if (h->parent && h->parent != w16_desktop) w16_invalidate_window(h->parent, &h->rw, 1, 1);
        }
    }
    if (moved || sized || (fl & SWP_FRAMECHANGED)) w16_set_window_rect(h, &nr, fl);
    if (fl & SWP_SHOWWINDOW) {
        if (!(h->style & WS_VISIBLE)) {
            h->style |= WS_VISIBLE;
            w16_invalidate_window(h, NULL, 1, 1);
            if (IsIconic(h)) w16_invalidate_icon_title(h);
        }
    }
    if (fl & SWP_FRAMECHANGED) w16_invalidate_window(h, NULL, 1, 1);
    /* USER's internal SWP_NOCLIENTMOVE / SWP_NOCLIENTSIZE tell DefWindowProc whether the client area
     * moved or changed size, also when only the frame changed: SetMenu moves the client of a window
     * that stays put, and WINMINE's F6 (WM_MOVE updates its position) shows that on real 3.11 */
    int cmoved = h->rc.left != oldrc.left || h->rc.top != oldrc.top;
    int csized = (h->rc.right - h->rc.left) != (oldrc.right - oldrc.left) ||
                 (h->rc.bottom - h->rc.top) != (oldrc.bottom - oldrc.top);
    wp.flags = fl | (moved ? 0 : SWP_NOMOVE) | (sized ? 0 : SWP_NOSIZE) | (cmoved ? 0 : W16_SWP_NOCLIENTMOVE) |
               (csized ? 0 : W16_SWP_NOCLIENTSIZE);
    if (moved || sized || cmoved || csized) SendMessage(h, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp);
    if (!(fl & SWP_NOACTIVATE) && h->parent == w16_desktop && (h->style & WS_VISIBLE) && !(fl & SWP_HIDEWINDOW))
        w16_activate(h, WA_ACTIVE);
    else if (!(fl & (SWP_NOACTIVATE | SWP_HIDEWINDOW)) && (h->style & WS_CHILD) && w16_valid(h))
        SendMessage(h, WM_CHILDACTIVATE, 0, 0); /* USER seg7:0382: a child window is told instead */
    return TRUE;
}

/* ------------------------------------------------------------------ DeferWindowPos */
/* USER seg7:00B5 / 0114 / 01FD. The positions are applied one after the other in the order they were
 * deferred; 3.1 computes them all first and repaints once (only the order of the repaints differs) */
struct W16Dwp { int n, cap; WINDOWPOS *p; };

HDWP BeginDeferWindowPos(int n)
{
    HDWP d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cap = n > 0 ? n : 4;
    d->p = calloc(d->cap, sizeof *d->p);
    if (!d->p) { free(d); return NULL; }
    return d;
}

HDWP DeferWindowPos(HDWP d, HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    if (!d) return NULL;
    if (!w16_valid(h)) return d;
    if (d->n == d->cap) {
        WINDOWPOS *np = realloc(d->p, sizeof *np * d->cap * 2);
        if (!np) return d;
        d->p = np;
        d->cap *= 2;
    }
    d->p[d->n++] = (WINDOWPOS){h, after, x, y, cx, cy, fl};
    return d;
}

BOOL EndDeferWindowPos(HDWP d)
{
    if (!d) return FALSE;
    for (int i = 0; i < d->n; i++) {
        WINDOWPOS *w = &d->p[i];
        if (w16_valid(w->hwnd)) SetWindowPos(w->hwnd, w->hwndInsertAfter, w->x, w->y, w->cx, w->cy, w->flags);
    }
    free(d->p);
    free(d);
    return TRUE;
}

BOOL MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint)
{
    return SetWindowPos(h, NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));
}
BOOL BringWindowToTop(HWND h) { return SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); }

/* ------------------------------------------------------------------ activation & focus */
void w16_activate(HWND h, int how)
{
    if (h && !w16_valid(h)) return;
    if (h) h = w16_top_level(h);
    if (h == w16_active) {
        if (h && h->parent) {
            if (h->parent->child != h) {
                raise_w(h);
                w16_invalidate_window(h, NULL, 1, 1);
            }
        }
        return;
    }
    HWND old = w16_active;
    if (w16_valid(old)) {
        SendMessage(old, WM_NCACTIVATE, FALSE, 0);
        SendMessage(old, WM_ACTIVATE, MAKELONG(WA_INACTIVE, IsIconic(old)), (LPARAM)h);
    }
    w16_active = h;
    if (!h) return;
    if (h->parent && h->parent->child != h) {
        raise_w(h);
        w16_invalidate_window(h, NULL, 1, 1);
    }
    if (!old) SendMessage(h, WM_ACTIVATEAPP, TRUE, 0);
    SendMessage(h, WM_NCACTIVATE, TRUE, 0);
    SendMessage(h, WM_ACTIVATE, MAKELONG(how, IsIconic(h)), (LPARAM)old);
}

HWND GetActiveWindow(void) { return w16_active; }

/* USER's WFFRAMEON: the caption shows the window active. A top-level window's follows the activation;
 * a child's (an MDI child) is what its last WM_NCACTIVATE set */
int w16_caption_active(HWND h)
{
    if (!w16_valid(h)) return 0;
    if (h->style & WS_CHILD) return h->active_frame;
    return h->parent == w16_desktop && h == w16_active;
}

WORD w16_hwnd16(HWND h) { WORD w16_cmd_slot(HWND h); return w16_valid(h) ? w16_cmd_slot(h) : 0; }
HWND SetActiveWindow(HWND h)
{
    HWND o = w16_active;
    w16_activate(h, WA_ACTIVE);
    return o;
}
HWND GetFocus(void) { return w16_focus; }
HWND SetFocus(HWND h)
{
    HWND old = w16_focus;
    if (h && !w16_valid(h)) return old;
    if (h == old) return old;
    if (h) {
        for (HWND p = h; p && p != w16_desktop; p = p->parent)
            if ((p->style & WS_DISABLED) && p != h) return old;
        HWND top = w16_top_level(h);
        if (top != w16_active) {
            /* USER seg1:381D: the top-level window is activated first, then the focus goes to h
             * whatever the activation did with it (a message box's WM_INITDIALOG focuses its
             * default button while the activation would pick the first one) */
            w16_activate(top, WA_ACTIVE);
            if (!w16_valid(h)) return old;
            old = w16_focus;
            if (h == old) return old;
        }
    }
    w16_focus = h;
    if (w16_valid(old)) SendMessage(old, WM_KILLFOCUS, (WPARAM)h, 0);
    if (w16_valid(h) && w16_focus == h) SendMessage(h, WM_SETFOCUS, (WPARAM)old, 0);
    return old;
}
HWND GetCapture(void) { return w16_capture; }
HWND SetCapture(HWND h) { HWND o = w16_capture; w16_capture = h; return o; }
void ReleaseCapture(void) { w16_capture = NULL; }

/* ------------------------------------------------------------------ creation */
/* Height kept free for a row of icons at the bottom of the screen (USER [0xc2], IconTitleWrap=1):
 * cyIcon/4 + (cyIcon + 4) + 2 * (icon title font height + cyBorder). The icon title font is
 * MS Sans Serif 8 (13 px); 72 on VGA, as measured on real 3.11. */
static int icon_area_height(void)
{
    int cyi = GetSystemMetrics(SM_CYICON);
    return cyi / 4 + cyi + 4 + 2 * (13 + GetSystemMetrics(SM_CYBORDER));
}

/* USER seg6:01B4 (GetDefaultPosition): the next cascade slot. Steps are SM_CXSIZE + cxFrame across
 * and cyCaption - cyBorder + cyBorder * BorderWidth down; the counter wraps once a slot would start
 * past a third of the screen. CS_BYTEALIGNCLIENT rounds x so the client area starts on a byte. */
static void default_rect(HWND h, RECT *r)
{
    int sw = w16_screen.w, sh = w16_screen.h, bw = w16_border_width;
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    int cxframe = cxb * (bw + 1);
    int x = (GetSystemMetrics(SM_CXSIZE) + cxframe) * default_pos_count;
    int y = (GetSystemMetrics(SM_CYCAPTION) - cyb + cyb * bw) * default_pos_count;
    if (sw / 3 < x || sh / 3 < y) {
        default_pos_count = 0;
        x = y = 0;
    }
    if (h->cls->wc.style & CS_BYTEALIGNCLIENT) x = ((cxframe + x + 7) & ~7) - cxframe;
    int slack = ((cxframe + 7) & ~7) - cxframe; /* [0x51c] */
    SetRect(r, x, y, sw - slack, sh - icon_area_height());
    default_pos_count++;
}

/* USER seg13:0E34, which CreateWindow (seg8:06FC) runs on every new window rectangle: on displays
 * under 8 bits per pixel (libw16 shows the 16-colour VGA), CS_BYTEALIGNWINDOW moves the window and
 * CS_BYTEALIGNCLIENT its client area to the nearest byte (8 pixels) across. */
static int byte_align_dx(HWND h)
{
    WORD cs = h->cls ? h->cls->wc.style : 0;
    int x = h->rw.left;
    if (cs & CS_BYTEALIGNWINDOW) return ((x + 4) & ~7) - x;
    if (!(cs & CS_BYTEALIGNCLIENT)) return 0;
    int frame = 0;
    DWORD cap = h->style & WS_CAPTION;
    if (h->style & WS_THICKFRAME) frame = w16_border_width + 1;
    else if (cap == WS_CAPTION || cap == WS_BORDER) frame = 1;
    if (cap == WS_DLGFRAME || (h->exstyle & WS_EX_DLGMODALFRAME)) frame = 5;
    x += frame * GetSystemMetrics(SM_CXBORDER);
    return ((x + 4) & ~7) - x;
}

/* The overlapped-window part of CreateWindow (USER, after "test [si+2Bh],0C0h"): runs for every
 * top-level overlapped window; one with an explicit position hands its cascade slot back. */
static void default_position(HWND h, int *x, int *y, int *cx, int *cy, int usedef_pos, int usedef_size)
{
    RECT d;
    default_rect(h, &d);
    if (usedef_pos) {
        *x = d.left;
        *y = d.top;
    } else if (default_pos_count)
        default_pos_count--;
    if (usedef_size) {
        *cx = d.right - *x;
        *cy = d.bottom - *y;
    } else if (usedef_pos) {
        /* default position, explicit size: slide back onto the screen */
        int over = *cx - w16_screen.w + *x;
        if (over > 0 && (*x -= over) < 0) *x = 0;
        over = *cy - w16_screen.h + *y;
        if (over > 0 && (*y -= over) < 0) *y = 0;
    }
}

HWND CreateWindowEx(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy,
                    HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    W16Class *c = w16_find_class(cls, inst);
    if (!c) {
        W16_LOG("CreateWindow: unknown class %s\n", IS_INTRESOURCE(cls) ? "#atom" : cls);
        return NULL;
    }
    HWND h = calloc(1, sizeof *h);
    h->magic = W16_WND_MAGIC;
    h->cls = c;
    h->proc = c->wc.lpfnWndProc;
    h->style = style;
    h->exstyle = ex;
    h->inst = inst ? inst : c->wc.hInstance;
    h->cbextra = c->wc.cbWndExtra;
    h->extra = calloc(1, h->cbextra + 64);
    rgn_init(&h->upd);
    h->text = strdup(title && !IS_INTRESOURCE(title) ? title : "");
    h->sb[0].max = h->sb[1].max = 100;
    if (style & WS_CHILD) {
        if (!w16_valid(parent)) { free(h); return NULL; }
        h->parent = parent;
        h->id = (UINT)(uintptr_t)menu;
    } else {
        h->parent = w16_desktop;
        h->owner = parent && w16_valid(parent) ? w16_top_level(parent) : NULL;
        if (!(style & WS_POPUP)) h->style |= WS_CLIPSIBLINGS | WS_CAPTION; /* overlapped always has a caption */
        h->menu = menu;
        if (!menu && c->wc.lpszMenuName) h->menu = LoadMenu(h->inst, c->wc.lpszMenuName);
    }
    int udp = x == CW_USEDEFAULT, uds = cx == CW_USEDEFAULT;
    if (!(style & (WS_CHILD | WS_POPUP)))
        default_position(h, &x, &y, &cx, &cy, udp, uds);
    else {
        if (udp) x = y = 0;
        if (uds) cx = cy = 0;
    }
    RECT pr = {0, 0, 0, 0};
    if (h->parent != w16_desktop) pr = h->parent->rc;
    /* the size within the window's limits: WM_GETMINMAXINFO comes before WM_NCCREATE (seg8:06D9, into
     * locals; the CREATESTRUCT USER passes is its own parameter frame) */
    int wcx = cx, wcy = cy;
    w16_clamp_window_size(h, &wcx, &wcy);
    h->rw = (RECT){pr.left + x, pr.top + y, pr.left + x + wcx, pr.top + y + wcy};
    if (h->parent == w16_desktop) OffsetRect(&h->rw, byte_align_dx(h), 0);
    h->restore = h->rw;
    OffsetRect(&h->restore, -pr.left, -pr.top);
    h->style &= ~(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE);
    link_top(h);
    if (h->parent != w16_desktop) {
        /* children go to the bottom of the z-order (creation order = tab order) */
        unlink_w(h);
        link_after(h, HWND_BOTTOM);
    }
    w16_nc_calc(h, &h->rw, &h->rc);
    CREATESTRUCT cs = {param, h->inst, menu, parent, cy, cx, y, x, (LONG)style, title, cls, ex};
    if (!SendMessage(h, WM_NCCREATE, 0, (LPARAM)&cs)) { DestroyWindow(h); return NULL; }
    w16_nc_calc(h, &h->rw, &h->rc);
    if (SendMessage(h, WM_CREATE, 0, (LPARAM)&cs) == -1) { DestroyWindow(h); return NULL; }
    if (!w16_valid(h)) return NULL;
    if (style & WS_VISIBLE) {
        SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
        SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left - pr.left, h->rc.top - pr.top));
    } else
        h->send_sizemove = 1; /* see ShowWindow */
    if (style & WS_CHILD) {
        /* USER seg8:0843: a child created minimised or maximised gets that state hidden
         * (MinMaximize SW_SHOWMINNOACTIVE / SW_SHOWMAXIMIZED, fKeepHidden), then the parent hears
         * WM_PARENTNOTIFY (seg8:0866), then WS_VISIBLE shows it without activating it */
        if (style & WS_MINIMIZE) w16_min_maximize(h, SW_SHOWMINNOACTIVE, 1);
        else if (style & WS_MAXIMIZE) w16_min_maximize(h, SW_SHOWMAXIMIZED, 1);
        if (!w16_valid(h)) return NULL;
        if (!(ex & WS_EX_NOPARENTNOTIFY)) SendMessage(h->parent, WM_PARENTNOTIFY, WM_CREATE, (LPARAM)h);
        if (!w16_valid(h)) return NULL;
        if (style & WS_VISIBLE) ShowWindow(h, SW_SHOW);
        return h;
    }
    /* USER CreateWindow (seg8:0843): WS_MINIMIZE / WS_MAXIMIZE go through MinMaximize
     * (SW_SHOWMINNOACTIVE / SW_SHOWMAXIMIZED) with the window kept hidden; only WS_VISIBLE shows it.
     * WINMINE creates its window minimized and invisible and restores it with ShowWindow later. */
    if (style & WS_MINIMIZE) w16_minimize(h);
    else if (style & WS_MAXIMIZE) w16_maximize(h);
    if (style & WS_VISIBLE)
        ShowWindow(h, (style & WS_MINIMIZE) ? SW_SHOWMINNOACTIVE : (style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : SW_SHOW);
    return h;
}

HWND CreateWindow(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy, HWND parent,
                  HMENU menu, HINSTANCE inst, void *param)
{
    return CreateWindowEx(0, cls, title, style, x, y, cx, cy, parent, menu, inst, param);
}

static void destroy_rec(HWND h)
{
    w16_clipboard_on_destroy(h);
    SendMessage(h, WM_DESTROY, 0, 0);
    for (HWND c = h->child; c;) {
        HWND n = c->next;
        if (w16_valid(c)) destroy_rec(c);
        c = n;
    }
}

static void free_rec(HWND h)
{
    for (HWND c = h->child; c;) {
        HWND n = c->next;
        free_rec(c);
        c = n;
    }
    SendMessage(h, WM_NCDESTROY, 0, 0);
    w16_clipboard_on_free(h);
    w16_timers_on_destroy(h);
    w16_caret_on_destroy(h);
    if (w16_focus == h) w16_focus = NULL;
    if (w16_capture == h) w16_capture = NULL;
    if (w16_active == h) w16_active = NULL;
    h->destroyed = 1;
    h->magic = 0xDEADBEEF;
    rgn_free(&h->upd);
    /* leak the struct itself on purpose: stale handles in queued messages stay safe */
}

void w16_destroy_children(HWND h)
{
    for (HWND c = h->child; c;) {
        HWND n = c->next;
        DestroyWindow(c);
        c = n;
    }
}

BOOL DestroyWindow(HWND h)
{
    if (!w16_valid(h) || h->in_destroy) return FALSE;
    h->in_destroy = 1;
    int was_active = (w16_active == h);
    /* destroy owned windows first */
    if (h->parent == w16_desktop)
        for (HWND c = w16_desktop->child; c;) {
            HWND n = c->next;
            if (c->owner == h) DestroyWindow(c);
            c = n;
        }
    /* a minimised child window's icon title goes with its CHECKPOINT */
    if (w16_valid(h->icon_title)) { HWND t = h->icon_title; h->icon_title = NULL; DestroyWindow(t); }
    if (h->style & WS_VISIBLE) {
        if (IsIconic(h)) w16_invalidate_icon_title(h);
        h->style &= ~WS_VISIBLE;
        if (h->parent == w16_desktop) w16_invalidate_screen_rect(&h->rw);
        else w16_invalidate_window(h->parent, &h->rw, 1, 1);
    }
    if ((h->style & WS_CHILD) && !(h->exstyle & WS_EX_NOPARENTNOTIFY) && w16_valid(h->parent))
        SendMessage(h->parent, WM_PARENTNOTIFY, WM_DESTROY, (LPARAM)h);
    if (w16_focus && (w16_focus == h || IsChild(h, w16_focus))) w16_focus = NULL;
    destroy_rec(h);
    unlink_w(h);
    HWND owner = h->owner;
    free_rec(h);
    if (was_active) {
        w16_active = NULL;
        HWND next = (w16_valid(owner) && (owner->style & WS_VISIBLE)) ? owner : NULL;
        if (!next)
            for (HWND c = w16_desktop->child; c; c = c->next)
                if ((c->style & WS_VISIBLE) && !(c->style & WS_DISABLED)) { next = c; break; }
        if (next) {
            w16_activate(next, WA_ACTIVE);
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ show / state */
/* USER seg14:0BF2 ShowWindow for a child window: showing or hiding never activates it or changes its
 * place in the z-order (SWP_NOZORDER | SWP_NOACTIVATE); minimising, maximising and restoring go
 * through MinMaximize. Under a hidden parent only the WS_VISIBLE bit changes. */
static BOOL show_child(HWND h, int cmd)
{
    int was = (h->style & WS_VISIBLE) != 0;
    UINT swp = SWP_NOSIZE | SWP_NOMOVE;
    switch (cmd) {
    case SW_HIDE:
        if (!was) return was;
        swp |= SWP_HIDEWINDOW;
        break;
    case SW_SHOWNORMAL:
    case SW_SHOWNOACTIVATE:
    case SW_RESTORE:
        if (h->style & (WS_MINIMIZE | WS_MAXIMIZE)) { w16_min_maximize(h, cmd, 0); return was; }
        if (was) return was;
        swp |= SWP_SHOWWINDOW;
        break;
    case SW_SHOWMINIMIZED:
    case SW_SHOWMAXIMIZED:
    case SW_MINIMIZE:
    case SW_SHOWMINNOACTIVE:
        w16_min_maximize(h, cmd, 0);
        return was;
    case SW_SHOW:
        if (was) return was;
        swp |= SWP_SHOWWINDOW;
        break;
    default: /* SW_SHOWNA */
        swp |= SWP_SHOWWINDOW;
        break;
    }
    if ((cmd != SW_HIDE) != was) SendMessage(h, WM_SHOWWINDOW, cmd != SW_HIDE, 0);
    if (!w16_valid(h)) return was;
    if (w16_window_visible(h->parent))
        SetWindowPos(h, NULL, 0, 0, 0, 0, swp | SWP_NOZORDER | SWP_NOACTIVATE);
    else if (cmd == SW_HIDE)
        h->style &= ~WS_VISIBLE;
    else
        h->style |= WS_VISIBLE;
    if (!w16_valid(h)) return was;
    /* seg2:090A: the focus on the hidden window itself goes to its parent */
    if (cmd == SW_HIDE && w16_focus == h) SetFocus(h->parent);
    if (h->style & WS_MINIMIZE) w16_show_icon_title(h, cmd != SW_HIDE);
    return was;
}

static BOOL show_window(HWND h, int cmd)
{
    if (!w16_valid(h)) return FALSE;
    if (h->style & WS_CHILD) return show_child(h, cmd);
    int was = (h->style & WS_VISIBLE) != 0;
    switch (cmd) {
    case SW_HIDE:
        if (was) {
            SendMessage(h, WM_SHOWWINDOW, FALSE, 0);
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (h == w16_active) {
                /* activation goes to the owner (a dialog's parent dialog or window), else to the
                 * topmost other visible, enabled window - as in DestroyWindow */
                w16_active = NULL;
                HWND next = (w16_valid(h->owner) && (h->owner->style & WS_VISIBLE) && !(h->owner->style & WS_DISABLED)) ? h->owner : NULL;
                for (HWND c = w16_desktop->child; c && !next; c = c->next)
                    if ((c->style & WS_VISIBLE) && !(c->style & WS_DISABLED) && c != h) next = c;
                if (next) w16_activate(next, WA_ACTIVE);
            }
            if (w16_focus && (w16_focus == h || IsChild(h, w16_focus))) w16_focus = NULL;
        }
        return was;
    case SW_SHOWMINIMIZED:
    case SW_MINIMIZE:
    case SW_SHOWMINNOACTIVE:
        if (!(h->style & WS_VISIBLE)) { h->style |= WS_VISIBLE; SendMessage(h, WM_SHOWWINDOW, TRUE, 0); }
        w16_minimize(h);
        if (cmd == SW_SHOWMINIMIZED) w16_activate(h, WA_ACTIVE);
        else if (cmd == SW_MINIMIZE && h->parent == w16_desktop) {
            /* USER seg6:1D96: the first visible, enabled window that is not an icon becomes active,
             * else the icon itself (real 3.11's Clock started minimised shows an active title) */
            HWND next = h;
            for (HWND c = w16_desktop->child; c; c = c->next)
                if ((c->style & WS_VISIBLE) && !(c->style & WS_DISABLED) && c != h && !IsIconic(c)) { next = c; break; }
            w16_activate(next, WA_ACTIVE);
        }
        return was;
    case SW_SHOWMAXIMIZED:
        if (!(h->style & WS_VISIBLE)) { h->style |= WS_VISIBLE; SendMessage(h, WM_SHOWWINDOW, TRUE, 0); }
        w16_maximize(h);
        w16_activate(h, WA_ACTIVE);
        return was;
    case SW_RESTORE:
    case SW_SHOWNORMAL:
        if (h->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
            if (!(h->style & WS_VISIBLE)) { h->style |= WS_VISIBLE; SendMessage(h, WM_SHOWWINDOW, TRUE, 0); }
            w16_restore(h);
            if (h->parent == w16_desktop) w16_activate(h, WA_ACTIVE);
            return was;
        }
        /* fall through */
    case SW_SHOW:
    case SW_SHOWNA:
    case SW_SHOWNOACTIVATE:
    default:
        if (!was) {
            SendMessage(h, WM_SHOWWINDOW, TRUE, 0);
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         ((cmd == SW_SHOWNA || cmd == SW_SHOWNOACTIVATE || (h->style & WS_CHILD)) ? SWP_NOACTIVATE : 0));
        } else if (h->parent == w16_desktop && cmd != SW_SHOWNA && cmd != SW_SHOWNOACTIVATE)
            w16_activate(h, WA_ACTIVE);
        return was;
    }
}

BOOL ShowWindow(HWND h, int cmd)
{
    BOOL was = show_window(h, cmd);
    /* USER's WFSENDSIZEMOVE: a window created without WS_VISIBLE gets its first WM_SIZE and WM_MOVE
     * only now. Apps rely on it, e.g. Notepad creates its edit control after the main window and
     * positions it from WM_SIZE. */
    if (cmd != SW_HIDE && w16_valid(h) && h->send_sizemove) {
        h->send_sizemove = 0;
        RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
        SendMessage(h, WM_SIZE, IsZoomed(h) ? SIZE_MAXIMIZED : IsIconic(h) ? SIZE_MINIMIZED : SIZE_RESTORED,
                    MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
        SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left - pr.left, h->rc.top - pr.top));
    }
    return was;
}

BOOL IsWindowVisible(HWND h) { return w16_valid(h) && w16_window_visible(h); }
BOOL IsWindowEnabled(HWND h) { return w16_valid(h) && !(h->style & WS_DISABLED); }
BOOL IsIconic(HWND h) { return w16_valid(h) && (h->style & WS_MINIMIZE); }
BOOL IsZoomed(HWND h) { return w16_valid(h) && (h->style & WS_MAXIMIZE); }
BOOL IsChild(HWND p, HWND h)
{
    if (!w16_valid(h)) return FALSE;
    for (HWND q = h->parent; q && q != w16_desktop; q = q->parent)
        if (q == p) return TRUE;
    return FALSE;
}
BOOL EnableWindow(HWND h, BOOL en)
{
    if (!w16_valid(h)) return FALSE;
    int was = (h->style & WS_DISABLED) != 0;
    if (en && was) { h->style &= ~WS_DISABLED; SendMessage(h, WM_ENABLE, TRUE, 0); }
    else if (!en && !was) {
        h->style |= WS_DISABLED;
        if (w16_focus == h || IsChild(h, w16_focus)) SetFocus(NULL);
        if (w16_capture == h) w16_capture = NULL;
        SendMessage(h, WM_ENABLE, FALSE, 0);
    }
    return was;
}

/* ------------------------------------------------------------------ tree queries */
HWND GetParent(HWND h)
{
    if (!w16_valid(h)) return NULL;
    if (h->style & WS_CHILD) return h->parent;
    return h->owner;
}
HWND SetParent(HWND h, HWND np)
{
    if (!w16_valid(h)) return NULL;
    HWND old = h->parent;
    if (!np) np = w16_desktop;
    RECT r = h->rw;
    unlink_w(h);
    h->parent = np;
    link_top(h);
    int dx = 0, dy = 0;
    (void)r; (void)dx; (void)dy;
    return old;
}
HWND GetDesktopWindow(void) { return w16_desktop; }
HWND GetTopWindow(HWND h) { if (!h) h = w16_desktop; return w16_valid(h) || h == w16_desktop ? h->child : NULL; }
HWND GetWindow(HWND h, UINT cmd)
{
    if (!w16_valid(h) && h != w16_desktop) return NULL;
    switch (cmd) {
    case GW_CHILD: return h->child;
    case GW_HWNDNEXT: return h->next;
    case GW_HWNDPREV: {
        if (!h->parent) return NULL;
        HWND prev = NULL;
        for (HWND c = h->parent->child; c && c != h; c = c->next) prev = c;
        return prev;
    }
    case GW_HWNDFIRST: return h->parent ? h->parent->child : NULL;
    case GW_HWNDLAST: {
        if (!h->parent) return NULL;
        HWND c = h->parent->child;
        while (c && c->next) c = c->next;
        return c;
    }
    case GW_OWNER: return h->owner;
    }
    return NULL;
}

static HWND from_point(HWND parent, POINT pt, int skip_disabled)
{
    for (HWND c = parent->child; c; c = c->next) {
        if (!(c->style & WS_VISIBLE) || !PtInRect(&c->rw, pt)) continue;
        if (c->style & WS_CHILD && skip_disabled && (c->style & WS_DISABLED)) continue;
        if (!(c->style & WS_MINIMIZE) && PtInRect(&c->rc, pt)) {
            HWND k = from_point(c, pt, skip_disabled);
            if (k) return k;
        }
        return c;
    }
    return NULL;
}
HWND WindowFromPoint(POINT pt) { HWND h = from_point(w16_desktop, pt, 1); return h ? h : w16_desktop; }

/* the window under pt once h answered WM_NCHITTEST with HTTRANSPARENT: the next sibling below h
 * there (its deepest child at pt), else h's parent - USER goes on down the z-order (a click on the
 * Desktop applet's spin arrows inside a group box reaches the arrows) */
HWND w16_window_under(HWND h, POINT pt)
{
    for (HWND s = h->next; s; s = s->next) {
        if (!(s->style & WS_VISIBLE) || !PtInRect(&s->rw, pt)) continue;
        if ((s->style & WS_CHILD) && (s->style & WS_DISABLED)) continue;
        if (!(s->style & WS_MINIMIZE) && PtInRect(&s->rc, pt)) {
            HWND k = from_point(s, pt, 1);
            if (k) return k;
        }
        return s;
    }
    return h->parent;
}
HWND ChildWindowFromPoint(HWND parent, POINT pt)
{
    if (!w16_valid(parent)) return NULL;
    ClientToScreen(parent, &pt);
    if (!PtInRect(&parent->rw, pt)) return NULL;
    for (HWND c = parent->child; c; c = c->next)
        if (PtInRect(&c->rw, pt)) return c;
    return parent;
}
HWND FindWindow(LPCSTR cls, LPCSTR title)
{
    for (HWND c = w16_desktop->child; c; c = c->next)
        if ((!cls || !strcasecmp(c->cls->name, cls)) && (!title || !strcmp(c->text, title))) return c;
    return NULL;
}
/* USER GetLastActivePopup: the popup owned by h that was active last, else h. libw16 keeps no
 * activation history, so the topmost visible window owned by h stands for it. */
HWND GetLastActivePopup(HWND h)
{
    if (!w16_valid(h)) return NULL;
    for (HWND c = w16_desktop->child; c; c = c->next)
        if (c != h && c->owner == h && (c->style & WS_VISIBLE)) return c;
    return h;
}
BOOL EnumChildWindows(HWND parent, WNDENUMPROC fn, LPARAM lp)
{
    if (!w16_valid(parent)) return FALSE;
    for (HWND c = parent->child; c;) {
        HWND n = c->next;
        if (!fn(c, lp)) return FALSE;
        if (w16_valid(c) && c->child && !EnumChildWindows(c, fn, lp)) return FALSE;
        c = n;
    }
    return TRUE;
}
BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp)
{
    for (HWND c = w16_desktop->child; c;) {
        HWND n = c->next;
        if (!fn(c, lp)) return FALSE;
        c = n;
    }
    return TRUE;
}

/* ExitWindows: every top-level window may refuse (WM_QUERYENDSESSION), then all are told the session
 * ends (WM_ENDSESSION) and the program exits. 3.1 then quits or restarts Windows (EW_RESTARTWINDOWS)
 * or reboots; asking the arch311 session for that is TODO (UNTESTED). */
static BOOL query_end(HWND h, LPARAM lp) { (void)lp; return SendMessage(h, WM_QUERYENDSESSION, 0, 0) != 0; }
static BOOL tell_end(HWND h, LPARAM lp) { SendMessage(h, WM_ENDSESSION, (WPARAM)lp, 0); return TRUE; }
BOOL ExitWindows(DWORD code, UINT reserved)
{
    (void)reserved;
    if (!EnumWindows(query_end, 0)) {
        EnumWindows(tell_end, FALSE);
        return FALSE;
    }
    EnumWindows(tell_end, TRUE);
    exit((int)(code & 0xFF));
}

/* ------------------------------------------------------------------ text, longs, words */
int GetWindowText(HWND h, LPSTR buf, int cb)
{
    if (!w16_valid(h) || cb <= 0) return 0;
    return (int)SendMessage(h, WM_GETTEXT, cb, (LPARAM)buf);
}
int GetWindowTextLength(HWND h) { return w16_valid(h) ? (int)SendMessage(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
void SetWindowText(HWND h, LPCSTR s) { if (w16_valid(h)) SendMessage(h, WM_SETTEXT, 0, (LPARAM)s); }

/* GWL_ID / GWW_ID of a window that is not a child is its menu handle: USER keeps a top-level window's
 * menu in the ID field (Clock removes and restores its menu bar with SetWindowWord(GWW_ID)). A menu
 * handle needs the pointer-sized w16_Get/SetWindowPtr here. */
intptr_t w16_GetWindowPtr(HWND h, int i)
{
    if (!w16_valid(h)) return 0;
    switch (i) {
    case GWL_WNDPROC: return (intptr_t)h->proc;
    case GWL_STYLE: return (LONG)h->style;
    case GWL_EXSTYLE: return h->exstyle;
    case GWL_ID: return (h->style & WS_CHILD) ? (intptr_t)h->id : (intptr_t)h->menu;
    case GWL_HINSTANCE: return (intptr_t)h->inst;
    case GWW_HWNDPARENT: return (intptr_t)GetParent(h);
    }
    if (i >= 0 && i + (int)sizeof(intptr_t) <= h->cbextra + 64) {
        intptr_t v;
        memcpy(&v, h->extra + i, sizeof v);
        return v;
    }
    return 0;
}
intptr_t w16_SetWindowPtr(HWND h, int i, intptr_t v)
{
    if (!w16_valid(h)) return 0;
    intptr_t o = w16_GetWindowPtr(h, i);
    switch (i) {
    case GWL_WNDPROC: h->proc = (WNDPROC)v; return o;
    case GWL_STYLE: h->style = (DWORD)v; return o;
    case GWL_EXSTYLE: h->exstyle = (DWORD)v; return o;
    case GWL_ID:
        if (h->style & WS_CHILD) h->id = (UINT)v;
        else h->menu = (HMENU)v; /* no redraw: the caller recalculates the frame (SWP_FRAMECHANGED) */
        return o;
    case GWL_HINSTANCE: h->inst = (HINSTANCE)v; return o;
    }
    if (i >= 0 && i + (int)sizeof(intptr_t) <= h->cbextra + 64) memcpy(h->extra + i, &v, sizeof v);
    return o;
}
LONG GetWindowLong(HWND h, int i)
{
    if (i >= 0 && w16_valid(h)) {
        LONG v = 0;
        if (i + 4 <= h->cbextra + 64) memcpy(&v, h->extra + i, 4);
        return v;
    }
    return (LONG)w16_GetWindowPtr(h, i);
}
LONG SetWindowLong(HWND h, int i, LONG v)
{
    if (i >= 0 && w16_valid(h)) {
        LONG o = GetWindowLong(h, i);
        if (i + 4 <= h->cbextra + 64) memcpy(h->extra + i, &v, 4);
        return o;
    }
    return (LONG)w16_SetWindowPtr(h, i, v);
}
WORD GetWindowWord(HWND h, int i)
{
    if (i >= 0 && w16_valid(h)) {
        WORD v = 0;
        if (i + 2 <= h->cbextra + 64) memcpy(&v, h->extra + i, 2);
        return v;
    }
    return (WORD)w16_GetWindowPtr(h, i);
}
WORD SetWindowWord(HWND h, int i, WORD v)
{
    if (i >= 0 && w16_valid(h)) {
        WORD o = GetWindowWord(h, i);
        if (i + 2 <= h->cbextra + 64) memcpy(h->extra + i, &v, 2);
        return o;
    }
    return (WORD)w16_SetWindowPtr(h, i, v);
}
LONG GetClassLong(HWND h, int i) { (void)h; (void)i; return 0; }
WORD GetClassWord(HWND h, int i) { (void)h; (void)i; return 0; }
/* GetClassLong for what does not fit in 32 bits here: GCL_WNDPROC is the class's window procedure
 * (COMMDLG's subclass procedures call it rather than the window's own, seg2:1978/1A12) */
intptr_t w16_GetClassPtr(HWND h, int i)
{
    if (!w16_valid(h)) return 0;
    switch (i) {
    case GCL_WNDPROC: return (intptr_t)h->cls->wc.lpfnWndProc;
    case GCW_HBRBACKGROUND: return (intptr_t)h->cls->wc.hbrBackground;
    case GCW_HCURSOR: return (intptr_t)h->cls->wc.hCursor;
    case GCW_HICON: return (intptr_t)h->cls->wc.hIcon;
    }
    return 0;
}
intptr_t w16_SetClassPtr(HWND h, int i, intptr_t v)
{
    if (!w16_valid(h)) return 0;
    intptr_t o = 0;
    switch (i) {
    case GCW_HBRBACKGROUND: o = (intptr_t)h->cls->wc.hbrBackground; h->cls->wc.hbrBackground = (HBRUSH)v; break;
    case GCW_HCURSOR: o = (intptr_t)h->cls->wc.hCursor; h->cls->wc.hCursor = (HCURSOR)v; break;
    case GCW_HICON: o = (intptr_t)h->cls->wc.hIcon; h->cls->wc.hIcon = (HICON)v; break;
    }
    return o;
}
WORD SetClassWord(HWND h, int i, WORD v) { return (WORD)w16_SetClassPtr(h, i, v); }

/* ------------------------------------------------------------------ properties */
/* a property is named by a string or by an integer atom (MAKEINTATOM: COMMDLG keeps its instance
 * data under atom 0xA000); an atom is kept as "#n", a form no string name uses here */
static const char *prop_name(LPCSTR name, char *buf)
{
    if (!IS_INTRESOURCE(name)) return name;
    snprintf(buf, 16, "#%u", (unsigned)(uintptr_t)name);
    return buf;
}
BOOL SetProp(HWND h, LPCSTR name, HANDLE v)
{
    char atom[16];
    if (!w16_valid(h)) return FALSE;
    name = prop_name(name, atom);
    for (W16Prop *p = h->props; p; p = p->next)
        if (!strcasecmp(p->name, name)) { p->val = v; return TRUE; }
    W16Prop *p = calloc(1, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->val = v;
    p->next = h->props;
    h->props = p;
    return TRUE;
}
HANDLE GetProp(HWND h, LPCSTR name)
{
    char atom[16];
    if (!w16_valid(h)) return NULL;
    name = prop_name(name, atom);
    for (W16Prop *p = h->props; p; p = p->next)
        if (!strcasecmp(p->name, name)) return p->val;
    return NULL;
}
HANDLE RemoveProp(HWND h, LPCSTR name)
{
    char atom[16];
    if (!w16_valid(h)) return NULL;
    name = prop_name(name, atom);
    for (W16Prop **p = &h->props; *p; p = &(*p)->next)
        if (!strcasecmp((*p)->name, name)) {
            W16Prop *q = *p;
            HANDLE v = q->val;
            *p = q->next;
            free(q);
            return v;
        }
    return NULL;
}

BOOL OpenIcon(HWND h) { return ShowWindow(h, SW_RESTORE); }
BOOL CloseWindow(HWND h) { return ShowWindow(h, SW_MINIMIZE); }
void DragAcceptFiles(HWND h, BOOL a)
{
    if (!w16_valid(h)) return;
    if (a) h->exstyle |= WS_EX_ACCEPTFILES;
    else h->exstyle &= ~WS_EX_ACCEPTFILES;
}
BOOL FlashWindow(HWND h, BOOL inv)
{
    if (!w16_valid(h)) return FALSE;
    SendMessage(h, WM_NCACTIVATE, inv ? !h->active_frame : (h == w16_active), 0);
    return TRUE;
}

/* ------------------------------------------------------------------ ScrollWindow */
void ScrollWindow(HWND h, int dx, int dy, LPCRECT scroll, LPCRECT clip)
{
    if (!w16_valid(h) || (!dx && !dy)) return;
    RECT r = scroll ? *scroll : (RECT){0, 0, h->rc.right - h->rc.left, h->rc.bottom - h->rc.top};
    if (clip) IntersectRect(&r, &r, clip);
    RECT sr = r;
    OffsetRect(&sr, h->rc.left, h->rc.top);
    /* only copy pixels when the whole area is visible; otherwise repaint it */
    Region vis;
    rgn_init(&vis);
    w16_calc_visrgn(h, 0, 0, &vis);
    Region test;
    rgn_init(&test);
    rgn_set(&test, &sr);
    for (int i = 0; i < vis.n; i++) rgn_sub(&test, &vis.r[i]);
    int full = rgn_empty(&test);
    w16_caret_hide_for_paint(h);
    if (full && w16_window_visible(h) && abs(dx) < sr.right - sr.left && abs(dy) < sr.bottom - sr.top) {
        W16Bitmap *s = &w16_screen;
        int w = sr.right - sr.left, ht = sr.bottom - sr.top;
        uint32_t *tmp = malloc((size_t)w * ht * 4);
        for (int y = 0; y < ht; y++) memcpy(tmp + y * w, s->px + (sr.top + y) * s->w + sr.left, w * 4);
        for (int y = 0; y < ht; y++) {
            int sy = y - dy;
            if (sy < 0 || sy >= ht) continue;
            for (int x = 0; x < w; x++) {
                int sx = x - dx;
                if (sx < 0 || sx >= w) continue;
                s->px[(sr.top + y) * s->w + sr.left + x] = tmp[sy * w + sx];
            }
        }
        free(tmp);
        w16_screen_dirty = 1;
        /* shift pending update region too */
        Region moved;
        rgn_init(&moved);
        rgn_copy(&moved, &h->upd);
        rgn_and(&moved, &sr);
        rgn_sub(&h->upd, &sr);
        rgn_offset(&moved, dx, dy);
        rgn_and(&moved, &sr);
        for (int i = 0; i < moved.n; i++) rgn_add(&h->upd, &moved.r[i]);
        rgn_free(&moved);
        RECT ex;
        if (dx > 0) { ex = (RECT){sr.left, sr.top, sr.left + dx, sr.bottom}; w16_invalidate_window(h, &ex, 1, 0); }
        if (dx < 0) { ex = (RECT){sr.right + dx, sr.top, sr.right, sr.bottom}; w16_invalidate_window(h, &ex, 1, 0); }
        if (dy > 0) { ex = (RECT){sr.left, sr.top, sr.right, sr.top + dy}; w16_invalidate_window(h, &ex, 1, 0); }
        if (dy < 0) { ex = (RECT){sr.left, sr.bottom + dy, sr.right, sr.bottom}; w16_invalidate_window(h, &ex, 1, 0); }
    } else
        w16_invalidate_window(h, &sr, 1, 0);
    rgn_free(&vis);
    rgn_free(&test);
    if (!scroll)
        for (HWND c = h->child; c; c = c->next) move(c, dx, dy);
    w16_caret_restore_after_paint(h);
}

BOOL ScrollDC(HDC dc, int dx, int dy, LPCRECT scroll, LPCRECT clip, HRGN upd, LPRECT rcupd)
{
    (void)upd;
    RECT r = *scroll;
    if (clip) IntersectRect(&r, &r, clip);
    HDC src = dc;
    BitBlt(dc, r.left + dx, r.top + dy, r.right - r.left, r.bottom - r.top, src, r.left, r.top, SRCCOPY);
    if (rcupd) {
        *rcupd = r;
        if (dy > 0) rcupd->bottom = r.top + dy;
        else if (dy < 0) rcupd->top = r.bottom + dy;
        if (dx > 0) rcupd->right = r.left + dx;
        else if (dx < 0) rcupd->left = r.right + dx;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ desktop window */
/* The desktop as USER 3.1 paints it: the desktop brush - COLOR_BACKGROUND's, which the WIN.INI
 * [Desktop] Pattern= replaces (SetDeskPattern, USER seg41:0A61) - and the wallpaper (SetDeskWallpaper
 * seg21:04D2, PaintDesktop seg21:086E): tiled from WallpaperOriginX/Y, or centred with the desktop
 * brush around it. */
static WORD desk_bits[16];          /* [0x5ec]: the pattern's rows, a 16 x 16 bitmap */
static int desk_has_pattern;
static HBRUSH desk_brush;           /* [0x52e] made from desk_bits in these colours */
static COLORREF desk_bg, desk_fg;
static HBITMAP wallpaper;           /* [0x89a] */
static int wallpaper_style;         /* [0x9a6]: TileWallpaper | WallpaperStyle, bit 0 tiles */
static int wallpaper_x, wallpaper_y; /* [0x996], [0x998] */

/* the pattern brush, or NULL for the plain COLOR_BACKGROUND brush (seg41:090C makes it again when
 * the colours change: clear bits in the desktop colour, set ones in the window text colour) */
HBRUSH w16_desktop_pattern_brush(void)
{
    if (!desk_has_pattern) return NULL;
    COLORREF bg = GetSysColor(COLOR_BACKGROUND), fg = GetSysColor(COLOR_WINDOWTEXT);
    if (desk_brush && desk_bg == bg && desk_fg == fg) return desk_brush;
    if (desk_brush) { desk_brush->stock = 0; DeleteObject(desk_brush); desk_brush = NULL; }
    HBITMAP mono = CreateBitmap(16, 16, 1, 1, desk_bits);
    HDC screen = GetDC(NULL);
    HDC dmono = CreateCompatibleDC(screen), dcol = CreateCompatibleDC(screen);
    HBITMAP col = CreateCompatibleBitmap(screen, 16, 16);
    SelectObject(dmono, mono);
    SelectObject(dcol, col);
    SetTextColor(dcol, bg);
    SetBkColor(dcol, fg);
    BitBlt(dcol, 0, 0, 16, 16, dmono, 0, 0, SRCCOPY);
    /* (3.1 GDI brushes use the top left 8 x 8 of the bitmap) */
    desk_brush = CreatePatternBrush(col);
    desk_brush->stock = 1;
    desk_bg = bg;
    desk_fg = fg;
    DeleteDC(dcol);
    DeleteDC(dmono);
    ReleaseDC(NULL, screen);
    DeleteObject(col);
    DeleteObject(mono);
    return desk_brush;
}

/* USER seg41:0A61: the pattern from a string of numbers, (LPCSTR)-1 for WIN.INI's; "(None)" or
 * nothing is the plain colour. FALSE when WIN.INI has no Pattern= */
BOOL w16_set_desk_pattern(LPCSTR pat)
{
    char buf[0x50];
    desk_has_pattern = 0;
    if (pat == (LPCSTR)-1) {
        if (!GetProfileString("Desktop", "Pattern", "", buf, sizeof buf)) return FALSE;
        pat = buf;
    }
    if (pat && *pat && lstrcmpi(pat, "(None)")) {
        /* sixteen numbers, the missing ones 0 (each a row of 16 pixels, the low byte on the left) */
        for (int i = 0; i < 16; i++) {
            SHORT n = 0;
            while (*pat && ((signed char)*pat < '0' || (signed char)*pat > '9')) pat++;
            while ((signed char)*pat >= '0' && (signed char)*pat <= '9') n = (SHORT)(n * 10 + *pat++ - '0');
            WORD w = (WORD)n;
            /* CreateBitmap rows are bytes in memory order: the word's low byte first */
            ((BYTE *)&desk_bits[i])[0] = LOBYTE(w);
            ((BYTE *)&desk_bits[i])[1] = HIBYTE(w);
        }
        desk_has_pattern = 1;
    }
    if (desk_brush) { desk_brush->stock = 0; DeleteObject(desk_brush); desk_brush = NULL; }
    SendMessage(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0);
    w16_desktop_redraw();
    return TRUE;
}

/* seg21:0131: a .BMP file (OpenFile's search) as a bitmap for the screen */
static HBITMAP load_wallpaper(LPCSTR file)
{
    OFSTRUCT of;
    HFILE f = OpenFile(file, &of, OF_READ);
    if (f == HFILE_ERROR) return NULL;
    HBITMAP bm = NULL;
    LONG n = _llseek(f, 0, 2);
    _llseek(f, 0, 0);
    uint8_t *d = n > 14 ? malloc(n) : NULL;
    if (d && (LONG)_lread(f, d, (UINT)n) == n && d[0] == 'B' && d[1] == 'M') {
        /* compared with 3.11 for winlogo.bmp (16 colours) only; how VGA shows 256-colour files
         * (256COLOR.BMP) is UNTESTED */
        HBITMAP w16_bitmap_from_dib(const uint8_t *d, int len, int force_color);
        bm = w16_bitmap_from_dib(d + 14, (int)n - 14, 1);
    }
    free(d);
    _lclose(f);
    return bm;
}

/* USER seg21:04D2: the wallpaper from a file name, NULL or (LPCSTR)-1 for WIN.INI's; "(None)" is
 * none. FALSE when the file cannot be loaded */
BOOL w16_set_desk_wallpaper(LPCSTR file)
{
    char buf[0xa0];
    if (wallpaper) { DeleteObject(wallpaper); wallpaper = NULL; }
    if (!file || file == (LPCSTR)-1) {
        if (!GetProfileString("Desktop", "Wallpaper", "", buf, 0x80)) return FALSE;
    } else
        snprintf(buf, sizeof buf, "%s", file);
    if (!lstrcmpi(buf, "(None)")) return TRUE;
    wallpaper_style = GetProfileInt("Desktop", "TileWallpaper", 1) | GetProfileInt("Desktop", "WallpaperStyle", 0);
    wallpaper = load_wallpaper(buf);
    if (!wallpaper) { wallpaper_style = 0; return FALSE; }
    wallpaper_x = GetProfileInt("Desktop", "WallpaperOriginX", 0);
    wallpaper_y = GetProfileInt("Desktop", "WallpaperOriginY", 0);
    if (!(wallpaper_style & 1)) {
        W16Bitmap *b = w16_bitmap_of(wallpaper);
        if (!wallpaper_x) wallpaper_x = (GetSystemMetrics(SM_CXSCREEN) - b->w) / 2;
        if (!wallpaper_y) wallpaper_y = (GetSystemMetrics(SM_CYSCREEN) - b->h) / 2;
    }
    return TRUE;
}

/* every window again, the desktop first (USER's RedrawWindow of the desktop with RDW_ALLCHILDREN) */
void w16_desktop_redraw(void)
{
    if (w16_desktop) w16_invalidate_screen_rect(&w16_desktop->rw);
}

/* seg21:06E8: the wallpaper in tiles from (x, y) over the clip box */
static void tile_wallpaper(HDC dc, int x, int y)
{
    RECT clip;
    W16Bitmap *b = w16_bitmap_of(wallpaper);
    GetClipBox(dc, &clip);
    while (b->w + x < clip.left) x += b->w;
    while (b->h + y < clip.top) y += b->h;
    while (clip.left < x) x -= b->w;
    while (clip.top < y) y -= b->h;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, wallpaper);
    for (int ty = y; ty < clip.bottom; ty += b->h)
        for (int tx = x; tx < clip.right; tx += b->w) BitBlt(dc, tx, ty, b->w, b->h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

/* seg21:086E PaintDesktop (the desktop DC is in screen coordinates already) */
static void paint_desktop(HDC dc)
{
    RECT clip;
    if (GetClipBox(dc, &clip) == NULLREGION) return;
    SetBrushOrg(dc, -dc->ox, -dc->oy);    /* (0, 0) on the screen */
    if (wallpaper_style & 1) {
        tile_wallpaper(dc, wallpaper_x, wallpaper_y);
        return;
    }
    /* seg21:07B7: centred, the desktop brush around it */
    W16Bitmap *b = w16_bitmap_of(wallpaper);
    RECT rc = {wallpaper_x, wallpaper_y, wallpaper_x + b->w, wallpaper_y + b->h};
    SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    tile_wallpaper(dc, wallpaper_x, wallpaper_y);
    RestoreDC(dc, -1);
    SaveDC(dc);
    ExcludeClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    GetClipBox(dc, &clip);
    FillRect(dc, &clip, w16_sys_brush(COLOR_BACKGROUND));
    RestoreDC(dc, -1);
}

/* the desktop behind a window (USER seg1:5881, WM_ICONERASEBKGND): the wallpaper in screen
 * coordinates, else the desktop brush */
void w16_paint_desktop(HDC dc, const RECT *r)
{
    if (!wallpaper) {
        /* the pattern lines up with the desktop's (USER fills with the brush aligned to the parent) */
        DWORD org = SetBrushOrg(dc, -dc->ox, -dc->oy);
        FillRect(dc, r, w16_sys_brush(COLOR_BACKGROUND));
        SetBrushOrg(dc, (SHORT)LOWORD(org), (SHORT)HIWORD(org));
        return;
    }
    DWORD org = SetViewportOrg(dc, -dc->ox, -dc->oy);
    paint_desktop(dc);
    SetViewportOrg(dc, (SHORT)LOWORD(org), (SHORT)HIWORD(org));
}

/* USER seg41:0D6E: a new border width grows or shrinks each sizable window around its client area
 * (a minimised one's restored rectangle), then everything is drawn again. Compared with 3.11 for the
 * Control Panel's window (border 3 -> 5); minimised windows and child windows are UNTESTED. */
static void border_resize(HWND h, int dx, int dy)
{
    for (; h; h = h->next) {
        if (h->style & WS_THICKFRAME) {
            if (h->style & WS_MINIMIZE) {
                InflateRect(&h->restore, dx, dy);
            } else {
                RECT r = h->rw;
                InflateRect(&r, dx, dy);
                RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
                SetWindowPos(h, NULL, r.left - pr.left, r.top - pr.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
            }
        }
        border_resize(h->child, dx, dy);
    }
}

void w16_border_changed(int old)
{
    int d = w16_border_width - old;
    w16_metric[SM_CXFRAME] = w16_metric[SM_CYFRAME] = w16_border_width + 1;
    if (w16_desktop) border_resize(w16_desktop->child, d * GetSystemMetrics(SM_CXBORDER), d * GetSystemMetrics(SM_CYBORDER));
    w16_desktop_redraw();
}

LRESULT w16_desktop_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        if (wallpaper) {
            paint_desktop(dc);
        } else {
            RECT r = {0, 0, w16_screen.w, w16_screen.h};
            FillRect(dc, &r, w16_sys_brush(COLOR_BACKGROUND));
        }
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        /* icons of minimised windows paint as windows themselves; their titles (in USER separate
         * windows of their own) are drawn here, under every window */
        w16_paint_icon_titles(ps.hdc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProc(h, m, wp, lp);
}

void w16_desktop_create(void)
{
    W16Class *c = w16_find_class("#32769", NULL);
    HWND h = calloc(1, sizeof *h);
    h->magic = W16_WND_MAGIC;
    h->cls = c;
    h->proc = c->wc.lpfnWndProc;
    h->style = WS_VISIBLE | WS_CLIPCHILDREN;
    h->text = strdup("");
    h->extra = calloc(1, 64);
    rgn_init(&h->upd);
    SetRect(&h->rw, 0, 0, w16_screen.w, w16_screen.h);
    h->rc = h->rw;
    w16_desktop = h;
    /* USER's start: the pattern and the wallpaper WIN.INI names */
    w16_set_desk_pattern((LPCSTR)-1);
    w16_set_desk_wallpaper((LPCSTR)-1);
    rgn_set(&h->upd, &h->rw);
    h->need_erase = 1;
}

BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *wp)
{
    if (!w16_valid(h) || !wp) return FALSE;
    RECT pr = h->parent && h->parent != w16_desktop ? h->parent->rc : (RECT){0, 0, 0, 0};
    wp->length = sizeof *wp;
    wp->flags = 0;
    wp->showCmd = !(h->style & WS_VISIBLE) ? SW_HIDE : IsIconic(h) ? SW_SHOWMINIMIZED : IsZoomed(h) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    RECT n = h->rw;
    OffsetRect(&n, -pr.left, -pr.top);
    if (h->style & (WS_MINIMIZE | WS_MAXIMIZE)) n = h->restore; /* kept in parent client coordinates */
    wp->rcNormalPosition = n;
    wp->ptMinPosition = (POINT){-1, -1};
    wp->ptMaxPosition = (POINT){-1, -1};
    return TRUE;
}
