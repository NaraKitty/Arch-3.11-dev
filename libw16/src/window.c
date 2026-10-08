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
    sysclass("#32770", w16_dialog_wndproc, CS_SAVEBITS, arrow, 30);
    sysclass("#32769", w16_desktop_proc, 0, arrow, 0);
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
void w16_calc_visrgn(HWND h, int window, int clipchildren, Region *out)
{
    rgn_clear(out);
    if (!w16_window_visible(h)) return;
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
    if (!w16_valid(h) || !w16_window_visible(h) || h->redraw_off) return;
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

void w16_send_paint_cascade(HWND h)
{
    if (!w16_valid(h)) return;
    if (needs_paint(h)) {
        if (!rgn_empty(&h->upd) || h->internal_paint) SendMessage(h, WM_PAINT, 0, 0);
        else if (h->need_ncpaint) {
            h->need_ncpaint = 0;
            SendMessage(h, WM_NCPAINT, 1, 0);
        }
        if (w16_valid(h) && needs_paint(h) && rgn_empty(&h->upd)) h->need_ncpaint = 0;
    }
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
        if (IsIconic(h) && !h->cls->wc.hIcon) ps->fErase = !SendMessage(h, WM_ICONERASEBKGND, (WPARAM)dc, 0);
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

BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    if (!w16_valid(h)) return FALSE;
    WINDOWPOS wp = {h, after, x, y, cx, cy, fl};
    RECT pr = {0, 0, 0, 0};
    if (h->parent && h->parent != w16_desktop) pr = h->parent->rc;
    if (fl & SWP_NOMOVE) { wp.x = h->rw.left - pr.left; wp.y = h->rw.top - pr.top; }
    if (fl & SWP_NOSIZE) { wp.cx = h->rw.right - h->rw.left; wp.cy = h->rw.bottom - h->rw.top; }
    SendMessage(h, WM_WINDOWPOSCHANGING, 0, (LPARAM)&wp);
    RECT nr = {pr.left + wp.x, pr.top + wp.y, pr.left + wp.x + wp.cx, pr.top + wp.y + wp.cy};
    int moved = nr.left != h->rw.left || nr.top != h->rw.top;
    int sized = (nr.right - nr.left) != (h->rw.right - h->rw.left) || (nr.bottom - nr.top) != (h->rw.bottom - h->rw.top);
    if (!(fl & SWP_NOZORDER) && h->parent) {
        if (after == HWND_TOP || after == HWND_TOPMOST || after == NULL) raise_w(h);
        else if (after == HWND_BOTTOM) { unlink_w(h); link_after(h, HWND_BOTTOM); }
        else if (w16_valid(after) && after->parent == h->parent && after != h) { unlink_w(h); link_after(h, after); }
        if (w16_window_visible(h)) w16_invalidate_window(h, NULL, 1, 1);
    }
    if (fl & SWP_HIDEWINDOW) {
        if (h->style & WS_VISIBLE) {
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
        }
    }
    if (fl & SWP_FRAMECHANGED) w16_invalidate_window(h, NULL, 1, 1);
    wp.flags = fl | (moved ? 0 : SWP_NOMOVE) | (sized ? 0 : SWP_NOSIZE);
    if (moved || sized) SendMessage(h, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp);
    if (!(fl & SWP_NOACTIVATE) && h->parent == w16_desktop && (h->style & WS_VISIBLE) && !(fl & SWP_HIDEWINDOW))
        w16_activate(h, WA_ACTIVE);
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
        if (top != w16_active) w16_activate(top, WA_ACTIVE);
        if (w16_focus != old) return old; /* activation moved focus */
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
static void default_position(HWND h, int *x, int *y, int *cx, int *cy, int usedef_pos, int usedef_size)
{
    int sw = w16_screen.w, sh = w16_screen.h;
    int step = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME) - 1;
    if (usedef_pos) {
        /* USER cascades CW_USEDEFAULT windows by caption+frame height */
        int n = default_pos_count++;
        int steps = max(1, (sh / 4) / step);
        *x = (n % steps) * step;
        *y = (n % steps) * step;
        (void)h;
    }
    if (usedef_size) {
        *cx = sw - *x - 0;
        *cy = (sh * 17) / 20 - *y;
        if (*cx < 100) *cx = sw * 3 / 4;
        if (*cy < 50) *cy = sh * 3 / 4;
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
    if (!(style & (WS_CHILD | WS_POPUP)) && (udp || uds)) {
        if (!udp) { /* keep x,y */ }
        default_position(h, &x, &y, &cx, &cy, udp, uds);
    } else {
        if (udp) x = y = 0;
        if (uds) cx = cy = 0;
    }
    RECT pr = {0, 0, 0, 0};
    if (h->parent != w16_desktop) pr = h->parent->rc;
    /* WM_GETMINMAXINFO for top-level sizable windows */
    if (h->parent == w16_desktop && (style & (WS_THICKFRAME | WS_CAPTION))) {
        MINMAXINFO mm = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
        int f = GetSystemMetrics(SM_CXFRAME);
        mm.ptMaxSize.x = w16_screen.w + 2 * f;
        mm.ptMaxSize.y = w16_screen.h + 2 * f;
        mm.ptMaxPosition.x = mm.ptMaxPosition.y = -f;
        mm.ptMinTrackSize.x = GetSystemMetrics(SM_CXMINTRACK);
        mm.ptMinTrackSize.y = GetSystemMetrics(SM_CYMINTRACK);
        mm.ptMaxTrackSize.x = mm.ptMaxSize.x;
        mm.ptMaxTrackSize.y = mm.ptMaxSize.y;
    }
    h->rw = (RECT){pr.left + x, pr.top + y, pr.left + x + cx, pr.top + y + cy};
    h->restore = h->rw;
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
    SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
    SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left - pr.left, h->rc.top - pr.top));
    if ((style & WS_CHILD) && !(ex & WS_EX_NOPARENTNOTIFY))
        SendMessage(h->parent, WM_PARENTNOTIFY, WM_CREATE, (LPARAM)h);
    if (style & WS_MINIMIZE) ShowWindow(h, SW_SHOWMINIMIZED);
    else if (style & WS_MAXIMIZE) ShowWindow(h, SW_SHOWMAXIMIZED);
    else if (style & WS_VISIBLE) ShowWindow(h, SW_SHOW);
    return h;
}

HWND CreateWindow(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy, HWND parent,
                  HMENU menu, HINSTANCE inst, void *param)
{
    return CreateWindowEx(0, cls, title, style, x, y, cx, cy, parent, menu, inst, param);
}

static void destroy_rec(HWND h)
{
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
    if (h->style & WS_VISIBLE) {
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
BOOL ShowWindow(HWND h, int cmd)
{
    if (!w16_valid(h)) return FALSE;
    int was = (h->style & WS_VISIBLE) != 0;
    switch (cmd) {
    case SW_HIDE:
        if (was) {
            SendMessage(h, WM_SHOWWINDOW, FALSE, 0);
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (h == w16_active) {
                w16_active = NULL;
                for (HWND c = w16_desktop->child; c; c = c->next)
                    if ((c->style & WS_VISIBLE) && c != h) { w16_activate(c, WA_ACTIVE); break; }
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
        else if (cmd == SW_MINIMIZE && w16_active == h) {
            for (HWND c = w16_desktop->child; c; c = c->next)
                if ((c->style & WS_VISIBLE) && c != h && !IsIconic(c)) { w16_activate(c, WA_ACTIVE); break; }
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

/* ------------------------------------------------------------------ text, longs, words */
int GetWindowText(HWND h, LPSTR buf, int cb)
{
    if (!w16_valid(h) || cb <= 0) return 0;
    return (int)SendMessage(h, WM_GETTEXT, cb, (LPARAM)buf);
}
int GetWindowTextLength(HWND h) { return w16_valid(h) ? (int)SendMessage(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
void SetWindowText(HWND h, LPCSTR s) { if (w16_valid(h)) SendMessage(h, WM_SETTEXT, 0, (LPARAM)s); }

intptr_t w16_GetWindowPtr(HWND h, int i)
{
    if (!w16_valid(h)) return 0;
    switch (i) {
    case GWL_WNDPROC: return (intptr_t)h->proc;
    case GWL_STYLE: return (LONG)h->style;
    case GWL_EXSTYLE: return h->exstyle;
    case GWL_ID: return h->id;
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
    case GWL_ID: h->id = (UINT)v; return o;
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
BOOL SetProp(HWND h, LPCSTR name, HANDLE v)
{
    if (!w16_valid(h)) return FALSE;
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
    if (!w16_valid(h)) return NULL;
    for (W16Prop *p = h->props; p; p = p->next)
        if (!strcasecmp(p->name, name)) return p->val;
    return NULL;
}
HANDLE RemoveProp(HWND h, LPCSTR name)
{
    if (!w16_valid(h)) return NULL;
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
static HBITMAP wallpaper;
static int wallpaper_tile = 1;

LRESULT w16_desktop_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT r = {0, 0, w16_screen.w, w16_screen.h};
        FillRect(dc, &r, w16_sys_brush(COLOR_BACKGROUND));
        if (wallpaper) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(mem, wallpaper);
            W16Bitmap *b = w16_bitmap_of(wallpaper);
            if (wallpaper_tile)
                for (int y = 0; y < r.bottom; y += b->h)
                    for (int x = 0; x < r.right; x += b->w) BitBlt(dc, x, y, b->w, b->h, mem, 0, 0, SRCCOPY);
            else
                BitBlt(dc, (r.right - b->w) / 2, (r.bottom - b->h) / 2, b->w, b->h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, o);
            DeleteDC(mem);
        }
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        /* icons of minimised windows sit on the desktop: they paint as windows themselves */
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
    char wp[260];
    GetProfileString("Desktop", "Wallpaper", "(None)", wp, sizeof wp);
    wallpaper_tile = GetProfileInt("Desktop", "TileWallpaper", 0);
    if (strcasecmp(wp, "(None)") && wp[0]) {
        /* wallpapers are .BMP files: from the user's ripped files or a DOS path */
        char path[1200];
        const char *base = strrchr(wp, '\\') ? strrchr(wp, '\\') + 1 : wp;
        snprintf(path, sizeof path, "%s/files/%s", w16_assets_dir(), base);
        FILE *f = fopen(path, "rb");
        if (!f && w16_dos_to_host(wp, path, sizeof path) == 0) f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *d = malloc(n);
            if (fread(d, 1, n, f) == (size_t)n && n > 14 && d[0] == 'B' && d[1] == 'M') {
                HBITMAP w16_bitmap_from_dib(const uint8_t *d, int len, int force_color);
                wallpaper = w16_bitmap_from_dib(d + 14, n - 14, 1);
            }
            free(d);
            fclose(f);
        }
    }
    rgn_set(&h->upd, &h->rw);
    h->need_erase = 1;
}
