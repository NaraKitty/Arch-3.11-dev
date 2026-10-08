/* TrueType fonts: the .TTF files among the user's ripped files (Arial, Times New Roman, Courier New,
 * Symbol, Wingdings) as 3.1's font mapper offers them when WIN.INI [TrueType] TTEnable is on.
 *
 * Outlines are hinted by the system's FreeType 2 with its v35 interpreter (the Windows 3.1/95
 * TrueType instruction semantics) and scan-converted by FreeType in monochrome. FreeType is opened at
 * run time (libfreetype.so.6), so libw16 builds without its headers; without it, TrueType faces fall
 * back to the raster fonts as before. Only FreeType 2's stable public ABI is declared below.
 *
 * Measured against real 3.11 (Clock's digital face, Arial at 77, 57, 28, 21 and 11 pixels per em):
 * the hinted advance widths and so every glyph position are identical; tmAscent and tmDescent are the
 * font's VDMX yMax and -yMin (77: 70 and 17, 57: 52 and 13, where usWinAscent and usWinDescent
 * scaled and rounded give 70/16 and 52/12 and put the big clock's time a row too low); the glyph
 * bitmaps match except for a few pixels per line whose centres lie on an outline edge (3.1's own
 * scan converter, in GDI, leaves them off: 3 pixels in "9:30:03 AM" at 28, 7 in "10/8/26" at 21,
 * none at 11).
 * UNTESTED: positive lfHeight (cell height), lfHeight 0, lfWidth, simulated bold/italic, symbol fonts,
 * characters 0x80-0x9F, underline/strikeout positions, tmExternalLeading, glyphs reaching past the
 * VDMX cell (3.1 clips them). */
#include "w16int.h"
#include <dirent.h>
#include <dlfcn.h>
#include <ctype.h>

/* ------------------------------------------------------------------ FreeType 2 public ABI */
typedef long FT_Pos, FT_Fixed, FT_Long;
typedef unsigned long FT_ULong;
typedef int FT_Error, FT_Int;
typedef unsigned int FT_UInt;
typedef short FT_Short;
typedef unsigned short FT_UShort;
typedef struct { FT_Pos x, y; } FT_Vector;
typedef struct { FT_Pos xMin, yMin, xMax, yMax; } FT_BBox;
typedef struct { void *data; void (*finalizer)(void *); } FT_Generic;
typedef struct {
    unsigned int rows, width;
    int pitch;
    unsigned char *buffer;
    unsigned short num_grays;
    unsigned char pixel_mode, palette_mode;
    void *palette;
} FT_Bitmap;
typedef struct { FT_Pos width, height, horiBearingX, horiBearingY, horiAdvance, vertBearingX, vertBearingY, vertAdvance; } FT_Glyph_Metrics;
typedef struct FT_GlyphSlotRec_ {
    void *library, *face, *next;
    FT_UInt glyph_index;
    FT_Generic generic;
    FT_Glyph_Metrics metrics;
    FT_Fixed linearHoriAdvance, linearVertAdvance;
    FT_Vector advance;
    unsigned int format;
    FT_Bitmap bitmap;
    FT_Int bitmap_left, bitmap_top;
} *FT_GlyphSlot;
typedef struct FT_FaceRec_ {
    FT_Long num_faces, face_index, face_flags, style_flags, num_glyphs;
    char *family_name, *style_name;
    FT_Int num_fixed_sizes;
    void *available_sizes;
    FT_Int num_charmaps;
    void *charmaps;
    FT_Generic generic;
    FT_BBox bbox;
    FT_UShort units_per_EM;
    FT_Short ascender, descender, height, max_advance_width, max_advance_height, underline_position,
        underline_thickness;
    FT_GlyphSlot glyph;
    void *size, *charmap;
} *FT_Face;
#define FT_STYLE_FLAG_ITALIC 1
#define FT_STYLE_FLAG_BOLD 2
#define FT_LOAD_RENDER (1L << 2)
#define FT_LOAD_NO_BITMAP (1L << 3)
#define FT_LOAD_MONOCHROME (1L << 12)
#define FT_LOAD_TARGET_MONO (2L << 16)

static struct {
    FT_Error (*Init_FreeType)(void **);
    FT_Error (*New_Face)(void *, const char *, FT_Long, FT_Face *);
    FT_Error (*Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
    FT_UInt (*Get_Char_Index)(FT_Face, FT_ULong);
    FT_Error (*Load_Glyph)(FT_Face, FT_UInt, FT_Long);
    FT_Error (*Property_Set)(void *, const char *, const char *, const void *);
    FT_Error (*Load_Sfnt_Table)(FT_Face, FT_ULong, FT_Long, unsigned char *, FT_ULong *);
} ft;
static void *ftlib;

/* ------------------------------------------------------------------ the font files */
typedef struct TTFile {
    FT_Face face;
    char family[LF_FACESIZE];
    int bold, italic, symbol;
    int win_ascent, win_descent, avg_width, weight, upem;
    struct { unsigned char has; short ymax, ymin; } vdmx[256]; /* by pixels per em */
    struct TTFile *next;
} TTFile;
static TTFile *files;

typedef struct { int done, w, h, left, top, pitch; unsigned char *bits; } TTGlyph;
struct W16TT {
    TTFile *file;
    int ppem;
    TTGlyph g[256];
};

static unsigned u16be(const unsigned char *p) { return p[0] << 8 | p[1]; }

/* VDMX (vertical device metrics): for each size in pixels per em, the highest and lowest pixel of the
 * hinted glyphs, in groups by device aspect ratio. GDI takes tmAscent = yMax and tmDescent = -yMin
 * from the first group whose ratio fits the display. The display has 96 x 96 dots per inch, so a
 * group xRatio : yStartRatio..yEndRatio fits when yStartRatio <= xRatio <= yEndRatio (0:0..0 is the
 * group for every ratio). The group's character set byte (all glyphs or the ANSI ones) is not looked
 * at: every ripped font has ANSI groups only. */
static void load_vdmx(TTFile *t, const unsigned char *v, unsigned long len)
{
    if (len < 6) return;
    unsigned long nratios = u16be(v + 4);
    if (6 + 6 * nratios > len) return;
    for (unsigned long i = 0; i < nratios; i++) {
        const unsigned char *r = v + 6 + 4 * i; /* bCharSet, xRatio, yStartRatio, yEndRatio */
        if (!(r[2] <= r[1] && r[1] <= r[3])) continue;
        unsigned long g = u16be(v + 6 + 4 * nratios + 2 * i);
        if (g + 4 > len) return;
        unsigned long recs = u16be(v + g);
        for (unsigned long k = 0; k < recs && g + 4 + 6 * k + 6 <= len; k++) {
            const unsigned char *e = v + g + 4 + 6 * k; /* yPelHeight, yMax, yMin */
            unsigned ppem = u16be(e);
            if (ppem < 256) {
                t->vdmx[ppem].has = 1;
                t->vdmx[ppem].ymax = (short)u16be(e + 2);
                t->vdmx[ppem].ymin = (short)u16be(e + 4);
            }
        }
        return;
    }
}

static void add_file(const char *path)
{
    FT_Face face;
    if (ft.New_Face(ftlib, path, 0, &face)) return;
    TTFile *t = calloc(1, sizeof *t);
    t->face = face;
    snprintf(t->family, sizeof t->family, "%s", face->family_name ? face->family_name : "");
    t->bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    t->italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    t->upem = face->units_per_EM;
    t->win_ascent = face->ascender;
    t->win_descent = -face->descender;
    t->weight = t->bold ? FW_BOLD : FW_NORMAL;
    unsigned char os2[96];
    FT_ULong len = sizeof os2;
    /* OS/2: xAvgCharWidth at 2, usWeightClass at 4, usWinAscent at 74, usWinDescent at 76 */
    if (!ft.Load_Sfnt_Table(face, 0x4F532F32UL /* 'OS/2' */, 0, os2, &len) && len >= 78) {
        t->avg_width = (short)u16be(os2 + 2);
        t->weight = u16be(os2 + 4);
        t->win_ascent = u16be(os2 + 74);
        t->win_descent = u16be(os2 + 76);
    }
    FT_ULong vlen = 0; /* a length of 0 asks for the table's size */
    if (!ft.Load_Sfnt_Table(face, 0x56444D58UL /* 'VDMX' */, 0, NULL, &vlen) && vlen) {
        unsigned char *v = malloc(vlen);
        if (v && !ft.Load_Sfnt_Table(face, 0x56444D58UL, 0, v, &vlen)) load_vdmx(t, v, vlen);
        free(v);
    }
    /* a symbol font has only the (3,0) cmap: its characters sit at 0xF000 + code */
    t->symbol = !ft.Get_Char_Index(face, 'A') && ft.Get_Char_Index(face, 0xF041);
    t->next = files;
    files = t;
}

static int tt_init(void)
{
    static int state; /* 0 not tried, 1 ready, -1 unavailable */
    if (state) return state > 0;
    state = -1;
    void *h = dlopen("libfreetype.so.6", RTLD_NOW);
    if (!h) h = dlopen("libfreetype.so", RTLD_NOW);
    if (!h) return 0;
    *(void **)&ft.Init_FreeType = dlsym(h, "FT_Init_FreeType");
    *(void **)&ft.New_Face = dlsym(h, "FT_New_Face");
    *(void **)&ft.Set_Pixel_Sizes = dlsym(h, "FT_Set_Pixel_Sizes");
    *(void **)&ft.Get_Char_Index = dlsym(h, "FT_Get_Char_Index");
    *(void **)&ft.Load_Glyph = dlsym(h, "FT_Load_Glyph");
    *(void **)&ft.Property_Set = dlsym(h, "FT_Property_Set");
    *(void **)&ft.Load_Sfnt_Table = dlsym(h, "FT_Load_Sfnt_Table");
    if (!ft.Init_FreeType || !ft.New_Face || !ft.Set_Pixel_Sizes || !ft.Get_Char_Index || !ft.Load_Glyph ||
        !ft.Load_Sfnt_Table || ft.Init_FreeType(&ftlib))
        return 0;
    unsigned int v35 = 35; /* the Windows 3.1/95 interpreter */
    if (!ft.Property_Set || ft.Property_Set(ftlib, "truetype", "interpreter-version", &v35)) {
        W16_LOG("TrueType: FreeType without the v35 interpreter, TrueType off\n");
        return 0;
    }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/files", w16_assets_dir());
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcasecmp(e->d_name + n - 4, ".TTF")) {
                char path[1400];
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                add_file(path);
            }
        }
        closedir(d);
    }
    state = files ? 1 : -1;
    return state > 0;
}

/* Windows ANSI (cp1252) to Unicode for 0x80-0x9F; the rest is Latin-1 */
static FT_ULong ansi_to_unicode(int c)
{
    static const unsigned short hi[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    if (c >= 0x80 && c < 0xA0 && hi[c - 0x80]) return hi[c - 0x80];
    return (FT_ULong)c;
}

static FT_UInt glyph_index(TTFile *t, int c)
{
    return ft.Get_Char_Index(t->face, t->symbol ? 0xF000 + (FT_ULong)c : ansi_to_unicode(c));
}

static int scaled(int v, int ppem, int upem) { return (v * ppem + upem / 2) / upem; }

/* the size in pixels per em for a cell height: the largest size VDMX lists whose cell (yMax - yMin) is
 * not higher, when the table reaches that height; else the height scaled by the em over usWinAscent +
 * usWinDescent. UNTESTED (Clock asks for character heights): both rules, and which of two sizes with
 * the same cell VDMX gives (Arial: 13 and 14, 77 and 78). */
static int cell_ppem(TTFile *t, int height)
{
    int ppem = 0;
    for (int p = 1; p < 256; p++) {
        if (!t->vdmx[p].has) continue;
        if (t->vdmx[p].ymax - t->vdmx[p].ymin > height) {
            if (ppem) return ppem;
            break;
        }
        ppem = p;
    }
    return MulDiv(height, t->upem, t->win_ascent + t->win_descent);
}

/* GDI's font mapper for a TrueType face: the face named in the LOGFONT, nearest in weight and slant;
 * the size in pixels per em is -lfHeight (character height), or found from the cell height lfHeight */
W16Font *w16_tt_realize(const LOGFONT *lf)
{
    if (!lf->lfFaceName[0] || !GetProfileInt("TrueType", "TTEnable", 1) || !tt_init()) return NULL;
    int want_bold = lf->lfWeight >= 600, want_italic = lf->lfItalic != 0;
    TTFile *best = NULL;
    int bestscore = 1 << 30;
    for (TTFile *t = files; t; t = t->next) {
        if (strcasecmp(t->family, lf->lfFaceName)) continue;
        int score = (t->bold != want_bold) * 2 + (t->italic != want_italic);
        if (score < bestscore) { bestscore = score; best = t; }
    }
    if (!best) return NULL;
    int ppem = lf->lfHeight < 0 ? -lf->lfHeight
             : lf->lfHeight > 0 ? cell_ppem(best, lf->lfHeight)
                                : MulDiv(12, 96, 72); /* UNTESTED: 3.1's default size */
    if (ppem < 1) ppem = 1;
    W16Font *f = calloc(1, sizeof *f);
    f->tt = calloc(1, sizeof *f->tt);
    f->tt->file = best;
    f->tt->ppem = ppem;
    snprintf(f->face, sizeof f->face, "%s", best->family);
    f->points = MulDiv(ppem, 72, 96);
    if (ppem < 256 && best->vdmx[ppem].has) {
        f->ascent = best->vdmx[ppem].ymax;
        f->height = best->vdmx[ppem].ymax - best->vdmx[ppem].ymin;
    } else { /* UNTESTED: every ripped font lists 8 to 255 pixels per em in VDMX */
        f->ascent = scaled(best->win_ascent, ppem, best->upem);
        f->height = f->ascent + scaled(best->win_descent, ppem, best->upem);
    }
    f->ileading = f->height - ppem;
    f->eleading = 0;
    f->avgw = scaled(best->avg_width, ppem, best->upem);
    f->maxw = scaled(best->face->max_advance_width, ppem, best->upem);
    f->weight = best->weight;
    f->italic = best->italic;
    f->underline = lf->lfUnderline != 0;
    f->strike = lf->lfStrikeOut != 0;
    f->charset = best->symbol ? SYMBOL_CHARSET : ANSI_CHARSET;
    f->pitchfam = (lf->lfPitchAndFamily & 0xF0) | 4 /* TMPF_TRUETYPE */ | 1 /* variable pitch */;
    f->first = 32;
    f->last = 255;
    f->defchar = 0x80 - 32;
    f->breakchar = 0;
    /* hinted advance widths: GDI's (measured: digits 16, ':' 8, 'A' 19, 'M' 23 at 28 ppem) */
    ft.Set_Pixel_Sizes(best->face, 0, ppem);
    for (int c = 0; c < 256; c++) {
        FT_UInt gi = glyph_index(best, c < 32 ? 32 : c);
        if (!ft.Load_Glyph(best->face, gi, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_MONO))
            f->widths[c] = (int)(best->face->glyph->advance.x >> 6);
    }
    return f;
}

static TTGlyph *glyph(W16Font *f, int c)
{
    TTGlyph *g = &f->tt->g[c & 255];
    if (g->done) return g;
    g->done = 1;
    TTFile *t = f->tt->file;
    ft.Set_Pixel_Sizes(t->face, 0, f->tt->ppem);
    if (ft.Load_Glyph(t->face, glyph_index(t, c), FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO)) return g;
    FT_GlyphSlot s = t->face->glyph;
    g->w = s->bitmap.width;
    g->h = s->bitmap.rows;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->pitch = s->bitmap.pitch < 0 ? -s->bitmap.pitch : s->bitmap.pitch;
    if (g->w && g->h && s->bitmap.buffer) {
        g->bits = malloc((size_t)g->pitch * g->h);
        memcpy(g->bits, s->bitmap.buffer, (size_t)g->pitch * g->h);
    }
    return g;
}

/* transparent glyphs, y the top of the character cell (device coordinates), as w16_draw_text_dev */
void w16_tt_draw_text(HDC dc, W16Font *f, int x, int y, const char *s, int n, uint32_t fg, const int *dx, int charextra,
                      const Region *clip, W16Bitmap *t)
{
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        TTGlyph *g = glyph(f, c);
        int gx = x + g->left, gy = y + f->ascent - g->top;
        for (int r = 0; r < g->h; r++) {
            int py = gy + r;
            if (py < 0 || py >= t->h) continue;
            const unsigned char *row = g->bits + (size_t)r * g->pitch;
            for (int k = 0; k < g->w; k++) {
                if (!(row[k >> 3] & (0x80 >> (k & 7)))) continue;
                int qx = gx + k;
                if (qx >= 0 && qx < t->w && rgn_contains(clip, qx, py)) t->px[py * t->w + qx] = fg;
            }
        }
        x += dx ? dx[i] : f->widths[c] + charextra;
    }
}
