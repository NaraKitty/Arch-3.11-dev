/* GDI palettes and bitmap bits.
 * Palettes: the reference display is 16-colour VGA, which is not a palette device (no RC_PALETTE), so
 * a logical palette only stores its entries: SelectPalette returns the previous one, RealizePalette
 * maps nothing and returns 0, UpdateColors does nothing. UNTESTED against 3.1's GDI return values.
 * Bitmap bits: rows are word aligned as GetObject reports them - 1 bit per pixel for monochrome
 * bitmaps, else VGA.DRV's device layout: 4 planes of 1 bit, the planes of each scan line one after
 * the other, the planes' bits forming the hardware pixel value, which is VGA.DRV's colour index with
 * 7 and 8 swapped (its attribute table). Measured: a .CLP bitmap in this layout shows the same
 * colours on 3.11 and here, and 3.11's Clipboard Viewer writes GetObject's BITMAP (planes 4, 1 bit,
 * bmBits 0) and GetBitmapBits' bits back byte for byte. CreateBitmapIndirect still reads
 * (planes 1, 4 bits) one colour index per nibble, high nibble first. */
#include "w16int.h"

int w16_vga_index(uint32_t rgb);
uint32_t w16_vga_color(int i);

HPALETTE CreatePalette(const LOGPALETTE *lp)
{
    if (!lp) return NULL;
    HGDIOBJ o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = OBJ_PAL;
    o->u.pal.n = lp->palNumEntries;
    o->u.pal.e = calloc(lp->palNumEntries ? lp->palNumEntries : 1, sizeof(PALETTEENTRY));
    if (o->u.pal.e) memcpy(o->u.pal.e, lp->palPalEntry, sizeof(PALETTEENTRY) * lp->palNumEntries);
    return o;
}

UINT GetPaletteEntries(HPALETTE p, UINT start, UINT n, PALETTEENTRY *out)
{
    if (!p || p->kind != OBJ_PAL) return 0;
    if (start >= (UINT)p->u.pal.n) return 0;
    if (n > (UINT)p->u.pal.n - start) n = (UINT)p->u.pal.n - start;
    if (out && p->u.pal.e) memcpy(out, p->u.pal.e + start, sizeof *out * n);
    return n;
}

/* the DC keeps its palette: a brush of PALETTEINDEX(i) shows that palette's entry i (measured: the
 * Clipboard Viewer's Palette view on 3.11's VGA shows each entry's colour, dithered as an RGB brush) */
HPALETTE SelectPalette(HDC dc, HPALETTE p, BOOL bkgnd)
{
    (void)bkgnd;
    HPALETTE def = GetStockObject(DEFAULT_PALETTE);
    if (!dc || (p && p->kind != OBJ_PAL)) return NULL;
    HPALETTE old = dc->pal ? dc->pal : def;
    dc->pal = p == def ? NULL : p;
    return old;
}

/* a PALETTEINDEX colour as the RGB of the DC's palette entry (left as it is without one) */
COLORREF w16_palette_color(HDC dc, COLORREF c)
{
    if ((c >> 24) != 1 || !dc || !dc->pal || !dc->pal->u.pal.e) return c;
    WORD i = (WORD)c;
    if (i >= dc->pal->u.pal.n) i = 0;
    const PALETTEENTRY *e = &dc->pal->u.pal.e[i];
    return RGB(e->peRed, e->peGreen, e->peBlue);
}

UINT RealizePalette(HDC dc) { (void)dc; return 0; }
int UpdateColors(HDC dc) { (void)dc; return 0; }

static int row_bytes(int w, int bits) { return ((w * bits + 15) / 16) * 2; }

/* hardware pixel value <-> VGA.DRV colour index (the same swap both ways) */
static int hw_index(int v) { return v == 7 ? 8 : v == 8 ? 7 : v; }

/* load packed bits into a bitmap's pixels */
static void put_bits(W16Bitmap *b, int planes, int bpp, const uint8_t *s, size_t cb)
{
    if (planes * bpp == 1) {
        int wb = row_bytes(b->w, 1);
        for (int y = 0; y < b->h && (size_t)(y + 1) * wb <= cb; y++)
            for (int x = 0; x < b->w; x++)
                b->px[y * b->w + x] = (s[y * wb + x / 8] & (0x80 >> (x & 7))) ? 0xFFFFFF : 0;
    } else if (planes == 4 && bpp == 1) {
        int wb = row_bytes(b->w, 1);
        for (int y = 0; y < b->h && (size_t)(y + 1) * wb * 4 <= cb; y++)
            for (int x = 0; x < b->w; x++) {
                int i = 0;
                for (int p = 0; p < 4; p++)
                    if (s[(y * 4 + p) * wb + x / 8] & (0x80 >> (x & 7))) i |= 1 << p;
                b->px[y * b->w + x] = w16_vga_color(hw_index(i));
            }
    } else {
        int wb = row_bytes(b->w, 4);
        for (int y = 0; y < b->h && (size_t)(y + 1) * wb <= cb; y++)
            for (int x = 0; x < b->w; x++) {
                uint8_t v = s[y * wb + x / 2];
                b->px[y * b->w + x] = w16_vga_color(x & 1 ? v & 15 : v >> 4);
            }
    }
}

HBITMAP CreateBitmapIndirect(const BITMAP *bm)
{
    if (!bm) return NULL;
    int planes = bm->bmPlanes ? bm->bmPlanes : 1, bpp = bm->bmBitsPixel ? bm->bmBitsPixel : 1;
    HBITMAP h = CreateBitmap(bm->bmWidth, bm->bmHeight, 1, planes * bpp == 1 ? 1 : 4, NULL);
    W16Bitmap *b = w16_bitmap_of(h);
    if (b && bm->bmBits) {
        size_t cb = (size_t)(planes == 4 && bpp == 1 ? row_bytes(b->w, 1) * 4 : row_bytes(b->w, planes * bpp == 1 ? 1 : 4)) * b->h;
        put_bits(b, planes, bpp, bm->bmBits, cb);
    }
    return h;
}

LONG SetBitmapBits(HBITMAP h, DWORD cb, const void *in)
{
    W16Bitmap *b = w16_bitmap_of(h);
    if (!b || !in) return 0;
    put_bits(b, b->mono ? 1 : 4, 1, in, cb);
    return (LONG)cb;
}

LONG GetBitmapBits(HBITMAP h, LONG cb, void *out)
{
    W16Bitmap *b = w16_bitmap_of(h);
    if (!b || !out || cb <= 0) return 0;
    int planes = b->mono ? 1 : 4, wb = row_bytes(b->w, 1);
    LONG total = (LONG)wb * planes * b->h;
    if (cb > total) cb = total;
    uint8_t *d = out;
    memset(d, 0, cb);
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            uint32_t p = b->px[y * b->w + x];
            int v = b->mono ? (p & 0xFFFFFF) != 0 : hw_index(w16_vga_index(p));
            for (int k = 0; k < planes; k++) {
                LONG at = ((LONG)y * planes + k) * wb + x / 8;
                if (at < cb && (v >> k & 1)) d[at] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    return cb;
}
