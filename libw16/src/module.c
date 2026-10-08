/* NE modules and resources, read from the user's ripped 3.11 files at run time */
#include "w16int.h"
#include <ctype.h>
#include <sys/stat.h>

static HINSTANCE modules;
static char assets[1024];

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }

const char *w16_assets_dir(void)
{
    if (!assets[0]) {
        const char *e = getenv("ARCH311_ASSETS");
        if (e && *e)
            snprintf(assets, sizeof assets, "%s", e);
        else {
            const char *x = getenv("XDG_DATA_HOME");
            if (x && *x)
                snprintf(assets, sizeof assets, "%s/arch311", x);
            else
                snprintf(assets, sizeof assets, "%s/.local/share/arch311", getenv("HOME") ? getenv("HOME") : ".");
        }
    }
    return assets;
}

static uint8_t *slurp(const char *path, size_t *sz)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? n : 1);
    if (fread(d, 1, n, f) != (size_t)n) {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *sz = n;
    return d;
}

static void pstr(const uint8_t *p, char *out, int cb)
{
    int n = p[0];
    if (n >= cb)
        n = cb - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

static int parse_ne(HINSTANCE m)
{
    const uint8_t *d = m->data;
    if (m->size < 0x40 || d[0] != 'M' || d[1] != 'Z')
        return 0;
    uint32_t ne = u32(d + 0x3C);
    if (ne + 0x40 > m->size || d[ne] != 'N' || d[ne + 1] != 'E')
        return 0;
    uint16_t res_off = u16(d + ne + 0x24), resname_off = u16(d + ne + 0x26);
    pstr(d + ne + resname_off, m->name, sizeof m->name);
    if (res_off == resname_off)
        return 1;
    const uint8_t *base = d + ne + res_off;
    int shift = u16(base);
    const uint8_t *p = base + 2;
    while (u16(p)) {
        uint16_t tid = u16(p), cnt = u16(p + 2);
        p += 8;
        for (int i = 0; i < cnt; i++, p += 12) {
            m->res = realloc(m->res, sizeof(W16Res) * (m->nres + 1));
            W16Res *r = &m->res[m->nres++];
            memset(r, 0, sizeof *r);
            if (tid & 0x8000)
                r->type_id = tid & 0x7FFF;
            else
                pstr(base + tid, r->type_name, sizeof r->type_name);
            uint16_t rid = u16(p + 6);
            if (rid & 0x8000)
                r->name_id = rid & 0x7FFF;
            else
                pstr(base + rid, r->name, sizeof r->name);
            r->off = (uint32_t)u16(p) << shift;
            r->len = (uint32_t)u16(p + 2) << shift;
            if (r->off + r->len > m->size)
                r->len = r->off < m->size ? m->size - r->off : 0;
        }
    }
    return 1;
}

HINSTANCE w16_module_open(const char *file)
{
    char up[32];
    snprintf(up, sizeof up, "%s", file);
    for (char *c = up; *c; c++)
        *c = toupper((unsigned char)*c);
    for (HINSTANCE m = modules; m; m = m->next)
        if (!strcmp(m->file, up) || !strcmp(m->name, up))
            return m;
    char path[1200];
    snprintf(path, sizeof path, "%s/files/%s", w16_assets_dir(), up);
    size_t sz;
    uint8_t *d = slurp(path, &sz);
    if (!d)
        return NULL;
    HINSTANCE m = calloc(1, sizeof *m);
    snprintf(m->file, sizeof m->file, "%s", up);
    m->data = d;
    m->size = sz;
    if (!parse_ne(m)) {
        free(d);
        free(m);
        return NULL;
    }
    m->next = modules;
    modules = m;
    return m;
}

HINSTANCE w16_system_module(const char *file) { return w16_module_open(file); }
HINSTANCE w16_load_module(LPCSTR f) { return w16_module_open(f); }

HINSTANCE GetModuleHandle(LPCSTR name)
{
    if (!name)
        return w16_module_open(w16_app_module);
    HINSTANCE m = w16_module_open(name);
    /* "DISPLAY" is the display driver's module name; libw16 shows what VGA.DRV would (its OEM
     * bitmaps, colours and OEMBIN resources come from the user's ripped VGA.DRV) */
    if (!m && !strcasecmp(name, "DISPLAY"))
        m = w16_module_open("VGA.DRV");
    return m;
}

static int match(int id, const char *nm, LPCSTR want)
{
    if (IS_INTRESOURCE(want))
        return id && id == (int)(uintptr_t)want;
    if (want[0] == '#')
        return id && id == atoi(want + 1);
    return !id && !strcasecmp(nm, want);
}

const W16Res *w16_find_res(HINSTANCE m, LPCSTR name, LPCSTR type)
{
    if (!m)
        return NULL;
    for (int i = 0; i < m->nres; i++) {
        W16Res *r = &m->res[i];
        if (match(r->type_id, r->type_name, type) && match(r->name_id, r->name, name))
            return r;
    }
    return NULL;
}

const uint8_t *w16_res_data(HINSTANCE m, const W16Res *r) { return m->data + r->off; }

HANDLE FindResource(HINSTANCE h, LPCSTR name, LPCSTR type) { return (HANDLE)w16_find_res(h, name, type); }
HGLOBAL LoadResource(HINSTANCE h, HANDLE res) { return res ? (HGLOBAL)w16_res_data(h, res) : NULL; }
void *LockResource(HGLOBAL h) { return h; }
BOOL FreeResource(HGLOBAL h) { (void)h; return FALSE; }
DWORD SizeofResource(HINSTANCE h, HANDLE res) { (void)h; return res ? ((W16Res *)res)->len : 0; }

int LoadString(HINSTANCE h, UINT id, LPSTR buf, int cb)
{
    const W16Res *r = w16_find_res(h, MAKEINTRESOURCE((id >> 4) + 1), RT_STRING);
    if (!r || cb <= 0) {
        if (cb > 0)
            buf[0] = 0;
        return 0;
    }
    const uint8_t *p = w16_res_data(h, r), *end = p + r->len;
    for (UINT i = 0; i < (id & 15) && p < end; i++)
        p += 1 + *p;
    if (p >= end) {
        buf[0] = 0;
        return 0;
    }
    int n = *p;
    if (n > cb - 1)
        n = cb - 1;
    memcpy(buf, p + 1, n);
    buf[n] = 0;
    return n;
}

void w16_require_assets(void)
{
    char path[1200];
    struct stat st;
    snprintf(path, sizeof path, "%s/files/USER.EXE", w16_assets_dir());
    if (stat(path, &st) == 0)
        return;
    fprintf(stderr,
            "arch311: the Windows 3.11 files have not been ripped yet.\n"
            "Run:  arch311-rip DISK1.IMG DISK2.IMG ... DISK6.IMG\n"
            "with images of your own Windows 3.11 floppies (expected in %s).\n",
            w16_assets_dir());
    exit(2);
}
