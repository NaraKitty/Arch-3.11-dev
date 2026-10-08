/* GDI palettes and bitmap bits.
 * Palettes: the reference display is 16-colour VGA, which is not a palette device (no RC_PALETTE), so
 * a logical palette only stores its entries: SelectPalette returns the previous one, RealizePalette
 * maps nothing and returns 0, UpdateColors does nothing. UNTESTED against 3.1's GDI return values.
 * Bitmap bits: rows are word aligned as GetObject reports them - 1 bit per pixel for monochrome
 * bitmaps, else (planes 1, 4 bits per pixel) one VGA colour index per nibble, high nibble first.
 * CreateBitmapIndirect / SetBitmapBits also take VGA.DRV's own layout (4 planes of 1 bit, the planes
 * of each scan line one after the other, plane 0 = bit 0 of the index), as .CLP files written by real
 * 3.11 hold it (UNTESTED: no such file compared yet). */
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

HPALETTE SelectPalette(HDC dc, HPALETTE p, BOOL bkgnd)
{
    static HPALETTE current;
    (void)dc; (void)bkgnd;
    HPALETTE old = current ? current : GetStockObject(DEFAULT_PALETTE);
    current = p;
    return old;
}

UINT RealizePalette(HDC dc) { (void)dc; return 0; }
int UpdateColors(HDC dc) { (void)dc; return 0; }

static int row_bytes(int w, int bits) { return ((w * bits + 15) / 16) * 2; }

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
                b->px[y * b->w + x] = w16_vga_color(i);
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
    put_bits(b, 1, b->mono ? 1 : 4, in, cb);
    return (LONG)cb;
}

LONG GetBitmapBits(HBITMAP h, LONG cb, void *out)
{
    W16Bitmap *b = w16_bitmap_of(h);
    if (!b || !out || cb <= 0) return 0;
    int bits = b->mono ? 1 : 4, wb = row_bytes(b->w, bits);
    LONG total = (LONG)wb * b->h;
    if (cb > total) cb = total;
    uint8_t *d = out;
    memset(d, 0, cb);
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            LONG at = (LONG)y * wb + (b->mono ? x / 8 : x / 2);
            if (at >= cb) return cb;
            uint32_t p = b->px[y * b->w + x];
            if (b->mono) { if (p & 0xFFFFFF) d[at] |= (uint8_t)(0x80 >> (x & 7)); }
            else d[at] |= (uint8_t)(x & 1 ? w16_vga_index(p) : w16_vga_index(p) << 4);
        }
    return cb;
}
