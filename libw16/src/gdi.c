/* GDI: objects, device contexts, raster operations, primitives.
 * The display is emulated as the 3.11 VGA driver sees it: 16 colours by default
 * (W16_COLORS=256|24 for richer output on modern screens). */
#include "w16int.h"
#include "commdlg.h" /* Escape / SP_* */
#include <math.h>

W16Bitmap w16_screen;
int w16_screen_dirty;
/* Default: true colour, so custom themes can use any colour. The default scheme only uses
 * the 16 VGA colours, so it looks exactly like 3.11 on VGA. W16_COLORS=16 emulates the VGA
 * driver's palette and dithering ("16-colour mode" in Display Settings). */
static int ncolors = 1 << 24;

static const uint32_t vga16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

/* What a VGA monitor shows for those 16 colours. VGA.DRV (seg4:00F0) loads the attribute table
 * 00 0C 0A 0E 01 15 23 07 0F 24 12 36 09 2D 1B 3F and reprograms DAC 7 = (33,34,35) and
 * DAC 15 = (48,49,50); the other entries keep the BIOS EGA colours. 6-bit DAC values appear as
 * v << 2 | v >> 4. Verified swatch by swatch against Paintbrush on real 3.11 (DOSBox-X).
 * The framebuffer keeps the logical colours (GetPixel, palette-index inversion); only what
 * reaches the screen or a screenshot is converted. W16_DAC=ideal shows the logical colours. */
static const uint32_t vgadac16[16] = {
    0x000000, 0xAA0055, 0x00AA55, 0xAAAA55, 0x0000AA, 0xAA55AA, 0x55AAAA, 0xC3C7CB,
    0x868A8E, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
static int dac_ideal;
uint32_t w16_display_px(uint32_t p)
{
    p &= 0xFFFFFF;
    if (dac_ideal) return p;
    for (int i = 0; i < 16; i++)
        if (vga16[i] == p) return vgadac16[i];
    return p;
}
/* the whole screen converted for presentation */
const uint32_t *w16_display_frame(void)
{
    static uint32_t *buf;
    static size_t cap;
    size_t n = (size_t)w16_screen.w * w16_screen.h;
    if (dac_ideal) return w16_screen.px;
    if (n > cap) { free(buf); buf = malloc(n * 4); cap = n; }
    uint32_t last_in = 0xFFFFFFFF, last_out = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = w16_screen.px[i];
        if (p != last_in) { last_in = p; last_out = w16_display_px(p) | (p & 0xFF000000); }
        buf[i] = last_out;
    }
    return buf;
}

void w16_screen_init(int w, int h)
{
    w16_screen.w = w;
    w16_screen.h = h;
    w16_screen.mono = 0;
    w16_screen.px = calloc((size_t)w * h, 4);
    const char *c = getenv("W16_COLORS");
    if (c) ncolors = atoi(c) == 16 ? 16 : atoi(c) == 256 ? 256 : 1 << 24;
    c = getenv("W16_DAC");
    dac_ideal = c && !strcmp(c, "ideal");
    w16_syscolors_realize(); /* (USER's start-up makes some WIN.INI colours solid, see sys.c) */
}

static uint32_t cref_to_rgb(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
static COLORREF rgb_to_cref(uint32_t p) { return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF); }

static int nearest16(uint32_t rgb)
{
    int best = 0, bd = 1 << 30;
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)((vga16[i] >> 16) & 255), dg = g - (int)((vga16[i] >> 8) & 255), db = b - (int)(vga16[i] & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* COLORREF -> displayed pixel for pens/text (nearest solid colour on a 16-colour device) */
/* inversion as the VGA hardware does it: on palette index (black<->white, light<->dark gray,
 * navy<->yellow ...). Non-VGA colours (true-colour themes) are inverted per channel. */
uint32_t w16_invert_px(uint32_t p)
{
    p &= 0xFFFFFF;
    for (int i = 0; i < 16; i++)
        if (vga16[i] == p) return vga16[i ^ 15];
    return ~p & 0xFFFFFF;
}

/* a presented (DAC) pixel inverted the way the VGA does it, by palette index */
uint32_t w16_invert_display_px(uint32_t d)
{
    uint32_t a = d & 0xFF000000;
    d &= 0xFFFFFF;
    for (int i = 0; i < 16; i++)
        if (w16_display_px(vga16[i]) == d) return a | w16_display_px(vga16[i ^ 15]);
    return a | (~d & 0xFFFFFF);
}

/* ------------------------------------------------------------------ VGA.DRV colour matching
 * On the 16-colour display, colours are matched and brushes dithered the way VGA.DRV does it:
 * ColorInfo / RealizeObject (seg1:1956) find the nearest of its 16 colours along a primary,
 * secondary or gray line chosen from the sorted channels; a brush colour it does not have is
 * dithered over the corners of the cube cell the colour lies in (seg1:24A1) with an 8x8 ordered
 * matrix. The driver's tables (its colours, levels, cells, ranks, matrix) are read from the
 * user's VGA.DRV, code segment 1, at run time; without it a rougher approximation is used. */
static const uint8_t *vgacs;    /* VGA.DRV seg1, NULL until checked or if unusable */
static int vgacs_tried;

static const uint8_t *vga_tables(void)
{
    if (!vgacs_tried) {
        vgacs_tried = 1;
        unsigned len = 0;
        HINSTANCE m = w16_system_module("VGA.DRV");
        const uint8_t *d = m ? w16_module_data(m, 1, &len) : NULL;
        /* the code that uses the tables (seg1:22A1 "sub ax,ax; mov ch,dl") and the matrix */
        if (d && len >= 0x22A5 && d[0x22A1] == 0x2B && d[0x22A2] == 0xC0 && d[0x22A3] == 0x8A && d[0x22A4] == 0xEA) {
            int seen[64] = {0}, ok = 1;
            for (int i = 0; i < 64; i++) {
                int v = d[0x2261 + i];
                if (v > 63 || seen[v]++) ok = 0;
            }
            for (int i = 0; i < 16 && ok; i++) {
                const uint8_t *e = d + 0x3C + 3 * i;
                uint32_t p = (uint32_t)e[0] << 16 | e[1] << 8 | e[2];
                int found = 0;
                for (int k = 0; k < 16; k++) found |= vga16[k] == p;
                ok = found;
            }
            if (ok) vgacs = d;
        }
    }
    return vgacs;
}

/* seg1:22A1: sorts three channel values (largest first); returns the swaps as 3 bits */
static int vga_sort3(int *a, int *b, int *c)
{
    int bits = 0, t, cf;
    cf = *a < *c; if (cf) { t = *a; *a = *c; *c = t; } bits = bits << 1 | cf;
    cf = *b < *c; if (cf) { t = *b; *b = *c; *c = t; } bits = bits << 1 | cf;
    cf = *a < *b; if (cf) { t = *a; *a = *b; *b = t; } bits = bits << 1 | cf;
    return bits;
}

/* seg1:2323: a colour index for sorted channels (bit 0 the largest, bit 2 the smallest, bit 3
 * intensity) back to the driver's R, G, B bit order */
static int vga_unsort(int code, int perm)
{
    int dl = code & 1, dh = (code >> 1) & 1, ah = (code >> 2) & 1, t;
    if (perm & 1) { t = dh; dh = dl; dl = t; }
    if (perm & 2) { t = ah; ah = dh; dh = t; }
    if (perm & 4) { t = ah; ah = dl; dl = t; }
    return (code & ~7) | ah << 2 | dh << 1 | dl;
}

/* seg1:1956: the driver colour index nearest to R, G, B; also the sorted channels for the dither */
static int vga_nearest(int R, int G, int B, int *perm, int *mx, int *md, int *mn)
{
    const uint8_t *cs = vgacs;
    int a = R, b = G, c = B;
    *perm = vga_sort3(&a, &b, &c);
    *mx = a; *md = b; *mn = c;
    if (a == 0) return 0;
    int x = a - b, y = b - c, z = c;
    const uint8_t *e = cs + 0x1944 + 3 * cs[0x193C + vga_sort3(&x, &y, &z)];
    const uint8_t *lv = cs + (e[0] | e[1] << 8), *co = cs + (e[2] | e[3] << 8);
    int n = e[4], dl = 0xFF, dh = 0xFF;
    for (int cl = n, i = 0; cl > 0; cl--, i++) {
        int d = lv[i] - a;
        if (d < 0) d = -d;
        if (d == 0) { dl = cl; break; }
        if (d < dh) { dl = cl; dh = d; }
    }
    return vga_unsort(co[(n - dl) & 0xFF], *perm);
}

static uint32_t vga_index_rgb(int i)
{
    const uint8_t *e = vgacs + 0x3C + 3 * (i & 15);
    return (uint32_t)e[0] << 16 | e[1] << 8 | e[2];
}

/* the solid colour the display shows for p (0xRRGGBB) */
static uint32_t nearest_rgb(uint32_t p)
{
    if (!vga_tables()) return vga16[nearest16(p)];
    int perm, a, b, c;
    return vga_index_rgb(vga_nearest((p >> 16) & 255, (p >> 8) & 255, p & 255, &perm, &a, &b, &c));
}

/* seg1:24A1: the 8x8 pattern of driver colour indices for sorted channels mx >= md >= mn */
static void vga_dither(int perm, int mx, int md, int mn, uint8_t pat[64])
{
    const uint8_t *cs = vgacs;
    int comp[3] = {mx, md, mn}, s, k;
    /* seg1:22BE: the cell - the first of four planes the colour is on the positive side of */
    for (s = 0; s < 3; s++) {
        const uint8_t *p = cs + 0x21DA + 12 * s;
        int16_t sum = 0;
        for (k = 0; k < 3; k++)
            sum += (int16_t)((comp[k] - (int16_t)(p[2 * k] | p[2 * k + 1] << 8)) * (int16_t)(p[6 + 2 * k] | p[7 + 2 * k] << 8));
        if (sum >= 0) break;
    }
    const uint8_t *t = cs + 0x220A + 16 * s;
    /* seg1:22EB: channels scaled to 0..64, less the cell's corner, times its matrix = weights */
    int sc[3], w[3];
    for (k = 0; k < 3; k++) sc[k] = (((comp[k] >> 1) + (comp[k] & 1)) >> 1) - t[k];
    for (int r = 0; r < 3; r++) {
        int16_t acc = 0;
        for (k = 0; k < 3; k++) acc += (int16_t)((int8_t)t[3 + 3 * r + k] * (int8_t)sc[k]);
        w[r] = acc;
    }
    /* seg1:2364: (weight, corner) pairs of the non-zero weights, the cell's first corner taking the
     * rest of 64 */
    uint8_t pw[4], pc[4];
    int n = 0, rest = (int16_t)(64 - w[0] - w[1] - w[2]);
    if (rest) { pw[n] = (uint8_t)rest; pc[n++] = t[12]; }
    for (k = 0; k < 3; k++)
        if ((uint8_t)w[k]) { pw[n] = (uint8_t)w[k]; pc[n++] = t[13 + k]; }
    for (k = 0; k < n; k++) pc[k] = (uint8_t)vga_unsort(cs[0x224A + pc[k]], perm);
    /* seg1:23A5: in the driver's colour order (rank table), an entry taken is marked colour 8 */
    if (n > 1) {
        uint8_t tw[4], tc[4];
        for (int o = 0; o < n; o++) {
            int best = 0, br = 0xFF;
            for (k = 0; k < n; k++)
                if (cs[0x2251 + (pc[k] & 15)] <= br) { br = cs[0x2251 + (pc[k] & 15)]; best = k; }
            tw[o] = pw[best];
            tc[o] = pc[best];
            pc[best] = 8;
        }
        memcpy(pw, tw, n);
        memcpy(pc, tc, n);
    }
    /* seg1:2404: a pixel takes the first colour whose running weight passes its matrix value */
    uint8_t prev[8] = {0}, cum = 0;
    memset(pat, 0, 64);
    for (int i = 0; i < n; i++) {
        cum = (uint8_t)(cum + pw[i]);
        for (int row = 0; row < 8; row++) {
            uint8_t m = 0;
            for (k = 0; k < 8; k++) m = (uint8_t)(m << 1 | (cs[0x2261 + row * 8 + k] < cum));
            uint8_t fresh = m ^ prev[row];
            prev[row] = m;
            for (k = 0; k < 8; k++)
                if (fresh & (0x80 >> k)) pat[row * 8 + k] = pc[i];
        }
    }
}

/* the solid colour a 16-colour display shows for c */
COLORREF GetNearestColor(HDC dc, COLORREF c)
{
    (void)dc;
    uint32_t p = w16_rgb(c);
    return RGB((p >> 16) & 255, (p >> 8) & 255, p & 255);
}

uint32_t w16_rgb(COLORREF c)
{
    if ((c >> 24) == 1) /* PALETTEINDEX */
        return vga16[c & 15];
    uint32_t p = cref_to_rgb(c & 0xFFFFFF);
    if (ncolors > 16) return p;
    return nearest_rgb(p);
}

/* brush colours on a 16-colour device: VGA.DRV RealizeObject (seg1:292A) - white and black are
 * solid; E0E0E0 with the 0x10 flag (the scroll bar colour, see SetSysColors) is its 50% white /
 * light gray pattern; a colour the driver has is solid; others are dithered (cached per colour).
 * x, y: the pixel's position in the 8x8 pattern. */
static const int bayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};

uint32_t w16_dither(COLORREF c, int x, int y)
{
    uint32_t p = cref_to_rgb(c & 0xFFFFFF);
    if (ncolors > 16) return p;
    if (vga_tables()) {
        static struct { COLORREF c; int used; uint32_t px[64]; } cache[64];
        static int next;
        x &= 7;
        y &= 7;
        if (p == 0xFFFFFF || p == 0) return p;
        if ((c & 0x10000000) && p == 0xE0E0E0) return ((x + y) & 1) ? 0xFFFFFF : vga_index_rgb(8);
        for (int i = 0; i < 64; i++)
            if (cache[i].used && cache[i].c == c) return cache[i].px[y * 8 + x];
        int perm, a, b, d, idx = vga_nearest((p >> 16) & 255, (p >> 8) & 255, p & 255, &perm, &a, &b, &d);
        int slot = next;
        next = (next + 1) & 63;
        cache[slot].c = c;
        cache[slot].used = 1;
        if (vga_index_rgb(idx) == p) {
            for (int i = 0; i < 64; i++) cache[slot].px[i] = p;
        } else {
            uint8_t pat[64];
            vga_dither(perm, a, b, d, pat);
            for (int i = 0; i < 64; i++) cache[slot].px[i] = vga_index_rgb(pat[i]);
        }
        return cache[slot].px[y * 8 + x];
    }
    int i = nearest16(p);
    if (vga16[i] == p) return p;
    /* no VGA.DRV: dither between the two nearest grey/colour levels per channel */
    int t = bayer8[y & 7][x & 7];
    int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    int ch[3] = {r, g, b}, out[3];
    for (int k = 0; k < 3; k++) {
        int v = ch[k];
        int lo = v < 128 ? 0 : 128, hi = v < 128 ? 128 : 255;
        int frac = (v - lo) * 64 / (hi - lo + 1);
        out[k] = frac > t ? hi : lo;
    }
    return vga16[nearest16((out[0] << 16) | (out[1] << 8) | out[2])];
}

/* ------------------------------------------------------------------ objects */
static struct W16GdiObj stock[17];
static int stock_init;

static void init_stock(void)
{
    if (stock_init) return;
    stock_init = 1;
    COLORREF bc[5] = {RGB(255, 255, 255), RGB(192, 192, 192), RGB(128, 128, 128), RGB(64, 64, 64), RGB(0, 0, 0)};
    for (int i = 0; i <= 4; i++) {
        stock[i].kind = OBJ_BRUSH;
        stock[i].stock = 1;
        stock[i].u.brush.style = BS_SOLID;
        stock[i].u.brush.color = bc[i];
    }
    stock[NULL_BRUSH].kind = OBJ_BRUSH;
    stock[NULL_BRUSH].stock = 1;
    stock[NULL_BRUSH].u.brush.style = BS_NULL;
    stock[WHITE_PEN] = (struct W16GdiObj){.kind = OBJ_PEN, .stock = 1, .u.pen = {PS_SOLID, 1, RGB(255, 255, 255)}};
    stock[BLACK_PEN] = (struct W16GdiObj){.kind = OBJ_PEN, .stock = 1, .u.pen = {PS_SOLID, 1, 0}};
    stock[NULL_PEN] = (struct W16GdiObj){.kind = OBJ_PEN, .stock = 1, .u.pen = {PS_NULL, 1, 0}};
    const char *faces[] = {[OEM_FIXED_FONT] = "Terminal", [ANSI_FIXED_FONT] = "Courier",
                           [ANSI_VAR_FONT] = "MS Sans Serif", [SYSTEM_FONT] = "System",
                           [DEVICE_DEFAULT_FONT] = "System", [SYSTEM_FIXED_FONT] = "Fixedsys"};
    int heights[] = {[OEM_FIXED_FONT] = 12, [ANSI_FIXED_FONT] = 13, [ANSI_VAR_FONT] = 13,
                     [SYSTEM_FONT] = 16, [DEVICE_DEFAULT_FONT] = 16, [SYSTEM_FIXED_FONT] = 15};
    for (int i = OEM_FIXED_FONT; i <= SYSTEM_FIXED_FONT; i++) {
        if (i == DEFAULT_PALETTE) continue;
        stock[i].kind = OBJ_FONT;
        stock[i].stock = 1;
        memset(&stock[i].u.font.lf, 0, sizeof(LOGFONT));
        stock[i].u.font.lf.lfHeight = heights[i];
        stock[i].u.font.lf.lfWeight = i == SYSTEM_FONT || i == DEVICE_DEFAULT_FONT ? FW_BOLD : FW_NORMAL;
        stock[i].u.font.lf.lfCharSet = i == OEM_FIXED_FONT ? OEM_CHARSET : ANSI_CHARSET;
        snprintf(stock[i].u.font.lf.lfFaceName, LF_FACESIZE, "%s", faces[i]);
    }
    stock[DEFAULT_PALETTE].kind = OBJ_PAL;
    stock[DEFAULT_PALETTE].stock = 1;
}

HGDIOBJ GetStockObject(int i)
{
    init_stock();
    return (i >= 0 && i <= 16 && stock[i].kind) ? &stock[i] : NULL;
}

static HGDIOBJ newobj(int kind)
{
    HGDIOBJ o = calloc(1, sizeof *o);
    o->kind = kind;
    return o;
}

HPEN CreatePen(int style, int width, COLORREF c)
{
    HGDIOBJ o = newobj(OBJ_PEN);
    o->u.pen.style = style;
    o->u.pen.width = width < 1 ? 1 : width;
    o->u.pen.color = c;
    return o;
}
HBRUSH CreateSolidBrush(COLORREF c)
{
    HGDIOBJ o = newobj(OBJ_BRUSH);
    o->u.brush.style = BS_SOLID;
    o->u.brush.color = c;
    return o;
}
HBRUSH CreateHatchBrush(int style, COLORREF c)
{
    HGDIOBJ o = newobj(OBJ_BRUSH);
    o->u.brush.style = BS_HATCHED;
    o->u.brush.hatch = style;
    o->u.brush.color = c;
    return o;
}
HBRUSH CreatePatternBrush(HBITMAP bm)
{
    HGDIOBJ o = newobj(OBJ_BRUSH);
    o->u.brush.style = BS_PATTERN;
    W16Bitmap *b = w16_bitmap_of(bm);
    o->u.brush.has_pat = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            o->u.brush.pat[y * 8 + x] = b && b->w && b->h ? b->px[(y % b->h) * b->w + (x % b->w)] : 0xFFFFFF;
    o->u.brush.color = b && b->mono ? 1 : 0; /* mono pattern flag */
    return o;
}
HBRUSH CreateBrushIndirect(const LOGBRUSH *lb)
{
    if (lb->lbStyle == BS_NULL) return GetStockObject(NULL_BRUSH);
    if (lb->lbStyle == BS_HATCHED) return CreateHatchBrush(lb->lbHatch, lb->lbColor);
    return CreateSolidBrush(lb->lbColor);
}
HFONT CreateFontIndirect(const LOGFONT *lf)
{
    HGDIOBJ o = newobj(OBJ_FONT);
    o->u.font.lf = *lf;
    return o;
}
HFONT CreateFont(int h, int w, int esc, int orient, int weight, BYTE italic, BYTE underline,
                 BYTE strike, BYTE charset, BYTE outprec, BYTE clipprec, BYTE quality,
                 BYTE pitchfam, LPCSTR face)
{
    LOGFONT lf = {h, w, esc, orient, weight, italic, underline, strike, charset, outprec,
                  clipprec, quality, pitchfam, ""};
    if (face) snprintf(lf.lfFaceName, LF_FACESIZE, "%s", face);
    return CreateFontIndirect(&lf);
}

W16Bitmap *w16_bitmap_of(HBITMAP h) { return (h && h->kind == OBJ_BITMAP) ? &h->u.bmp : NULL; }

HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits)
{
    HGDIOBJ o = newobj(OBJ_BITMAP);
    W16Bitmap *b = &o->u.bmp;
    b->w = w > 0 ? w : 1;
    b->h = h > 0 ? h : 1;
    b->mono = planes * bpp == 1;
    b->px = calloc((size_t)b->w * b->h, 4);
    if (bits && b->mono) {
        int wb = ((b->w + 15) / 16) * 2; /* Win16 DDB rows are word aligned */
        const uint8_t *s = bits;
        for (int y = 0; y < b->h; y++)
            for (int x = 0; x < b->w; x++)
                b->px[y * b->w + x] = (s[y * wb + x / 8] & (0x80 >> (x & 7))) ? 0xFFFFFF : 0;
    }
    return o;
}
HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h)
{
    int mono = dc && dc->is_mem && (!dc->target || dc->target->mono);
    return CreateBitmap(w, h, 1, mono ? 1 : 4, NULL);
}
HBITMAP CreateDiscardableBitmap(HDC dc, int w, int h) { return CreateCompatibleBitmap(dc, w, h); }

/* DIB (as stored in resources) -> bitmap */
HBITMAP w16_bitmap_from_dib(const uint8_t *d, int len, int force_color)
{
    (void)len;
    uint32_t hs = d[0] | d[1] << 8 | d[2] << 16 | d[3] << 24;
    int w, h, bpp, ncol, pal4;
    if (hs == 12) {
        w = d[4] | d[5] << 8; h = d[6] | d[7] << 8; bpp = d[10] | d[11] << 8; pal4 = 0;
        ncol = bpp <= 8 ? 1 << bpp : 0;
    } else {
        w = (int32_t)(d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24);
        h = (int32_t)(d[8] | d[9] << 8 | d[10] << 16 | d[11] << 24);
        bpp = d[14] | d[15] << 8;
        int clr = d[32] | d[33] << 8;
        ncol = clr ? clr : (bpp <= 8 ? 1 << bpp : 0);
        pal4 = 1;
    }
    int flip = h > 0;
    if (h < 0) h = -h;
    const uint8_t *pal = d + hs;
    const uint8_t *bits = pal + ncol * (pal4 ? 4 : 3);
    int stride = ((w * bpp + 31) / 32) * 4;
    HBITMAP o = CreateBitmap(w, h, 1, 4, NULL);
    W16Bitmap *b = &o->u.bmp;
    uint32_t cols[256];
    for (int i = 0; i < ncol && i < 256; i++) {
        const uint8_t *e = pal + i * (pal4 ? 4 : 3);
        cols[i] = (e[2] << 16) | (e[1] << 8) | e[0];
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bits + (size_t)(flip ? h - 1 - y : y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t p;
            if (bpp == 1) p = cols[(row[x / 8] >> (7 - (x & 7))) & 1];
            else if (bpp == 4) p = cols[(row[x / 2] >> ((x & 1) ? 0 : 4)) & 15];
            else if (bpp == 8) p = cols[row[x]];
            else if (bpp == 24) p = (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            else p = 0;
            b->px[y * w + x] = ncolors > 16 ? p : w16_rgb(rgb_to_cref(p));
        }
    }
    if (bpp == 1 && !force_color && ncol == 2 &&
        ((cols[0] == 0 && cols[1] == 0xFFFFFF) || (cols[0] == 0xFFFFFF && cols[1] == 0)))
        b->mono = 1;
    return o;
}

HBITMAP LoadBitmap(HINSTANCE h, LPCSTR name)
{
    HINSTANCE m = h;
    if (!m) {
        if (IS_INTRESOURCE(name)) {
            W16Bitmap *ob = w16_obm((int)(uintptr_t)name);
            if (!ob) return NULL;
            HBITMAP o = CreateBitmap(ob->w, ob->h, 1, ob->mono ? 1 : 4, NULL);
            memcpy(o->u.bmp.px, ob->px, (size_t)ob->w * ob->h * 4);
            return o;
        }
        return NULL;
    }
    const W16Res *r = w16_find_res(m, name, RT_BITMAP);
    if (!r) return NULL;
    return w16_bitmap_from_dib(w16_res_data(m, r), r->len, 0);
}

W16Bitmap *w16_obm(int id)
{
    static W16Bitmap *cache[64];
    int slot = id - 32734;
    if (slot < 0 || slot >= 64) return NULL;
    if (cache[slot]) return cache[slot];
    HINSTANCE drv = w16_system_module("VGA.DRV");
    if (!drv) return NULL;
    const W16Res *r = w16_find_res(drv, MAKEINTRESOURCE(id), RT_BITMAP);
    if (!r) return NULL;
    HBITMAP b = w16_bitmap_from_dib(w16_res_data(drv, r), r->len, 0);
    cache[slot] = &b->u.bmp;
    return cache[slot];
}

BOOL DeleteObject(HGDIOBJ o)
{
    if (!o || o->stock) return FALSE;
    if (o->kind == OBJ_BITMAP) free(o->u.bmp.px);
    if (o->kind == OBJ_RGN) rgn_free(&o->u.rgn);
    o->kind = 0;
    free(o);
    return TRUE;
}

int GetObject(HGDIOBJ o, int cb, void *out)
{
    if (!o) return 0;
    if (o->kind == OBJ_BITMAP && cb >= (int)sizeof(BITMAP)) {
        BITMAP *b = out;
        memset(b, 0, sizeof *b);
        b->bmWidth = o->u.bmp.w;
        b->bmHeight = o->u.bmp.h;
        b->bmPlanes = 1;
        b->bmBitsPixel = o->u.bmp.mono ? 1 : 4;
        b->bmWidthBytes = ((b->bmWidth * b->bmBitsPixel + 15) / 16) * 2;
        return sizeof *b;
    }
    if (o->kind == OBJ_FONT && cb >= (int)sizeof(LOGFONT)) {
        memcpy(out, &o->u.font.lf, sizeof(LOGFONT));
        return sizeof(LOGFONT);
    }
    if (o->kind == OBJ_BRUSH && cb >= (int)sizeof(LOGBRUSH)) {
        LOGBRUSH *lb = out;
        lb->lbStyle = o->u.brush.style;
        lb->lbColor = o->u.brush.color;
        lb->lbHatch = o->u.brush.hatch;
        return sizeof *lb;
    }
    return 0;
}

HBRUSH w16_sys_brush(int i)
{
    static HBRUSH b[W16_NUM_SYSCOLORS];
    static COLORREF c[W16_NUM_SYSCOLORS];
    if (!b[i] || c[i] != w16_syscolor[i]) {
        if (b[i]) { b[i]->stock = 0; DeleteObject(b[i]); }
        b[i] = CreateSolidBrush(w16_syscolor[i]);
        b[i]->stock = 1;
        c[i] = w16_syscolor[i];
    }
    return b[i];
}
HPEN w16_sys_pen(int i)
{
    static HPEN p[W16_NUM_SYSCOLORS];
    static COLORREF c[W16_NUM_SYSCOLORS];
    if (!p[i] || c[i] != w16_syscolor[i]) {
        if (p[i]) { p[i]->stock = 0; DeleteObject(p[i]); }
        p[i] = CreatePen(PS_SOLID, 1, w16_syscolor[i]);
        p[i]->stock = 1;
        c[i] = w16_syscolor[i];
    }
    return p[i];
}

/* ------------------------------------------------------------------ DCs */
static void dc_defaults(HDC dc)
{
    init_stock();
    dc->pen = &stock[BLACK_PEN];
    dc->brush = &stock[WHITE_BRUSH];
    dc->font = &stock[SYSTEM_FONT];
    dc->text = RGB(0, 0, 0);
    dc->bk = RGB(255, 255, 255);
    dc->bkmode = OPAQUE;
    dc->rop2 = R2_COPYPEN;
    dc->align = 0;
    dc->mapmode = MM_TEXT;
    dc->wextx = dc->wexty = dc->vextx = dc->vexty = 1;
}

HDC CreateCompatibleDC(HDC ref)
{
    (void)ref;
    HDC dc = calloc(1, sizeof *dc);
    dc_defaults(dc);
    dc->is_mem = 1;
    static HBITMAP onebyone;
    if (!onebyone) onebyone = CreateBitmap(1, 1, 1, 1, NULL);
    dc->sel_bitmap = onebyone;
    dc->target = &onebyone->u.bmp;
    rgn_init(&dc->vis);
    rgn_init(&dc->clip);
    rgn_set(&dc->vis, &(RECT){0, 0, 1, 1});
    return dc;
}

HDC CreateDC(LPCSTR drv, LPCSTR dev, LPCSTR port, const void *init)
{
    (void)dev; (void)port; (void)init;
    if (drv && !strcasecmp(drv, "DISPLAY")) {
        HDC dc = w16_make_dc(NULL, 1);
        return dc;
    }
    /* printers: an information-only DC; real printing goes through CUPS (TODO T-PRN-01) */
    HDC dc = CreateCompatibleDC(NULL);
    dc->is_info = 1;
    return dc;
}
HDC CreateIC(LPCSTR a, LPCSTR b, LPCSTR c, const void *d) { HDC dc = CreateDC(a, b, c, d); dc->is_info = 1; return dc; }

BOOL DeleteDC(HDC dc)
{
    if (!dc) return FALSE;
    while (dc->saved) RestoreDC(dc, -1);
    rgn_free(&dc->vis);
    rgn_free(&dc->clip);
    free(dc);
    return TRUE;
}

/* DC for a window: client (window_dc=0) or whole window (1). h==NULL -> whole screen */
HDC w16_make_dc(HWND h, int window_dc)
{
    HDC dc = calloc(1, sizeof *dc);
    dc_defaults(dc);
    rgn_init(&dc->vis);
    rgn_init(&dc->clip);
    dc->hwnd = h;
    if (!h || h == w16_desktop) {
        rgn_set(&dc->vis, &(RECT){0, 0, w16_screen.w, w16_screen.h});
        if (h == w16_desktop) {
            /* desktop DC excludes top-level windows */
            for (HWND c = w16_desktop->child; c; c = c->next)
                if (c->style & WS_VISIBLE) rgn_sub(&dc->vis, &c->rw);
        }
    } else {
        int clipch = (h->style & WS_CLIPCHILDREN) != 0;
        w16_calc_visrgn(h, window_dc, clipch, &dc->vis);
        dc->ox = window_dc ? h->rw.left : h->rc.left;
        dc->oy = window_dc ? h->rw.top : h->rc.top;
    }
    return dc;
}

int SaveDC(HDC dc)
{
    HDC s = malloc(sizeof *s);
    *s = *dc;
    rgn_init(&s->vis);
    rgn_init(&s->clip);
    rgn_copy(&s->clip, &dc->clip);
    s->saved = dc->saved;
    dc->saved = s;
    return ++dc->saved_depth;
}
BOOL RestoreDC(HDC dc, int n)
{
    if (n < 0) n = dc->saved_depth + n + 1;
    if (n < 1 || n > dc->saved_depth) return FALSE;
    while (dc->saved && dc->saved_depth >= n) {
        HDC s = dc->saved;
        Region vis = dc->vis;
        rgn_free(&dc->clip);
        *dc = *s; /* the state from before that SaveDC, its depth and older saves included */
        dc->vis = vis;
        free(s);
    }
    return TRUE;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ o)
{
    if (!dc || !o) return NULL;
    HGDIOBJ old = NULL;
    switch (o->kind) {
    case OBJ_PEN: old = dc->pen; dc->pen = o; break;
    case OBJ_BRUSH: old = dc->brush; dc->brush = o; break;
    case OBJ_FONT: old = dc->font; dc->font = o; break;
    case OBJ_BITMAP:
        if (!dc->is_mem) return NULL;
        old = dc->sel_bitmap;
        dc->sel_bitmap = o;
        dc->target = &o->u.bmp;
        rgn_set(&dc->vis, &(RECT){0, 0, o->u.bmp.w, o->u.bmp.h});
        break;
    case OBJ_RGN:
        SelectClipRgn(dc, o);
        return (HGDIOBJ)(uintptr_t)SIMPLEREGION;
    default: break;
    }
    return old;
}

COLORREF SetTextColor(HDC dc, COLORREF c) { COLORREF o = dc->text; dc->text = c; return o; }
COLORREF GetTextColor(HDC dc) { return dc->text; }
COLORREF SetBkColor(HDC dc, COLORREF c) { COLORREF o = dc->bk; dc->bk = c; return o; }
COLORREF GetBkColor(HDC dc) { return dc->bk; }
int SetBkMode(HDC dc, int m) { int o = dc->bkmode; dc->bkmode = m; return o; }
int GetBkMode(HDC dc) { return dc->bkmode; }
int SetROP2(HDC dc, int r) { int o = dc->rop2; dc->rop2 = r; return o; }
UINT SetTextAlign(HDC dc, UINT a) { UINT o = dc->align; dc->align = a; return o; }
int SetMapMode(HDC dc, int m) { int o = dc->mapmode; dc->mapmode = m; return o; }
int GetMapMode(HDC dc) { return dc->mapmode; }
DWORD SetWindowOrg(HDC dc, int x, int y) { DWORD o = MAKELONG(dc->worgx, dc->worgy); dc->worgx = x; dc->worgy = y; return o; }
DWORD SetViewportOrg(HDC dc, int x, int y) { DWORD o = MAKELONG(dc->vorgx, dc->vorgy); dc->vorgx = x; dc->vorgy = y; return o; }
DWORD SetWindowExt(HDC dc, int x, int y) { DWORD o = MAKELONG(dc->wextx, dc->wexty); if (dc->mapmode >= MM_ISOTROPIC) { dc->wextx = x ? x : 1; dc->wexty = y ? y : 1; } return o; }
DWORD SetViewportExt(HDC dc, int x, int y) { DWORD o = MAKELONG(dc->vextx, dc->vexty); if (dc->mapmode >= MM_ISOTROPIC) { dc->vextx = x ? x : 1; dc->vexty = y ? y : 1; } return o; }
DWORD GetWindowOrg(HDC dc) { return MAKELONG(dc->worgx, dc->worgy); }
DWORD GetViewportOrg(HDC dc) { return MAKELONG(dc->vorgx, dc->vorgy); }
DWORD SetBrushOrg(HDC dc, int x, int y) { DWORD o = MAKELONG(dc->brushorgx, dc->brushorgy); dc->brushorgx = x; dc->brushorgy = y; return o; }
BOOL UnrealizeObject(HGDIOBJ o) { (void)o; return TRUE; }
int SetTextCharacterExtra(HDC dc, int e) { int o = dc->charextra; dc->charextra = e; return o; }

/* a * b / c rounded to the nearest integer (halves away from zero) */
int MulDiv(int a, int b, int c)
{
    if (!c) return -1;
    long long p = (long long)a * b;
    int neg = (p < 0) != (c < 0);
    long long q = ((p < 0 ? -p : p) + (c < 0 ? -(long long)c : c) / 2) / (c < 0 ? -(long long)c : c);
    return (int)(neg ? -q : q);
}

/* window -> viewport scaling rounds like GDI's (MulDiv), not truncates */
void w16_lp_to_dp(HDC dc, int *x, int *y)
{
    if (dc->mapmode == MM_TEXT) {
        *x = *x - dc->worgx + dc->vorgx + dc->ox;
        *y = *y - dc->worgy + dc->vorgy + dc->oy;
    } else {
        *x = MulDiv(*x - dc->worgx, dc->vextx, dc->wextx) + dc->vorgx + dc->ox;
        *y = MulDiv(*y - dc->worgy, dc->vexty, dc->wexty) + dc->vorgy + dc->oy;
    }
}
static int lp_len(HDC dc, int v, int horiz)
{
    if (dc->mapmode == MM_TEXT) return v;
    return horiz ? MulDiv(v, dc->vextx, dc->wextx) : MulDiv(v, dc->vexty, dc->wexty);
}
BOOL LPtoDP(HDC dc, LPPOINT p, int n)
{
    for (int i = 0; i < n; i++) { w16_lp_to_dp(dc, &p[i].x, &p[i].y); p[i].x -= dc->ox; p[i].y -= dc->oy; }
    return TRUE;
}
BOOL DPtoLP(HDC dc, LPPOINT p, int n)
{
    for (int i = 0; i < n; i++) {
        if (dc->mapmode == MM_TEXT) { p[i].x += dc->worgx - dc->vorgx; p[i].y += dc->worgy - dc->vorgy; }
        else {
            p[i].x = (int)((long)(p[i].x - dc->vorgx) * dc->wextx / dc->vextx) + dc->worgx;
            p[i].y = (int)((long)(p[i].y - dc->vorgy) * dc->wexty / dc->vexty) + dc->worgy;
        }
    }
    return TRUE;
}

int GetDeviceCaps(HDC dc, int i)
{
    (void)dc;
    switch (i) {
    case DRIVERVERSION: return 0x30A;
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZSIZE: return 208;
    case VERTSIZE: return 156;
    case HORZRES: return w16_screen.w;
    case VERTRES: return w16_screen.h;
    case BITSPIXEL: return 1;
    case PLANES: return ncolors > 16 ? 1 : 4;
    case NUMBRUSHES: return -1;
    case NUMPENS: return 80;
    case NUMFONTS: return 0;
    case NUMCOLORS: return ncolors > 256 ? -1 : ncolors;
    case ASPECTX: return 36;
    case ASPECTY: return 36;
    case ASPECTXY: return 51;
    case LOGPIXELSX: return 96;
    case LOGPIXELSY: return 96;
    case RASTERCAPS: return 0x0699;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ clipping */
/* effective clip region in target coordinates */
void w16_dc_clip_iter_begin(HDC dc, Region *out)
{
    rgn_init(out);
    rgn_copy(out, &dc->vis);
    if (dc->use_clip) rgn_and_rgn(out, &dc->clip);
}

static void clip_from_vis_if_needed(HDC dc)
{
    if (!dc->use_clip) {
        rgn_copy(&dc->clip, &dc->vis);
        dc->use_clip = 1;
    }
}

int IntersectClipRect(HDC dc, int l, int t, int r, int b)
{
    w16_lp_to_dp(dc, &l, &t);
    w16_lp_to_dp(dc, &r, &b);
    clip_from_vis_if_needed(dc);
    rgn_and(&dc->clip, &(RECT){l, t, r, b});
    return rgn_empty(&dc->clip) ? NULLREGION : SIMPLEREGION;
}
int ExcludeClipRect(HDC dc, int l, int t, int r, int b)
{
    w16_lp_to_dp(dc, &l, &t);
    w16_lp_to_dp(dc, &r, &b);
    clip_from_vis_if_needed(dc);
    rgn_sub(&dc->clip, &(RECT){l, t, r, b});
    return rgn_empty(&dc->clip) ? NULLREGION : COMPLEXREGION;
}
HRGN CreateRectRgn(int l, int t, int r, int b)
{
    HGDIOBJ o = newobj(OBJ_RGN);
    rgn_init(&o->u.rgn);
    rgn_set(&o->u.rgn, &(RECT){l, t, r, b});
    return o;
}
HRGN CreateRectRgnIndirect(LPCRECT r) { return CreateRectRgn(r->left, r->top, r->right, r->bottom); }
int SelectClipRgn(HDC dc, HRGN r)
{
    if (!r) { dc->use_clip = 0; rgn_clear(&dc->clip); return SIMPLEREGION; }
    rgn_copy(&dc->clip, &r->u.rgn);
    rgn_offset(&dc->clip, dc->ox, dc->oy);
    dc->use_clip = 1;
    return SIMPLEREGION;
}
int GetClipBox(HDC dc, LPRECT r)
{
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_bounds(&e, r);
    int n = e.n;
    rgn_free(&e);
    OffsetRect(r, -dc->ox, -dc->oy);
    POINT p[2] = {{r->left, r->top}, {r->right, r->bottom}};
    DPtoLP(dc, p, 2);
    SetRect(r, p[0].x, p[0].y, p[1].x, p[1].y);
    return n == 0 ? NULLREGION : n == 1 ? SIMPLEREGION : COMPLEXREGION;
}
BOOL PtVisible(HDC dc, int x, int y)
{
    w16_lp_to_dp(dc, &x, &y);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    int v = rgn_contains(&e, x, y);
    rgn_free(&e);
    return v;
}
BOOL RectVisible(HDC dc, LPCRECT r)
{
    RECT d = *r;
    w16_lp_to_dp(dc, &d.left, &d.top);
    w16_lp_to_dp(dc, &d.right, &d.bottom);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, &d);
    int v = !rgn_empty(&e);
    rgn_free(&e);
    return v;
}

/* ------------------------------------------------------------------ pixel plumbing */
static W16Bitmap *tgt(HDC dc) { return dc->target ? dc->target : &w16_screen; }

static inline uint32_t rop2_apply(int rop, uint32_t pen, uint32_t dst)
{
    switch (rop) {
    case R2_BLACK: return 0;
    case R2_NOTMERGEPEN: return ~(pen | dst) & 0xFFFFFF;
    case R2_MASKNOTPEN: return ~pen & dst & 0xFFFFFF;
    case R2_NOTCOPYPEN: return ~pen & 0xFFFFFF;
    case R2_MASKPENNOT: return pen & ~dst & 0xFFFFFF;
    case R2_NOT: return w16_invert_px(dst);
    case R2_XORPEN: return (pen ^ dst) & 0xFFFFFF;
    case R2_NOTMASKPEN: return ~(pen & dst) & 0xFFFFFF;
    case R2_MASKPEN: return pen & dst;
    case R2_NOTXORPEN: return ~(pen ^ dst) & 0xFFFFFF;
    case R2_NOP: return dst;
    case R2_MERGENOTPEN: return (~pen | dst) & 0xFFFFFF;
    case R2_MERGEPENNOT: return (pen | ~dst) & 0xFFFFFF;
    case R2_MERGEPEN: return pen | dst;
    case R2_WHITE: return 0xFFFFFF;
    default: return pen;
    }
}

static void mark_dirty(HDC dc) { if (!dc->target) w16_screen_dirty = 1; }

/* to a mono target, any colour other than the background maps to black (GDI rule) */
static uint32_t to_target(HDC dc, uint32_t rgb)
{
    if (dc->target && dc->target->mono)
        return rgb == w16_rgb(dc->bk) || rgb == 0xFFFFFF ? 0xFFFFFF : 0;
    return rgb;
}

static void put_rop(HDC dc, Region *clip, int x, int y, uint32_t c, int rop)
{
    if (!rgn_contains(clip, x, y)) return;
    W16Bitmap *t = tgt(dc);
    uint32_t *p = &t->px[y * t->w + x];
    *p = to_target(dc, rop2_apply(rop, c, *p));
}

/* brush colour at device pixel x,y. Patterns start at the DC's origin plus its brush origin, as GDI
 * hands the driver's RealizeObject (VGA.DRV rotates the pattern by it, seg1:2B3C/2B63) - measured:
 * MAIN.CPL Color's dithered sample in the dialog's client DC */
static uint32_t brush_px(HDC dc, HBRUSH b, int x, int y)
{
    int bx = (x - dc->ox - dc->brushorgx) & 7, by = (y - dc->oy - dc->brushorgy) & 7;
    switch (b->u.brush.style) {
    case BS_SOLID: return w16_dither(b->u.brush.color, bx, by);
    case BS_HATCHED: {
        int on = 0;
        switch (b->u.brush.hatch) {
        case HS_HORIZONTAL: on = by == 7; break;
        case HS_VERTICAL: on = bx == 7; break;
        case HS_FDIAGONAL: on = bx == by; break;
        case HS_BDIAGONAL: on = bx == 7 - by; break;
        case HS_CROSS: on = bx == 7 || by == 7; break;
        case HS_DIAGCROSS: on = bx == by || bx == 7 - by; break;
        }
        return on ? w16_rgb(b->u.brush.color) : (dc->bkmode == OPAQUE ? w16_rgb(dc->bk) : 0x1000000);
    }
    case BS_PATTERN: {
        uint32_t p = b->u.brush.pat[by * 8 + bx];
        if (b->u.brush.color == 1) /* mono pattern: 0 -> text colour, 1 -> bk colour */
            return p ? w16_rgb(dc->bk) : w16_rgb(dc->text);
        return p;
    }
    default: return 0x1000000; /* null */
    }
}

void w16_fill_solid_dev(HDC dc, const RECT *r, uint32_t rgb)
{
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, r);
    W16Bitmap *t = tgt(dc);
    rgb = to_target(dc, rgb);
    for (int i = 0; i < e.n; i++) {
        RECT a = e.r[i];
        a.left = max(a.left, 0); a.top = max(a.top, 0);
        a.right = min(a.right, t->w); a.bottom = min(a.bottom, t->h);
        for (int y = a.top; y < a.bottom; y++) {
            uint32_t *p = &t->px[y * t->w];
            for (int x = a.left; x < a.right; x++) p[x] = rgb;
        }
    }
    rgn_free(&e);
    mark_dirty(dc);
}

static void fill_brush_rop(HDC dc, const RECT *r, HBRUSH b, int rop2)
{
    if (!b || b->u.brush.style == BS_NULL) return;
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, r);
    W16Bitmap *t = tgt(dc);
    for (int i = 0; i < e.n; i++) {
        RECT a = e.r[i];
        a.left = max(a.left, 0); a.top = max(a.top, 0);
        a.right = min(a.right, t->w); a.bottom = min(a.bottom, t->h);
        for (int y = a.top; y < a.bottom; y++)
            for (int x = a.left; x < a.right; x++) {
                uint32_t c = brush_px(dc, b, x, y);
                if (c & 0x1000000) continue;
                uint32_t *p = &t->px[y * t->w + x];
                *p = to_target(dc, rop2_apply(rop2, c, *p));
            }
    }
    rgn_free(&e);
    mark_dirty(dc);
}

void w16_fill_rect_dev(HDC dc, const RECT *r, HBRUSH b) { fill_brush_rop(dc, r, b, R2_COPYPEN); }

void w16_invert_dev(HDC dc, const RECT *r)
{
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, r);
    W16Bitmap *t = tgt(dc);
    for (int i = 0; i < e.n; i++)
        for (int y = max(e.r[i].top, 0); y < min(e.r[i].bottom, t->h); y++)
            for (int x = max(e.r[i].left, 0); x < min(e.r[i].right, t->w); x++)
                t->px[y * t->w + x] = w16_invert_px(t->px[y * t->w + x]);
    rgn_free(&e);
    mark_dirty(dc);
}

/* the gray halftone brush covers the pixels with an odd x + y counted from the DC's origin and
 * brush origin: a focus rectangle in SND.CPL's OK button, whose window starts at (380, 85), has its
 * dots where screen x + y is even */
static int gray_px(HDC dc, int x, int y) { return (x - dc->ox - dc->brushorgx + y - dc->oy - dc->brushorgy) & 1; }

/* a gray caret (CreateCaret with bitmap 1): the halftone inverts every other pixel */
void w16_invert_dev_gray(HDC dc, const RECT *r)
{
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, r);
    W16Bitmap *t = tgt(dc);
    for (int i = 0; i < e.n; i++)
        for (int y = max(e.r[i].top, 0); y < min(e.r[i].bottom, t->h); y++)
            for (int x = max(e.r[i].left, 0); x < min(e.r[i].right, t->w); x++)
                if (gray_px(dc, x, y)) t->px[y * t->w + x] = w16_invert_px(t->px[y * t->w + x]);
    rgn_free(&e);
    mark_dirty(dc);
}

void w16_hline(HDC dc, int x1, int x2, int y, uint32_t rgb) { w16_fill_solid_dev(dc, &(RECT){x1, y, x2, y + 1}, rgb); }
void w16_vline(HDC dc, int x, int y1, int y2, uint32_t rgb) { w16_fill_solid_dev(dc, &(RECT){x, y1, x + 1, y2}, rgb); }

/* ------------------------------------------------------------------ USER rectangle APIs */
static void lp_rect(HDC dc, LPCRECT r, RECT *d)
{
    *d = *r;
    w16_lp_to_dp(dc, &d->left, &d->top);
    w16_lp_to_dp(dc, &d->right, &d->bottom);
    if (d->left > d->right) { int t = d->left; d->left = d->right; d->right = t; }
    if (d->top > d->bottom) { int t = d->top; d->top = d->bottom; d->bottom = t; }
}

static HBRUSH brush_arg(HBRUSH b)
{
    /* FillRect accepts (HBRUSH)(COLOR_xxx + 1) */
    uintptr_t v = (uintptr_t)b;
    if (v >= 1 && v <= W16_NUM_SYSCOLORS) return w16_sys_brush((int)v - 1);
    return b;
}

int FillRect(HDC dc, LPCRECT r, HBRUSH b)
{
    RECT d;
    lp_rect(dc, r, &d);
    w16_fill_rect_dev(dc, &d, brush_arg(b));
    return 1;
}
int FrameRect(HDC dc, LPCRECT r, HBRUSH b)
{
    RECT d;
    lp_rect(dc, r, &d);
    b = brush_arg(b);
    if (d.right <= d.left || d.bottom <= d.top) return 0;
    w16_fill_rect_dev(dc, &(RECT){d.left, d.top, d.right, d.top + 1}, b);
    w16_fill_rect_dev(dc, &(RECT){d.left, d.bottom - 1, d.right, d.bottom}, b);
    w16_fill_rect_dev(dc, &(RECT){d.left, d.top, d.left + 1, d.bottom}, b);
    w16_fill_rect_dev(dc, &(RECT){d.right - 1, d.top, d.right, d.bottom}, b);
    return 1;
}
/* GDI FillRgn: the region is in logical coordinates of the DC */
BOOL FillRgn(HDC dc, HRGN rgn, HBRUSH b)
{
    if (!dc || !rgn || rgn->kind != OBJ_RGN) return FALSE;
    b = brush_arg(b);
    for (int i = 0; i < rgn->u.rgn.n; i++) {
        RECT d;
        lp_rect(dc, &rgn->u.rgn.r[i], &d);
        w16_fill_rect_dev(dc, &d, b);
    }
    return TRUE;
}
void InvertRect(HDC dc, LPCRECT r)
{
    RECT d;
    lp_rect(dc, r, &d);
    w16_invert_dev(dc, &d);
}
/* PATINVERT of one pattern pixel: palette indices XOR (as the VGA planes do), or the RGB bits for
 * colours outside the VGA palette */
static uint32_t xor_px(uint32_t dst, uint32_t pat)
{
    int a = -1, b = -1;
    for (int i = 0; i < 16; i++) {
        if (vga16[i] == (dst & 0xFFFFFF)) a = i;
        if (vga16[i] == (pat & 0xFFFFFF)) b = i;
    }
    return a >= 0 && b >= 0 ? vga16[a ^ b] : ((dst ^ pat) & 0xFFFFFF);
}

/* USER seg1:2069 / seg1:1FAA: the gray brush PATINVERTed as four full-length one-pixel strips (top,
 * bottom, left, right), so each corner is inverted twice and stays as it was. The brush is a
 * monochrome pattern, so GDI colours it with the DC's text colour (0 bits) and background colour
 * (1 bits, gray_px): black on white inverts every other pixel; the highlight colours a focused combo
 * box field is drawn with turn it black and yellow on the blue (both as on 3.11). */
static void focus_strip(HDC dc, W16Bitmap *t, Region *e, int x0, int y0, int w, int h)
{
    uint32_t fg = w16_rgb(dc->text), bg = w16_rgb(dc->bk);
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < t->w && y < t->h && rgn_contains(e, x, y))
                t->px[y * t->w + x] = xor_px(t->px[y * t->w + x], gray_px(dc, x, y) ? bg : fg);
}

void DrawFocusRect(HDC dc, LPCRECT r)
{
    RECT d;
    lp_rect(dc, r, &d);
    int w = d.right - d.left, h = d.bottom - d.top;
    if (w <= 0 || h <= 0) return;
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    W16Bitmap *t = tgt(dc);
    focus_strip(dc, t, &e, d.left, d.top, w, 1);
    focus_strip(dc, t, &e, d.left, d.bottom - 1, w, 1);
    focus_strip(dc, t, &e, d.left, d.top, 1, h);
    focus_strip(dc, t, &e, d.right - 1, d.top, 1, h);
    rgn_free(&e);
    mark_dirty(dc);
}

/* ------------------------------------------------------------------ lines & shapes */
static const uint8_t pen_pattern[5][8] = {
    {1, 1, 1, 1, 1, 1, 1, 1}, /* solid */
    {1, 1, 1, 1, 1, 1, 0, 0}, /* dash (approximation of VGA.DRV 6 on 2 off scaled) */
    {1, 0, 1, 0, 1, 0, 1, 0}, /* dot */
    {1, 1, 1, 1, 0, 0, 1, 0}, /* dashdot */
    {1, 1, 1, 0, 1, 0, 1, 0}, /* dashdotdot */
};

static void draw_line_dev(HDC dc, Region *clip, int x0, int y0, int x1, int y1, int last)
{
    HPEN p = dc->pen;
    if (!p || p->u.pen.style == PS_NULL) return;
    uint32_t c = w16_rgb(p->u.pen.color);
    uint32_t bk = w16_rgb(dc->bk);
    int w = p->u.pen.width;
    int style = p->u.pen.style;
    if (style > PS_DASHDOTDOT) style = PS_SOLID;
    if (w > 1) style = PS_SOLID;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0;
    for (;;) {
        if (x0 == x1 && y0 == y1 && !last) break;
        if (w == 1) {
            if (pen_pattern[style][n & 7]) put_rop(dc, clip, x0, y0, c, dc->rop2);
            else if (dc->bkmode == OPAQUE) put_rop(dc, clip, x0, y0, bk, dc->rop2);
        } else {
            int h = w / 2;
            for (int yy = y0 - h; yy < y0 - h + w; yy++)
                for (int xx = x0 - h; xx < x0 - h + w; xx++) put_rop(dc, clip, xx, yy, c, dc->rop2);
        }
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

DWORD MoveTo(HDC dc, int x, int y)
{
    DWORD o = MAKELONG(dc->curx, dc->cury);
    dc->curx = x;
    dc->cury = y;
    return o;
}
BOOL LineTo(HDC dc, int x, int y)
{
    int x0 = dc->curx, y0 = dc->cury, x1 = x, y1 = y;
    w16_lp_to_dp(dc, &x0, &y0);
    w16_lp_to_dp(dc, &x1, &y1);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    draw_line_dev(dc, &e, x0, y0, x1, y1, 0);
    rgn_free(&e);
    dc->curx = x;
    dc->cury = y;
    mark_dirty(dc);
    return TRUE;
}
BOOL Polyline(HDC dc, const POINT *p, int n)
{
    if (n < 2) return FALSE;
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    for (int i = 0; i + 1 < n; i++) {
        int x0 = p[i].x, y0 = p[i].y, x1 = p[i + 1].x, y1 = p[i + 1].y;
        w16_lp_to_dp(dc, &x0, &y0);
        w16_lp_to_dp(dc, &x1, &y1);
        draw_line_dev(dc, &e, x0, y0, x1, y1, 0);
    }
    rgn_free(&e);
    mark_dirty(dc);
    return TRUE;
}

/* scanline polygon fill (alternate), device coordinates: the pixels whose integer centres lie inside;
 * the pen outline then adds the boundary (measured on MAIN.CPL's spin-arrow triangles) */
static void fill_poly_dev(HDC dc, const POINT *pt, int n)
{
    if (!dc->brush || dc->brush->u.brush.style == BS_NULL || n < 3) return;
    int ymin = pt[0].y, ymax = pt[0].y;
    for (int i = 1; i < n; i++) { ymin = min(ymin, pt[i].y); ymax = max(ymax, pt[i].y); }
    double *xs = malloc(sizeof(double) * n);
    for (int y = ymin; y <= ymax; y++) {
        int k = 0;
        for (int i = 0; i < n; i++) {
            POINT a = pt[i], b = pt[(i + 1) % n];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                xs[k++] = a.x + (y - a.y) * (b.x - a.x) / (double)(b.y - a.y);
        }
        for (int i = 1; i < k; i++)
            for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) { double t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
        for (int i = 0; i + 1 < k; i += 2) {
            int l = (int)ceil(xs[i] - 1e-9), r = (int)floor(xs[i + 1] + 1e-9);
            if (l <= r) fill_brush_rop(dc, &(RECT){l, y, r + 1, y + 1}, dc->brush, dc->rop2);
        }
    }
    free(xs);
}

BOOL Polygon(HDC dc, const POINT *p, int n)
{
    POINT *d = malloc(sizeof(POINT) * (n + 1));
    for (int i = 0; i < n; i++) { d[i] = p[i]; w16_lp_to_dp(dc, &d[i].x, &d[i].y); }
    fill_poly_dev(dc, d, n);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    for (int i = 0; i < n; i++)
        draw_line_dev(dc, &e, d[i].x, d[i].y, d[(i + 1) % n].x, d[(i + 1) % n].y, 0);
    rgn_free(&e);
    free(d);
    mark_dirty(dc);
    return TRUE;
}

BOOL Rectangle(HDC dc, int l, int t, int r, int b)
{
    RECT d;
    lp_rect(dc, &(RECT){l, t, r, b}, &d);
    HPEN p = dc->pen;
    int pw = (p && p->u.pen.style != PS_NULL) ? p->u.pen.width : 0;
    if (p && p->u.pen.style == PS_NULL) { d.right--; d.bottom--; } /* NULL pen: fill shrinks */
    RECT in = d;
    InflateRect(&in, -pw, -pw);
    fill_brush_rop(dc, &in, dc->brush, dc->rop2);
    if (pw) {
        uint32_t c = w16_rgb(p->u.pen.color);
        if (p->u.pen.style == PS_SOLID || p->u.pen.style == PS_INSIDEFRAME || pw > 1) {
            HBRUSH tmp = CreateSolidBrush(rgb_to_cref(c));
            fill_brush_rop(dc, &(RECT){d.left, d.top, d.right, d.top + pw}, tmp, dc->rop2);
            fill_brush_rop(dc, &(RECT){d.left, d.bottom - pw, d.right, d.bottom}, tmp, dc->rop2);
            fill_brush_rop(dc, &(RECT){d.left, d.top + pw, d.left + pw, d.bottom - pw}, tmp, dc->rop2);
            fill_brush_rop(dc, &(RECT){d.right - pw, d.top + pw, d.right, d.bottom - pw}, tmp, dc->rop2);
            DeleteObject(tmp);
        } else {
            Region e;
            w16_dc_clip_iter_begin(dc, &e);
            draw_line_dev(dc, &e, d.left, d.top, d.right - 1, d.top, 0);
            draw_line_dev(dc, &e, d.right - 1, d.top, d.right - 1, d.bottom - 1, 0);
            draw_line_dev(dc, &e, d.right - 1, d.bottom - 1, d.left, d.bottom - 1, 0);
            draw_line_dev(dc, &e, d.left, d.bottom - 1, d.left, d.top, 0);
            rgn_free(&e);
        }
    }
    mark_dirty(dc);
    return TRUE;
}

static int ellipse_points(RECT d, POINT **out)
{
    double cx = (d.left + d.right - 1) / 2.0, cy = (d.top + d.bottom - 1) / 2.0;
    double rx = (d.right - d.left - 1) / 2.0, ry = (d.bottom - d.top - 1) / 2.0;
    int n = (int)(fabs(rx) + fabs(ry)) * 4 + 16;
    POINT *p = malloc(sizeof(POINT) * n);
    for (int i = 0; i < n; i++) {
        double a = 2 * M_PI * i / n;
        p[i].x = (int)lround(cx + rx * cos(a));
        p[i].y = (int)lround(cy + ry * sin(a));
    }
    *out = p;
    return n;
}

BOOL Ellipse(HDC dc, int l, int t, int r, int b)
{
    RECT d;
    lp_rect(dc, &(RECT){l, t, r, b}, &d);
    POINT *p;
    int n = ellipse_points(d, &p);
    fill_poly_dev(dc, p, n);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    for (int i = 0; i < n; i++) draw_line_dev(dc, &e, p[i].x, p[i].y, p[(i + 1) % n].x, p[(i + 1) % n].y, 0);
    rgn_free(&e);
    free(p);
    mark_dirty(dc);
    return TRUE;
}
BOOL RoundRect(HDC dc, int l, int t, int r, int b, int w, int h)
{
    (void)w; (void)h; /* TODO: rounded corners (T-GDI-02) */
    return Rectangle(dc, l, t, r, b);
}
static BOOL arcish(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2, int mode)
{
    RECT d;
    lp_rect(dc, &(RECT){l, t, r, b}, &d);
    w16_lp_to_dp(dc, &x1, &y1);
    w16_lp_to_dp(dc, &x2, &y2);
    double cx = (d.left + d.right) / 2.0, cy = (d.top + d.bottom) / 2.0;
    double rx = (d.right - d.left) / 2.0, ry = (d.bottom - d.top) / 2.0;
    double a1 = atan2(-(y1 - cy) * rx, (x1 - cx) * ry), a2 = atan2(-(y2 - cy) * rx, (x2 - cx) * ry);
    while (a2 <= a1) a2 += 2 * M_PI; /* counter-clockwise from start to end */
    int n = (int)((rx + ry) * (a2 - a1)) + 8;
    POINT *p = malloc(sizeof(POINT) * (n + 2));
    for (int i = 0; i < n; i++) {
        double a = a1 + (a2 - a1) * i / (n - 1);
        p[i].x = (int)lround(cx + rx * cos(a));
        p[i].y = (int)lround(cy - ry * sin(a));
    }
    int m = n;
    if (mode == 2) p[m++] = (POINT){(int)cx, (int)cy};
    if (mode) fill_poly_dev(dc, p, m);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    for (int i = 0; i + 1 < m; i++) draw_line_dev(dc, &e, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, 0);
    if (mode) draw_line_dev(dc, &e, p[m - 1].x, p[m - 1].y, p[0].x, p[0].y, 0);
    rgn_free(&e);
    free(p);
    mark_dirty(dc);
    return TRUE;
}
BOOL Arc(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2) { return arcish(dc, l, t, r, b, x1, y1, x2, y2, 0); }
BOOL Chord(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2) { return arcish(dc, l, t, r, b, x1, y1, x2, y2, 1); }
BOOL Pie(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2) { return arcish(dc, l, t, r, b, x1, y1, x2, y2, 2); }

COLORREF SetPixel(HDC dc, int x, int y, COLORREF c)
{
    w16_lp_to_dp(dc, &x, &y);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    put_rop(dc, &e, x, y, w16_rgb(c), R2_COPYPEN);
    rgn_free(&e);
    mark_dirty(dc);
    return rgb_to_cref(w16_rgb(c));
}
COLORREF GetPixel(HDC dc, int x, int y)
{
    w16_lp_to_dp(dc, &x, &y);
    W16Bitmap *t = tgt(dc);
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return (COLORREF)-1;
    return rgb_to_cref(t->px[y * t->w + x]);
}
BOOL FloodFill(HDC dc, int x, int y, COLORREF border)
{
    w16_lp_to_dp(dc, &x, &y);
    W16Bitmap *t = tgt(dc);
    uint32_t bc = w16_rgb(border);
    if (x < 0 || y < 0 || x >= t->w || y >= t->h || t->px[y * t->w + x] == bc) return FALSE;
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    uint8_t *seen = calloc((size_t)t->w * t->h, 1);
    int cap = 4096, n = 0;
    POINT *st = malloc(sizeof(POINT) * cap);
    st[n++] = (POINT){x, y};
    while (n) {
        POINT p = st[--n];
        if (p.x < 0 || p.y < 0 || p.x >= t->w || p.y >= t->h) continue;
        size_t i = (size_t)p.y * t->w + p.x;
        if (seen[i] || t->px[i] == bc || !rgn_contains(&e, p.x, p.y)) continue;
        seen[i] = 1;
        if (n + 4 >= cap) { cap *= 2; st = realloc(st, sizeof(POINT) * cap); }
        st[n++] = (POINT){p.x + 1, p.y}; st[n++] = (POINT){p.x - 1, p.y};
        st[n++] = (POINT){p.x, p.y + 1}; st[n++] = (POINT){p.x, p.y - 1};
    }
    for (int yy = 0; yy < t->h; yy++)
        for (int xx = 0; xx < t->w; xx++)
            if (seen[(size_t)yy * t->w + xx]) {
                uint32_t c = brush_px(dc, dc->brush, xx, yy);
                if (!(c & 0x1000000)) t->px[(size_t)yy * t->w + xx] = c;
            }
    free(seen);
    free(st);
    rgn_free(&e);
    mark_dirty(dc);
    return TRUE;
}

/* ------------------------------------------------------------------ BitBlt (ternary raster ops) */
static inline uint32_t rop3(uint8_t rop, uint32_t P, uint32_t S, uint32_t D)
{
    switch (rop) {
    case 0xCC: return S;
    case 0xF0: return P;
    case 0x00: return 0;
    case 0xFF: return 0xFFFFFF;
    case 0x55: return w16_invert_px(D);
    case 0x66: return S ^ D;
    case 0x88: return S & D;
    case 0xEE: return S | D;
    case 0x33: return ~S & 0xFFFFFF;
    case 0x5A: return P ^ D;
    case 0xBB: return (~S | D) & 0xFFFFFF;
    case 0x44: return S & ~D & 0xFFFFFF;
    case 0xC0: return P & S;
    case 0xB8: return ((P ^ D) & S) ^ P; /* PSDPxax */
    case 0xE2: return ((P ^ D) & S) ^ D; /* DSPDxax */
    default: break;
    }
    uint32_t r = 0;
    for (int i = 0; i < 8; i++)
        if (rop & (1 << i)) {
            uint32_t m = ((i & 4) ? P : ~P) & ((i & 2) ? S : ~S) & ((i & 1) ? D : ~D);
            r |= m;
        }
    return r & 0xFFFFFF;
}

static int rop_uses_src(uint8_t r) { return ((r >> 2) & 0x33) != (r & 0x33); }
static int rop_uses_pat(uint8_t r) { return ((r >> 4) & 0x0F) != (r & 0x0F); }

static void blit(HDC dc, int x, int y, int w, int h, const W16Bitmap *src, HDC sdc, int sx, int sy, int sw, int sh, DWORD ropl)
{
    uint8_t rop = (ropl >> 16) & 0xFF;
    int usrc = rop_uses_src(rop) && src, upat = rop_uses_pat(rop);
    W16Bitmap *t = tgt(dc);
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    rgn_and(&e, &(RECT){min(x, x + w), min(y, y + h), max(x, x + w), max(y, y + h)});
    uint32_t fg = w16_rgb(dc->text), bg = w16_rgb(dc->bk);
    uint32_t sbk = sdc ? w16_rgb(sdc->bk) : 0xFFFFFF;
    int dmono = t->mono;
    for (int i = 0; i < e.n; i++) {
        RECT a = e.r[i];
        a.left = max(a.left, 0); a.top = max(a.top, 0);
        a.right = min(a.right, t->w); a.bottom = min(a.bottom, t->h);
        for (int yy = a.top; yy < a.bottom; yy++)
            for (int xx = a.left; xx < a.right; xx++) {
                uint32_t S = 0, P = 0;
                if (usrc) {
                    int rx = sw == w ? sx + (xx - x) : sx + (int)((long)(xx - x) * sw / w);
                    int ry = sh == h ? sy + (yy - y) : sy + (int)((long)(yy - y) * sh / h);
                    if (rx < 0 || ry < 0 || rx >= src->w || ry >= src->h) continue;
                    S = src->px[ry * src->w + rx];
                    if (src->mono && !dmono) S = S ? bg : fg;          /* mono -> colour */
                    else if (!src->mono && dmono) S = S == sbk ? 0xFFFFFF : 0; /* colour -> mono */
                }
                if (upat) {
                    P = brush_px(dc, dc->brush, xx, yy);
                    if (P & 0x1000000) P = 0;
                    if (dmono) P = P == 0xFFFFFF ? 0xFFFFFF : (P == w16_rgb(dc->bk) ? 0xFFFFFF : 0);
                }
                uint32_t *dp = &t->px[yy * t->w + xx];
                uint32_t v = rop3(rop, P, S, *dp);
                if (!dmono && ncolors <= 16) v = w16_rgb(rgb_to_cref(v));
                if (dmono) v = v == 0xFFFFFF ? 0xFFFFFF : (v ? (v == 0xFFFFFF ? 0xFFFFFF : 0) : 0);
                *dp = v;
            }
    }
    rgn_free(&e);
    mark_dirty(dc);
}

void w16_blit_bitmap(HDC dc, int x, int y, const W16Bitmap *bm, int sx, int sy, int w, int h, DWORD rop)
{
    blit(dc, x, y, w, h, bm, NULL, sx, sy, w, h, rop);
}

BOOL BitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    w16_lp_to_dp(dc, &x, &y);
    w = lp_len(dc, w, 1);
    h = lp_len(dc, h, 0);
    const W16Bitmap *s = NULL;
    if (src) {
        w16_lp_to_dp(src, &sx, &sy);
        s = src->target ? src->target : &w16_screen;
        if (s == tgt(dc) && s == &w16_screen) {
            /* screen to screen: copy source first */
            W16Bitmap tmp = {s->w, s->h, 0, malloc((size_t)s->w * s->h * 4)};
            memcpy(tmp.px, s->px, (size_t)s->w * s->h * 4);
            blit(dc, x, y, w, h, &tmp, src, sx, sy, w, h, rop);
            free(tmp.px);
            return TRUE;
        }
    }
    blit(dc, x, y, w, h, s, src, sx, sy, w, h, rop);
    return TRUE;
}
BOOL StretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    w16_lp_to_dp(dc, &x, &y);
    w = lp_len(dc, w, 1);
    h = lp_len(dc, h, 0);
    const W16Bitmap *s = NULL;
    if (src) { w16_lp_to_dp(src, &sx, &sy); s = src->target ? src->target : &w16_screen; }
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    blit(dc, x, y, w, h, s, src, sx, sy, sw, sh, rop);
    return TRUE;
}
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    w16_lp_to_dp(dc, &x, &y);
    w = lp_len(dc, w, 1);
    h = lp_len(dc, h, 0);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    blit(dc, x, y, w, h, NULL, NULL, 0, 0, w, h, rop);
    return TRUE;
}

/* ------------------------------------------------------------------ PNG screenshot (tests) */
#include <zlib.h>
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t crc = crc32(0, (const uint8_t *)type, 4);
    crc = crc32(crc, d, n);
    be32(h, crc);
    fwrite(h, 1, 4, f);
}
int w16_screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int w = w16_screen.w, h = w16_screen.h;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t rl = (size_t)w * 3 + 1, raw_n = rl * h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y * rl] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = w16_display_px(w16_screen.px[y * w + x]);
            raw[y * rl + 1 + x * 3] = p >> 16;
            raw[y * rl + 2 + x * 3] = p >> 8;
            raw[y * rl + 3 + x * 3] = p;
        }
    }
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    compress(z, &zn, raw, raw_n);
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    fclose(f);
    return 0;
}

/* GDI Escape: the 3.x printing interface. TODO(T-PRN-01): render printer DCs to PDF and hand
 * them to CUPS; until then printer DCs refuse STARTDOC so apps show their "cannot print" box. */
int Escape(HDC dc, int esc, int cb, LPCSTR in, void *out)
{
    (void)cb; (void)in; (void)out;
    if (!dc) return SP_ERROR;
    switch (esc) {
    case QUERYESCSUPPORT:
        /* the display driver's: VGA.DRV answers for MOUSETRAILS with its pointer count */
        if (in && *(const int *)in == MOUSETRAILS) return w16_trails_query();
        return 0;
    case MOUSETRAILS:
        return in ? w16_trails_escape(*(const int *)in) : 0;
    case SETABORTPROC:
        return 1;
    default:
        return SP_ERROR;
    }
}
