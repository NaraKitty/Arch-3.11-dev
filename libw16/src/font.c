/* Raster fonts (.FON / .FNT) from the user's 3.11 disks, the GDI font mapper, text output */
#include "w16int.h"
#include <ctype.h>

static W16Font *fonts;
static int fonts_loaded;

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }

static void add_fnt(const uint8_t *d, uint32_t len)
{
    if (len < 118) return;
    int ver = u16(d);
    if (u16(d + 66) & 1) return; /* vector font: not handled (Roman/Script/Modern) */
    W16Font *f = calloc(1, sizeof *f);
    f->fnt = d;
    f->v3 = ver >= 0x300;
    f->points = u16(d + 68);
    f->ascent = u16(d + 74);
    f->ileading = u16(d + 76);
    f->eleading = u16(d + 78);
    f->italic = d[80];
    f->underline = d[81];
    f->strike = d[82];
    f->weight = u16(d + 83);
    f->charset = d[85];
    f->height = u16(d + 88);
    f->pitchfam = d[90];
    f->avgw = u16(d + 91);
    f->maxw = u16(d + 93);
    f->first = d[95];
    f->last = d[96];
    f->defchar = d[97];
    f->breakchar = d[98];
    uint32_t face = u32(d + 105);
    if (face && face < len) snprintf(f->face, sizeof f->face, "%s", (const char *)d + face);
    int ent = f->v3 ? 6 : 4, tab = f->v3 ? 148 : 118;
    for (int c = 0; c < 256; c++) {
        int ch = (c < f->first || c > f->last) ? f->first + f->defchar : c;
        if ((uint32_t)(tab + (ch - f->first) * ent + 2) <= len)
            f->widths[c] = u16(d + tab + (ch - f->first) * ent);
    }
    f->next = fonts;
    fonts = f;
}

static void load_fon(const char *file)
{
    HINSTANCE m = w16_module_open(file);
    if (!m) return;
    for (int i = 0; i < m->nres; i++)
        if (m->res[i].type_id == 8) add_fnt(m->data + m->res[i].off, m->res[i].len);
}

void w16_fonts_init(void)
{
    if (fonts_loaded) return;
    fonts_loaded = 1;
    /* order matters only for ties; later loads are searched first */
    const char *files[] = {"SMALLE.FON", "SYMBOLE.FON", "COURE.FON", "SERIFE.FON", "SSERIFE.FON",
                           "VGAOEM.FON", "VGAFIX.FON", "VGASYS.FON"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) load_fon(files[i]);
}

W16Font *w16_font_system(void)
{
    LOGFONT lf;
    GetObject(GetStockObject(SYSTEM_FONT), sizeof lf, &lf);
    return w16_font_realize(&lf);
}

static const char *substitute(const char *face)
{
    static const char *subs[][2] = {{"Helv", "MS Sans Serif"}, {"Helvetica", "MS Sans Serif"},
                                    {"Tms Rmn", "MS Serif"}, {"Times", "MS Serif"},
                                    {"Arial", "MS Sans Serif"}, {"Times New Roman", "MS Serif"},
                                    {"Courier New", "Courier"}, {"Wingdings", "Symbol"}};
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++)
        if (!strcasecmp(face, subs[i][0])) return subs[i][1];
    return face;
}

typedef struct Realized {
    LOGFONT lf;
    W16Font *f;
    struct Realized *next;
} Realized;
static Realized *realized;

W16Font *w16_font_realize(const LOGFONT *lf)
{
    w16_fonts_init();
    for (Realized *r = realized; r; r = r->next)
        if (!memcmp(&r->lf, lf, sizeof *lf)) return r->f;
    const char *face = lf->lfFaceName[0] ? substitute(lf->lfFaceName) : NULL;
    if (!face) {
        int fam = lf->lfPitchAndFamily & 0xF0, pitch = lf->lfPitchAndFamily & 3;
        if (lf->lfCharSet == OEM_CHARSET) face = "Terminal";
        else if (fam == FF_SWISS) face = "MS Sans Serif";
        else if (fam == FF_ROMAN) face = "MS Serif";
        else if (fam == FF_MODERN || pitch == FIXED_PITCH) face = "Courier";
        else face = "System";
    }
    int want = lf->lfHeight;
    W16Font *best = NULL;
    int bestscore = 1 << 30;
    for (int pass = 0; pass < 2 && !best; pass++)
        for (W16Font *f = fonts; f; f = f->next) {
            if (pass == 0 && strcasecmp(f->face, face)) continue;
            if (pass == 1 && strcasecmp(f->face, "System")) continue;
            int h = want > 0 ? f->height : want < 0 ? f->height - f->ileading : 0;
            int target = want > 0 ? want : -want;
            int score;
            if (want == 0)
                score = abs(f->height - (strcasecmp(face, "MS Sans Serif") ? 16 : 13));
            else if (h <= target)
                score = (target - h) * 2;       /* prefer the largest font that fits */
            else
                score = (h - target) * 2 + 1000; /* larger only if nothing fits */
            if (lf->lfWeight >= 600 && f->weight >= 600) score -= 1;
            if (score < bestscore) { bestscore = score; best = f; }
        }
    if (!best) best = fonts;
    W16Font *out = best;
    int bold = lf->lfWeight >= 600 && best->weight < 600;
    if (bold || lf->lfItalic || lf->lfUnderline || lf->lfStrikeOut) {
        out = malloc(sizeof *out);
        *out = *best;
        out->next = NULL;
        if (bold) {
            out->bold_sim = 1;
            out->weight = FW_BOLD;
            out->avgw += 1;
            out->maxw += 1;
            for (int i = 0; i < 256; i++) out->widths[i] += 1;
        }
        out->italic = lf->lfItalic != 0;
        out->underline = lf->lfUnderline != 0;
        out->strike = lf->lfStrikeOut != 0;
    }
    Realized *r = malloc(sizeof *r);
    r->lf = *lf;
    r->f = out;
    r->next = realized;
    realized = r;
    return out;
}

W16Font *w16_dc_font(HDC dc)
{
    if (!dc->font) return w16_font_system();
    if (!dc->font->u.font.f) dc->font->u.font.f = w16_font_realize(&dc->font->u.font.lf);
    return dc->font->u.font.f;
}

int w16_text_width(W16Font *f, const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += f->widths[(unsigned char)s[i]];
    return w;
}

static void glyph_rows(W16Font *f, int ch, int *w, const uint8_t **data)
{
    if (ch < f->first || ch > f->last) ch = f->first + f->defchar;
    int ent = f->v3 ? 6 : 4, tab = f->v3 ? 148 : 118;
    const uint8_t *e = f->fnt + tab + (ch - f->first) * ent;
    *w = u16(e);
    *data = f->fnt + (f->v3 ? u32(e + 2) : u16(e + 2));
}

/* draw (transparent) glyphs; y is the top of the character cell, device coordinates */
void w16_draw_text_dev(HDC dc, W16Font *f, int x, int y, const char *s, int n, uint32_t fg,
                       const int *dx, int charextra)
{
    Region e;
    w16_dc_clip_iter_begin(dc, &e);
    RECT bounds;
    rgn_bounds(&e, &bounds);
    W16Bitmap *t = dc->target ? dc->target : &w16_screen;
    if (dc->target && dc->target->mono) fg = fg == 0xFFFFFF ? 0xFFFFFF : 0;
    int h = f->height;
    int x0 = x;
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char)s[i];
        int gw;
        const uint8_t *g;
        glyph_rows(f, ch, &gw, &g);
        int adv = f->widths[ch] + charextra;
        if (x < bounds.right && x + gw + 2 > bounds.left && y < bounds.bottom && y + h > bounds.top) {
            int cols = (gw + 7) / 8;
            for (int row = 0; row < h; row++) {
                int py = y + row;
                if (py < 0 || py >= t->h) continue;
                int shear = f->italic ? (f->ascent - row) / 3 : 0;
                for (int c = 0; c < cols; c++) {
                    uint8_t bits = g[c * h + row];
                    if (!bits) continue;
                    for (int b = 0; b < 8; b++) {
                        if (!(bits & (0x80 >> b))) continue;
                        int px = x + c * 8 + b + shear;
                        for (int k = 0; k <= f->bold_sim; k++) {
                            int qx = px + k;
                            if (qx >= 0 && qx < t->w && rgn_contains(&e, qx, py)) t->px[py * t->w + qx] = fg;
                        }
                    }
                }
            }
        }
        x += dx ? dx[i] : adv;
    }
    int lines[2] = {f->underline ? f->ascent + 1 : -1, f->strike ? f->ascent - (f->ascent - f->ileading) / 3 : -1};
    for (int k = 0; k < 2; k++) {
        if (lines[k] < 0) continue;
        int py = y + min(lines[k], h - 1);
        for (int px = x0; px < x; px++)
            if (px >= 0 && px < t->w && py >= 0 && py < t->h && rgn_contains(&e, px, py)) t->px[py * t->w + px] = fg;
    }
    rgn_free(&e);
    if (!dc->target) w16_screen_dirty = 1;
}

/* ------------------------------------------------------------------ GDI text API */
static int extent(HDC dc, W16Font *f, LPCSTR s, int n)
{
    return w16_text_width(f, s, n) + dc->charextra * n;
}

BOOL ExtTextOut(HDC dc, int x, int y, UINT opt, LPCRECT r, LPCSTR s, UINT n, const int *dx)
{
    W16Font *f = w16_dc_font(dc);
    if (dc->align & TA_UPDATECP) { x = dc->curx; y = dc->cury; }
    int w = 0;
    if (dx) for (UINT i = 0; i < n; i++) w += dx[i];
    else w = extent(dc, f, s, n);
    int ha = dc->align & 6, va = dc->align & 24;
    int lx = x, ly = y;
    if (ha == TA_CENTER) lx -= w / 2;
    else if (ha == TA_RIGHT) lx -= w;
    if (va == TA_BASELINE) ly -= f->ascent;
    else if (va == TA_BOTTOM) ly -= f->height;
    if (dc->align & TA_UPDATECP) dc->curx = (ha == TA_RIGHT) ? x - w : (ha == TA_CENTER ? x : x + w);
    int dx0 = lx, dy0 = ly;
    w16_lp_to_dp(dc, &dx0, &dy0);
    int saved_clip = 0;
    Region old;
    if (r) {
        RECT d = *r;
        w16_lp_to_dp(dc, &d.left, &d.top);
        w16_lp_to_dp(dc, &d.right, &d.bottom);
        if (opt & ETO_OPAQUE) w16_fill_solid_dev(dc, &d, w16_rgb(dc->bk));
        if (opt & ETO_CLIPPED) {
            rgn_init(&old);
            rgn_copy(&old, &dc->clip);
            saved_clip = dc->use_clip ? 2 : 1;
            if (!dc->use_clip) { rgn_copy(&dc->clip, &dc->vis); dc->use_clip = 1; }
            rgn_and(&dc->clip, &d);
        }
    }
    if (dc->bkmode == OPAQUE && !(r && (opt & ETO_OPAQUE)))
        w16_fill_solid_dev(dc, &(RECT){dx0, dy0, dx0 + w + f->bold_sim * 0, dy0 + f->height}, w16_rgb(dc->bk));
    w16_draw_text_dev(dc, f, dx0, dy0, s, n, w16_rgb(dc->text), dx, dc->charextra);
    if (saved_clip) {
        rgn_copy(&dc->clip, &old);
        dc->use_clip = saved_clip == 2;
        rgn_free(&old);
    }
    return TRUE;
}

BOOL TextOut(HDC dc, int x, int y, LPCSTR s, int n) { return ExtTextOut(dc, x, y, 0, NULL, s, n, NULL); }

/* GDI's extents include the overhang of a simulated bold or italic font once per string
 * (TEXTMETRIC.tmOverhang); measured on 3.11 dialog text: mnemonic underlines, centred button text
 * and right-aligned statics all come out one pixel wider than the advance widths */
static int overhang(W16Font *f, int n) { return n > 0 ? f->bold_sim : 0; }

DWORD GetTextExtent(HDC dc, LPCSTR s, int n)
{
    W16Font *f = w16_dc_font(dc);
    return MAKELONG(extent(dc, f, s, n) + overhang(f, n), f->height);
}
BOOL GetTextExtentPoint(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
    W16Font *f = w16_dc_font(dc);
    sz->cx = extent(dc, f, s, n) + overhang(f, n);
    sz->cy = f->height;
    return TRUE;
}
BOOL GetTextMetrics(HDC dc, LPTEXTMETRIC tm)
{
    W16Font *f = w16_dc_font(dc);
    memset(tm, 0, sizeof *tm);
    tm->tmHeight = f->height;
    tm->tmAscent = f->ascent;
    tm->tmDescent = f->height - f->ascent;
    tm->tmInternalLeading = f->ileading;
    tm->tmExternalLeading = f->eleading;
    tm->tmAveCharWidth = f->avgw;
    tm->tmMaxCharWidth = f->maxw;
    tm->tmWeight = f->weight;
    tm->tmItalic = f->italic;
    tm->tmUnderlined = f->underline;
    tm->tmStruckOut = f->strike;
    tm->tmFirstChar = f->first;
    tm->tmLastChar = f->last;
    tm->tmDefaultChar = f->first + f->defchar;
    tm->tmBreakChar = f->first + f->breakchar;
    tm->tmPitchAndFamily = f->pitchfam;
    tm->tmCharSet = f->charset;
    tm->tmOverhang = f->bold_sim;
    tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
    return TRUE;
}
int GetTextFace(HDC dc, int cb, LPSTR buf)
{
    W16Font *f = w16_dc_font(dc);
    snprintf(buf, cb, "%s", f->face);
    return strlen(buf);
}
/* VGA.DRV GetCharWidth (seg1:17DC) adds 2 to every width of a simulated bold font when Windows runs
 * in protected mode on a 386 or better, although text output advances by only 1 extra per character
 * (measured: MAIN.CPL's Date & Time fields are sized from digit widths of 8 in its bold dialog font,
 * whose digits advance 7) */
BOOL GetCharWidth(HDC dc, UINT first, UINT last, int *out)
{
    W16Font *f = w16_dc_font(dc);
    for (UINT c = first; c <= last && c < 256; c++) *out++ = f->widths[c] + (f->bold_sim ? 1 : 0);
    return TRUE;
}
int AddFontResource(LPCSTR file)
{
    const char *b = strrchr(file, '\\');
    load_fon(b ? b + 1 : file);
    return 1;
}

/* ------------------------------------------------------------------ tabbed text */
static int next_tab(int x, int origin, int ntabs, const int *tabs, int avg)
{
    int rel = x - origin;
    if (ntabs == 0 || !tabs) {
        int t = 8 * avg;
        return origin + (rel / t + 1) * t;
    }
    if (ntabs == 1) {
        int t = tabs[0] > 0 ? tabs[0] : 8 * avg;
        return origin + (rel / t + 1) * t;
    }
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] > rel) return origin + tabs[i];
    return x + avg;
}

static int tabbed(HDC dc, int x, int y, LPCSTR s, int n, int ntabs, const int *tabs, int origin, int draw)
{
    W16Font *f = w16_dc_font(dc);
    int cx = x;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || s[i] == '\t') {
            int seg = extent(dc, f, s + start, i - start);
            if (draw && i > start) TextOut(dc, cx, y, s + start, i - start);
            cx += seg;
            if (i < n) {
                int nx = next_tab(cx, origin, ntabs, tabs, f->avgw);
                if (draw && dc->bkmode == OPAQUE) {
                    int a = cx, b2 = y, c = nx, d = y + f->height;
                    w16_lp_to_dp(dc, &a, &b2);
                    w16_lp_to_dp(dc, &c, &d);
                    w16_fill_solid_dev(dc, &(RECT){a, b2, c, d}, w16_rgb(dc->bk));
                }
                cx = nx;
            }
            start = i + 1;
        }
    }
    return cx - x;
}
LONG TabbedTextOut(HDC dc, int x, int y, LPCSTR s, int n, int ntabs, const int *tabs, int origin)
{
    int w = tabbed(dc, x, y, s, n, ntabs, tabs, origin, 1);
    return MAKELONG(w, w16_dc_font(dc)->height);
}
DWORD GetTabbedTextExtent(HDC dc, LPCSTR s, int n, int ntabs, const int *tabs)
{
    int w = tabbed(dc, 0, 0, s, n, ntabs, tabs, 0, 0);
    return MAKELONG(w, w16_dc_font(dc)->height);
}

/* ------------------------------------------------------------------ prefix (&) text */
/* advance of the text with '&' prefixes removed */
static int prefix_advance(HDC dc, const char *s, int n)
{
    W16Font *f = w16_dc_font(dc);
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '&') { if (i + 1 < n && s[i + 1] == '&') i++; else continue; }
        w += f->widths[(unsigned char)s[i]] + dc->charextra;
    }
    return w;
}

/* its extent, as USER measures it (GetTextExtent: plus the overhang) */
int w16_prefix_text_width(HDC dc, const char *s, int n)
{
    int w = prefix_advance(dc, s, n);
    return w > 0 ? w + w16_dc_font(dc)->bold_sim : w;
}

char w16_mnemonic(const char *s)
{
    for (; s && *s; s++)
        if (*s == '&') {
            if (s[1] == '&') { s++; continue; }
            return toupper((unsigned char)s[1]);
        }
    return 0;
}

/* draw with '&' prefixes processed: underline the mnemonic character (like USER does) */
void w16_draw_prefix_text(HDC dc, int x, int y, const char *s, int n, int noprefix)
{
    if (noprefix) { TextOut(dc, x, y, s, n); return; }
    W16Font *f = w16_dc_font(dc);
    char buf[1024];
    int m = 0, ul = -1;
    for (int i = 0; i < n && m < (int)sizeof buf - 1; i++) {
        if (s[i] == '&') {
            if (i + 1 < n && s[i + 1] == '&') { buf[m++] = '&'; i++; continue; }
            if (i + 1 < n) ul = m;
            continue;
        }
        buf[m++] = s[i];
    }
    TextOut(dc, x, y, buf, m);
    if (ul >= 0) {
        /* underline: the extent of the mnemonic character (overhang included), from its origin */
        int ux = x + extent(dc, f, buf, ul), uw = f->widths[(unsigned char)buf[ul]] + f->bold_sim;
        int uy = y + f->ascent + 1;
        int a = ux, b = uy, c = ux + uw, d = uy + 1;
        w16_lp_to_dp(dc, &a, &b);
        w16_lp_to_dp(dc, &c, &d);
        w16_fill_solid_dev(dc, &(RECT){a, b, c, d}, w16_rgb(dc->text));
    }
}

/* ------------------------------------------------------------------ DrawText (USER seg6:0571) */
/* seg6:0311: where the word at p ends: at a blank (with DT_WORDBREAK) or a tab, which is a word by
 * itself when it comes first, or before a CR or LF */
static int dt_word_end(const char *s, int p, int n, int wordbreak)
{
    for (int first = 1; p < n; p++, first = 0) {
        char c = s[p];
        if ((c == ' ' && wordbreak) || c == '\t') return p + first;
        if (c == '\r' || c == '\n') return p;
    }
    return p;
}

/* seg6:0360: a piece of a line from x (relative to the line's start): its advance (without the
 * overhang) is added to x, which is returned. Tabs - always when measuring, with DT_EXPANDTABS when
 * drawing - go to the next multiple of the tab width, counted from half an average character on.
 * Drawn at org + x on line y when `draw`. */
static int dt_piece(HDC dc, W16Font *f, int x, int org, int y, const char *s, int n, int tabw, int expand,
                    int noprefix, int draw)
{
    int st = 0;
    for (int k = 0; k <= n; k++) {
        if (k < n && !(expand && s[k] == '\t')) continue;
        if (draw && k > st) w16_draw_prefix_text(dc, org + x, y, s + st, k - st, noprefix);
        x += noprefix ? extent(dc, f, s + st, k - st) : prefix_advance(dc, s + st, k - st);
        if (k < n && tabw) x = ((x + f->avgw / 2) / tabw + 1) * tabw;
        st = k + 1;
    }
    return x;
}

/* one line [s, s + n) at y: aligned on the line's extent - trailing blanks included, as 3.1
 * measures the whole piece - and drawn; returns that extent (overhang included) */
static int dt_line(HDC dc, W16Font *f, const char *s, int n, LPRECT r, int y, UINT fmt, int tabw)
{
    int noprefix = (fmt & DT_NOPREFIX) != 0, expand = (fmt & DT_EXPANDTABS) != 0;
    int org = r->left, width = r->right - r->left;
    if (fmt & (DT_CENTER | DT_RIGHT)) {
        int d = width - (dt_piece(dc, f, 0, 0, 0, s, n, tabw, 1, noprefix, 0) + f->bold_sim);
        /* a line wider than the rectangle starts left of it by half the excess, rounded down
         * (measured: a 7-px "/" in MAIN.CPL's 6-px Date & Time separator starts 1 px left) */
        if (fmt & DT_CENTER) d >>= 1;
        org += d;
    }
    int w = dt_piece(dc, f, 0, org, y, s, n, tabw, expand, noprefix, !(fmt & DT_CALCRECT));
    return n > 0 ? w + f->bold_sim : w;   /* GetTextExtent: the overhang once */
}

int DrawText(HDC dc, LPCSTR s, int n, LPRECT r, UINT fmt)
{
    W16Font *f = w16_dc_font(dc);
    if (n < 0) n = strlen(s);
    UINT fmt0 = fmt;
    int tabs = 8;
    if (fmt & DT_TABSTOP) {
        /* the tab size is in the high byte, which then carries no flags */
        tabs = (fmt >> 8) & 0xFF;
        fmt &= ~0xFF00u;
    }
    int lh = f->height + ((fmt & DT_EXTERNALLEADING) ? f->eleading : 0);
    int width = r->right - r->left;
    int tabw = tabs * f->avgw;
    int wordbreak = (fmt & DT_WORDBREAK) != 0, left = !(fmt & (DT_CENTER | DT_RIGHT));
    int y = r->top, maxw = 0;
    int saved = 0;
    if (!(fmt & (DT_NOCLIP | DT_CALCRECT))) { saved = SaveDC(dc); IntersectClipRect(dc, r->left, r->top, r->right, r->bottom); }
    if (n == 0 || width == 0) {
        /* 3.1 draws nothing (and leaves DT_CALCRECT's rectangle undefined) */
    } else if (fmt & DT_SINGLELINE) {
        if (fmt & DT_VCENTER) y = r->top + (r->bottom - r->top - f->height) / 2;
        else if (fmt & DT_BOTTOM) y = r->bottom - f->height;
        maxw = dt_line(dc, f, s, n, r, y, fmt, tabw);
    } else {
        /* words are added while they fit; a word that does not fit starts the next line unless it
         * is the first (a long word overflows). Left-aligned text drops one blank at a wrap or after
         * a CR/LF; other lines keep their blanks, trailing ones included. */
        int ls = 0, le = 0, p = 0, x = 0;
        while (p < n) {
            int we = dt_word_end(s, p, n, wordbreak), brk = 0;
            le = we;
            x = dt_piece(dc, f, x, 0, 0, s + p, we - p, tabw, 1, (fmt & DT_NOPREFIX) != 0, 0);
            if (wordbreak && x + f->bold_sim > width && p != ls) {
                if (left && s[p] == ' ') p++;
                we = le = p;
                brk = 1;
            } else if (we < n && (s[we] == '\r' || s[we] == '\n')) {
                char c = s[we++];
                if (we < n && s[we] == (c ^ 7)) we++;   /* CR LF or LF CR */
                brk = 1;
                if (left && we < n && s[we] == ' ') we++;
            }
            p = we;
            if (!brk) continue;
            maxw = max(maxw, dt_line(dc, f, s + ls, le - ls, r, y, fmt, tabw));
            y += lh;
            ls = le = p;
            x = 0;
            if (!(fmt & (DT_NOCLIP | DT_CALCRECT)) && r->bottom < y) break;   /* below the rectangle */
        }
        if (le > ls) maxw = max(maxw, dt_line(dc, f, s + ls, le - ls, r, y, fmt, tabw));
    }
    if (saved) RestoreDC(dc, saved);
    if (fmt & DT_CALCRECT) {
        r->right = r->left + maxw;
        /* a word wider than the rectangle widens it, and the text is laid out again */
        if (width < maxw) return DrawText(dc, s, n, r, fmt0);
        r->bottom = y + lh;   /* the last line counts even when empty (text ending in a break) */
    }
    return y + lh - r->top;
}

/* GrayString: draws text dimmed. With a solid gray available (16 colours have one),
 * USER draws text in COLOR_GRAYTEXT. */
BOOL GrayString(HDC dc, HBRUSH b, void *fn, LPARAM data, int n, int x, int y, int cx, int cy)
{
    (void)b; (void)cx; (void)cy;
    if (fn) return FALSE;
    const char *s = (const char *)data;
    if (n <= 0) n = strlen(s);
    COLORREF old = SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    int m = SetBkMode(dc, TRANSPARENT);
    TextOut(dc, x, y, s, n);
    SetBkMode(dc, m);
    SetTextColor(dc, old);
    return TRUE;
}

void w16_draw_gray_text(HDC dc, int x, int y, const char *s, int n, int noprefix)
{
    COLORREF old = SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    w16_draw_prefix_text(dc, x, y, s, n, noprefix);
    SetTextColor(dc, old);
}

/* Grayed text where GRAYTEXT would vanish into the background (push buttons: GRAYTEXT ==
 * BTNFACE on VGA): like GrayString with the 50% gray brush, only every other text pixel is
 * drawn, in the normal text colour. GrayString grays the text in a bitmap of its own, so the
 * checkerboard starts at the text's origin, not the screen's (measured on 3.11: the Desktop
 * applet's disabled "Test" and "Setup..." buttons, 27 px apart, have the same phase) */
void w16_draw_stippled_text(HDC dc, int x, int y, const char *s, int n, int noprefix, COLORREF fg)
{
    TEXTMETRIC tm;
    GetTextMetrics(dc, &tm);
    int w = w16_prefix_text_width(dc, s, n), h = tm.tmHeight;
    if (w <= 0 || h <= 0) return;
    COLORREF *save = malloc(sizeof *save * w * h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) save[j * w + i] = GetPixel(dc, x + i, y + j);
    COLORREF old = SetTextColor(dc, fg);
    w16_draw_prefix_text(dc, x, y, s, n, noprefix);
    SetTextColor(dc, old);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if ((i + j) & 1) {
                COLORREF c = save[j * w + i];
                if (c != (COLORREF)-1 && GetPixel(dc, x + i, y + j) != c) SetPixel(dc, x + i, y + j, c);
            }
        }
    free(save);
}
