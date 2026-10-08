/* USER's clipboard (seg39), and how arch311 programs share it.
 *
 * Within a program everything is 3.1's: a table of {format, handle} entries; SetClipboardData with a
 * NULL handle promises the format (delayed rendering: GetClipboardData then sends WM_RENDERFORMAT to
 * the owner); CloseClipboard marks CF_TEXT / CF_OEMTEXT for synthesis from the other one (handle -1,
 * converted on the first GetClipboardData); EmptyClipboard sends WM_DESTROYCLIPBOARD to the old owner
 * and makes the window that opened the clipboard the owner; a changed clipboard sends
 * WM_DRAWCLIPBOARD down the viewer chain once it is closed; the owner's destruction sends
 * WM_RENDERALLFORMATS and keeps only what it rendered (seg8:094C).
 *
 * In 3.1 every program shares USER's single table. arch311 programs are separate processes, so a
 * program that changes the clipboard publishes it in a shared folder ($ARCH311_CLIPBOARD, else
 * $XDG_RUNTIME_DIR/arch311-clipboard, else /tmp/arch311-clipboard-UID) when it closes the clipboard,
 * and the others take it over the next time they look (OpenClipboard, IsClipboardFormatAvailable,
 * CountClipboardFormats and the message loop): as if another task had emptied and filled it -
 * WM_DESTROYCLIPBOARD to their owner, WM_DRAWCLIPBOARD to their viewers, no owner. Differences that
 * follow (each UNTESTED against 3.1 programs that depend on them): formats promised for delayed
 * rendering are rendered (WM_RENDERFORMAT) when the clipboard is closed, before publishing;
 * CF_OWNERDISPLAY (painted by its owner) does not reach other programs; registered formats travel by
 * name; text is also put on the Linux clipboard, and Linux text that arch311 did not put there
 * arrives as CF_TEXT. */
#include "w16int.h"
#include <SDL.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

typedef struct { UINT fmt; HANDLE h; } ClipEnt;
#define PENDING ((HANDLE)(intptr_t)-1)      /* text to synthesize from the other text format */

static ClipEnt *ents;      /* [0x160] */
static int nents;          /* [0x15e] */
static int open_task;      /* [0x162] */
static HWND owner;         /* [0x164] */
static HWND viewer;        /* [0x166] */
static int changed;        /* [0x168] */
static int in_draw;        /* [0x16a] */
static HWND open_wnd;      /* [0x16c] */

HTASK GetCurrentTask(void) { return (HTASK)(uintptr_t)1; } /* one task per program */

ATOM w16_user_atom_add(LPCSTR name);
ATOM w16_user_atom_delete(ATOM atom);
UINT w16_user_atom_name(ATOM atom, LPSTR buf, int cb);
void w16_user_atom_ref(ATOM atom);

static void import_shared(void);
static void publish(void);
static int local_change;   /* this program changed the clipboard since it opened it */

/* seg39:0000 */
static ClipEnt *find_fmt(UINT fmt)
{
    if (!fmt || !ents) return NULL;
    for (int i = 0; i < nents; i++) if (ents[i].fmt == fmt) return &ents[i];
    return NULL;
}

/* seg39:018A: 0 owner display, 1 GDI object, 2 global memory, 3 metafile picture */
static int fmt_type(UINT fmt)
{
    if (fmt == CF_DSPMETAFILEPICT || fmt == CF_METAFILEPICT) return 3;
    if (fmt == CF_BITMAP || fmt == CF_PALETTE || fmt == CF_DSPBITMAP) return 1;
    if (fmt == CF_OWNERDISPLAY) return 0;
    return 2;
}

/* seg39:069A */
static int is_pending(const ClipEnt *e) { return (e->fmt == CF_TEXT || e->fmt == CF_OEMTEXT) && e->h == PENDING; }

/* seg39:01C2 */
static void free_data(ClipEnt *e)
{
    if (!e->h) return;
    switch (fmt_type(e->fmt)) {
    case 1: DeleteObject(e->h); break;
    case 3: {
        METAFILEPICT *mp = GlobalLock(e->h);
        if (mp && mp->hMF) DeleteMetaFile(mp->hMF);
        GlobalUnlock(e->h);
    }   /* fall through */
    case 2: if (!is_pending(e)) GlobalFree(e->h); break;
    }
}

/* seg39:0226 */
static void send_owner(UINT m, WPARAM wp)
{
    if (owner && w16_valid(owner)) SendMessage(owner, m, wp, 0);
}

/* seg39:05AC */
static void draw_clipboard(void)
{
    changed = 0;
    if (in_draw || !viewer) return;
    in_draw = 1;
    if (w16_valid(viewer)) SendMessage(viewer, WM_DRAWCLIPBOARD, (WPARAM)owner, 0);
    in_draw = 0;
}

static void clear_table(void)
{
    for (int i = 0; i < nents; i++) {
        if (ents[i].fmt >= 0xC000) w16_user_atom_delete((ATOM)ents[i].fmt);
        free_data(&ents[i]);
    }
    free(ents);
    ents = NULL;
    nents = 0;
}

BOOL IsClipboardFormatAvailable(UINT fmt) { import_shared(); return find_fmt(fmt) != NULL; }
int CountClipboardFormats(void) { import_shared(); return nents; }
HWND GetClipboardViewer(void) { return viewer; }
HWND GetClipboardOwner(void) { return owner; }
HWND GetOpenClipboardWindow(void) { return open_wnd; }

/* seg39:004B */
int GetClipboardFormatName(UINT fmt, LPSTR buf, int cb)
{
    if (fmt < 0xC000) return 0;
    return (int)w16_user_atom_name((ATOM)fmt, buf, cb);
}
/* seg1:8214 */
UINT RegisterClipboardFormat(LPCSTR name) { return w16_user_atom_add(name); }

/* seg39:0080: the first listed format the clipboard has (zero entries skipped); -1 if none of them, 0 if
 * the clipboard is empty */
int GetPriorityClipboardFormat(UINT *list, int n)
{
    import_shared();
    if (!nents || !ents) return 0;
    for (int i = 0; i < n; i++)
        if (list[i] && find_fmt(list[i])) return (int)list[i];
    return -1;
}

/* seg39:0104 */
HWND SetClipboardViewer(HWND h)
{
    HWND old = viewer;
    viewer = h;
    draw_clipboard();
    return old;
}

/* seg39:0125 */
BOOL ChangeClipboardChain(HWND remove, HWND next)
{
    if (!viewer) return FALSE;
    if (remove == viewer) { viewer = next; return TRUE; }
    /* 3.1 passes MAKELONG(next, 0); libw16 window handles are pointers, so lParam is the handle */
    return (BOOL)SendMessage(viewer, WM_CHANGECBCHAIN, (WPARAM)remove, (LPARAM)next);
}

/* seg39:060B */
BOOL OpenClipboard(HWND h)
{
    import_shared();
    if (open_wnd != h && open_task) return FALSE;
    open_wnd = h;
    open_task = 1;
    return TRUE;
}

/* seg39:0261 */
BOOL EmptyClipboard(void)
{
    if (!open_task) return FALSE;
    if (owner) send_owner(WM_DESTROYCLIPBOARD, 0);
    clear_table();
    owner = open_wnd;
    changed = 1;
    local_change = 1;
    return TRUE;
}

/* seg39:0315 */
static HANDLE set_data(UINT fmt, HANDLE h)
{
    if (!open_task || !fmt) return NULL;
    ClipEnt *e = find_fmt(fmt);
    if (!e) {
        ClipEnt *n = realloc(ents, sizeof *n * (nents + 1));
        if (!n) return NULL;
        ents = n;
        if (fmt >= 0xC000) w16_user_atom_ref((ATOM)fmt);
        e = &ents[nents++];
        e->fmt = fmt;
        e->h = NULL;
    } else
        free_data(e);
    e->h = h;
    /* 3.1 then makes the memory (and a picture's metafile) shareable (GMEM_DDESHARE): nothing to do
     * in one address space */
    changed = 1;
    local_change = 1;
    return h;
}

/* seg39:02F4 */
HANDLE SetClipboardData(UINT fmt, HANDLE h)
{
    if (fmt == 0xFFFF) return NULL;
    return set_data(fmt, h);
}

/* seg39:0425 */
HANDLE GetClipboardData(UINT fmt)
{
    if (!open_task) return NULL;
    ClipEnt *e = find_fmt(fmt);
    if (!e) return NULL;
    HANDLE h = e->h;
    if (!h) {                                       /* delayed rendering */
        if (owner) {
            int c = changed;
            send_owner(WM_RENDERFORMAT, fmt);
            changed = c;
        }
        e = find_fmt(fmt);
        h = e ? e->h : NULL;
    } else if (h == PENDING) {                      /* synthesize from the other text format */
        HANDLE src = GetClipboardData(fmt == CF_OEMTEXT ? CF_TEXT : CF_OEMTEXT);
        h = NULL;
        if (src) {
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, GlobalSize(src));
            const char *s = g ? GlobalLock(src) : NULL;
            char *d = s ? GlobalLock(g) : NULL;
            if (d) {
                if (fmt == CF_TEXT) OemToAnsi(s, d); else AnsiToOem(s, d);
                GlobalUnlock(g);
                h = g;
            } else if (g)
                GlobalFree(g);
            if (s) GlobalUnlock(src);
        }
        e = find_fmt(fmt);
        if (e) e->h = h;
    }
    return h;
}

/* seg39:0643 */
BOOL CloseClipboard(void)
{
    if (!open_task) return FALSE;
    int oem = find_fmt(CF_OEMTEXT) != NULL, text = find_fmt(CF_TEXT) != NULL;
    if (text) { if (!oem) set_data(CF_OEMTEXT, PENDING); }
    else if (oem) set_data(CF_TEXT, PENDING);
    if (local_change) publish();
    local_change = 0;
    open_wnd = NULL;
    open_task = 0;
    if (changed) draw_clipboard();
    return TRUE;
}

/* seg39:06BB */
UINT EnumClipboardFormats(UINT fmt)
{
    if (!open_task || !ents) return 0;
    if (!fmt) return nents ? ents[0].fmt : 0;
    ClipEnt *e = find_fmt(fmt);
    if (!e || e + 1 == ents + nents) return 0;
    return e[1].fmt;
}

/* the owner's destruction (seg8:09D5 -> seg8:094C), before it gets WM_DESTROY */
void w16_clipboard_on_destroy(HWND h)
{
    if (!h || h != owner) return;
    send_owner(WM_RENDERALLFORMATS, 0);
    int n = 0, text = 0;
    for (int i = 0; i < nents; i++) {
        ClipEnt *e = &ents[i];
        if (!e->h) continue;
        if (e->h == PENDING && !text) continue;
        ents[n++] = *e;
        if (e->h != PENDING && (e->fmt == CF_TEXT || e->fmt == CF_OEMTEXT)) text = 1;
    }
    owner = NULL;
    if (nents != n) changed = 1;
    nents = n;
    if (changed) draw_clipboard();
}

/* a destroyed window leaves USER's records (seg8:0C3C) */
void w16_clipboard_on_free(HWND h)
{
    if (viewer == h) viewer = NULL;
    if (owner == h) owner = NULL;
    if (open_wnd == h) open_wnd = NULL;
}

/* ------------------------------------------------------------------ sharing between programs */
static char *shared_path(void)
{
    static char path[512];
    if (path[0]) return path;
    const char *d = getenv("ARCH311_CLIPBOARD");
    char dir[400];
    if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    else if ((d = getenv("XDG_RUNTIME_DIR")) && *d) snprintf(dir, sizeof dir, "%s/arch311-clipboard", d);
    else snprintf(dir, sizeof dir, "/tmp/arch311-clipboard-%u", (unsigned)getuid());
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/data", dir);
    return path;
}

static long long last_seq;          /* the shared clipboard this program has */
static char *last_linux_text;       /* the Linux clipboard text arch311 last put there or took */

static void put_bytes(FILE *f, UINT fmt, char kind, const void *p, size_t n)
{
    char name[256] = "-";
    if (fmt >= 0xC000 && !w16_user_atom_name((ATOM)fmt, name, sizeof name)) return;
    fprintf(f, "F %u %c %zu %s\n", fmt >= 0xC000 ? 0 : fmt, kind, n, name);
    fwrite(p, 1, n, f);
    fputc('\n', f);
}

static void linux_text(const char *ansi)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    char *u = w16_ansi_to_utf8(ansi);
    free(last_linux_text);
    last_linux_text = strdup(u);
    SDL_SetClipboardText(u);
    free(u);
}

static void publish(void)
{
    /* formats promised for delayed rendering are rendered now (see the top of the file) */
    for (int i = 0; i < nents; i++)
        if (!ents[i].h && ents[i].fmt != CF_OWNERDISPLAY && owner) {
            int c = changed;
            send_owner(WM_RENDERFORMAT, ents[i].fmt);
            changed = c;
        }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.%d", shared_path(), (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long seq = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    fprintf(f, "ARCH311CLIP 1 %lld %d\n", seq, (int)getpid());
    for (int i = 0; i < nents; i++) {
        ClipEnt *e = &ents[i];
        if (!e->h || e->h == PENDING) continue;
        switch (fmt_type(e->fmt)) {
        case 2: {
            void *p = GlobalLock(e->h);
            if (p) put_bytes(f, e->fmt, 'M', p, GlobalSize(e->h));
            GlobalUnlock(e->h);
            if (e->fmt == CF_TEXT && p) { linux_text(GlobalLock(e->h)); GlobalUnlock(e->h); }
            break;
        }
        case 1: {
            if (e->h && ((HGDIOBJ)e->h)->kind == OBJ_BITMAP) {
                W16Bitmap *b = w16_bitmap_of(e->h);
                size_t n = 12 + (size_t)b->w * b->h * 4;
                uint8_t *buf = malloc(n);
                if (!buf) break;
                int32_t hd[3] = {b->w, b->h, b->mono};
                memcpy(buf, hd, 12);
                memcpy(buf + 12, b->px, (size_t)b->w * b->h * 4);
                put_bytes(f, e->fmt, 'B', buf, n);
                free(buf);
            } else if (e->h && ((HGDIOBJ)e->h)->kind == OBJ_PAL) {
                PALETTEENTRY pe[256];
                UINT n = GetPaletteEntries(e->h, 0, 256, pe);
                put_bytes(f, e->fmt, 'P', pe, n * sizeof *pe);
            }
            break;
        }
        case 3: {
            METAFILEPICT *mp = GlobalLock(e->h);
            if (mp && mp->hMF) {
                void *bits = GlobalLock(mp->hMF);
                size_t n = GlobalSize(mp->hMF);
                uint8_t *buf = malloc(6 + n);
                if (buf && bits) {
                    int16_t hd[3] = {mp->mm, mp->xExt, mp->yExt};
                    memcpy(buf, hd, 6);
                    memcpy(buf + 6, bits, n);
                    put_bytes(f, e->fmt, 'Z', buf, 6 + n);
                }
                free(buf);
                GlobalUnlock(mp->hMF);
            }
            GlobalUnlock(e->h);
            break;
        }
        }
    }
    fclose(f);
    if (rename(tmp, shared_path()) == 0) last_seq = seq;
    else unlink(tmp);
}

static HANDLE load_entry(char kind, const uint8_t *p, size_t n)
{
    switch (kind) {
    case 'M': {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
        void *d = g ? GlobalLock(g) : NULL;
        if (d) { memcpy(d, p, n); GlobalUnlock(g); }
        return g;
    }
    case 'B': {
        if (n < 12) return NULL;
        int32_t hd[3];
        memcpy(hd, p, 12);
        if (hd[0] <= 0 || hd[1] <= 0 || n < 12 + (size_t)hd[0] * hd[1] * 4) return NULL;
        HBITMAP b = CreateBitmap(hd[0], hd[1], 1, hd[2] ? 1 : 4, NULL);
        W16Bitmap *bm = w16_bitmap_of(b);
        if (bm) memcpy(bm->px, p + 12, (size_t)hd[0] * hd[1] * 4);
        return b;
    }
    case 'P': {
        UINT cnt = (UINT)(n / sizeof(PALETTEENTRY));
        LOGPALETTE *lp = malloc(sizeof *lp + cnt * sizeof(PALETTEENTRY));
        if (!lp) return NULL;
        lp->palVersion = 0x300;
        lp->palNumEntries = (WORD)cnt;
        memcpy(lp->palPalEntry, p, cnt * sizeof(PALETTEENTRY));
        HPALETTE h = CreatePalette(lp);
        free(lp);
        return h;
    }
    case 'Z': {
        if (n < 6) return NULL;
        HGLOBAL bits = GlobalAlloc(GMEM_MOVEABLE, n - 6);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(METAFILEPICT));
        void *d = bits ? GlobalLock(bits) : NULL;
        METAFILEPICT *mp = g ? GlobalLock(g) : NULL;
        if (!d || !mp) { if (bits) GlobalFree(bits); if (g) GlobalFree(g); return NULL; }
        memcpy(d, p + 6, n - 6);
        GlobalUnlock(bits);
        int16_t hd[3];
        memcpy(hd, p, 6);
        mp->mm = hd[0]; mp->xExt = hd[1]; mp->yExt = hd[2];
        mp->hMF = SetMetaFileBits(bits);
        GlobalUnlock(g);
        return g;
    }
    }
    return NULL;
}

/* take over another program's clipboard, as if its task had emptied and filled USER's */
static void take_over(void)
{
    if (owner) send_owner(WM_DESTROYCLIPBOARD, 0);
    clear_table();
    owner = NULL;
}

static void synth_text_pair(void)
{
    int oem = find_fmt(CF_OEMTEXT) != NULL, text = find_fmt(CF_TEXT) != NULL;
    open_task = 1;
    if (text && !oem) set_data(CF_OEMTEXT, PENDING);
    else if (oem && !text) set_data(CF_TEXT, PENDING);
    open_task = 0;
    local_change = 0;
}

static void import_linux_text(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_HasClipboardText()) return;
    char *u = SDL_GetClipboardText();
    if (!u) return;
    if (last_linux_text && !strcmp(u, last_linux_text)) { SDL_free(u); return; }
    free(last_linux_text);
    last_linux_text = strdup(u);
    char *a = w16_utf8_to_ansi(u);
    SDL_free(u);
    size_t n = 0;
    for (char *p = a; *p; p++) n += (*p == '\n' && (p == a || p[-1] != '\r')) ? 2 : 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n + 1);
    char *d = g ? GlobalLock(g) : NULL;
    if (d) {
        for (char *p = a; *p; p++) {
            if (*p == '\n' && (p == a || p[-1] != '\r')) *d++ = '\r';
            *d++ = *p;
        }
        *d = 0;
        GlobalUnlock(g);
    }
    free(a);
    if (!g) return;
    take_over();
    open_task = 1;
    set_data(CF_TEXT, g);
    open_task = 0;
    synth_text_pair();
    draw_clipboard();
}

static void import_shared(void)
{
    static DWORD last_check;
    if (open_task) return;                          /* never while this program has it open */
    DWORD now = GetTickCount();
    if (last_check && now - last_check < 100) return;
    last_check = now;
    FILE *f = fopen(shared_path(), "rb");
    if (!f) { import_linux_text(); return; }
    long long seq = 0;
    int pid = 0;
    if (fscanf(f, "ARCH311CLIP 1 %lld %d\n", &seq, &pid) != 2 || seq == last_seq || pid == (int)getpid()) {
        if (seq) last_seq = seq;
        fclose(f);
        import_linux_text();
        return;
    }
    last_seq = seq;
    take_over();
    unsigned fmt;
    char kind, name[256];
    size_t n;
    while (fscanf(f, "F %u %c %zu %255s", &fmt, &kind, &n, name) == 4) {
        fgetc(f);
        uint8_t *buf = malloc(n ? n : 1);
        if (!buf || fread(buf, 1, n, f) != n) { free(buf); break; }
        fgetc(f);
        UINT cf = fmt ? fmt : RegisterClipboardFormat(name);
        HANDLE h = load_entry(kind, buf, n);
        free(buf);
        if (!h || !cf) continue;
        open_task = 1;
        set_data(cf, h);
        open_task = 0;
        if (!fmt) w16_user_atom_delete((ATOM)cf);   /* set_data took its own reference */
    }
    fclose(f);
    synth_text_pair();
    /* the Linux clipboard now holds what this content put there */
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_HasClipboardText()) {
        char *u = SDL_GetClipboardText();
        free(last_linux_text);
        last_linux_text = u ? strdup(u) : NULL;
        SDL_free(u);
    }
    draw_clipboard();
}

/* the message loop's look at the shared clipboard */
void w16_clipboard_poll(void) { import_shared(); }
