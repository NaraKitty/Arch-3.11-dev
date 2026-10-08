/* clipboard (bridged to the Linux clipboard), WinHelp, ShellExecute, drag & drop, startup */
#include "w16int.h"
#include <SDL.h>
#include <unistd.h>

/* ------------------------------------------------------------------ clipboard */
static HWND clip_owner, clip_open;
static HGLOBAL clip_text;

BOOL OpenClipboard(HWND h) { if (clip_open) return FALSE; clip_open = h ? h : (HWND)1; return TRUE; }
BOOL CloseClipboard(void) { clip_open = NULL; return TRUE; }
BOOL EmptyClipboard(void)
{
    if (clip_text) GlobalFree(clip_text);
    clip_text = NULL;
    clip_owner = clip_open;
    return TRUE;
}

/* cp1252 <-> UTF-8 for the host clipboard */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

char *w16_ansi_to_utf8(const char *s)
{
    size_t n = strlen(s);
    char *o = malloc(n * 3 + 1), *p = o;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c >= 0x80 && c < 0xA0) c = cp1252_hi[c - 0x80];
        if (c < 0x80) *p++ = c;
        else if (c < 0x800) { *p++ = 0xC0 | (c >> 6); *p++ = 0x80 | (c & 0x3F); }
        else { *p++ = 0xE0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 0x3F); *p++ = 0x80 | (c & 0x3F); }
    }
    *p = 0;
    return o;
}

char *w16_utf8_to_ansi(const char *s)
{
    char *o = malloc(strlen(s) + 1), *p = o;
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        unsigned c;
        if (*u < 0x80) c = *u++;
        else if ((*u & 0xE0) == 0xC0 && u[1]) { c = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); u += 2; }
        else if ((*u & 0xF0) == 0xE0 && u[1] && u[2]) { c = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); u += 3; }
        else if ((*u & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) { c = '?'; u += 4; }
        else { c = '?'; u++; }
        if (c >= 0x100) {
            int m = '?';
            for (int i = 0; i < 32; i++) if (cp1252_hi[i] == c) m = 0x80 + i;
            c = m;
        }
        *p++ = (char)c;
    }
    *p = 0;
    return o;
}

HANDLE SetClipboardData(UINT fmt, HANDLE data)
{
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT) return data; /* other formats: kept by the app only (TODO T-CLIP-02) */
    if (clip_text && clip_text != data) GlobalFree(clip_text);
    clip_text = data;
    if (data && SDL_WasInit(SDL_INIT_VIDEO)) {
        char *s = GlobalLock(data);
        char *u = w16_ansi_to_utf8(s ? s : "");
        SDL_SetClipboardText(u);
        free(u);
        GlobalUnlock(data);
    }
    return data;
}

HANDLE GetClipboardData(UINT fmt)
{
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT) return NULL;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_HasClipboardText()) {
        char *u = SDL_GetClipboardText();
        char *a = w16_utf8_to_ansi(u ? u : "");
        SDL_free(u);
        /* the Linux clipboard uses LF; Windows text uses CRLF */
        size_t n = 0;
        for (char *p = a; *p; p++) n += (*p == '\n') ? 2 : 1;
        HGLOBAL g = GlobalAlloc(GHND, n + 1);
        char *d = GlobalLock(g);
        for (char *p = a; *p; p++) {
            if (*p == '\n' && (p == a || p[-1] != '\r')) *d++ = '\r';
            *d++ = *p;
        }
        *d = 0;
        GlobalUnlock(g);
        free(a);
        if (clip_text) GlobalFree(clip_text);
        clip_text = g;
    }
    return clip_text;
}

BOOL IsClipboardFormatAvailable(UINT fmt)
{
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT) return FALSE;
    if (SDL_WasInit(SDL_INIT_VIDEO)) return SDL_HasClipboardText() || clip_text != NULL;
    return clip_text != NULL;
}
UINT EnumClipboardFormats(UINT fmt) { return (fmt == 0 && IsClipboardFormatAvailable(CF_TEXT)) ? CF_TEXT : 0; }
int CountClipboardFormats(void) { return IsClipboardFormatAvailable(CF_TEXT) ? 1 : 0; }
HWND SetClipboardViewer(HWND h) { (void)h; return NULL; }
BOOL ChangeClipboardChain(HWND h, HWND n) { (void)h; (void)n; return TRUE; }

/* ------------------------------------------------------------------ external programs */
static void spawn(const char *const argv[])
{
    if (fork() == 0) {
        setsid();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
}

BOOL WinHelp(HWND h, LPCSTR file, UINT cmd, DWORD data)
{
    (void)h; (void)data;
    if (cmd == HELP_QUIT || !file) return TRUE;
    /* the native WinHelp port (arch311-winhelp) reads the user's .HLP files */
    char host[1024];
    char base[260];
    const char *b = strrchr(file, '\\');
    snprintf(base, sizeof base, "%s", b ? b + 1 : file);
    snprintf(host, sizeof host, "%s/files/%s", w16_assets_dir(), base);
    for (char *c = host + strlen(w16_assets_dir()) + 7; *c; c++) *c = (char)(uintptr_t)AnsiUpper((LPSTR)(uintptr_t)(unsigned char)*c);
    const char *argv[] = {"arch311-winhelp", host, NULL};
    if (access(host, R_OK) != 0) return FALSE;
    spawn(argv);
    return TRUE;
}

HINSTANCE ShellExecute(HWND h, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    (void)h; (void)op; (void)params; (void)dir; (void)show;
    char host[1024];
    if (w16_dos_to_host(file, host, sizeof host)) snprintf(host, sizeof host, "%s", file);
    const char *argv[] = {"xdg-open", host, NULL};
    spawn(argv);
    return (HINSTANCE)(uintptr_t)33;
}

UINT DragQueryFile(HANDLE drop, UINT i, LPSTR buf, UINT cb)
{
    /* drop = NUL-separated list of DOS paths, double-NUL terminated */
    const char *p = drop;
    UINT n = 0;
    while (p && *p) {
        if (n == i) {
            if (buf) snprintf(buf, cb, "%s", p);
            return strlen(p);
        }
        p += strlen(p) + 1;
        n++;
    }
    return i == 0xFFFF ? n : 0;
}
void DragFinish(HANDLE drop) { free(drop); }

/* ------------------------------------------------------------------ startup */
void w16_video_init(void);
void w16_desktop_create(void);
void w16_script_init(void);

int main(int argc, char **argv)
{
    w16_sys_init();
    w16_require_assets();
    w16_screen_init(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    w16_video_init();
    w16_fonts_init();
    w16_register_system_classes();
    w16_desktop_create();
    w16_script_init();
    /* command line as one string, like Win16 lpCmdLine; Linux paths become DOS paths */
    char cmd[4096] = "";
    for (int i = 1; i < argc; i++) {
        char dos[1024];
        const char *a = argv[i];
        if (a[0] == '/' && w16_host_to_dos(a, dos, sizeof dos) == 0) a = dos;
        if (strlen(cmd) + strlen(a) + 2 < sizeof cmd) {
            if (cmd[0]) strcat(cmd, " ");
            strcat(cmd, a);
        }
    }
    HINSTANCE inst = GetModuleHandle(NULL);
    if (!inst) {
        fprintf(stderr, "arch311: %s was not found among your ripped files\n", w16_app_module);
        return 2;
    }
    return WinMain(inst, NULL, cmd, SW_SHOWNORMAL);
}

/* ------------------------------------------------------------------ SystemParametersInfo */
BOOL SystemParametersInfo(UINT action, UINT param, void *pv, UINT winini)
{
    (void)param; (void)winini;
    switch (action) {
    case SPI_GETBEEP:
        if (pv) { char b[8]; GetProfileString("windows", "Beep", "yes", b, sizeof b); *(BOOL *)pv = !strcasecmp(b, "yes"); }
        return TRUE;
    case SPI_GETBORDER:
        if (pv) *(int *)pv = w16_border_width;
        return TRUE;
    case SPI_ICONHORIZONTALSPACING:
        if (pv) *(int *)pv = GetProfileInt("desktop", "IconSpacing", 75);
        return TRUE;
    case SPI_GETICONTITLELOGFONT: {
        /* WIN.INI [desktop] IconTitleFaceName / IconTitleSize / IconTitleStyle, 3.1 defaults */
        LOGFONT *lf = pv;
        if (!lf) return FALSE;
        memset(lf, 0, sizeof *lf);
        int pt = GetProfileInt("desktop", "IconTitleSize", 8);
        lf->lfHeight = -((pt * 96 + 36) / 72);
        lf->lfWeight = GetProfileInt("desktop", "IconTitleStyle", 0) ? FW_BOLD : FW_NORMAL;
        GetProfileString("desktop", "IconTitleFaceName", "MS Sans Serif", lf->lfFaceName, sizeof lf->lfFaceName);
        return TRUE;
    }
    }
    return FALSE;
}
