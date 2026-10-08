/* Dialog templates built in code, in the same Win16 binary format as DIALOG resources, so dialogs
 * written for arch311 (new applets) go through exactly the same dialog engine as the 3.11 ones. */
#include "w16int.h"

struct W16DlgTemplate {
    uint8_t *b;
    size_t n, cap;
    size_t count_at; /* offset of the item count byte */
};

static void put(W16DlgTemplate *t, const void *p, size_t n)
{
    if (t->n + n > t->cap) {
        t->cap = (t->n + n) * 2 + 64;
        t->b = realloc(t->b, t->cap);
    }
    memcpy(t->b + t->n, p, n);
    t->n += n;
}
static void put8(W16DlgTemplate *t, int v) { uint8_t b = (uint8_t)v; put(t, &b, 1); }
static void put16(W16DlgTemplate *t, int v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(t, b, 2); }
static void put32(W16DlgTemplate *t, DWORD v) { put16(t, v & 0xFFFF); put16(t, v >> 16); }
static void putsz(W16DlgTemplate *t, const char *s) { put(t, s ? s : "", strlen(s ? s : "") + 1); }

W16DlgTemplate *w16_dlgt_new(DWORD style, int x, int y, int cx, int cy, LPCSTR caption, int pt, LPCSTR face)
{
    W16DlgTemplate *t = calloc(1, sizeof *t);
    if (face) style |= DS_SETFONT;
    put32(t, style);
    t->count_at = t->n;
    put8(t, 0);
    put16(t, x); put16(t, y); put16(t, cx); put16(t, cy);
    put8(t, 0); /* no menu */
    put8(t, 0); /* default dialog class */
    putsz(t, caption);
    if (style & DS_SETFONT) {
        put16(t, pt);
        putsz(t, face);
    }
    return t;
}

void w16_dlgt_add(W16DlgTemplate *t, LPCSTR cls, LPCSTR text, int id, DWORD style, int x, int y, int cx, int cy)
{
    static const char *pre[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX"};
    put16(t, x); put16(t, y); put16(t, cx); put16(t, cy);
    put16(t, id);
    put32(t, style | WS_CHILD | WS_VISIBLE);
    int k = -1;
    for (int i = 0; i < 6; i++) if (!strcasecmp(cls, pre[i])) k = i;
    if (k >= 0) put8(t, 0x80 + k);
    else putsz(t, cls);
    putsz(t, text);
    put8(t, 0); /* no creation data */
    t->b[t->count_at]++;
}

const void *w16_dlgt_data(W16DlgTemplate *t) { return t->b; }

void w16_dlgt_free(W16DlgTemplate *t)
{
    if (!t) return;
    free(t->b);
    free(t);
}
