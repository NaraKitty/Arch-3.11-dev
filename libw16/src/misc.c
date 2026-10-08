/* clipboard (bridged to the Linux clipboard), WinHelp, ShellExecute, drag & drop, startup */
#include "w16int.h"
#include <SDL.h>
#include <unistd.h>

/* ------------------------------------------------------------------ clipboard */
/* the clipboard itself is USER's, in clipbrd.c */
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
void w16_script_finish(void);

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
    int r = WinMain(inst, NULL, cmd, SW_SHOWNORMAL);
    w16_script_finish(); /* test scripts: shots of the screen the program left */
    return r;
}

/* ------------------------------------------------------------------ SystemParametersInfo */
/* 0 selects the default, 500 ms */
BOOL SetDoubleClickTime(UINT ms) { w16_dblclk_time = ms ? ms : 500; return TRUE; }
UINT GetDoubleClickTime(void) { return w16_dblclk_time; }
/* returns the previous setting; the buttons are swapped as SDL reports them (msg.c) */
BOOL SwapMouseButton(BOOL swap) { BOOL was = w16_swap_buttons; w16_swap_buttons = swap != 0; return was; }

/* USER seg41:0E91: a setting in WIN.INI as USER writes it ("%d" of a 16-bit int) */
static BOOL spi_write(LPCSTR section, LPCSTR key, int v)
{
    char t[8];
    wsprintf(t, "%d", (SHORT)v);
    return WriteProfileString(section, key, t);
}

/* the settings of the Desktop applet, as USER seg41:0ED6 keeps and writes them: SPIF_UPDATEINIFILE
 * writes the key to "Windows" or "Desktop" (USER's spelling), SPIF_SENDWININICHANGE then tells every
 * window with that section name, only when something was written */
static BOOL spi_desktop(UINT action, UINT param, void *pv, UINT winini)
{
    static const char szWindows[] = "Windows", szDesktop[] = "Desktop";
    const char *section = szWindows;
    BOOL wrote = FALSE, update = (winini & SPIF_UPDATEINIFILE) != 0;
    int v = (SHORT)param;
    switch (action) {
    case SPI_SETBORDER: {
        /* 1..50; nothing changes or is written when the width stays */
        int old = w16_border_width;
        w16_border_width = v < 1 ? 1 : v > 50 ? 50 : v;
        if (old == w16_border_width) return TRUE;
        w16_border_changed(old);
        if (update) wrote = spi_write(szWindows, "BorderWidth", v);   /* the value asked for */
        break;
    }
    case SPI_ICONHORIZONTALSPACING:
        if (pv) {
            *(int *)pv = w16_icon_spacing;
            return TRUE;
        }
        if (!param) return TRUE;
        w16_icon_spacing = (UINT)GetSystemMetrics(SM_CXICON) < (WORD)param ? (WORD)param : GetSystemMetrics(SM_CXICON);
        section = szDesktop;
        if (update) wrote = spi_write(szDesktop, "IconSpacing", w16_icon_spacing);
        break;
    case SPI_GETSCREENSAVETIMEOUT:
        if (pv) *(int *)pv = w16_screen_save < 0 ? -w16_screen_save : w16_screen_save;
        return TRUE;
    case SPI_SETSCREENSAVETIMEOUT:
        /* (USER restarts its idle count; arch311 has no screen saver to start yet) */
        w16_screen_save = w16_screen_save < 0 ? -v : v;
        if (update) wrote = spi_write(szWindows, "ScreenSaveTimeOut", v);
        break;
    case SPI_GETSCREENSAVEACTIVE:
        if (pv) *(BOOL *)pv = w16_screen_save > 0;
        return TRUE;
    case SPI_SETSCREENSAVEACTIVE:
        if ((param && w16_screen_save < 0) || (!param && w16_screen_save > 0)) w16_screen_save = -w16_screen_save;
        if (update) wrote = spi_write(szWindows, "ScreenSaveActive", param != 0);
        break;
    case SPI_GETGRIDGRANULARITY:
        if (pv) *(int *)pv = w16_grid / 8;
        return TRUE;
    case SPI_SETGRIDGRANULARITY:
        w16_grid = (SHORT)(param << 3);
        if (w16_grid < 1) w16_grid = 1;
        section = szDesktop;
        if (update) wrote = spi_write(szDesktop, "GridGranularity", v);
        break;
    case SPI_SETDESKWALLPAPER:
        if (!w16_set_desk_wallpaper(pv)) return FALSE;
        section = szDesktop;
        if (update && pv != (void *)-1) wrote = WriteProfileString(szDesktop, "Wallpaper", pv ? pv : "(None)");
        w16_desktop_redraw();
        break;
    case SPI_SETDESKPATTERN:
        if (v == -1 && pv) return FALSE;
        if (!w16_set_desk_pattern(v == -1 ? (LPCSTR)-1 : pv)) return FALSE;
        section = szDesktop;
        if (update) wrote = WriteProfileString(szDesktop, "Pattern", pv);   /* (NULL removes it, as in USER) */
        break;
    case SPI_GETICONTITLEWRAP:
        if (pv) *(int *)pv = w16_icon_title_wrap;
        return TRUE;
    case SPI_SETICONTITLEWRAP:
        /* turning it on when it is on writes nothing */
        if (w16_icon_title_wrap && param) return TRUE;
        w16_icon_title_wrap = param != 0;
        /* (USER lays the minimised windows' titles out again; libw16 icon titles do not wrap yet) */
        section = szDesktop;
        if (update) wrote = spi_write(szDesktop, "IconTitleWrap", w16_icon_title_wrap);
        break;
    case SPI_GETFASTTASKSWITCH:
        if (pv) *(int *)pv = w16_fast_switch;
        return TRUE;
    case SPI_SETFASTTASKSWITCH:
        w16_fast_switch = param == 1;
        if (update) wrote = spi_write(szWindows, "CoolSwitch", w16_fast_switch);
        break;
    default:
        return FALSE;
    }
    if (wrote && (winini & SPIF_SENDWININICHANGE)) SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM)section);
    return TRUE;
}

BOOL SystemParametersInfo(UINT action, UINT param, void *pv, UINT winini)
{
    switch (action) {
    case SPI_SETBORDER:
    case SPI_ICONHORIZONTALSPACING:
    case SPI_GETSCREENSAVETIMEOUT:
    case SPI_SETSCREENSAVETIMEOUT:
    case SPI_GETSCREENSAVEACTIVE:
    case SPI_SETSCREENSAVEACTIVE:
    case SPI_GETGRIDGRANULARITY:
    case SPI_SETGRIDGRANULARITY:
    case SPI_SETDESKWALLPAPER:
    case SPI_SETDESKPATTERN:
    case SPI_GETICONTITLEWRAP:
    case SPI_SETICONTITLEWRAP:
    case SPI_GETFASTTASKSWITCH:
    case SPI_SETFASTTASKSWITCH:
        return spi_desktop(action, param, pv, winini);
    case SPI_GETBEEP:
        if (pv) *(BOOL *)pv = w16_beep;
        return TRUE;
    case SPI_SETBEEP:
        w16_beep = param != 0;
        if (winini & SPIF_UPDATEINIFILE) WriteProfileString("windows", "Beep", w16_beep ? "yes" : "no");
        if (winini & SPIF_SENDWININICHANGE) SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM) "windows");
        return TRUE;
    case SPI_GETBORDER:
        if (pv) *(int *)pv = w16_border_width;
        return TRUE;
    case SPI_GETKEYBOARDSPEED:
        if (pv) *(int *)pv = w16_kbd_speed;
        return TRUE;
    case SPI_GETKEYBOARDDELAY:
        if (pv) *(int *)pv = w16_kbd_delay;
        return TRUE;
    case SPI_SETKEYBOARDSPEED:
    case SPI_SETKEYBOARDDELAY: {
        int v = (int)param, spd = action == SPI_SETKEYBOARDSPEED;
        if (spd) w16_kbd_speed = v < 0 ? 0 : v > 31 ? 31 : v;
        else w16_kbd_delay = v < 0 ? 0 : v > 3 ? 3 : v;
        if (winini & SPIF_UPDATEINIFILE) {
            char t[8];
            wsprintf(t, "%d", spd ? w16_kbd_speed : w16_kbd_delay);
            WriteProfileString("windows", spd ? "KeyboardSpeed" : "KeyboardDelay", t);
        }
        if (winini & SPIF_SENDWININICHANGE) SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM) "windows");
        return TRUE;
    }
    case SPI_GETMOUSE:
        if (pv) memcpy(pv, w16_mouse_params, sizeof w16_mouse_params);
        return TRUE;
    case SPI_SETMOUSE:
        /* stored and written to WIN.INI; the pointer itself moves under the host's acceleration
         * (TODO: apply 3.1 thresholds when arch311 owns the pointer) */
        if (!pv) return FALSE;
        memcpy(w16_mouse_params, pv, sizeof w16_mouse_params);
        if (winini & SPIF_UPDATEINIFILE) {
            static const char *const key[3] = {"MouseThreshold1", "MouseThreshold2", "MouseSpeed"};
            for (int i = 0; i < 3; i++) {
                char t[8];
                wsprintf(t, "%d", w16_mouse_params[i]);
                WriteProfileString("windows", key[i], t);
            }
        }
        if (winini & SPIF_SENDWININICHANGE) SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM) "windows");
        return TRUE;
    case SPI_SETDOUBLECLICKTIME:
        SetDoubleClickTime(param);
        if (winini & SPIF_UPDATEINIFILE) { char t[8]; wsprintf(t, "%u", w16_dblclk_time); WriteProfileString("windows", "DoubleClickSpeed", t); }
        return TRUE;
    case SPI_SETMOUSEBUTTONSWAP:
        SwapMouseButton(param != 0);
        if (winini & SPIF_UPDATEINIFILE) WriteProfileString("windows", "SwapMouseButtons", param ? "yes" : "no");
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
    case SPI_SETLANGDRIVER:
        /* USER loads the language driver (a 16-bit DLL: not on arch311) and with SPIF_UPDATEINIFILE
         * records it in SYSTEM.INI [boot] LANGUAGE.DLL (measured: International's German gave
         * "language.dll=langger.dll"). UNTESTED: what 3.1 returns when the DLL cannot be loaded */
        if (winini & SPIF_UPDATEINIFILE) WritePrivateProfileString("boot", "LANGUAGE.DLL", pv ? (LPCSTR)pv : "", "SYSTEM.INI");
        return TRUE;
    }
    return FALSE;
}
