/* GDI memory metafiles: a 3.1 metafile handle is the global handle of its bits (METAHEADER then the
 * records), so SetMetaFileBits and GetMetaFileBits only hand the handle over. PlayMetaFile replays the
 * records through libw16's GDI: the drawing, attribute, mapping, clipping, object and DIB records 3.1
 * programs put in clipboard pictures. Records not handled are skipped (W16_LOG names them); creation
 * records still take their object slot so later indices stay right. Recording metafiles
 * (CreateMetaFile) is not here yet. UNTESTED against pictures made by real 3.1 programs. */
#include "w16int.h"

#pragma pack(push, 2)
typedef struct { WORD mtType, mtHeaderSize, mtVersion; DWORD mtSize; WORD mtNoObjects; DWORD mtMaxRecord; WORD mtNoParameters; } METAHEADER;
#pragma pack(pop)

HMETAFILE SetMetaFileBits(HGLOBAL h) { return IsValidMetaFile(h) ? h : NULL; }
HGLOBAL GetMetaFileBits(HMETAFILE h) { return h; }
BOOL DeleteMetaFile(HMETAFILE h) { return h && !GlobalFree(h); }

BOOL IsValidMetaFile(HMETAFILE h)
{
    if (!h || GlobalSize(h) < sizeof(METAHEADER)) return FALSE;
    const METAHEADER *m = GlobalLock(h);
    BOOL ok = m && (m->mtType == 1 || m->mtType == 2) && m->mtHeaderSize == 9 && (m->mtVersion == 0x300 || m->mtVersion == 0x100);
    GlobalUnlock(h);
    return ok;
}

static COLORREF cref(const WORD *p) { return (COLORREF)p[0] | (COLORREF)p[1] << 16; }
static short S(WORD w) { return (short)w; }

/* a packed DIB (BITMAPINFO + bits) drawn into dst, stretched as StretchBlt does */
static void draw_dib(HDC dc, const uint8_t *dib, size_t cb, int dx, int dy, int dw, int dh,
                     int sx, int sy, int sw, int sh, DWORD rop)
{
    if (cb < sizeof(BITMAPINFOHEADER)) return;
    const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)dib;
    int ncol = bih->biClrUsed ? (int)bih->biClrUsed : bih->biBitCount <= 8 ? 1 << bih->biBitCount : 0;
    const uint8_t *bits = dib + bih->biSize + ncol * 4;
    if (bits > dib + cb) return;
    HBITMAP bm = CreateDIBitmap(dc, bih, CBM_INIT, bits, (const BITMAPINFO *)dib, DIB_RGB_COLORS);
    if (!bm) return;
    HDC mdc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mdc, bm);
    /* a DIB's source rectangle counts from its bottom line */
    int top = (int)bih->biHeight - sy - sh;
    StretchBlt(dc, dx, dy, dw, dh, mdc, sx, top, sw, sh, rop);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    DeleteObject(bm);
}

BOOL PlayMetaFile(HDC dc, HMETAFILE h)
{
    if (!dc || !IsValidMetaFile(h)) return FALSE;
    const uint8_t *base = GlobalLock(h);
    size_t total = GlobalSize(h);
    const METAHEADER *mh = (const METAHEADER *)base;
    int nobj = mh->mtNoObjects;
    HGDIOBJ *obj = calloc(nobj ? nobj : 1, sizeof *obj);
    int saved = SaveDC(dc);
    size_t off = mh->mtHeaderSize * 2;
    while (off + 6 <= total) {
        const uint8_t *r = base + off;
        DWORD size = (DWORD)(r[0] | r[1] << 8 | r[2] << 16 | (DWORD)r[3] << 24);
        WORD fn = (WORD)(r[4] | r[5] << 8);
        if (fn == 0 || size < 3 || off + size * 2 > total) break;
        const WORD *p = (const WORD *)(r + 6);
        size_t np = size - 3;
        HGDIOBJ made = NULL;
        int creates = 0;
        switch (fn) {
        case 0x0201: SetBkColor(dc, cref(p)); break;
        case 0x0102: SetBkMode(dc, S(p[0])); break;
        case 0x0103: SetMapMode(dc, S(p[0])); break;
        case 0x0104: SetROP2(dc, S(p[0])); break;
        case 0x0106: case 0x0107: break;          /* poly fill / stretch-blt mode: GDI defaults only */
        case 0x0108: SetTextCharacterExtra(dc, S(p[0])); break;
        case 0x0209: SetTextColor(dc, cref(p)); break;
        case 0x012E: SetTextAlign(dc, p[0]); break;
        case 0x020B: SetWindowOrg(dc, S(p[1]), S(p[0])); break;
        case 0x020C: SetWindowExt(dc, S(p[1]), S(p[0])); break;
        case 0x020D: SetViewportOrg(dc, S(p[1]), S(p[0])); break;
        case 0x020E: SetViewportExt(dc, S(p[1]), S(p[0])); break;
        case 0x0213: LineTo(dc, S(p[1]), S(p[0])); break;
        case 0x0214: MoveTo(dc, S(p[1]), S(p[0])); break;
        case 0x0415: ExcludeClipRect(dc, S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x0416: IntersectClipRect(dc, S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x0418: Ellipse(dc, S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x041B: Rectangle(dc, S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x041F: SetPixel(dc, S(p[3]), S(p[2]), cref(p)); break;
        case 0x061C: RoundRect(dc, S(p[5]), S(p[4]), S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x061D: PatBlt(dc, S(p[5]), S(p[4]), S(p[3]), S(p[2]), cref(p)); break;
        case 0x0817: Arc(dc, S(p[7]), S(p[6]), S(p[5]), S(p[4]), S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x081A: Pie(dc, S(p[7]), S(p[6]), S(p[5]), S(p[4]), S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x0830: Chord(dc, S(p[7]), S(p[6]), S(p[5]), S(p[4]), S(p[3]), S(p[2]), S(p[1]), S(p[0])); break;
        case 0x001E: SaveDC(dc); break;
        case 0x0127: RestoreDC(dc, S(p[0])); break;
        case 0x0324: case 0x0325: {
            int n = S(p[0]);
            if (n <= 0 || (size_t)(1 + 2 * n) > np) break;
            POINT *pt = malloc(sizeof *pt * n);
            if (!pt) break;
            for (int i = 0; i < n; i++) { pt[i].x = S(p[1 + 2 * i]); pt[i].y = S(p[2 + 2 * i]); }
            if (fn == 0x0324) Polygon(dc, pt, n); else Polyline(dc, pt, n);
            free(pt);
            break;
        }
        case 0x0538: {
            int npoly = S(p[0]);
            const WORD *cnt = p + 1, *q = p + 1 + npoly;
            for (int k = 0; k < npoly && q < p + np; k++) {
                int n = S(cnt[k]);
                POINT *pt = malloc(sizeof *pt * (n > 0 ? n : 1));
                if (!pt) break;
                for (int i = 0; i < n; i++) { pt[i].x = S(q[2 * i]); pt[i].y = S(q[2 * i + 1]); }
                Polygon(dc, pt, n);
                free(pt);
                q += 2 * n;
            }
            break;
        }
        case 0x0521: {
            int n = S(p[0]);
            const char *s = (const char *)(p + 1);
            const WORD *xy = p + 1 + (n + 1) / 2;
            if (n > 0 && xy + 1 < p + np) TextOut(dc, S(xy[1]), S(xy[0]), s, n);
            break;
        }
        case 0x0A32: {
            int y = S(p[0]), x = S(p[1]), n = S(p[2]);
            UINT opt = p[3];
            const WORD *q = p + 4;
            RECT rc, *prc = NULL;
            if (opt & (ETO_OPAQUE | ETO_CLIPPED)) {
                rc.left = S(q[0]); rc.top = S(q[1]); rc.right = S(q[2]); rc.bottom = S(q[3]);
                prc = &rc;
                q += 4;
            }
            const char *s = (const char *)q;
            const WORD *dxw = q + (n + 1) / 2;
            int *dx = NULL;
            if (n > 0 && dxw + n <= p + np) {
                dx = malloc(sizeof *dx * n);
                if (dx) for (int i = 0; i < n; i++) dx[i] = S(dxw[i]);
            }
            ExtTextOut(dc, x, y, opt, prc, s, n, dx);
            free(dx);
            break;
        }
        case 0x012D: if (p[0] < nobj && obj[p[0]]) SelectObject(dc, obj[p[0]]); break;
        case 0x01F0:
            if (p[0] < nobj && obj[p[0]]) {
                DeleteObject(obj[p[0]]);
                obj[p[0]] = NULL;
            }
            break;
        case 0x02FA: {                              /* CreatePenIndirect: style, width (x, y), colour */
            creates = 1;
            made = CreatePen(S(p[0]), S(p[1]), cref(p + 3));
            break;
        }
        case 0x02FC: {                              /* CreateBrushIndirect: style, colour, hatch */
            creates = 1;
            LOGBRUSH lb = {p[0], cref(p + 1), S(p[3])};
            made = CreateBrushIndirect(&lb);
            break;
        }
        case 0x02FB: {                              /* CreateFontIndirect: the 16-bit LOGFONT */
            creates = 1;
            LOGFONT lf;
            memset(&lf, 0, sizeof lf);
            lf.lfHeight = S(p[0]); lf.lfWidth = S(p[1]); lf.lfEscapement = S(p[2]); lf.lfOrientation = S(p[3]);
            lf.lfWeight = S(p[4]);
            const uint8_t *b = (const uint8_t *)(p + 5);
            lf.lfItalic = b[0]; lf.lfUnderline = b[1]; lf.lfStrikeOut = b[2]; lf.lfCharSet = b[3];
            lf.lfOutPrecision = b[4]; lf.lfClipPrecision = b[5]; lf.lfQuality = b[6]; lf.lfPitchAndFamily = b[7];
            if (np * 2 > 18) snprintf(lf.lfFaceName, LF_FACESIZE, "%.*s", (int)(np * 2 - 18 < LF_FACESIZE - 1 ? np * 2 - 18 : LF_FACESIZE - 1), (const char *)(b + 8));
            made = CreateFontIndirect(&lf);
            break;
        }
        case 0x00F7: case 0x06FF: case 0x0142: case 0x01F9:
            creates = 1;                            /* palette, region, DIB pattern brush: the slot only */
            W16_LOG("PlayMetaFile: record %04X takes an object slot, not drawn\n", fn);
            break;
        case 0x0234: case 0x0035: break;            /* SelectPalette / RealizePalette: not a palette device */
        case 0x0940:                                /* DIBBitBlt: rop, ysrc, xsrc, [dib] or h, w, ydst, xdst */
            if (np > 8) draw_dib(dc, (const uint8_t *)(p + 8), (np - 8) * 2, S(p[7]), S(p[6]), S(p[5]), S(p[4]),
                                 S(p[3]), S(p[2]), S(p[5]), S(p[4]), cref(p));
            else if (np >= 8) PatBlt(dc, S(p[7]), S(p[6]), S(p[5]), S(p[4]), cref(p));
            break;
        case 0x0B41:                                /* DIBStretchBlt: rop, sh, sw, sy, sx, dh, dw, dy, dx, dib */
            if (np > 10) draw_dib(dc, (const uint8_t *)(p + 10), (np - 10) * 2, S(p[9]), S(p[8]), S(p[7]), S(p[6]),
                                  S(p[5]), S(p[4]), S(p[3]), S(p[2]), cref(p));
            break;
        case 0x0F43:                                /* StretchDIBits: rop, usage, sh, sw, sy, sx, dh, dw, dy, dx, dib */
            if (np > 11) draw_dib(dc, (const uint8_t *)(p + 11), (np - 11) * 2, S(p[10]), S(p[9]), S(p[8]), S(p[7]),
                                  S(p[6]), S(p[5]), S(p[4]), S(p[3]), cref(p));
            break;
        case 0x0626: break;                         /* Escape */
        default:
            W16_LOG("PlayMetaFile: record %04X skipped\n", fn);
            break;
        }
        if (creates) {
            int i;
            for (i = 0; i < nobj && obj[i]; i++) ;
            if (i < nobj) obj[i] = made ? made : (HGDIOBJ)GetStockObject(NULL_BRUSH);
            else if (made) DeleteObject(made);
        }
        off += size * 2;
    }
    RestoreDC(dc, saved);
    for (int i = 0; i < nobj; i++) if (obj[i] && !obj[i]->stock) DeleteObject(obj[i]);
    free(obj);
    GlobalUnlock(h);
    return TRUE;
}
