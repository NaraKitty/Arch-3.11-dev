/* USER menus: menu objects, menu bar layout/drawing, popup menu windows, tracking.
 * Metrics measured from real 3.11 (VGA): bar item = 8 + text + 8; popup item height = font+2;
 * popup text at x=16, accelerators at 16 + max(text) + 8, right margin 13; separator 7 px. */
#include "w16int.h"
#include <ctype.h>

#define BAR_PAD 8
#define POP_LEFT 16
#define POP_GAP 8
#define POP_RIGHT 13
#define SEP_H 7

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ------------------------------------------------------------------ objects */
HMENU CreateMenu(void) { return calloc(1, sizeof(struct W16Menu)); }
HMENU CreatePopupMenu(void) { HMENU m = CreateMenu(); m->is_popup = 1; return m; }

BOOL DestroyMenu(HMENU m)
{
    if (!m) return FALSE;
    for (int i = 0; i < m->n; i++) {
        if (m->it[i].sub) DestroyMenu(m->it[i].sub);
        free(m->it[i].text);
    }
    free(m->it);
    free(m);
    return TRUE;
}

static void insert_item(HMENU m, int pos, UINT flags, UINT_PTR_W16 id, LPCSTR text)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->it = realloc(m->it, sizeof(struct W16MenuItem) * m->cap);
    }
    if (pos < 0 || pos > m->n) pos = m->n;
    memmove(&m->it[pos + 1], &m->it[pos], sizeof(struct W16MenuItem) * (m->n - pos));
    struct W16MenuItem *it = &m->it[pos];
    memset(it, 0, sizeof *it);
    it->flags = flags & ~(MF_BYPOSITION | MF_INSERT | MF_APPEND | MF_CHANGE | MF_DELETE | MF_REMOVE);
    if (flags & MF_POPUP) { it->sub = (HMENU)id; if (it->sub) it->sub->is_popup = 1; }
    else it->id = (UINT)id;
    if (flags & MF_SEPARATOR) it->text = strdup("");
    else if (flags & (MF_BITMAP | MF_OWNERDRAW)) it->text = strdup("");
    else it->text = strdup(text ? text : "");
    if (!(flags & MF_POPUP) && !(flags & MF_SEPARATOR) && !it->text[0] && it->id == 0) it->flags |= MF_SEPARATOR;
    m->n++;
    m->height = 0;
}

static const uint8_t *parse(HMENU m, const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        UINT fl = u16(p);
        p += 2;
        UINT id = 0;
        if (!(fl & MF_POPUP)) { id = u16(p); p += 2; }
        const char *txt = (const char *)p;
        p += strlen(txt) + 1;
        if (fl & MF_POPUP) {
            HMENU sub = CreatePopupMenu();
            p = parse(sub, p, end);
            insert_item(m, -1, (fl & ~MF_END) | MF_POPUP, (UINT_PTR_W16)sub, txt);
        } else
            insert_item(m, -1, (fl & ~MF_END), id, txt);
        if (fl & MF_END) break;
    }
    return p;
}

HMENU LoadMenuIndirect(const void *tmpl)
{
    const uint8_t *d = tmpl;
    HMENU m = CreateMenu();
    parse(m, d + 4 + u16(d + 2), d + 0x10000);
    return m;
}

HMENU LoadMenu(HINSTANCE h, LPCSTR name)
{
    const W16Res *r = w16_find_res(h, name, RT_MENU);
    if (!r) return NULL;
    const uint8_t *d = w16_res_data(h, r);
    HMENU m = CreateMenu();
    parse(m, d + 4 + u16(d + 2), d + r->len);
    return m;
}

static int find(HMENU m, UINT id, UINT flags, HMENU *owner)
{
    if (!m) return -1;
    if (flags & MF_BYPOSITION) { *owner = m; return (int)id < m->n ? (int)id : -1; }
    for (int i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & MF_POPUP) && m->it[i].id == id) { *owner = m; return i; }
        if (m->it[i].sub) {
            int k = find(m->it[i].sub, id, flags, owner);
            if (k >= 0) return k;
        }
    }
    /* a popup can be addressed by its handle */
    for (int i = 0; i < m->n; i++)
        if (m->it[i].sub && (UINT)(uintptr_t)m->it[i].sub == id) { *owner = m; return i; }
    return -1;
}

BOOL AppendMenu(HMENU m, UINT f, UINT_PTR_W16 id, LPCSTR t) { if (!m) return FALSE; insert_item(m, -1, f, id, t); return TRUE; }
BOOL InsertMenu(HMENU m, UINT pos, UINT f, UINT_PTR_W16 id, LPCSTR t)
{
    if (!m) return FALSE;
    HMENU o = m;
    int i = (f & MF_BYPOSITION) ? (pos == (UINT)-1 ? m->n : (int)pos) : find(m, pos, f, &o);
    if (i < 0) { o = m; i = m->n; }
    insert_item(o, i, f, id, t);
    return TRUE;
}
BOOL ModifyMenu(HMENU m, UINT pos, UINT f, UINT_PTR_W16 id, LPCSTR t)
{
    HMENU o;
    int i = find(m, pos, f, &o);
    if (i < 0) return FALSE;
    struct W16MenuItem *it = &o->it[i];
    free(it->text);
    it->flags = f & ~(MF_BYPOSITION);
    if (f & MF_POPUP) it->sub = (HMENU)id; else { it->id = (UINT)id; }
    it->text = strdup((f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) || !t ? "" : t);
    o->height = 0;
    return TRUE;
}
BOOL RemoveMenu(HMENU m, UINT pos, UINT f)
{
    HMENU o;
    int i = find(m, pos, f, &o);
    if (i < 0) return FALSE;
    free(o->it[i].text);
    memmove(&o->it[i], &o->it[i + 1], sizeof(struct W16MenuItem) * (o->n - i - 1));
    o->n--;
    o->height = 0;
    return TRUE;
}
BOOL DeleteMenu(HMENU m, UINT pos, UINT f)
{
    HMENU o;
    int i = find(m, pos, f, &o);
    if (i < 0) return FALSE;
    if (o->it[i].sub) DestroyMenu(o->it[i].sub);
    o->it[i].sub = NULL;
    return RemoveMenu(o, i, MF_BYPOSITION);
}
BOOL ChangeMenu(HMENU m, UINT cmd, LPCSTR t, UINT id, UINT f)
{
    if (f & MF_APPEND) return AppendMenu(m, f, id, t);
    if (f & MF_DELETE) return DeleteMenu(m, cmd, f);
    if (f & MF_REMOVE) return RemoveMenu(m, cmd, f);
    if (f & MF_CHANGE) return ModifyMenu(m, cmd, f, id, t);
    return InsertMenu(m, cmd, f, id, t);
}
BOOL EnableMenuItem(HMENU m, UINT id, UINT f)
{
    HMENU o;
    int i = find(m, id, f, &o);
    if (i < 0) return -1;
    UINT old = o->it[i].flags & (MF_GRAYED | MF_DISABLED);
    o->it[i].flags = (o->it[i].flags & ~(MF_GRAYED | MF_DISABLED)) | (f & (MF_GRAYED | MF_DISABLED));
    if (!o->is_popup && o->owner && old != (f & (MF_GRAYED | MF_DISABLED))) DrawMenuBar(o->owner);
    return old;
}
DWORD CheckMenuItem(HMENU m, UINT id, UINT f)
{
    HMENU o;
    int i = find(m, id, f, &o);
    if (i < 0) return (DWORD)-1;
    DWORD old = o->it[i].flags & MF_CHECKED;
    if (f & MF_CHECKED) o->it[i].flags |= MF_CHECKED; else o->it[i].flags &= ~MF_CHECKED;
    return old;
}
UINT GetMenuState(HMENU m, UINT id, UINT f)
{
    HMENU o;
    int i = find(m, id, f, &o);
    if (i < 0) return (UINT)-1;
    UINT fl = o->it[i].flags;
    if (o->it[i].sub) fl = (fl & 0xFF) | (o->it[i].sub->n << 8);
    return fl;
}
int GetMenuItemCount(HMENU m) { return m ? m->n : -1; }
UINT GetMenuItemID(HMENU m, int pos)
{
    if (!m || pos < 0 || pos >= m->n) return (UINT)-1;
    return m->it[pos].sub ? (UINT)-1 : m->it[pos].id;
}
HMENU GetSubMenu(HMENU m, int pos) { return (m && pos >= 0 && pos < m->n) ? m->it[pos].sub : NULL; }
int GetMenuString(HMENU m, UINT id, LPSTR buf, int cb, UINT f)
{
    HMENU o;
    int i = find(m, id, f, &o);
    if (i < 0 || cb <= 0) return 0;
    snprintf(buf, cb, "%s", o->it[i].text);
    return strlen(buf);
}
HMENU GetMenu(HWND h) { return w16_valid(h) && !(h->style & WS_CHILD) ? h->menu : NULL; }
BOOL SetMenu(HWND h, HMENU m)
{
    if (!w16_valid(h) || (h->style & WS_CHILD)) return FALSE;
    h->menu = m;
    if (m) m->owner = h;
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RECT rc;
    w16_nc_calc(h, &h->rw, &rc);
    h->rc = rc;
    w16_invalidate_window(h, NULL, 1, 1);
    SendMessage(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
    return TRUE;
}
DWORD GetMenuCheckMarkDimensions(void) { return MAKELONG(14, 14); }
BOOL HiliteMenuItem(HWND h, HMENU m, UINT id, UINT f)
{
    HMENU o;
    int i = find(m, id, f, &o);
    if (i < 0) return FALSE;
    if (f & MF_HILITE) o->it[i].flags |= MF_HILITE; else o->it[i].flags &= ~MF_HILITE;
    DrawMenuBar(h);
    return TRUE;
}

HMENU w16_default_sysmenu(HWND h)
{
    HINSTANCE user = w16_system_module("USER.EXE");
    HMENU outer = user ? LoadMenu(user, MAKEINTRESOURCE((h->style & WS_CHILD) ? 2 : 1)) : NULL;
    HMENU m = outer && outer->n ? outer->it[0].sub : NULL;
    if (outer && outer->n) { outer->it[0].sub = NULL; DestroyMenu(outer); }
    if (!m) {
        m = CreatePopupMenu();
        AppendMenu(m, 0, SC_RESTORE, "&Restore");
        AppendMenu(m, 0, SC_MOVE, "&Move");
        AppendMenu(m, 0, SC_SIZE, "&Size");
        AppendMenu(m, 0, SC_MINIMIZE, "Mi&nimize");
        AppendMenu(m, 0, SC_MAXIMIZE, "Ma&ximize");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, 0, SC_CLOSE, "&Close\tAlt+F4");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, 0, SC_TASKLIST, "S&witch To...\tCtrl+Esc");
    }
    m->is_popup = 1;
    m->is_sys = 1;
    return m;
}

HMENU GetSystemMenu(HWND h, BOOL revert)
{
    if (!w16_valid(h)) return NULL;
    if (revert) { if (h->sysmenu) DestroyMenu(h->sysmenu); h->sysmenu = NULL; return NULL; }
    if (!h->sysmenu) h->sysmenu = w16_default_sysmenu(h);
    return h->sysmenu;
}

/* the system menu reflects the window state before it is shown */
static void prep_sysmenu(HWND h, HMENU m)
{
    int zoomed = IsZoomed(h), iconic = IsIconic(h);
    int sizable = (h->style & WS_THICKFRAME) && !zoomed && !iconic;
    EnableMenuItem(m, SC_RESTORE, (zoomed || iconic) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SC_MOVE, zoomed ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(m, SC_SIZE, sizable ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SC_MINIMIZE, ((h->style & WS_MINIMIZEBOX) && !iconic) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SC_MAXIMIZE, ((h->style & WS_MAXIMIZEBOX) && !zoomed) ? MF_ENABLED : MF_GRAYED);
}

/* ------------------------------------------------------------------ text helpers */
static void split_tab(const char *s, int *left_n, const char **accel)
{
    const char *t = strchr(s, '\t');
    if (!t) t = strchr(s, '\b'); /* \a / \b right-align markers */
    *left_n = t ? (int)(t - s) : (int)strlen(s);
    *accel = t ? t + 1 : NULL;
}

static int pfx_width(W16Font *f, const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '&') { if (i + 1 < n && s[i + 1] == '&') i++; else continue; }
        w += f->widths[(unsigned char)s[i]];
    }
    return w;
}

/* ------------------------------------------------------------------ menu bar */
static void layout_bar(HWND h, HMENU m, int width)
{
    W16Font *f = w16_font_system();
    int ih = GetSystemMetrics(SM_CYMENU);
    int x = 0, y = 0;
    for (int i = 0; i < m->n; i++) {
        struct W16MenuItem *it = &m->it[i];
        int tw = pfx_width(f, it->text, strlen(it->text));
        int w = BAR_PAD + tw + BAR_PAD;
        if (x > 0 && (x + w > width || (it->flags & (MF_MENUBREAK | MF_MENUBARBREAK)))) { x = 0; y += ih; }
        SetRect(&it->rc, x, y, x + w, y + ih);
        x += w;
    }
    /* MF_HELP: the item and the ones after it go to the right edge */
    for (int i = 0; i < m->n; i++)
        if (m->it[i].flags & MF_HELP) {
            int shift = width - m->it[m->n - 1].rc.right;
            if (shift > 0)
                for (int k = i; k < m->n; k++) if (m->it[k].rc.top == m->it[i].rc.top) OffsetRect(&m->it[k].rc, shift, 0);
            break;
        }
    m->height = y + ih + 1;
    m->owner = h;
}

int w16_menubar_height(HWND h, int width)
{
    if (!h->menu) return 0;
    layout_bar(h, h->menu, width);
    return h->menu->height;
}

static int bar_left(HWND h) { return h->rc.left - h->rw.left - ((h->style & WS_VSCROLL) ? 0 : 0); }

static void bar_origin(HWND h, int *x, int *y)
{
    /* window coordinates of the menu bar's top-left */
    RECT rc;
    w16_nc_calc(h, &h->rw, &rc);
    *x = rc.left - h->rw.left;
    *y = rc.top - h->rw.top - h->menu->height;
    (void)bar_left;
}

static void draw_bar_item(HWND h, HDC dc, int i, int hilite)
{
    HMENU m = h->menu;
    struct W16MenuItem *it = &m->it[i];
    int ox, oy;
    bar_origin(h, &ox, &oy);
    RECT r = it->rc;
    OffsetRect(&r, ox, oy);
    FillRect(dc, &r, w16_sys_brush(hilite ? COLOR_HIGHLIGHT : COLOR_MENU));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    int gray = (it->flags & MF_GRAYED) != 0;
    SetTextColor(dc, GetSysColor(gray ? COLOR_GRAYTEXT : hilite ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    w16_draw_prefix_text(dc, r.left + BAR_PAD, r.top, it->text, strlen(it->text), 0);
}

void w16_draw_menubar(HWND h, HDC wdc)
{
    HMENU m = h->menu;
    if (!m) return;
    RECT rc;
    w16_nc_calc(h, &h->rw, &rc);
    layout_bar(h, m, rc.right - rc.left);
    int ox, oy;
    bar_origin(h, &ox, &oy);
    int w = rc.right - rc.left;
    RECT bg = {ox, oy, ox + w, oy + m->height - 1};
    FillRect(wdc, &bg, w16_sys_brush(COLOR_MENU));
    for (int i = 0; i < m->n; i++) draw_bar_item(h, wdc, i, (m->it[i].flags & MF_HILITE) != 0);
    RECT line = {ox, oy + m->height - 1, ox + w, oy + m->height};
    FillRect(wdc, &line, w16_sys_brush(COLOR_WINDOWFRAME));
}

void DrawMenuBar(HWND h)
{
    if (!w16_valid(h) || !h->menu || !w16_window_visible(h)) return;
    RECT rc;
    int oldh = h->menu->height;
    w16_nc_calc(h, &h->rw, &rc);
    if (oldh && oldh != h->menu->height) {
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        h->rc = rc;
        w16_invalidate_window(h, NULL, 1, 1);
        return;
    }
    HDC dc = GetWindowDC(h);
    w16_draw_menubar(h, dc);
    ReleaseDC(h, dc);
}

/* ------------------------------------------------------------------ popup windows */
typedef struct {
    HMENU menu;
    HWND hwnd;
    int sel;
    int xtab;       /* accelerator column */
    HWND owner;
    int parent_item;
} Popup;

static void popup_layout(HMENU m, int *w, int *h, int *xtab)
{
    W16Font *f = w16_font_system();
    int maxl = 0, maxa = 0, y = 0;
    for (int i = 0; i < m->n; i++) {
        struct W16MenuItem *it = &m->it[i];
        if (it->flags & MF_SEPARATOR) continue;
        int ln;
        const char *acc;
        split_tab(it->text, &ln, &acc);
        maxl = max(maxl, pfx_width(f, it->text, ln));
        if (acc) maxa = max(maxa, pfx_width(f, acc, strlen(acc)));
    }
    *xtab = POP_LEFT + maxl + POP_GAP;
    int inner = maxa ? *xtab + maxa + POP_RIGHT : POP_LEFT + maxl + POP_RIGHT;
    for (int i = 0; i < m->n; i++) {
        struct W16MenuItem *it = &m->it[i];
        int ih = (it->flags & MF_SEPARATOR) ? SEP_H : f->height + 2;
        SetRect(&it->rc, 1, 1 + y, 1 + inner, 1 + y + ih);
        y += ih;
    }
    *w = inner + 2;
    *h = y + 2;
}

static void draw_checkerboard_text(HDC dc, int x, int y, const char *s, int n, COLORREF fg)
{
    /* disabled text on a highlight: every other pixel, like GrayString's 50% pattern */
    HBITMAP bm = CreateBitmap(w16_screen.w, 32, 1, 4, NULL);
    W16Bitmap *b = w16_bitmap_of(bm);
    for (int i = 0; i < b->w * b->h; i++) b->px[i] = 0x1000000;
    HDC mem = CreateCompatibleDC(NULL);
    SelectObject(mem, bm);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    SelectObject(mem, GetStockObject(SYSTEM_FONT));
    /* draw into the scratch bitmap */
    for (int i = 0; i < b->w * b->h; i++) b->px[i] = 0;
    w16_draw_prefix_text(mem, 0, 0, s, n, 0);
    int dx = x, dy = y;
    w16_lp_to_dp(dc, &dx, &dy);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    uint32_t c = w16_rgb(fg);
    for (int yy = 0; yy < 20; yy++)
        for (int xx = 0; xx < 400 && xx < b->w; xx++) {
            if (!b->px[yy * b->w + xx]) continue;
            int px = dx + xx, py = dy + yy;
            if (((px + py) & 1) || !rgn_contains(&e, px, py)) continue;
            w16_screen.px[py * w16_screen.w + px] = c;
        }
    rgn_free(&e);
    w16_screen_dirty = 1;
    DeleteDC(mem);
    DeleteObject(bm);
}

static void popup_draw_item(Popup *p, HDC dc, int i)
{
    struct W16MenuItem *it = &p->menu->it[i];
    RECT r = it->rc;
    int sel = (i == p->sel);
    if (it->flags & MF_SEPARATOR) {
        FillRect(dc, &r, w16_sys_brush(COLOR_MENU));
        RECT l = {r.left, r.top + 3, r.right, r.top + 4};
        FillRect(dc, &l, w16_sys_brush(COLOR_MENUTEXT));
        return;
    }
    FillRect(dc, &r, w16_sys_brush(sel ? COLOR_HIGHLIGHT : COLOR_MENU));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    int gray = (it->flags & MF_GRAYED) != 0;
    COLORREF fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : gray ? COLOR_GRAYTEXT : COLOR_MENUTEXT);
    SetTextColor(dc, fg);
    int ln;
    const char *acc;
    split_tab(it->text, &ln, &acc);
    if (it->flags & MF_CHECKED) {
        W16Bitmap *ck = w16_obm(OBM_CHECK);
        if (ck) {
            int cx = r.left + 1, cy = r.top + (r.bottom - r.top - ck->h) / 2;
            HDC tmp = dc;
            int a = cx, b = cy;
            w16_lp_to_dp(tmp, &a, &b);
            /* mono bitmap: black = check (text colour), white = transparent */
            Region e;
            w16_dc_clip_iter_begin(dc, &e);
            uint32_t c = w16_rgb(fg);
            for (int yy = 0; yy < ck->h; yy++)
                for (int xx = 0; xx < ck->w; xx++)
                    if (!ck->px[yy * ck->w + xx] && rgn_contains(&e, a + xx, b + yy)) w16_screen.px[(b + yy) * w16_screen.w + a + xx] = c;
            rgn_free(&e);
        }
    }
    if (sel && gray) {
        draw_checkerboard_text(dc, r.left - 1 + POP_LEFT, r.top, it->text, ln, GetSysColor(COLOR_HIGHLIGHTTEXT));
        if (acc) draw_checkerboard_text(dc, r.left - 1 + p->xtab, r.top, acc, strlen(acc), GetSysColor(COLOR_HIGHLIGHTTEXT));
    } else {
        w16_draw_prefix_text(dc, r.left - 1 + POP_LEFT, r.top, it->text, ln, 0);
        if (acc) w16_draw_prefix_text(dc, r.left - 1 + p->xtab, r.top, acc, strlen(acc), 0);
    }
    if (it->sub && !p->menu->is_sys) {
        /* cascading arrow */
        W16Bitmap *ar = w16_obm(OBM_MNARROW);
        if (ar) {
            int a = r.right - ar->w - 2, b = r.top + (r.bottom - r.top - ar->h) / 2;
            w16_lp_to_dp(dc, &a, &b);
            Region e;
            w16_dc_clip_iter_begin(dc, &e);
            uint32_t c = w16_rgb(fg);
            for (int yy = 0; yy < ar->h; yy++)
                for (int xx = 0; xx < ar->w; xx++)
                    if (!ar->px[yy * ar->w + xx] && rgn_contains(&e, a + xx, b + yy)) w16_screen.px[(b + yy) * w16_screen.w + a + xx] = c;
            rgn_free(&e);
        }
    }
}

static LRESULT popup_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Popup *p = h->ctl;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, w16_sys_brush(COLOR_MENU));
        for (int i = 0; p && i < p->menu->n; i++) popup_draw_item(p, dc, i);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCPAINT: {
        HDC dc = GetWindowDC(h);
        RECT r;
        GetWindowRect(h, &r);
        OffsetRect(&r, -r.left, -r.top);
        r.right -= 1; r.bottom -= 1; /* shadow column/row */
        FrameRect(dc, &r, w16_sys_brush(COLOR_WINDOWFRAME));
        /* 1-pixel shadow to the right and below (measured: light gray, AND-ed with the screen) */
        int x0 = h->rw.right - 1, y0 = h->rw.top + 1, y1 = h->rw.bottom;
        for (int y = y0; y < y1 && y < w16_screen.h; y++)
            if (x0 >= 0 && x0 < w16_screen.w && y >= 0) w16_screen.px[y * w16_screen.w + x0] &= w16_rgb(RGB(192, 192, 192));
        int yb = h->rw.bottom - 1;
        for (int x = h->rw.left + 1; x < h->rw.right - 1 && yb < w16_screen.h; x++)
            if (x >= 0 && x < w16_screen.w && yb >= 0) w16_screen.px[yb * w16_screen.w + x] &= w16_rgb(RGB(192, 192, 192));
        w16_screen_dirty = 1;
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return PtInRect(&h->rw, (POINT){(SHORT)LOWORD(lp), (SHORT)HIWORD(lp)}) ? HTCLIENT : HTNOWHERE;
    }
    return DefWindowProc(h, m, wp, lp);
}

static void ensure_popup_class(void)
{
    static int done;
    if (done) return;
    done = 1;
    WNDCLASS wc = {0};
    wc.style = CS_SAVEBITS | CS_GLOBALCLASS;
    wc.lpfnWndProc = popup_proc;
    wc.lpszClassName = "#32768";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}

static Popup *popup_open(HWND owner, HMENU menu, int x, int y, int item_index, int is_sys)
{
    ensure_popup_class();
    SendMessage(owner, WM_INITMENUPOPUP, (WPARAM)menu, MAKELPARAM(item_index, is_sys));
    Popup *p = calloc(1, sizeof *p);
    p->menu = menu;
    p->sel = -1;
    p->owner = owner;
    p->parent_item = item_index;
    int w, h;
    popup_layout(menu, &w, &h, &p->xtab);
    w += 1; h += 1; /* shadow */
    if (x + w > w16_screen.w) x = w16_screen.w - w;
    if (y + h > w16_screen.h) y = max(0, y - h);
    if (x < 0) x = 0;
    p->hwnd = CreateWindowEx(WS_EX_TOPMOST, "#32768", "", WS_POPUP, x, y, w, h, owner, NULL, NULL, NULL);
    /* client area excludes the border (1) and the shadow (1) */
    p->hwnd->ctl = p;
    p->hwnd->rc = (RECT){x + 1, y + 1, x + w - 2, y + h - 2};
    OffsetRect(&p->hwnd->rc, 0, 0);
    /* item rects are relative to the client: shift by -1 */
    for (int i = 0; i < menu->n; i++) OffsetRect(&menu->it[i].rc, -1, -1);
    SetWindowPos(p->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    p->hwnd->rc = (RECT){x + 1, y + 1, x + w - 2, y + h - 2};
    w16_invalidate_window(p->hwnd, NULL, 1, 1);
    UpdateWindow(p->hwnd);
    return p;
}

static void popup_close(Popup *p)
{
    if (!p) return;
    DestroyWindow(p->hwnd);
    free(p);
}

static void popup_select(Popup *p, int i, HWND owner)
{
    if (i == p->sel) return;
    int old = p->sel;
    p->sel = i;
    HDC dc = GetDC(p->hwnd);
    if (old >= 0 && old < p->menu->n) popup_draw_item(p, dc, old);
    if (i >= 0) popup_draw_item(p, dc, i);
    ReleaseDC(p->hwnd, dc);
    if (i >= 0) {
        struct W16MenuItem *it = &p->menu->it[i];
        SendMessage(owner, WM_MENUSELECT, it->sub ? (WPARAM)(uintptr_t)it->sub : it->id,
                    MAKELPARAM(it->flags | MF_HILITE | (p->menu->is_sys ? MF_SYSMENU : 0), 0));
    }
}

static int popup_item_at(Popup *p, POINT pt)
{
    if (!p || !PtInRect(&p->hwnd->rw, pt)) return -2;
    ScreenToClient(p->hwnd, &pt);
    for (int i = 0; i < p->menu->n; i++)
        if (PtInRect(&p->menu->it[i].rc, pt)) return (p->menu->it[i].flags & MF_SEPARATOR) ? -1 : i;
    return -1;
}

static int next_selectable(HMENU m, int from, int dir)
{
    if (!m->n) return -1;
    int i = from;
    for (int k = 0; k < m->n; k++) {
        i = (i + dir + m->n) % m->n;
        if (!(m->it[i].flags & MF_SEPARATOR)) return i;
    }
    return -1;
}

static int mnemonic_index(HMENU m, int ch)
{
    ch = toupper(ch);
    for (int i = 0; i < m->n; i++)
        if (w16_mnemonic(m->it[i].text) == ch) return i;
    return -1;
}

/* ------------------------------------------------------------------ tracking */
typedef struct {
    HWND h;
    int bar_sel;     /* selected bar item, or -1 */
    int sys;         /* system menu is open/selected */
    Popup *pop[8];   /* stack of open popups */
    int npop;
} Track;

static void bar_hilite(Track *t, int i)
{
    HMENU m = t->h->menu;
    if (!m) return;
    if (t->bar_sel == i) return;
    HDC dc = GetWindowDC(t->h);
    if (t->bar_sel >= 0 && t->bar_sel < m->n) draw_bar_item(t->h, dc, t->bar_sel, 0);
    t->bar_sel = i;
    if (i >= 0) draw_bar_item(t->h, dc, i, 1);
    ReleaseDC(t->h, dc);
    if (i >= 0) {
        struct W16MenuItem *it = &m->it[i];
        SendMessage(t->h, WM_MENUSELECT, it->sub ? (WPARAM)(uintptr_t)it->sub : it->id, MAKELPARAM(it->flags | MF_HILITE, 0));
    }
}

static void sys_hilite(Track *t, int on)
{
    if (t->sys == on) return;
    t->sys = on;
    if (IsIconic(t->h)) return;
    /* invert the system-menu box in the caption */
    HDC dc = GetWindowDC(t->h);
    int l, tp;
    RECT c = t->h->rw;
    (void)c;
    extern void w16_sysbox_rect(HWND h, RECT *r);
    RECT r;
    w16_sysbox_rect(t->h, &r);
    (void)l; (void)tp;
    InvertRect(dc, &r);
    ReleaseDC(t->h, dc);
}

static void close_popups(Track *t, int keep)
{
    while (t->npop > keep) popup_close(t->pop[--t->npop]);
    UpdateWindow(t->h);
}

static void open_bar_popup(Track *t, int i)
{
    HMENU m = t->h->menu;
    close_popups(t, 0);
    if (i < 0 || !m->it[i].sub || (m->it[i].flags & MF_GRAYED)) return;
    int ox, oy;
    bar_origin(t->h, &ox, &oy);
    RECT r = m->it[i].rc;
    OffsetRect(&r, t->h->rw.left + ox, t->h->rw.top + oy);
    t->pop[t->npop++] = popup_open(t->h, m->it[i].sub, r.left, r.bottom, i, 0);
}

static void open_sys_popup(Track *t)
{
    close_popups(t, 0);
    HMENU sm = GetSystemMenu(t->h, FALSE);
    prep_sysmenu(t->h, sm);
    RECT r;
    extern void w16_sysbox_rect(HWND h, RECT *r);
    if (IsIconic(t->h)) {
        int x = t->h->rw.left, y = t->h->rw.top;
        int w, hh, xt;
        popup_layout(sm, &w, &hh, &xt);
        t->pop[t->npop++] = popup_open(t->h, sm, x, y - hh - 1, 0, 1);
        return;
    }
    w16_sysbox_rect(t->h, &r);
    OffsetRect(&r, t->h->rw.left, t->h->rw.top);
    t->pop[t->npop++] = popup_open(t->h, sm, r.left, r.bottom, 0, 1);
}

static void execute(Track *t, Popup *p, int i, UINT *cmd, int *is_sys)
{
    struct W16MenuItem *it = &p->menu->it[i];
    if (it->flags & (MF_GRAYED | MF_DISABLED)) { *cmd = 0; return; }
    *cmd = it->id;
    *is_sys = p->menu->is_sys;
    (void)t;
}

static void run_tracking(Track *t, int start_open, int keyboard)
{
    HWND h = t->h;
    UINT cmd = 0;
    int is_sys = 0, done = 0;
    SetCapture(h);
    if (start_open) {
        if (t->sys) open_sys_popup(t);
        else if (t->bar_sel >= 0) open_bar_popup(t, t->bar_sel);
        if (keyboard && t->npop) popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h);
    }
    int mouse_down = (w16_keystate[VK_LBUTTON] & 0x80) != 0;
    MSG m;
    while (!done) {
        w16_present();
        if (!GetMessage(&m, NULL, 0, 0)) { PostQuitMessage(m.wParam); break; }
        Popup *top = t->npop ? t->pop[t->npop - 1] : NULL;
        switch (m.message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: {
            POINT pt = m.pt;
            if (m.message == WM_LBUTTONDOWN || m.message == WM_LBUTTONDBLCLK) mouse_down = 1;
            /* inside an open popup? */
            int hitpop = -1, item = -2;
            for (int k = t->npop - 1; k >= 0; k--) {
                item = popup_item_at(t->pop[k], pt);
                if (item != -2) { hitpop = k; break; }
            }
            if (hitpop >= 0) {
                if (hitpop < t->npop - 1) close_popups(t, hitpop + 1);
                Popup *p = t->pop[hitpop];
                if (mouse_down || m.message == WM_LBUTTONUP || p->sel >= 0 || 1) popup_select(p, item, h);
                if (item >= 0 && p->menu->it[item].sub && !(p->menu->it[item].flags & MF_GRAYED) && t->npop == hitpop + 1) {
                    RECT r = p->menu->it[item].rc;
                    OffsetRect(&r, p->hwnd->rc.left, p->hwnd->rc.top);
                    t->pop[t->npop++] = popup_open(h, p->menu->it[item].sub, r.right - 3, r.top - 1, item, 0);
                }
                if (m.message == WM_LBUTTONUP && item >= 0 && !p->menu->it[item].sub) {
                    execute(t, p, item, &cmd, &is_sys);
                    done = 1;
                }
                break;
            }
            /* over the menu bar? */
            int wx = pt.x - h->rw.left, wy = pt.y - h->rw.top;
            int bi = -1;
            if (h->menu && !IsIconic(h)) {
                int ox, oy;
                bar_origin(h, &ox, &oy);
                for (int i = 0; i < h->menu->n; i++) {
                    RECT r = h->menu->it[i].rc;
                    OffsetRect(&r, ox, oy);
                    if (PtInRect(&r, (POINT){wx, wy})) bi = i;
                }
            }
            extern void w16_sysbox_rect(HWND h, RECT *r);
            RECT sb;
            int over_sys = 0;
            if ((h->style & WS_SYSMENU) && !IsIconic(h)) {
                w16_sysbox_rect(h, &sb);
                over_sys = PtInRect(&sb, (POINT){wx, wy});
            } else if (IsIconic(h))
                over_sys = PtInRect(&h->rw, pt);
            if (bi >= 0 && (mouse_down || m.message != WM_MOUSEMOVE || t->npop)) {
                if (bi != t->bar_sel || t->sys) {
                    sys_hilite(t, 0);
                    bar_hilite(t, bi);
                    open_bar_popup(t, bi);
                } else if (m.message == WM_LBUTTONDOWN && t->npop && !keyboard) {
                    /* clicking the open item again closes the menu */
                    done = 1;
                }
                if (m.message == WM_LBUTTONUP && !t->npop) {
                    /* item without a popup: execute */
                    struct W16MenuItem *it = &h->menu->it[bi];
                    if (!it->sub && !(it->flags & MF_GRAYED)) { cmd = it->id; done = 1; }
                }
                if (t->npop && t->pop[0]->sel >= 0 && m.message == WM_MOUSEMOVE) popup_select(t->pop[0], -1, h);
                break;
            }
            if (over_sys && (mouse_down || m.message != WM_MOUSEMOVE)) {
                if (!t->sys) {
                    bar_hilite(t, -1);
                    sys_hilite(t, 1);
                    open_sys_popup(t);
                }
                break;
            }
            if (top && m.message == WM_MOUSEMOVE && top->sel >= 0 && !top->menu->it[top->sel].sub) popup_select(top, -1, h);
            if (m.message == WM_LBUTTONDOWN) done = 1; /* click elsewhere cancels */
            if (m.message == WM_LBUTTONUP && !t->npop) done = 1;
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)m.wParam;
            HMENU bar = h->menu;
            TranslateMessage(&m); /* like USER's menu loop: letters arrive as WM_CHAR for the mnemonics */
            switch (vk) {
            case VK_ESCAPE:
                if (t->npop > 1) close_popups(t, t->npop - 1);
                else if (t->npop == 1 && keyboard) {
                    close_popups(t, 0);
                } else done = 1;
                break;
            case VK_MENU:
            case VK_F10:
                done = 1;
                break;
            case VK_LEFT:
            case VK_RIGHT: {
                int dir = vk == VK_RIGHT ? 1 : -1;
                if (top && dir > 0 && top->sel >= 0 && top->menu->it[top->sel].sub && !top->menu->is_sys) {
                    Popup *p = top;
                    RECT r = p->menu->it[p->sel].rc;
                    OffsetRect(&r, p->hwnd->rc.left, p->hwnd->rc.top);
                    t->pop[t->npop++] = popup_open(h, p->menu->it[p->sel].sub, r.right - 3, r.top - 1, p->sel, 0);
                    popup_select(t->pop[t->npop - 1], next_selectable(t->pop[t->npop - 1]->menu, -1, 1), h);
                    break;
                }
                if (t->npop > 1 && dir < 0) { close_popups(t, t->npop - 1); break; }
                /* move along the bar; the system menu sits left of the first item */
                int was_open = t->npop > 0;
                int n = bar ? bar->n : 0;
                int pos = t->sys ? -1 : t->bar_sel;
                int has_sys = (h->style & WS_SYSMENU) != 0;
                for (int k = 0; k < n + 1; k++) {
                    pos += dir;
                    if (pos < -1) pos = n - 1;
                    if (pos >= n) pos = -1;
                    if (pos == -1 && has_sys) break;
                    if (pos >= 0) break;
                }
                if (pos == -1) { bar_hilite(t, -1); sys_hilite(t, 1); if (was_open) { open_sys_popup(t); popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h); } else close_popups(t, 0); }
                else {
                    sys_hilite(t, 0);
                    bar_hilite(t, pos);
                    if (was_open) {
                        open_bar_popup(t, pos);
                        if (t->npop) popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h);
                    } else close_popups(t, 0);
                }
                break;
            }
            case VK_UP:
            case VK_DOWN:
                if (!top) {
                    if (t->sys) open_sys_popup(t);
                    else if (t->bar_sel >= 0) open_bar_popup(t, t->bar_sel);
                    if (t->npop) popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h);
                } else
                    popup_select(top, next_selectable(top->menu, top->sel < 0 ? (vk == VK_DOWN ? -1 : 0) : top->sel, vk == VK_DOWN ? 1 : -1), h);
                break;
            case VK_RETURN:
                if (top && top->sel >= 0) {
                    struct W16MenuItem *it = &top->menu->it[top->sel];
                    if (it->sub) {
                        RECT r = it->rc;
                        OffsetRect(&r, top->hwnd->rc.left, top->hwnd->rc.top);
                        t->pop[t->npop++] = popup_open(h, it->sub, r.right - 3, r.top - 1, top->sel, 0);
                        popup_select(t->pop[t->npop - 1], next_selectable(t->pop[t->npop - 1]->menu, -1, 1), h);
                    } else { execute(t, top, top->sel, &cmd, &is_sys); done = 1; }
                } else if (!top) {
                    if (t->sys) open_sys_popup(t);
                    else if (t->bar_sel >= 0) {
                        struct W16MenuItem *it = &bar->it[t->bar_sel];
                        if (!it->sub) { if (!(it->flags & MF_GRAYED)) cmd = it->id; done = 1; break; }
                        open_bar_popup(t, t->bar_sel);
                    }
                    if (t->npop) popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h);
                }
                break;
            }
            break;
        }
        case WM_CHAR:
        case WM_SYSCHAR: {
            int ch = (int)m.wParam;
            if (ch == 27 || ch == '\r') break;
            if (top) {
                int i = mnemonic_index(top->menu, ch);
                if (i >= 0) {
                    popup_select(top, i, h);
                    struct W16MenuItem *it = &top->menu->it[i];
                    if (it->sub) {
                        RECT r = it->rc;
                        OffsetRect(&r, top->hwnd->rc.left, top->hwnd->rc.top);
                        t->pop[t->npop++] = popup_open(h, it->sub, r.right - 3, r.top - 1, i, 0);
                        popup_select(t->pop[t->npop - 1], next_selectable(t->pop[t->npop - 1]->menu, -1, 1), h);
                    } else { execute(t, top, i, &cmd, &is_sys); done = 1; }
                } else MessageBeep(0);
            } else if (h->menu) {
                int i = mnemonic_index(h->menu, ch);
                if (ch == ' ' && (h->style & WS_SYSMENU)) { bar_hilite(t, -1); sys_hilite(t, 1); open_sys_popup(t); popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h); }
                else if (i >= 0) {
                    sys_hilite(t, 0);
                    bar_hilite(t, i);
                    if (h->menu->it[i].sub) {
                        open_bar_popup(t, i);
                        if (t->npop) popup_select(t->pop[0], next_selectable(t->pop[0]->menu, -1, 1), h);
                    } else { if (!(h->menu->it[i].flags & MF_GRAYED)) cmd = h->menu->it[i].id; done = 1; }
                } else MessageBeep(0);
            }
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            break;
        default:
            if (m.message == WM_PAINT || m.message == WM_TIMER) DispatchMessage(&m);
            break;
        }
    }
    close_popups(t, 0);
    bar_hilite(t, -1);
    sys_hilite(t, 0);
    ReleaseCapture();
    SendMessage(h, WM_MENUSELECT, 0, MAKELPARAM(0xFFFF, 0));
    SendMessage(h, WM_EXITMENULOOP, 0, 0);
    if (cmd) {
        if (is_sys) PostMessage(h, WM_SYSCOMMAND, cmd, 0);
        else PostMessage(h, WM_COMMAND, cmd, 0);
    }
}

void w16_menu_track_bar(HWND h, int x, int y, int key_char, int by_key)
{
    Track t = {h, -1, 0, {0}, 0};
    SendMessage(h, WM_ENTERMENULOOP, 0, 0);
    SendMessage(h, WM_INITMENU, (WPARAM)(uintptr_t)h->menu, 0);
    if (!h->menu) return;
    if (by_key) {
        if (key_char) {
            int i = mnemonic_index(h->menu, key_char);
            if (i < 0) {
                MessageBeep(0);
                return;
            }
            bar_hilite(&t, i);
            if (!h->menu->it[i].sub) {
                if (!(h->menu->it[i].flags & MF_GRAYED)) PostMessage(h, WM_COMMAND, h->menu->it[i].id, 0);
                bar_hilite(&t, -1);
                return;
            }
            run_tracking(&t, 1, 1);
        } else {
            bar_hilite(&t, next_selectable(h->menu, -1, 1));
            run_tracking(&t, 0, 1);
        }
        return;
    }
    /* mouse: select the item under the cursor and open it */
    int ox, oy;
    bar_origin(h, &ox, &oy);
    for (int i = 0; i < h->menu->n; i++) {
        RECT r = h->menu->it[i].rc;
        OffsetRect(&r, h->rw.left + ox, h->rw.top + oy);
        if (PtInRect(&r, (POINT){x, y})) {
            bar_hilite(&t, i);
            break;
        }
    }
    if (t.bar_sel < 0) return;
    run_tracking(&t, 1, 0);
}

void w16_menu_track_sys(HWND h, int by_key)
{
    Track t = {h, -1, 0, {0}, 0};
    SendMessage(h, WM_ENTERMENULOOP, 0, 0);
    SendMessage(h, WM_INITMENU, (WPARAM)(uintptr_t)GetSystemMenu(h, FALSE), 0);
    sys_hilite(&t, 1);
    run_tracking(&t, 1, by_key);
}

BOOL TrackPopupMenu(HMENU m, UINT flags, int x, int y, int r, HWND h, LPCRECT rc)
{
    (void)r; (void)rc;
    Track t = {h, -1, 0, {0}, 0};
    int w, hh, xt;
    popup_layout(m, &w, &hh, &xt);
    if (flags & TPM_CENTERALIGN) x -= w / 2;
    else if (flags & TPM_RIGHTALIGN) x -= w;
    SendMessage(h, WM_INITMENU, (WPARAM)(uintptr_t)m, 0);
    t.pop[t.npop++] = popup_open(h, m, x, y, 0, 0);
    run_tracking(&t, 0, 0);
    return TRUE;
}

/* ------------------------------------------------------------------ accelerators */
struct W16Accel { int n; struct { BYTE fl; WORD key, cmd; } a[256]; };

HACCEL LoadAccelerators(HINSTANCE h, LPCSTR name)
{
    const W16Res *r = w16_find_res(h, name, RT_ACCELERATOR);
    if (!r) return NULL;
    const uint8_t *d = w16_res_data(h, r);
    HACCEL a = calloc(1, sizeof *a);
    for (uint32_t p = 0; p + 5 <= r->len && a->n < 256; p += 5) {
        a->a[a->n].fl = d[p];
        a->a[a->n].key = u16(d + p + 1);
        a->a[a->n].cmd = u16(d + p + 3);
        a->n++;
        if (d[p] & 0x80) break;
    }
    return a;
}

int TranslateAccelerator(HWND h, HACCEL a, LPMSG m)
{
    if (!a || !w16_valid(h)) return 0;
    int vk = m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN;
    int ch = m->message == WM_CHAR || m->message == WM_SYSCHAR;
    if (!vk && !ch) return 0;
    int shift = (w16_keystate[VK_SHIFT] & 0x80) != 0, ctrl = (w16_keystate[VK_CONTROL] & 0x80) != 0,
        alt = (w16_keystate[VK_MENU] & 0x80) != 0;
    for (int i = 0; i < a->n; i++) {
        BYTE fl = a->a[i].fl;
        int match = 0;
        if (fl & FVIRTKEY) {
            if (vk && a->a[i].key == m->wParam && !!(fl & FSHIFT) == shift && !!(fl & FCONTROL) == ctrl && !!(fl & FALT) == alt) match = 1;
        } else if (ch) {
            if (a->a[i].key == m->wParam && (!(fl & FALT) || alt)) match = 1;
        }
        if (!match) continue;
        HWND t = h;
        if (IsIconic(t)) {
            SendMessage(t, WM_SYSCOMMAND, a->a[i].cmd, 0);
            return 1;
        }
        /* disabled menu items do not fire */
        HMENU mm = t->menu;
        HMENU o;
        int idx = mm ? find(mm, a->a[i].cmd, 0, &o) : -1;
        if (idx >= 0) {
            SendMessage(t, WM_INITMENU, (WPARAM)(uintptr_t)mm, 0);
            /* the popup containing the item gets WM_INITMENUPOPUP so its state is fresh */
            for (int k = 0; k < mm->n; k++) {
                if (mm->it[k].sub) {
                    HMENU oo;
                    if (find(mm->it[k].sub, a->a[i].cmd, 0, &oo) >= 0)
                        SendMessage(t, WM_INITMENUPOPUP, (WPARAM)mm->it[k].sub, MAKELPARAM(k, 0));
                }
            }
            idx = find(mm, a->a[i].cmd, 0, &o);
            if (idx >= 0 && (o->it[idx].flags & (MF_GRAYED | MF_DISABLED))) return 1;
        }
        SendMessage(t, WM_COMMAND, a->a[i].cmd, MAKELPARAM(0, 1));
        return 1;
    }
    return 0;
}
