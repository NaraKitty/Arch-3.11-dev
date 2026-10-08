/* KERNEL-side services: system colours/metrics, INI files, memory, strings, files */
#include "w16int.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int w16_debug;
int w16_border_width = 3;
int w16_kbd_speed = 31, w16_kbd_delay = 2;
UINT w16_dblclk_time = 500;
int w16_swap_buttons;
int w16_mouse_params[3] = {2, 10, 1};
BOOL w16_beep = TRUE; /* WIN.INI Beep: MessageBeep plays the system sounds */

/* Windows 3.1 "Windows Default" colours (USER defaults; overridable from [colors] in WIN.INI) */
COLORREF w16_syscolor[W16_NUM_SYSCOLORS] = {
    RGB(192, 192, 192), /* scrollbar */
    RGB(192, 192, 192), /* background */
    RGB(0, 0, 128),     /* active caption */
    RGB(255, 255, 255), /* inactive caption */
    RGB(255, 255, 255), /* menu */
    RGB(255, 255, 255), /* window */
    RGB(0, 0, 0),       /* window frame */
    RGB(0, 0, 0),       /* menu text */
    RGB(0, 0, 0),       /* window text */
    RGB(255, 255, 255), /* caption text */
    RGB(192, 192, 192), /* active border */
    RGB(192, 192, 192), /* inactive border */
    RGB(255, 255, 255), /* app workspace */
    RGB(0, 0, 128),     /* highlight */
    RGB(255, 255, 255), /* highlight text */
    RGB(192, 192, 192), /* button face */
    RGB(128, 128, 128), /* button shadow */
    RGB(192, 192, 192), /* gray text (3.1 VGA default, measured) */
    RGB(0, 0, 0),       /* button text */
    RGB(0, 0, 0),       /* inactive caption text */
    RGB(255, 255, 255), /* button highlight */
};

static const char *color_keys[W16_NUM_SYSCOLORS] = {
    "Scrollbar", "Background", "ActiveTitle", "InactiveTitle", "Menu", "Window", "WindowFrame",
    "MenuText", "WindowText", "TitleText", "ActiveBorder", "InactiveBorder", "AppWorkspace",
    "Hilight", "HilightText", "ButtonFace", "ButtonShadow", "GrayText", "ButtonText",
    "InactiveTitleText", "ButtonHilight"};

/* metrics for the VGA display driver at 640x480 (VGA.DRV / USER) */
int w16_metric[SM_CMETRICS];

static void init_metrics(int sw, int sh)
{
    int *m = w16_metric;
    int bw = w16_border_width;
    m[SM_CXSCREEN] = sw;
    m[SM_CYSCREEN] = sh;
    m[SM_CXVSCROLL] = 17;
    m[SM_CYHSCROLL] = 17;
    m[SM_CYCAPTION] = 20;
    m[SM_CXBORDER] = 1;
    m[SM_CYBORDER] = 1;
    m[SM_CXDLGFRAME] = 4;
    m[SM_CYDLGFRAME] = 4;
    m[SM_CYVTHUMB] = 17;
    m[SM_CXHTHUMB] = 17;
    m[SM_CXICON] = 32;
    m[SM_CYICON] = 32;
    m[SM_CXCURSOR] = 32;
    m[SM_CYCURSOR] = 32;
    m[SM_CYMENU] = 18;
    m[SM_CXFULLSCREEN] = sw;
    m[SM_CYFULLSCREEN] = sh - 20;
    m[SM_CYKANJIWINDOW] = 0;
    m[SM_MOUSEPRESENT] = 1;
    m[SM_CYVSCROLL] = 17;
    m[SM_CXHSCROLL] = 17;
    m[SM_CXMIN] = 100;
    m[SM_CYMIN] = 27;
    m[SM_CXSIZE] = 18;
    m[SM_CYSIZE] = 18;
    m[SM_CXFRAME] = bw + 1;
    m[SM_CYFRAME] = bw + 1;
    m[SM_CXMINTRACK] = 100;
    m[SM_CYMINTRACK] = 27;
    m[SM_CXDOUBLECLK] = 4;
    m[SM_CYDOUBLECLK] = 4;
    m[SM_CXICONSPACING] = 77;
    m[SM_CYICONSPACING] = 77;
    m[SM_MENUDROPALIGNMENT] = 0;
}

int GetSystemMetrics(int i) { return (i >= 0 && i < SM_CMETRICS) ? w16_metric[i] : 0; }
COLORREF GetSysColor(int i) { return (i >= 0 && i < W16_NUM_SYSCOLORS) ? w16_syscolor[i] : 0; }

/* USER, both at start-up (seg3:0748, jump table 07D1) and in SetSysColors (seg41:0C06, table 0C50):
 * the text-like colours are made solid with the display's GetNearestColor; a scroll bar colour of
 * exactly E0E0E0 gets the 0x10 flag in its high byte (VGA.DRV then realizes the brush as its 50% gray
 * pattern) */
static COLORREF snap_syscolor(int k, COLORREF c)
{
    switch (k) {
    case COLOR_SCROLLBAR:
        if (c == 0x00E0E0E0) c |= 0x10000000;
        break;
    case COLOR_MENU: case COLOR_WINDOW: case COLOR_WINDOWFRAME: case COLOR_MENUTEXT:
    case COLOR_WINDOWTEXT: case COLOR_CAPTIONTEXT: case COLOR_HIGHLIGHT: case COLOR_HIGHLIGHTTEXT:
    case COLOR_BTNTEXT: case COLOR_INACTIVECAPTIONTEXT:
        c = GetNearestColor(NULL, c);
        break;
    }
    return c;
}

/* the start-up half, once the display (W16_COLORS) is set up: libw16 reads WIN.INI before that */
void w16_syscolors_realize(void)
{
    for (int k = 0; k < W16_NUM_SYSCOLORS; k++) w16_syscolor[k] = snap_syscolor(k, w16_syscolor[k]);
}

/* SetSysColors (seg41:0C06): every top-level window then gets WM_SYSCOLORCHANGE and the whole screen
 * is redrawn, frames and children included (RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN
 * on the desktop) */
void SetSysColors(int n, const int *idx, const COLORREF *v)
{
    for (int i = 0; i < n; i++) {
        int k = idx[i];
        if (k < 0 || k >= W16_NUM_SYSCOLORS) continue;
        w16_syscolor[k] = snap_syscolor(k, v[i]);
    }
    SendMessage(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0);
    w16_invalidate_screen_rect(&(RECT){0, 0, w16_metric[SM_CXSCREEN], w16_metric[SM_CYSCREEN]});
}

/* ------------------------------------------------------------------ config dir / INI files */
const char *w16_config_dir(void)
{
    static char d[1024];
    if (!d[0]) {
        const char *x = getenv("XDG_CONFIG_HOME");
        if (x && *x)
            snprintf(d, sizeof d, "%s/arch311", x);
        else
            snprintf(d, sizeof d, "%s/.config/arch311", getenv("HOME") ? getenv("HOME") : ".");
        /* mkdir -p: $HOME/.config may not exist yet */
        for (char *c = d + 1; *c; c++)
            if (*c == '/') { *c = 0; mkdir(d, 0755); *c = '/'; }
        mkdir(d, 0755);
    }
    return d;
}

static void ini_path(LPCSTR file, char *out, size_t cb)
{
    const char *base = file;
    for (const char *c = file; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':')
            base = c + 1;
    if (strchr(file, '\\') || strchr(file, ':')) {
        /* explicit DOS path: map it, unless it points at the Windows directory */
        char host[1024];
        if (w16_dos_to_host(file, host, sizeof host) == 0 && !strstr(file, "WINDOWS")) {
            snprintf(out, cb, "%s", host);
            return;
        }
    }
    char up[260];
    snprintf(up, sizeof up, "%s", base);
    for (char *c = up; *c; c++)
        *c = toupper((unsigned char)*c);
    snprintf(out, cb, "%s/%s", w16_config_dir(), up);
    /* first use of WIN.INI / SYSTEM.INI: seed from the user's setup templates */
    if (access(out, F_OK) != 0 && (!strcmp(up, "WIN.INI") || !strcmp(up, "SYSTEM.INI") ||
                                   !strcmp(up, "CONTROL.INI"))) {
        char src[1200];
        const char *tmpl = !strcmp(up, "WIN.INI") ? "WIN.SRC" : !strcmp(up, "SYSTEM.INI") ? "SYSTEM.SRC" : "CONTROL.SRC";
        snprintf(src, sizeof src, "%s/files/%s", w16_assets_dir(), tmpl);
        FILE *i = fopen(src, "rb"), *o = i ? fopen(out, "wb") : NULL;
        if (i && o) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, i)) > 0)
                fwrite(buf, 1, n, o);
        }
        if (i) fclose(i);
        if (o) fclose(o);
    }
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    n = fread(d, 1, n, f);
    d[n] = 0;
    fclose(f);
    return d;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t')
        b++;
    memmove(s, b, strlen(b) + 1);
}

/* returns 1 and copies value if found. key==NULL: list keys (NUL separated) */
static int ini_get(const char *path, LPCSTR app, LPCSTR key, LPSTR out, int cb, int *len)
{
    char *d = read_all(path);
    if (!d)
        return 0;
    int in = 0, found = 0, pos = 0;
    char *save, *line = strtok_r(d, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char l[1024];
        snprintf(l, sizeof l, "%s", line);
        trim(l);
        if (l[0] == '[') {
            char *e = strchr(l, ']');
            if (e)
                *e = 0;
            in = !strcasecmp(l + 1, app);
            continue;
        }
        if (!in || l[0] == ';')
            continue;
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = 0;
        char k[256];
        snprintf(k, sizeof k, "%s", l);
        trim(k);
        char *v = eq + 1;
        trim(v);
        if (!key) {
            int n = strlen(k);
            if (pos + n + 2 <= cb) {
                memcpy(out + pos, k, n + 1);
                pos += n + 1;
            }
            found = 1;
        } else if (!strcasecmp(k, key)) {
            /* strip surrounding quotes like KERNEL does */
            int n = strlen(v);
            if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                v[n - 1] = 0;
                v++;
            }
            snprintf(out, cb, "%s", v);
            *len = strlen(out);
            found = 1;
            break;
        }
    }
    if (!key && found) {
        if (pos < cb)
            out[pos] = 0;
        *len = pos;
    }
    free(d);
    return found;
}

static int ini_set(const char *path, LPCSTR app, LPCSTR key, LPCSTR val)
{
    char *d = read_all(path);
    if (!d)
        d = strdup("");
    size_t cap = strlen(d) + strlen(app) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 64;
    char *out = malloc(cap);
    out[0] = 0;
    int in = 0, done = 0, sawsec = 0;
    char *p = d;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p + 1) : strlen(p);
        char l[1024];
        snprintf(l, sizeof l, "%.*s", (int)n, p);
        char t[1024];
        snprintf(t, sizeof t, "%s", l);
        trim(t);
        if (t[0] == '[') {
            if (in && !done && key && val) {
                strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n");
                done = 1;
            }
            char *e = strchr(t, ']');
            if (e) *e = 0;
            in = !strcasecmp(t + 1, app);
            if (in) sawsec = 1;
            if (in && !key) { p += n; continue; } /* delete whole section */
        } else if (in) {
            if (!key) { p += n; continue; }
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = 0;
                trim(t);
                if (!strcasecmp(t, key)) {
                    if (val && !done) {
                        strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n");
                    }
                    done = 1;
                    p += n;
                    continue;
                }
            }
        }
        strncat(out, p, n);
        p += n;
    }
    if (!done && key && val) {
        if (!sawsec) {
            size_t L = strlen(out);
            if (L && out[L - 1] != '\n') strcat(out, "\r\n");
            strcat(out, "\r\n["); strcat(out, app); strcat(out, "]\r\n");
        } else if (in) {
            size_t L = strlen(out);
            if (L && out[L - 1] != '\n') strcat(out, "\r\n");
        }
        if (sawsec && !in) {
            /* section exists earlier: rebuild by inserting after its header */
            char hdr[300];
            snprintf(hdr, sizeof hdr, "[%s]", app);
            char *h = strcasestr(out, hdr);
            if (h) {
                char *eol = strchr(h, '\n');
                size_t at = eol ? (size_t)(eol - out + 1) : strlen(out);
                char line[2048];
                snprintf(line, sizeof line, "%s=%s\r\n", key, val);
                char *o2 = malloc(strlen(out) + strlen(line) + 1);
                memcpy(o2, out, at);
                strcpy(o2 + at, line);
                strcat(o2, out + at);
                free(out);
                out = o2;
                done = 1;
            }
        }
        if (!done) {
            strcat(out, key); strcat(out, "="); strcat(out, val); strcat(out, "\r\n");
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    }
    free(out);
    free(d);
    return f != NULL;
}

int GetPrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, int cb, LPCSTR file)
{
    char path[1200];
    int len = 0;
    ini_path(file, path, sizeof path);
    if (cb <= 0)
        return 0;
    if (app && ini_get(path, app, key, out, cb, &len))
        return len;
    snprintf(out, cb, "%s", def ? def : "");
    return strlen(out);
}
int GetPrivateProfileInt(LPCSTR app, LPCSTR key, int def, LPCSTR file)
{
    char b[64];
    if (!GetPrivateProfileString(app, key, "", b, sizeof b, file))
        return def;
    if (!isdigit((unsigned char)b[0]) && b[0] != '-')
        return 0;
    return atoi(b);
}
BOOL WritePrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file)
{
    char path[1200];
    ini_path(file, path, sizeof path);
    return ini_set(path, app, key, val);
}
int GetProfileString(LPCSTR a, LPCSTR k, LPCSTR d, LPSTR o, int cb) { return GetPrivateProfileString(a, k, d, o, cb, "WIN.INI"); }
int GetProfileInt(LPCSTR a, LPCSTR k, int d) { return GetPrivateProfileInt(a, k, d, "WIN.INI"); }
BOOL WriteProfileString(LPCSTR a, LPCSTR k, LPCSTR v) { return WritePrivateProfileString(a, k, v, "WIN.INI"); }

UINT GetWindowsDirectory(LPSTR buf, UINT cb) { snprintf(buf, cb, "C:\\WINDOWS"); return strlen(buf); }
UINT GetSystemDirectory(LPSTR buf, UINT cb) { snprintf(buf, cb, "C:\\WINDOWS\\SYSTEM"); return strlen(buf); }

void w16_sys_init(void)
{
    char b[64];
    const char *dbg = getenv("W16_DEBUG");
    w16_debug = dbg && *dbg && *dbg != '0';
    w16_border_width = GetProfileInt("windows", "BorderWidth", 3);
    if (w16_border_width < 1) w16_border_width = 1;
    if (w16_border_width > 49) w16_border_width = 49;
    w16_kbd_speed = GetProfileInt("windows", "KeyboardSpeed", 31);
    w16_kbd_delay = GetProfileInt("windows", "KeyboardDelay", 2);
    if (w16_kbd_speed < 0 || w16_kbd_speed > 31) w16_kbd_speed = 31;
    if (w16_kbd_delay < 0 || w16_kbd_delay > 3) w16_kbd_delay = 2;
    /* mouse, as USER's init reads it: DoubleClickSpeed 0 = 500 ms; MouseThreshold1 defaults to the
     * driver's X threshold (MOUSE.DRV Inquire: 2), MouseThreshold2 (read when MouseSpeed is 2) to 10 */
    SetDoubleClickTime(GetProfileInt("windows", "DoubleClickSpeed", 0));
    GetProfileString("windows", "SwapMouseButtons", "no", b, sizeof b);
    w16_swap_buttons = !strcasecmp(b, "yes") || !strcasecmp(b, "true") || !strcasecmp(b, "on") || atoi(b) != 0;
    GetProfileString("windows", "Beep", "yes", b, sizeof b);
    w16_beep = !strcasecmp(b, "yes") || !strcasecmp(b, "true") || !strcasecmp(b, "on") || atoi(b) != 0;
    w16_mouse_params[0] = GetProfileInt("windows", "MouseThreshold1", 2);
    w16_mouse_params[2] = GetProfileInt("windows", "MouseSpeed", 1);
    if (w16_mouse_params[2] == 2) w16_mouse_params[1] = GetProfileInt("windows", "MouseThreshold2", 10);
    w16_trails_init();
    /* USER's start-up (seg3:0748 with its reader seg3:06D7): three numbers, each the next run of
     * digits (anything else between them is skipped), kept as a byte. 3.1 reads on past the end of a
     * value with fewer than three numbers; libw16 stops there and takes 0 for the rest. */
    for (int i = 0; i < W16_NUM_SYSCOLORS; i++) {
        if (GetProfileString("colors", color_keys[i], "", b, sizeof b) && b[0]) {
            BYTE v[3] = {0, 0, 0};
            const char *p = b;
            for (int k = 0; k < 3; k++) {
                while (*p && (*p < '0' || *p > '9')) p++;
                while (*p >= '0' && *p <= '9') v[k] = (BYTE)(v[k] * 10 + (*p++ - '0'));
            }
            w16_syscolor[i] = RGB(v[0], v[1], v[2]);
        }
    }
    int sw = 640, sh = 480;
    const char *s = getenv("W16_SCREEN");
    if (s)
        sscanf(s, "%dx%d", &sw, &sh);
    init_metrics(sw, sh);
    /* like a program started from a DOS prompt: the current directory is where it was launched,
     * if that folder is on a mapped drive */
    char host[1024], dos[300];
    if (getcwd(host, sizeof host) && w16_host_to_dos(host, dos, sizeof dos) == 0) w16_chdir(dos);
}

/* ------------------------------------------------------------------ time */
DWORD GetTickCount(void)
{
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec && !t0.tv_nsec)
        t0 = t;
    return (DWORD)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000) + 60000;
}
DWORD GetCurrentTime(void) { return GetTickCount(); }

/* ------------------------------------------------------------------ memory */
#define MEM_MAGIC 0x4D454D31u
typedef struct { uint32_t magic; uint32_t locks; size_t size; void *p; } MemH;

static void *mem_alloc(size_t n, int zero)
{
    MemH *h = calloc(1, sizeof *h);
    h->magic = MEM_MAGIC;
    h->size = n;
    h->p = zero ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    if (!h->p) { free(h); return NULL; }
    return h;
}
static MemH *mh(void *h) { MemH *m = h; return (m && m->magic == MEM_MAGIC) ? m : NULL; }
static void *mem_realloc(void *h, size_t n, int zero)
{
    MemH *m = mh(h);
    if (!m) return NULL;
    void *p = realloc(m->p, n ? n : 1);
    if (!p) return NULL;
    if (zero && n > m->size) memset((char *)p + m->size, 0, n - m->size);
    m->p = p;
    m->size = n;
    return h;
}
HGLOBAL GlobalAlloc(UINT f, DWORD n) { return mem_alloc(n, f & GMEM_ZEROINIT); }
HGLOBAL GlobalReAlloc(HGLOBAL h, DWORD n, UINT f) { return mem_realloc(h, n, f & GMEM_ZEROINIT); }
void *GlobalLock(HGLOBAL h) { MemH *m = mh(h); if (!m) return NULL; m->locks++; return m->p; }
BOOL GlobalUnlock(HGLOBAL h) { MemH *m = mh(h); if (m && m->locks) m->locks--; return m && m->locks; }
HGLOBAL GlobalFree(HGLOBAL h) { MemH *m = mh(h); if (!m) return h; free(m->p); m->magic = 0; free(m); return NULL; }
DWORD GlobalSize(HGLOBAL h) { MemH *m = mh(h); return m ? m->size : 0; }
HLOCAL LocalAlloc(UINT f, UINT n) { return mem_alloc(n, f & LMEM_ZEROINIT); }
HLOCAL LocalReAlloc(HLOCAL h, UINT n, UINT f) { return mem_realloc(h, n, f & LMEM_ZEROINIT); }
void *LocalLock(HLOCAL h) { return GlobalLock(h); }
BOOL LocalUnlock(HLOCAL h) { return GlobalUnlock(h); }
HLOCAL LocalFree(HLOCAL h) { return GlobalFree(h); }
UINT LocalSize(HLOCAL h) { return GlobalSize(h); }

/* ------------------------------------------------------------------ strings (ANSI = cp1252) */
static unsigned char up1252(unsigned char c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0x9A || c == 0x9C || c == 0x9E) return c - 16;
    if (c == 0xFF) return 0x9F;
    return c;
}
static unsigned char lo1252(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x8A || c == 0x8C || c == 0x8E) return c + 16;
    if (c == 0x9F) return 0xFF;
    return c;
}
int lstrlen(LPCSTR s) { return s ? (int)strlen(s) : 0; }
LPSTR lstrcpy(LPSTR d, LPCSTR s) { return strcpy(d, s ? s : ""); }
LPSTR lstrcat(LPSTR d, LPCSTR s) { return strcat(d, s ? s : ""); }
int lstrcmp(LPCSTR a, LPCSTR b) { return strcmp(a, b); }
int lstrcmpi(LPCSTR a, LPCSTR b)
{
    for (;; a++, b++) {
        int x = lo1252(*(unsigned char *)a), y = lo1252(*(unsigned char *)b);
        if (x != y || !x) return x - y;
    }
}
LPSTR AnsiUpper(LPSTR s)
{
    if (IS_INTRESOURCE(s)) return (LPSTR)(uintptr_t)up1252((unsigned char)(uintptr_t)s);
    for (char *c = s; *c; c++) *c = up1252(*c);
    return s;
}
LPSTR AnsiLower(LPSTR s)
{
    if (IS_INTRESOURCE(s)) return (LPSTR)(uintptr_t)lo1252((unsigned char)(uintptr_t)s);
    for (char *c = s; *c; c++) *c = lo1252(*c);
    return s;
}
LPSTR AnsiNext(LPCSTR s) { return (LPSTR)(*s ? s + 1 : s); }
LPSTR AnsiPrev(LPCSTR start, LPCSTR s) { return (LPSTR)(s > start ? s - 1 : s); }
/* the DOS layer here already speaks ANSI (see w16.h) */
void OemToAnsi(LPCSTR oem, LPSTR ansi) { if (oem != ansi) memmove(ansi, oem, strlen(oem) + 1); }
void AnsiToOem(LPCSTR ansi, LPSTR oem) { if (oem != ansi) memmove(oem, ansi, strlen(ansi) + 1); }
static UINT error_mode;
UINT SetErrorMode(UINT mode) { UINT was = error_mode; error_mode = mode; return was; }
BOOL IsCharAlpha(char c) { unsigned char u = c; return isalpha(u) || (u >= 0xC0 && u != 0xD7 && u != 0xF7); }
BOOL IsCharAlphaNumeric(char c) { return IsCharAlpha(c) || isdigit((unsigned char)c); }
BOOL IsCharUpper(char c) { return IsCharAlpha(c) && up1252(c) == (unsigned char)c; }
BOOL IsCharLower(char c) { return IsCharAlpha(c) && lo1252(c) == (unsigned char)c; }

/* wsprintf: Win16 semantics. %d %i %u %x %X %c %s, flags '-' '0' '#', width, .precision;
 * 'l' prefix accepted. Integer arguments are 32-bit (pass int/LONG, not long). */
int wvsprintf(LPSTR buf, LPCSTR fmt, va_list ap)
{
    char *o = buf;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        if (*f == '%') { *o++ = '%'; continue; }
        int left = 0, zero = 0, alt = 0, width = 0, prec = -1;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; while (isdigit((unsigned char)*f)) prec = prec * 10 + (*f++ - '0'); }
        if (*f == 'l' || *f == 'L') f++;
        char tmp[64];
        const char *s = tmp;
        int len;
        switch (*f) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", va_arg(ap, int)); break;
        case 'u': snprintf(tmp, sizeof tmp, "%u", va_arg(ap, unsigned)); break;
        case 'x': snprintf(tmp, sizeof tmp, alt ? "0x%x" : "%x", va_arg(ap, unsigned)); break;
        case 'X': snprintf(tmp, sizeof tmp, alt ? "0X%X" : "%X", va_arg(ap, unsigned)); break;
        case 'c': tmp[0] = (char)va_arg(ap, int); tmp[1] = 0; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
        default: tmp[0] = 0; f--; break;
        }
        len = strlen(s);
        if (*f == 's' && prec >= 0 && len > prec) len = prec;
        int pad = width > len ? width - len : 0;
        if (!left) while (pad-- > 0) *o++ = (zero && *f != 's') ? '0' : ' ';
        memcpy(o, s, len);
        o += len;
        if (left) while (pad-- > 0) *o++ = ' ';
    }
    *o = 0;
    return o - buf;
}
int wsprintf(LPSTR buf, LPCSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}
void OutputDebugString(LPCSTR s) { if (w16_debug) fputs(s, stderr); }
FARPROC MakeProcInstance(FARPROC p, HINSTANCE h) { (void)h; return p; }
void FreeProcInstance(FARPROC p) { (void)p; }
BOOL Yield(void) { w16_pump(0); return TRUE; }

/* ------------------------------------------------------------------ drives & files */
/* A drive maps a letter onto a Linux folder ("C=/home/user" in the drives file). A mount maps one
 * DOS directory inside a drive onto another folder ("C:\WINDOWS=/path"). Unless the C: folder has
 * a WINDOWS directory of its own, C:\WINDOWS and C:\WINDOWS\SYSTEM are the user's ripped 3.11
 * files, so programs find their sounds, bitmaps and help files where 3.11 kept them. */
typedef struct { char letter; char root[1024]; } Drive;
typedef struct { char letter; char dir[260]; char root[1024]; } Mount; /* dir "\\WINDOWS", upper case */
static Drive drives[26];
static int ndrives;
static Mount mounts[16];
static int nmounts;
static void norm_dos(LPCSTR in, char *out, size_t cb);

static void add_mount(char letter, const char *dir, const char *root)
{
    if (nmounts == 16) return;
    Mount *m = &mounts[nmounts++];
    m->letter = toupper((unsigned char)letter);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    size_t l = strlen(m->dir);
    while (l > 1 && m->dir[l - 1] == '\\') m->dir[--l] = 0;
    AnsiUpper(m->dir);
    snprintf(m->root, sizeof m->root, "%s", root);
    l = strlen(m->root);
    while (l > 1 && m->root[l - 1] == '/') m->root[--l] = 0;
}

static int has_mount(char letter, const char *dir)
{
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].letter == letter && !strcasecmp(mounts[i].dir, dir)) return 1;
    return 0;
}

/* `name` exists in Linux folder `dir`, in any letter case */
static int ci_exists(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    int found = 0;
    for (struct dirent *e; d && !found && (e = readdir(d));) found = !strcasecmp(e->d_name, name);
    if (d) closedir(d);
    return found;
}

static void load_drives(void)
{
    if (ndrives)
        return;
    char path[1200];
    snprintf(path, sizeof path, "%s/drives", w16_config_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char l[1100];
        while (fgets(l, sizeof l, f) && ndrives < 26) {
            trim(l);
            char *eq = strchr(l, '=');
            if (strlen(l) > 2 && isalpha((unsigned char)l[0]) && l[1] == '=') {
                drives[ndrives].letter = toupper((unsigned char)l[0]);
                snprintf(drives[ndrives].root, sizeof drives[0].root, "%s", l + 2);
                ndrives++;
            } else if (eq && isalpha((unsigned char)l[0]) && l[1] == ':' && l[2] == '\\') {
                *eq = 0;
                add_mount(l[0], l + 2, eq + 1);
            }
        }
        fclose(f);
    }
    if (!ndrives) {
        drives[0].letter = 'C';
        snprintf(drives[0].root, sizeof drives[0].root, "%s", getenv("HOME") ? getenv("HOME") : "/");
        drives[1].letter = 'Z';
        strcpy(drives[1].root, "/");
        ndrives = 2;
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# DOS drive letters seen by 3.11 apps -> Linux folders\nC=%s\nZ=/\n"
                       "# a DOS directory can be placed on another folder, e.g. C:\\WINDOWS=/path\n", drives[0].root);
            fclose(f);
        }
    }
    char croot[1024];
    int c = -1;
    for (int i = 0; i < ndrives; i++)
        if (drives[i].letter == 'C') c = i;
    if (c >= 0 && !has_mount('C', "\\WINDOWS") && !ci_exists(drives[c].root, "WINDOWS")) {
        snprintf(croot, sizeof croot, "%s/files", w16_assets_dir());
        add_mount('C', "\\WINDOWS", croot);
        if (!has_mount('C', "\\WINDOWS\\SYSTEM")) add_mount('C', "\\WINDOWS\\SYSTEM", croot);
    }
}

/* the mount points directly inside DOS directory `dosdir` ("C:\" -> "WINDOWS"), for directory listings */
int w16_mount_children(LPCSTR dosdir, char names[][64], int max)
{
    load_drives();
    char n[300];
    int k = 0;
    norm_dos(dosdir, n, sizeof n);
    const char *in = n[3] ? n + 2 : ""; /* "\\WINDOWS", or "" for the root */
    for (int i = 0; i < nmounts && k < max; i++) {
        const char *bs = strrchr(mounts[i].dir, '\\');
        if (mounts[i].letter != n[0] || !bs || (size_t)(bs - mounts[i].dir) != strlen(in) ||
            strncasecmp(mounts[i].dir, in, bs - mounts[i].dir))
            continue;
        snprintf(names[k++], 64, "%s", bs + 1);
    }
    return k;
}

static char cur_drive = 'C';
static char cur_dir[260] = "\\";

/* resolve each path component case-insensitively */
static void ci_resolve(char *host)
{
    char out[2048] = "";
    char *save, *tmp = strdup(host);
    int abs = host[0] == '/';
    if (abs) strcpy(out, "");
    for (char *c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", out[0] ? out : (abs ? "/" : "."));
        char cand[2100];
        snprintf(cand, sizeof cand, "%s/%s", out, c);
        if (access(abs ? cand : cand + 1, F_OK) != 0) {
            int found = 0;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)))
                    if (!strcasecmp(e->d_name, c)) {
                        snprintf(cand, sizeof cand, "%s/%s", out, e->d_name);
                        found = 1;
                        break;
                    }
                closedir(d);
            }
            /* a name that does not exist yet (a file being created): DOS apps pass upper
             * case, Linux convention is lower case */
            if (!found) {
                AnsiLower(c);
                snprintf(cand, sizeof cand, "%s/%s", out, c);
            }
        }
        snprintf(out, sizeof out, "%s", cand);
    }
    free(tmp);
    if (!abs && out[0] == '/') memmove(out, out + 1, strlen(out));
    if (!out[0]) strcpy(out, "/");
    strcpy(host, out);
}

/* DOS keeps one current directory per drive */
static char drive_dir[26][260];

/* "x:\a\..\b\." -> "X:\a\b" with the letter case kept: the full DOS path, "." and ".." resolved,
 * trailing dots dropped ("NAME." is NAME), no trailing backslash except at the root */
static void dos_resolve(LPCSTR in, char *out, size_t cb)
{
    char full[520], *parts[64];
    int n = 0;
    char drv = cur_drive;
    const char *p = in;
    if (p[0] && p[1] == ':') { drv = toupper((unsigned char)p[0]); p += 2; }
    const char *base = drv == cur_drive ? cur_dir : drv >= 'A' && drv <= 'Z' && drive_dir[drv - 'A'][0] ? drive_dir[drv - 'A'] : "\\";
    if (*p == '\\' || *p == '/')
        snprintf(full, sizeof full, "%s", p);
    else
        snprintf(full, sizeof full, "%s\\%s", base, p);
    char *save;
    for (char *c = strtok_r(full, "\\/", &save); c && n < 64; c = strtok_r(NULL, "\\/", &save)) {
        size_t l = strlen(c);
        if (!strcmp(c, "..")) { if (n) n--; continue; }
        while (l && c[l - 1] == '.') c[--l] = 0;
        if (!l) continue; /* ".", "..." */
        parts[n++] = c;
    }
    size_t o = snprintf(out, cb, "%c:", drv);
    if (!n) snprintf(out + o, cb - o, "\\");
    for (int i = 0; i < n && o < cb; i++) o += snprintf(out + o, cb - o, "\\%s", parts[i]);
}

int w16_dos_to_host(LPCSTR dos, char *host, size_t cb)
{
    load_drives();
    char full[520];
    dos_resolve(dos, full, sizeof full);
    char drv = full[0];
    const char *root = NULL, *rest = full + 2;
    for (int i = 0; i < ndrives; i++)
        if (drives[i].letter == drv) root = drives[i].root;
    if (!root)
        return -1;
    /* a mounted directory takes over its part of the drive */
    size_t best = 0;
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].dir);
        if (mounts[i].letter == drv && l > best && !strncasecmp(full + 2, mounts[i].dir, l) &&
            (full[2 + l] == '\\' || !full[2 + l])) {
            root = mounts[i].root;
            rest = full + 2 + l;
            best = l;
        }
    }
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s/%s", root, rest);
    for (char *c = tmp; *c; c++) if (*c == '\\') *c = '/';
    /* collapse // */
    char *w = tmp;
    for (char *r = tmp; *r; r++) if (!(r[0] == '/' && r[1] == '/')) *w++ = *r;
    *w = 0;
    ci_resolve(tmp);
    snprintf(host, cb, "%s", tmp);
    return 0;
}

int w16_host_to_dos(const char *host, LPSTR dos, size_t cb)
{
    load_drives();
    int best = -1, mount = 0;
    size_t bl = 0;
    for (int i = 0; i < ndrives; i++) {
        size_t l = strlen(drives[i].root);
        if (!strncmp(host, drives[i].root, l) && (host[l] == '/' || !host[l] || (l == 1)) && l > bl) {
            best = i;
            bl = l;
        }
    }
    /* the deepest folder wins: a mount inside the C: folder maps back to its DOS directory */
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].root);
        if (l > 1 && !strncmp(host, mounts[i].root, l) && (host[l] == '/' || !host[l]) && l > bl) {
            best = i;
            bl = l;
            mount = 1;
        }
    }
    if (best < 0) return -1;
    const char *rest = host + (bl == 1 ? 0 : bl);
    if (mount)
        snprintf(dos, cb, "%c:%s%s", mounts[best].letter, mounts[best].dir, rest);
    else
        snprintf(dos, cb, "%c:%s", drives[best].letter, *rest ? rest : "\\");
    for (char *c = dos; *c; c++) if (*c == '/') *c = '\\';
    return 0;
}

HFILE _lopen(LPCSTR name, int mode)
{
    char h[2048];
    if (w16_dos_to_host(name, h, sizeof h)) return HFILE_ERROR;
    int fl = (mode & 3) == OF_WRITE ? O_WRONLY : (mode & 3) == OF_READWRITE ? O_RDWR : O_RDONLY;
    int fd = open(h, fl);
    return fd < 0 ? HFILE_ERROR : fd;
}
HFILE _lcreat(LPCSTR name, int attr)
{
    (void)attr;
    char h[2048];
    if (w16_dos_to_host(name, h, sizeof h)) return HFILE_ERROR;
    int fd = open(h, O_RDWR | O_CREAT | O_TRUNC, 0644);
    return fd < 0 ? HFILE_ERROR : fd;
}
UINT _lread(HFILE f, void *b, UINT n) { ssize_t r = read(f, b, n); return r < 0 ? (UINT)-1 : (UINT)r; }
UINT _lwrite(HFILE f, const void *b, UINT n)
{
    if (n == 0) { off_t p = lseek(f, 0, SEEK_CUR); return ftruncate(f, p) ? (UINT)-1 : 0; }
    ssize_t r = write(f, b, n);
    return r < 0 ? (UINT)-1 : (UINT)r;
}
LONG _llseek(HFILE f, LONG off, int o) { return (LONG)lseek(f, off, o == 0 ? SEEK_SET : o == 1 ? SEEK_CUR : SEEK_END); }
HFILE _lclose(HFILE f) { return close(f) ? HFILE_ERROR : 0; }

/* OpenFile's search for a file named without a directory: the current directory, the Windows
 * directory, then the system directory (3.1 goes on to the program's directory and PATH; the
 * programs live in the Windows directory here and PATH holds Linux folders) */
static int search_file(LPCSTR name, char *dos, size_t cb)
{
    char dirs[3][260] = {""};
    GetWindowsDirectory(dirs[1], sizeof dirs[1]);
    GetSystemDirectory(dirs[2], sizeof dirs[2]);
    for (int i = 0; i < 3; i++) {
        char t[600], h[2048];
        struct stat st;
        snprintf(t, sizeof t, "%s%s%s", dirs[i], i ? "\\" : "", name);
        if (w16_dos_to_host(t, h, sizeof h) == 0 && stat(h, &st) == 0 && !S_ISDIR(st.st_mode)) {
            norm_dos(t, dos, cb);
            return 0;
        }
    }
    return -1;
}

HFILE OpenFile(LPCSTR name, OFSTRUCT *of, UINT style)
{
    char h[2048], given[300], path[300];
    snprintf(given, sizeof given, "%s", (style & OF_REOPEN) && of ? of->szPathName : name);
    const char *base = given;
    for (const char *c = given; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':') base = c + 1;
    /* szPathName gets the full upper-case path where the file was found */
    if ((style & (OF_CREATE | OF_PARSE)) || !(base == given || (style & (OF_SEARCH | OF_REOPEN)) == OF_SEARCH) ||
        search_file(base, path, sizeof path))
        norm_dos(given, path, sizeof path);
    if (of) {
        memset(of, 0, sizeof *of);
        of->cBytes = sizeof *of > 255 ? 255 : sizeof *of; /* BYTE field; our szPathName is 260 */
        of->fFixedDisk = 1;
        snprintf(of->szPathName, sizeof of->szPathName, "%s", path);
    }
    if (style & OF_PARSE)
        return 0;
    if (w16_dos_to_host(path, h, sizeof h)) {
        if (of) of->nErrCode = 3;
        return HFILE_ERROR;
    }
    if (style & OF_DELETE) {
        int r = unlink(h);
        if (r && of) of->nErrCode = 2;
        return r ? HFILE_ERROR : 1;
    }
    int fd;
    if (style & OF_CREATE)
        fd = open(h, O_RDWR | O_CREAT | O_TRUNC, 0644);
    else {
        int m = style & 3;
        fd = open(h, m == OF_WRITE ? O_WRONLY : m == OF_READWRITE ? O_RDWR : O_RDONLY);
    }
    if (fd < 0) {
        if (of) of->nErrCode = errno == ENOENT ? 2 : errno == EACCES ? 5 : 3;
        return HFILE_ERROR;
    }
    if (style & OF_EXIST) {
        close(fd);
        return 1;
    }
    return fd;
}

/* ------------------------------------------------------------------ current drive / directory (INT 21h 0Eh/19h/3Bh/47h) */
/* "X:\A\..\B\." -> "X:\B" (upper case, no trailing backslash except the root) */
static void norm_dos(LPCSTR in, char *out, size_t cb)
{
    dos_resolve(in, out, cb);
    AnsiUpper(out);
}
void w16_dos_fullpath(LPCSTR dos, LPSTR out, size_t cb) { norm_dos(dos, out, cb); }

int w16_drive_root(char letter, char *root, size_t cb)
{
    load_drives();
    letter = toupper((unsigned char)letter);
    for (int i = 0; i < ndrives; i++)
        if (drives[i].letter == letter) {
            if (root) snprintf(root, cb, "%s", drives[i].root);
            return 0;
        }
    return -1;
}

void w16_getcwd(LPSTR dos, size_t cb) { snprintf(dos, cb, "%c:%s", cur_drive, cur_dir); }

int w16_chdir(LPCSTR dos)
{
    char n[300], host[2048];
    struct stat st;
    if (dos[0] && dos[1] == ':' && !dos[2]) { /* "X:" selects the drive, keeping its directory */
        char d = toupper((unsigned char)dos[0]);
        if (d < 'A' || d > 'Z' || w16_drive_root(d, NULL, 0)) return -2;
        const char *keep = drive_dir[d - 'A'][0] ? drive_dir[d - 'A'] : "\\";
        snprintf(n, sizeof n, "%c:%s", d, keep);
    } else
        norm_dos(dos, n, sizeof n);
    if (w16_drive_root(n[0], NULL, 0)) return -2;
    if (w16_dos_to_host(n, host, sizeof host) || stat(host, &st) || !S_ISDIR(st.st_mode)) return -1;
    cur_drive = n[0];
    snprintf(cur_dir, sizeof cur_dir, "%s", n + 2);
    snprintf(drive_dir[cur_drive - 'A'], sizeof drive_dir[0], "%s", cur_dir);
    return 0;
}
