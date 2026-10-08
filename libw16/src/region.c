/* rectangle-list regions: enough for visible regions, update regions and clipping */
#include "w16int.h"

static int r_empty(const RECT *r) { return r->left >= r->right || r->top >= r->bottom; }

void rgn_init(Region *g) { g->n = g->cap = 0; g->r = NULL; }
void rgn_free(Region *g) { free(g->r); rgn_init(g); }
void rgn_clear(Region *g) { g->n = 0; }

static void push(Region *g, const RECT *r)
{
    if (r_empty(r))
        return;
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->r = realloc(g->r, sizeof(RECT) * g->cap);
    }
    g->r[g->n++] = *r;
}

void rgn_set(Region *g, const RECT *r) { g->n = 0; push(g, r); }

void rgn_copy(Region *d, const Region *s)
{
    d->n = 0;
    for (int i = 0; i < s->n; i++)
        push(d, &s->r[i]);
}

void rgn_sub(Region *g, const RECT *c)
{
    if (r_empty(c) || g->n == 0)
        return;
    Region out;
    rgn_init(&out);
    for (int i = 0; i < g->n; i++) {
        RECT a = g->r[i];
        if (c->right <= a.left || c->left >= a.right || c->bottom <= a.top || c->top >= a.bottom) {
            push(&out, &a);
            continue;
        }
        RECT t;
        /* top band */
        t = (RECT){a.left, a.top, a.right, max(a.top, c->top)};
        push(&out, &t);
        /* bottom band */
        t = (RECT){a.left, min(a.bottom, c->bottom), a.right, a.bottom};
        push(&out, &t);
        int y0 = max(a.top, c->top), y1 = min(a.bottom, c->bottom);
        t = (RECT){a.left, y0, max(a.left, c->left), y1};
        push(&out, &t);
        t = (RECT){min(a.right, c->right), y0, a.right, y1};
        push(&out, &t);
    }
    free(g->r);
    *g = out;
}

void rgn_add(Region *g, const RECT *r)
{
    if (r_empty(r))
        return;
    rgn_sub(g, r);
    push(g, r);
}

void rgn_and(Region *g, const RECT *c)
{
    int j = 0;
    for (int i = 0; i < g->n; i++) {
        RECT a = g->r[i];
        a.left = max(a.left, c->left);
        a.top = max(a.top, c->top);
        a.right = min(a.right, c->right);
        a.bottom = min(a.bottom, c->bottom);
        if (!r_empty(&a))
            g->r[j++] = a;
    }
    g->n = j;
}

void rgn_and_rgn(Region *g, const Region *o)
{
    Region out;
    rgn_init(&out);
    for (int i = 0; i < g->n; i++)
        for (int k = 0; k < o->n; k++) {
            RECT a = g->r[i], c = o->r[k];
            a.left = max(a.left, c.left);
            a.top = max(a.top, c.top);
            a.right = min(a.right, c.right);
            a.bottom = min(a.bottom, c.bottom);
            push(&out, &a);
        }
    free(g->r);
    *g = out;
}

void rgn_offset(Region *g, int dx, int dy)
{
    for (int i = 0; i < g->n; i++)
        OffsetRect(&g->r[i], dx, dy);
}

int rgn_empty(const Region *g) { return g->n == 0; }

void rgn_bounds(const Region *g, RECT *o)
{
    if (!g->n) {
        SetRectEmpty(o);
        return;
    }
    *o = g->r[0];
    for (int i = 1; i < g->n; i++) {
        o->left = min(o->left, g->r[i].left);
        o->top = min(o->top, g->r[i].top);
        o->right = max(o->right, g->r[i].right);
        o->bottom = max(o->bottom, g->r[i].bottom);
    }
}

int rgn_contains(const Region *g, int x, int y)
{
    for (int i = 0; i < g->n; i++)
        if (x >= g->r[i].left && x < g->r[i].right && y >= g->r[i].top && y < g->r[i].bottom)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ RECT helpers (USER) */
void SetRect(LPRECT r, int l, int t, int rt, int b) { r->left = l; r->top = t; r->right = rt; r->bottom = b; }
void SetRectEmpty(LPRECT r) { SetRect(r, 0, 0, 0, 0); }
void CopyRect(LPRECT d, LPCRECT s) { *d = *s; }
BOOL IsRectEmpty(LPCRECT r) { return r_empty(r); }
BOOL PtInRect(LPCRECT r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
void OffsetRect(LPRECT r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; }
void InflateRect(LPRECT r, int dx, int dy) { r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy; }
BOOL EqualRect(LPCRECT a, LPCRECT b) { return !memcmp(a, b, sizeof(RECT)); }
BOOL IntersectRect(LPRECT d, LPCRECT a, LPCRECT b)
{
    RECT t = {max(a->left, b->left), max(a->top, b->top), min(a->right, b->right), min(a->bottom, b->bottom)};
    if (r_empty(&t)) {
        SetRectEmpty(d);
        return FALSE;
    }
    *d = t;
    return TRUE;
}
BOOL UnionRect(LPRECT d, LPCRECT a, LPCRECT b)
{
    if (r_empty(a)) { *d = *b; return !r_empty(b); }
    if (r_empty(b)) { *d = *a; return TRUE; }
    RECT t = {min(a->left, b->left), min(a->top, b->top), max(a->right, b->right), max(a->bottom, b->bottom)};
    *d = t;
    return TRUE;
}
