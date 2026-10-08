/* icons and cursors from the user's files (application modules and VGA.DRV) */
#include "w16int.h"

struct W16Icon {
    int w, h, hotx, hoty;
    uint32_t *xorpx;   /* 0xRRGGBB */
    uint8_t *andm;     /* 1 = transparent (screen kept / inverted) */
    int cursor;
    void *native;      /* SDL_Cursor for cursors in windowed mode */
};

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }

static HICON from_dib(const uint8_t *d, int cursor)
{
    int hot_x = 0, hot_y = 0;
    if (cursor) { hot_x = u16(d); hot_y = u16(d + 2); d += 4; }
    uint32_t hs = u32(d);
    int w, h2, bpp, ncol;
    if (hs == 12) { w = u16(d + 4); h2 = u16(d + 6); bpp = u16(d + 10); ncol = 1 << bpp; }
    else { w = (int32_t)u32(d + 4); h2 = (int32_t)u32(d + 8); bpp = u16(d + 14); ncol = u32(d + 32); if (!ncol && bpp <= 8) ncol = 1 << bpp; }
    int h = h2 / 2;
    int pal4 = hs != 12;
    const uint8_t *pal = d + hs;
    const uint8_t *xb = pal + ncol * (pal4 ? 4 : 3);
    int xs = ((w * bpp + 31) / 32) * 4;
    const uint8_t *ab = xb + xs * h;
    int as = ((w + 31) / 32) * 4;
    HICON ic = calloc(1, sizeof *ic);
    ic->w = w; ic->h = h; ic->hotx = hot_x; ic->hoty = hot_y; ic->cursor = cursor;
    ic->xorpx = calloc(w * h, 4);
    ic->andm = calloc(w * h, 1);
    for (int y = 0; y < h; y++) {
        const uint8_t *xr = xb + (h - 1 - y) * xs, *ar = ab + (h - 1 - y) * as;
        for (int x = 0; x < w; x++) {
            int idx = 0;
            uint32_t p = 0;
            if (bpp == 1) idx = (xr[x / 8] >> (7 - (x & 7))) & 1;
            else if (bpp == 4) idx = (xr[x / 2] >> ((x & 1) ? 0 : 4)) & 15;
            else if (bpp == 8) idx = xr[x];
            if (bpp <= 8) {
                const uint8_t *e = pal + idx * (pal4 ? 4 : 3);
                p = (e[2] << 16) | (e[1] << 8) | e[0];
            } else if (bpp == 24) p = (xr[x * 3 + 2] << 16) | (xr[x * 3 + 1] << 8) | xr[x * 3];
            ic->xorpx[y * w + x] = p;
            ic->andm[y * w + x] = (ar[x / 8] >> (7 - (x & 7))) & 1;
        }
    }
    return ic;
}

/* USER's choice of image from a group directory (GetIconId, seg12:052B). An icon (seg12:040C): the
 * one at the icon size with the display's colour count (16 on VGA); else, at that size, the one with
 * the most colours under it; else, at any size, the most colours not over it; else the first. The
 * VGA driver's own icons need this: IDI_ASTERISK has an 8-colour image with a bright blue disc before
 * the 16-colour one with the dark blue disc 3.11 shows. A cursor (seg12:04F7): the first of the
 * cursor size, else the first. */
static int pick_image(const uint8_t *d, int n, int cursor)
{
    if (cursor) {
        for (int i = 0; i < n; i++) {
            const uint8_t *e = d + 6 + i * 14;
            if (u16(e) == GetSystemMetrics(SM_CXCURSOR) && u16(e + 2) / 2 == GetSystemMetrics(SM_CYCURSOR)) return i;
        }
        return 0;
    }
    int bits = GetDeviceCaps(NULL, BITSPIXEL) * GetDeviceCaps(NULL, PLANES);
    int disp = bits >= 16 ? 32000 : 1 << bits;
    int fewer = 0, ifewer = 0, any = 0, iany = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = d + 6 + i * 14;
        int colors = e[2], w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        if (colors <= disp && any < colors) { any = colors; iany = i; }
        if (w != GetSystemMetrics(SM_CXICON) || h != GetSystemMetrics(SM_CYICON)) continue;
        if (colors == disp) return i;
        if (colors < disp && fewer < colors) { fewer = colors; ifewer = i; }
    }
    return fewer ? ifewer : any ? iany : 0;
}

static HICON load_group(HINSTANCE m, LPCSTR name, int cursor)
{
    const W16Res *g = w16_find_res(m, name, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON);
    if (!g) return NULL;
    const uint8_t *d = w16_res_data(m, g);
    int n = u16(d + 4);
    if (n <= 0) return NULL;
    int best = u16(d + 6 + pick_image(d, n, cursor) * 14 + 12);
    const W16Res *r = w16_find_res(m, MAKEINTRESOURCE(best), cursor ? RT_CURSOR : RT_ICON);
    if (!r) return NULL;
    HICON ic = from_dib(w16_res_data(m, r), cursor);
    if (ic && !cursor && (ic->w != 32 || ic->h != 32) && ic->w > 0 && ic->h > 0) {
        /* an icon of another size is stretched to the display's 32 x 32 (SYSEDIT's child icon is
         * only 64 x 64 monochrome; real 3.11 shows it at 32 x 32 - measured: SysEdit's minimised
         * children). Shrunk as StretchBlt's BLACKONWHITE mode: of the pixels that become one, the
         * darkest wins and the mask stays opaque if any is (thin black lines survive, as measured) */
        uint32_t *x = calloc(32 * 32, sizeof *x);
        uint8_t *a = calloc(32 * 32, 1);
        for (int y = 0; y < 32; y++)
            for (int xx = 0; xx < 32; xx++) {
                int x0 = xx * ic->w / 32, x1 = max(x0 + 1, (xx + 1) * ic->w / 32);
                int y0 = y * ic->h / 32, y1 = max(y0 + 1, (y + 1) * ic->h / 32);
                uint32_t best = 0xFFFFFFFF;
                int lum = 1 << 30, m = 1;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++) {
                        uint32_t p = ic->xorpx[sy * ic->w + sx];
                        int l = (p >> 16 & 255) + (p >> 8 & 255) + (p & 255);
                        if (l < lum) { lum = l; best = p; }
                        m &= ic->andm[sy * ic->w + sx];
                    }
                x[y * 32 + xx] = best;
                a[y * 32 + xx] = m;
            }
        free(ic->xorpx);
        free(ic->andm);
        ic->xorpx = x;
        ic->andm = a;
        ic->w = ic->h = 32;
    }
    return ic;
}

static HICON cache_get(HINSTANCE m, LPCSTR name, int cursor)
{
    typedef struct C { HINSTANCE m; uintptr_t id; char nm[64]; int cur; HICON ic; struct C *next; } C;
    static C *cache;
    for (C *c = cache; c; c = c->next)
        if (c->m == m && c->cur == cursor &&
            (IS_INTRESOURCE(name) ? c->id == (uintptr_t)name : (!c->id && !strcasecmp(c->nm, name))))
            return c->ic;
    HICON ic = load_group(m, name, cursor);
    if (!ic) return NULL;
    C *c = calloc(1, sizeof *c);
    c->m = m; c->cur = cursor; c->ic = ic;
    if (IS_INTRESOURCE(name)) c->id = (uintptr_t)name;
    else snprintf(c->nm, sizeof c->nm, "%s", name);
    c->next = cache;
    cache = c;
    return ic;
}

HCURSOR LoadCursor(HINSTANCE h, LPCSTR name)
{
    if (!h) {
        HINSTANCE drv = w16_system_module("VGA.DRV");
        HCURSOR c = drv ? cache_get(drv, name, 1) : NULL;
        if (!c) { HINSTANCE u = w16_system_module("USER.EXE"); c = u ? cache_get(u, name, 1) : NULL; }
        return c;
    }
    return cache_get(h, name, 1);
}

HICON LoadIcon(HINSTANCE h, LPCSTR name)
{
    if (!h) {
        HINSTANCE drv = w16_system_module("VGA.DRV");
        HICON c = drv ? cache_get(drv, name, 0) : NULL;
        if (!c) { HINSTANCE u = w16_system_module("USER.EXE"); c = u ? cache_get(u, name, 0) : NULL; }
        return c;
    }
    return cache_get(h, name, 0);
}

BOOL DestroyIcon(HICON i) { (void)i; return TRUE; }
HICON w16_icon_for_size(HICON i, int w, int h) { (void)w; (void)h; return i; }

BOOL DrawIcon(HDC dc, int x, int y, HICON ic)
{
    if (!ic) return FALSE;
    w16_lp_to_dp(dc, &x, &y);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    W16Bitmap *t = dc->target ? dc->target : &w16_screen;
    for (int yy = 0; yy < ic->h; yy++)
        for (int xx = 0; xx < ic->w; xx++) {
            int px = x + xx, py = y + yy;
            if (px < 0 || py < 0 || px >= t->w || py >= t->h || !rgn_contains(&e, px, py)) continue;
            uint32_t *d = &t->px[py * t->w + px];
            uint32_t v = ic->andm[yy * ic->w + xx] ? *d : 0;
            *d = (v ^ ic->xorpx[yy * ic->w + xx]) & 0xFFFFFF;
        }
    rgn_free(&e);
    if (!dc->target) w16_screen_dirty = 1;
    return TRUE;
}

/* accessors for the presenter (SDL cursor creation / software cursor) */
void w16_icon_info(HICON ic, int *w, int *h, int *hx, int *hy, const uint32_t **xorpx, const uint8_t **andm)
{
    *w = ic->w; *h = ic->h; *hx = ic->hotx; *hy = ic->hoty; *xorpx = ic->xorpx; *andm = ic->andm;
}
void **w16_icon_native(HICON ic) { return &ic->native; }

/* standard 16-colour order of 4bpp icon/DIB pixels */
static const uint32_t std16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

HICON CreateIcon(HINSTANCE inst, int w, int h, BYTE planes, BYTE bpp, const void *andbits, const void *xorbits)
{
    (void)inst;
    if (w <= 0 || h <= 0 || planes != 1 || (bpp != 1 && bpp != 4)) return NULL;
    const uint8_t *a = andbits, *x = xorbits;
    int as = ((w + 15) / 16) * 2, xs = ((w * bpp + 15) / 16) * 2; /* WORD-aligned rows, top-down */
    HICON ic = calloc(1, sizeof *ic);
    ic->w = w; ic->h = h;
    ic->xorpx = calloc((size_t)w * h, 4);
    ic->andm = calloc((size_t)w * h, 1);
    for (int y = 0; y < h; y++)
        for (int i = 0; i < w; i++) {
            int idx = bpp == 1 ? (x[y * xs + i / 8] >> (7 - (i & 7))) & 1 : (x[y * xs + i / 2] >> ((i & 1) ? 0 : 4)) & 15;
            ic->xorpx[y * w + i] = bpp == 1 ? (idx ? 0xFFFFFF : 0) : std16[idx];
            ic->andm[y * w + i] = (a[y * as + i / 8] >> (7 - (i & 7))) & 1;
        }
    return ic;
}
