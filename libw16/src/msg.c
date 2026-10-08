/* USER: message queue, input (SDL2), timers, caret, cursor, DefWindowProc, presentation.
 * Also: W16_SCRIPT test driver for automated comparisons with real 3.11. */
#include "w16int.h"
#include <SDL.h>
#include <ctype.h>
#include <unistd.h>

POINT w16_mouse;
DWORD w16_msg_time;
int w16_quit_posted, w16_quit_code;
int w16_keystate[256];
static int headless;

/* ------------------------------------------------------------------ queue */
typedef struct QMsg { MSG m; struct QMsg *next; } QMsg;
static QMsg *qhead, *qtail;

static void enqueue(HWND h, UINT m, WPARAM wp, LPARAM lp, int front)
{
    QMsg *q = calloc(1, sizeof *q);
    q->m.hwnd = h; q->m.message = m; q->m.wParam = wp; q->m.lParam = lp;
    q->m.time = GetTickCount(); q->m.pt = w16_mouse;
    if (front) { q->next = qhead; qhead = q; if (!qtail) qtail = q; return; }
    if (qtail) qtail->next = q; else qhead = q;
    qtail = q;
}
void w16_post(HWND h, UINT m, WPARAM wp, LPARAM lp) { enqueue(h, m, wp, lp, 0); }
BOOL PostMessage(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (h && h != (HWND)0xFFFF && !w16_valid(h)) return FALSE;
    enqueue(h, m, wp, lp, 0);
    return TRUE;
}
void PostQuitMessage(int code) { w16_quit_posted = 1; w16_quit_code = code; }

static int in_send;
LRESULT SendMessage(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (h == (HWND)0xFFFF) { /* HWND_BROADCAST */
        for (HWND c = w16_desktop->child; c;) { HWND n = c->next; SendMessage(c, m, wp, lp); c = n; }
        return 0;
    }
    if (!w16_valid(h) && !(h == w16_desktop && h)) return 0;
    in_send++;
    LRESULT r = h->proc(h, m, wp, lp);
    in_send--;
    return r;
}
BOOL InSendMessage(void) { return FALSE; }
LRESULT CallWindowProc(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p ? p(h, m, wp, lp) : 0; }

/* USER's atom table, shared with RegisterClipboardFormat (atom.c) */
ATOM w16_user_atom_add(LPCSTR name);
UINT RegisterWindowMessage(LPCSTR name) { return w16_user_atom_add(name); }

static HWND cmd_hwnd_table[256];
HWND W16_CMD_HWND(LPARAM lp)
{
    /* LOWORD(lParam) of WM_COMMAND from a control carries a small slot index */
    int i = LOWORD(lp) & 0xFF;
    return w16_valid(cmd_hwnd_table[i]) ? cmd_hwnd_table[i] : NULL;
}
WORD w16_cmd_slot(HWND h)
{
    static int next = 1;
    for (int i = 1; i < 256; i++) if (cmd_hwnd_table[i] == h) return i;
    for (int k = 0; k < 255; k++) {
        int i = next;
        next = next % 255 + 1;
        if (!w16_valid(cmd_hwnd_table[i])) { cmd_hwnd_table[i] = h; return i; }
    }
    return 0;
}
LPARAM W16_CMD_LPARAM(HWND ctl, int code) { return MAKELPARAM(w16_cmd_slot(ctl), code); }
void w16_notify_parent(HWND h, int code)
{
    if (!w16_valid(h) || !w16_valid(h->parent)) return;
    SendMessage(h->parent, WM_COMMAND, h->id, MAKELPARAM(w16_cmd_slot(h), code));
}

/* ------------------------------------------------------------------ timers */
typedef struct Timer { HWND h; UINT id; UINT ms; DWORD due; TIMERPROC fn; int sys; struct Timer *next; } Timer;
static Timer *timers;
UINT SetTimer(HWND h, UINT id, UINT ms, TIMERPROC fn)
{
    if (ms < 55) ms = 55; /* the 3.x timer resolution is one 18.2 Hz tick */
    for (Timer *t = timers; t; t = t->next)
        if (t->h == h && t->id == id && h) { t->ms = ms; t->due = GetTickCount() + ms; t->fn = fn; return id; }
    Timer *t = calloc(1, sizeof *t);
    static UINT autoid = 0x7F00;
    t->h = h; t->id = h ? id : ++autoid; t->ms = ms; t->due = GetTickCount() + ms; t->fn = fn;
    t->next = timers;
    timers = t;
    return t->id;
}
BOOL KillTimer(HWND h, UINT id)
{
    for (Timer **p = &timers; *p; p = &(*p)->next)
        if ((*p)->h == h && (*p)->id == id) { Timer *t = *p; *p = t->next; free(t); return TRUE; }
    return FALSE;
}
void w16_timers_on_destroy(HWND h)
{
    for (Timer **p = &timers; *p;)
        if ((*p)->h == h) { Timer *t = *p; *p = t->next; free(t); }
        else p = &(*p)->next;
}
static Timer *due_timer(DWORD now, int *wait)
{
    Timer *best = NULL;
    for (Timer *t = timers; t; t = t->next) {
        int d = (int)(t->due - now);
        if (d <= 0 && (!best || (int)(t->due - best->due) < 0)) best = t;
        else if (d > 0 && d < *wait) *wait = d;
    }
    return best;
}

/* ------------------------------------------------------------------ caret */
static struct { HWND h; int x, y, w, ht, hide, on, created, gray; DWORD next; UINT blink; } caret = {.blink = 530};

static void caret_xor(void)
{
    if (!w16_valid(caret.h)) return;
    HDC dc = GetDC(caret.h);
    RECT r = {caret.x, caret.y, caret.x + caret.w, caret.y + caret.ht};
    int a = r.left, b = r.top, c = r.right, d = r.bottom;
    w16_lp_to_dp(dc, &a, &b);
    w16_lp_to_dp(dc, &c, &d);
    if (caret.gray) w16_invert_dev_gray(dc, &(RECT){a, b, c, d});
    else w16_invert_dev(dc, &(RECT){a, b, c, d});
    ReleaseDC(caret.h, dc);
    caret.on = !caret.on;
}
static void caret_off(void) { if (caret.on) caret_xor(); }
BOOL CreateCaret(HWND h, HBITMAP bm, int w, int ht)
{
    DestroyCaret();
    caret.gray = bm == (HBITMAP)1;
    caret.h = h; caret.w = w ? w : GetSystemMetrics(SM_CXBORDER); caret.ht = ht ? ht : GetSystemMetrics(SM_CYBORDER);
    caret.hide = 1; caret.on = 0; caret.created = 1;
    /* (the blink time is USER's, from WIN.INI at the start and SetCaretBlinkTime since) */
    return TRUE;
}
void DestroyCaret(void) { caret_off(); caret.h = NULL; caret.created = 0; }
void ShowCaret(HWND h)
{
    if (!caret.created || (h && h != caret.h)) return;
    if (caret.hide > 0 && --caret.hide == 0) { caret_xor(); caret.next = GetTickCount() + caret.blink; }
}
void HideCaret(HWND h)
{
    if (!caret.created || (h && h != caret.h)) return;
    if (caret.hide++ == 0) caret_off();
}
void SetCaretPos(int x, int y)
{
    if (!caret.created) return;
    int was = caret.on;
    caret_off();
    caret.x = x; caret.y = y;
    if (was || caret.hide == 0) { caret_xor(); caret.next = GetTickCount() + caret.blink; }
}
void GetCaretPos(LPPOINT p) { p->x = caret.x; p->y = caret.y; }
void SetCaretBlinkTime(UINT ms) { caret.blink = ms; }
UINT GetCaretBlinkTime(void) { return caret.blink; }
void w16_caret_hide_for_paint(HWND h) { if (caret.created && (caret.h == h || IsChild(h, caret.h))) HideCaret(caret.h); }
void w16_caret_restore_after_paint(HWND h) { if (caret.created && (caret.h == h || IsChild(h, caret.h))) ShowCaret(caret.h); }
void w16_caret_on_destroy(HWND h) { if (caret.h == h) DestroyCaret(); }
static void caret_tick(DWORD now, int *wait)
{
    if (!caret.created || caret.hide > 0 || !w16_valid(caret.h)) return;
    int d = (int)(caret.next - now);
    if (d <= 0) { caret_xor(); caret.next = now + caret.blink; d = caret.blink; }
    if (d < *wait) *wait = d;
}

/* ------------------------------------------------------------------ presentation */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int scale = 1;
static HCURSOR cur_cursor;
void w16_icon_info(HICON ic, int *w, int *h, int *hx, int *hy, const uint32_t **xorpx, const uint8_t **andm);
void **w16_icon_native(HICON ic);

void w16_video_init(void)
{
    const char *s = getenv("W16_SCALE");
    scale = s ? atoi(s) : 0;
    if (getenv("W16_HEADLESS")) { headless = 1; return; }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "libw16: SDL_Init failed: %s (set W16_HEADLESS=1 for tests)\n", SDL_GetError());
        exit(1);
    }
    if (scale <= 0) {
        SDL_DisplayMode dm;
        scale = 1;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0)
            while ((scale + 1) * w16_screen.w <= dm.w && (scale + 1) * w16_screen.h <= dm.h - 64) scale++;
    }
    win = SDL_CreateWindow(w16_app_module ? w16_app_module : "arch311", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           w16_screen.w * scale, w16_screen.h * scale, SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, w16_screen.w, w16_screen.h);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w16_screen.w, w16_screen.h);
    SDL_StartTextInput();
}

/* ------------------------------------------------------------------ mouse trails
 * VGA.DRV's MOUSETRAILS escape (seg5:00C7): up to 7 pointer images in all; the positions the pointer
 * passed are drawn into the presented frame (the pointer itself is the host cursor). */
static int trails = 1;            /* pointer images, 1 = off ([0x1e7]) */
static int trails_saved;          /* WIN.INI MouseTrails: < 0 = off with the count remembered ([0x12]) */
static POINT trail[8];
static DWORD trail_tick;
static int cursor_count;          /* ShowCursor counter */

static void trails_set(int n) { trails = n; for (int i = 0; i < 8; i++) trail[i] = w16_mouse; w16_screen_dirty = 1; }

void w16_trails_init(void)
{
    int n = GetProfileInt("windows", "MouseTrails", 0); /* UNTESTED against the driver's own startup */
    if (n > 7) n = 7;
    if (n < -7) n = -7;
    trails_saved = n;
    trails_set(n > 0 ? n : 1);
}

int w16_trails_query(void) { return trails_saved < 0 ? trails_saved : trails; }

int w16_trails_escape(int n)
{
    int was = trails_saved;
    if (n > 0) trails_saved = n > 7 ? 7 : n;
    else if (n < 0) {
        if (was > 0) return was;                 /* already on */
        trails_saved = was ? -was : 7;           /* the remembered count, or 7 */
    } else {
        if (was <= 0) return was;
        trails_saved = -was;
    }
    trails_set(trails_saved > 0 ? trails_saved : 1);
    if (n >= -1) {
        /* written as the driver does: sign or space, then the digit */
        char v[3] = {trails_saved > 0 ? ' ' : '-', (char)('0' + abs(trails_saved)), 0};
        WriteProfileString("windows", "MouseTrails", v);
    }
    return trails_saved;
}

/* the trail follows the pointer and gathers on it when the pointer rests (one step per 55 ms tick) */
static void trails_step(void)
{
    if (trails <= 1) return;
    DWORD now = GetTickCount();
    if (now - trail_tick < 55 && trail[0].x == w16_mouse.x && trail[0].y == w16_mouse.y) return;
    trail_tick = now;
    int moved = 0;
    for (int i = trails - 1; i > 0; i--) {
        if (trail[i].x != trail[i - 1].x || trail[i].y != trail[i - 1].y) moved = 1;
        trail[i] = trail[i - 1];
    }
    trail[0] = w16_mouse;
    if (moved) w16_screen_dirty = 1;
}

/* pointer images at the trail positions, drawn into a copy of the frame (AND mask, then XOR) */
static const uint32_t *trails_overlay(const uint32_t *frame)
{
    static uint32_t *buf;
    static size_t cap;
    if (trails <= 1 || !cur_cursor || cursor_count < 0) return frame;
    size_t n = (size_t)w16_screen.w * w16_screen.h;
    if (n > cap) { free(buf); buf = malloc(n * 4); cap = n; }
    memcpy(buf, frame, n * 4);
    int w, h, hx, hy;
    const uint32_t *xp;
    const uint8_t *am;
    w16_icon_info(cur_cursor, &w, &h, &hx, &hy, &xp, &am);
    for (int t = 1; t < trails; t++) {
        int ox = trail[t].x - hx, oy = trail[t].y - hy;
        if (trail[t].x == w16_mouse.x && trail[t].y == w16_mouse.y) continue;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int X = ox + x, Y = oy + y;
                if (X < 0 || Y < 0 || X >= w16_screen.w || Y >= w16_screen.h) continue;
                int i = y * w + x;
                uint32_t *d = &buf[(size_t)Y * w16_screen.w + X];
                if (am[i]) { if (xp[i]) *d = w16_invert_display_px(*d); }
                else *d = (*d & 0xFF000000) | w16_display_px(xp[i]);
            }
    }
    return buf;
}

void w16_present(void)
{
    if (headless) return;
    trails_step();
    if (!w16_screen_dirty) return;
    w16_screen_dirty = 0;
    SDL_UpdateTexture(tex, NULL, trails_overlay(w16_display_frame()), w16_screen.w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

HCURSOR SetCursor(HCURSOR c)
{
    HCURSOR o = cur_cursor;
    cur_cursor = c;
    if (headless || !c || c == o) return o;
    void **nat = w16_icon_native(c);
    if (!*nat) {
        int w, h, hx, hy;
        const uint32_t *xp;
        const uint8_t *am;
        w16_icon_info(c, &w, &h, &hx, &hy, &xp, &am);
        int s = scale > 0 ? scale : 1;
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w * s, h * s, 32, SDL_PIXELFORMAT_ARGB8888);
        uint32_t *px = sf->pixels;
        for (int y = 0; y < h * s; y++)
            for (int x = 0; x < w * s; x++) {
                int i = (y / s) * w + x / s;
                uint32_t v;
                if (am[i]) v = xp[i] ? 0xFF000000 : 0; /* inverted pixels approximated as black */
                else v = 0xFF000000 | w16_display_px(xp[i]);
                px[y * (sf->pitch / 4) + x] = v;
            }
        *nat = SDL_CreateColorCursor(sf, hx * s, hy * s);
        SDL_FreeSurface(sf);
    }
    if (*nat) SDL_SetCursor(*nat);
    return o;
}
int ShowCursor(BOOL show) { cursor_count += show ? 1 : -1; if (!headless) SDL_ShowCursor(cursor_count >= 0); w16_screen_dirty = 1; return cursor_count; }
void GetCursorPos(LPPOINT p) { *p = w16_mouse; }

/* USER ClipCursor: the pointer stays inside the rectangle (right and bottom exclusive) until
 * ClipCursor(NULL); positions from the host are clamped to it */
static RECT cursor_clip;
static int cursor_clipped;
static void clamp_mouse(void)
{
    if (!cursor_clipped) return;
    if (w16_mouse.x < cursor_clip.left) w16_mouse.x = cursor_clip.left;
    if (w16_mouse.x >= cursor_clip.right) w16_mouse.x = cursor_clip.right - 1;
    if (w16_mouse.y < cursor_clip.top) w16_mouse.y = cursor_clip.top;
    if (w16_mouse.y >= cursor_clip.bottom) w16_mouse.y = cursor_clip.bottom - 1;
}
void ClipCursor(LPCRECT r)
{
    RECT s = {0, 0, w16_screen.w, w16_screen.h};
    cursor_clipped = r && IntersectRect(&cursor_clip, r, &s);
    clamp_mouse();
}
void GetClipCursor(LPRECT r)
{
    if (cursor_clipped) *r = cursor_clip;
    else SetRect(r, 0, 0, w16_screen.w, w16_screen.h);
}

void SetCursorPos(int x, int y)
{
    w16_mouse.x = x; w16_mouse.y = y;
    clamp_mouse();
    if (win) SDL_WarpMouseInWindow(win, w16_mouse.x * scale, w16_mouse.y * scale);
}

/* ------------------------------------------------------------------ keyboard */
static int sdl_to_vk(SDL_Keycode k, SDL_Scancode sc)
{
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= '0' && k <= '9') return k;
    switch (k) {
    case SDLK_BACKSPACE: return VK_BACK; case SDLK_TAB: return VK_TAB; case SDLK_RETURN: return VK_RETURN;
    case SDLK_KP_ENTER: return VK_RETURN; case SDLK_ESCAPE: return VK_ESCAPE; case SDLK_SPACE: return VK_SPACE;
    case SDLK_PAGEUP: return VK_PRIOR; case SDLK_PAGEDOWN: return VK_NEXT; case SDLK_END: return VK_END;
    case SDLK_HOME: return VK_HOME; case SDLK_LEFT: return VK_LEFT; case SDLK_UP: return VK_UP;
    case SDLK_RIGHT: return VK_RIGHT; case SDLK_DOWN: return VK_DOWN; case SDLK_INSERT: return VK_INSERT;
    case SDLK_DELETE: return VK_DELETE; case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL; case SDLK_LALT: case SDLK_RALT: return VK_MENU;
    case SDLK_CAPSLOCK: return VK_CAPITAL; case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK; case SDLK_SCROLLLOCK: return VK_SCROLL;
    case SDLK_PAUSE: return VK_PAUSE; case SDLK_PRINTSCREEN: return VK_SNAPSHOT;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY; case SDLK_KP_PLUS: return VK_ADD; case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_DIVIDE: return VK_DIVIDE; case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_SEMICOLON: return 0xBA; case SDLK_EQUALS: return 0xBB; case SDLK_COMMA: return 0xBC;
    case SDLK_MINUS: return 0xBD; case SDLK_PERIOD: return 0xBE; case SDLK_SLASH: return 0xBF;
    case SDLK_BACKQUOTE: return 0xC0; case SDLK_LEFTBRACKET: return 0xDB; case SDLK_BACKSLASH: return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD; case SDLK_QUOTE: return 0xDE;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + (k - SDLK_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
    if (sc == SDL_SCANCODE_KP_0) return VK_NUMPAD0;
    return 0;
}

/* US layout (KBDUS) VK -> character */
static int vk_to_char(int vk, int shift, int ctrl, int caps)
{
    if (ctrl) {
        if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 1;
        if (vk == 0xDB) return 27;
        if (vk == 0xDC) return 28;
        if (vk == 0xDD) return 29;
        if (vk == VK_BACK) return 127;
        if (vk == VK_RETURN) return 10;
        return 0;
    }
    if (vk >= 'A' && vk <= 'Z') return (shift ^ caps) ? vk : vk + 32;
    static const char num[] = ")!@#$%^&*(";
    if (vk >= '0' && vk <= '9') return shift ? num[vk - '0'] : vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return '0' + vk - VK_NUMPAD0;
    switch (vk) {
    case VK_SPACE: return ' '; case VK_RETURN: return '\r'; case VK_BACK: return 8; case VK_TAB: return 9;
    case VK_ESCAPE: return 27; case VK_MULTIPLY: return '*'; case VK_ADD: return '+'; case VK_SUBTRACT: return '-';
    case VK_DIVIDE: return '/'; case VK_DECIMAL: return '.';
    case 0xBA: return shift ? ':' : ';'; case 0xBB: return shift ? '+' : '='; case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-'; case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`'; case 0xDB: return shift ? '{' : '['; case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']'; case 0xDE: return shift ? '"' : '\'';
    }
    return 0;
}

int GetKeyState(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    int v = w16_keystate[vk];
    /* Win16's int is 16 bits: a key that is down reads negative (GetKeyState(VK_SHIFT) < 0) */
    return (v & 0x80 ? (int)(SHORT)0xFF80 : 0) | (v & 1);
}
int GetAsyncKeyState(int vk) { return GetKeyState(vk); }

BOOL TranslateMessage(const MSG *m)
{
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return FALSE;
    int shift = w16_keystate[VK_SHIFT] & 0x80, ctrl = (w16_keystate[VK_CONTROL] & 0x80) && !(w16_keystate[VK_MENU] & 0x80);
    int c = vk_to_char((int)m->wParam, shift != 0, ctrl, w16_keystate[VK_CAPITAL] & 1);
    if (!c) return FALSE;
    enqueue(m->hwnd, m->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, c, m->lParam, 1);
    return TRUE;
}

/* ------------------------------------------------------------------ mouse routing */
static DWORD last_click_time;
static POINT last_click_pt;
static HWND last_click_hwnd;
static UINT last_click_msg;
/* the next click starts afresh, not as the second of a double click (USER clears its double-click
 * time, e.g. for a list box's WM_LBTRACKPOINT answered 2: seg35:13CD) */
void w16_cancel_dblclk(void) { last_click_hwnd = NULL; }

static int mk_flags(void)
{
    return ((w16_keystate[VK_LBUTTON] & 0x80) ? MK_LBUTTON : 0) | ((w16_keystate[VK_RBUTTON] & 0x80) ? MK_RBUTTON : 0) |
           ((w16_keystate[VK_MBUTTON] & 0x80) ? MK_MBUTTON : 0) | ((w16_keystate[VK_SHIFT] & 0x80) ? MK_SHIFT : 0) |
           ((w16_keystate[VK_CONTROL] & 0x80) ? MK_CONTROL : 0);
}

static HWND modal_block(HWND h); /* returns NULL if input to h is blocked */

/* base = WM_MOUSEMOVE / WM_LBUTTONDOWN ...; route to the right window as client or NC message */
static void mouse_event(UINT base)
{
    POINT p = w16_mouse;
    HWND h = w16_capture;
    int hit = HTCLIENT;
    if (!h) {
        h = WindowFromPoint(p);
        if (!h || h == w16_desktop) {
            if (base != WM_MOUSEMOVE) { /* desktop click */ }
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            if (h == w16_desktop && base == WM_LBUTTONDBLCLK) { /* Task List */ }
            return;
        }
        hit = (int)SendMessage(h, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
        while (hit == HTTRANSPARENT && h->parent && h->parent != w16_desktop) {
            h = w16_window_under(h, p);
            hit = (int)SendMessage(h, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
        }
    }
    /* disabled / modal-blocked windows */
    HWND top = w16_top_level(h);
    int blocked = !w16_capture && (!modal_block(h) || (top->style & WS_DISABLED) || ((h->style & WS_DISABLED)));
    if (!w16_capture && blocked) {
        if (base == WM_LBUTTONDOWN || base == WM_RBUTTONDOWN) MessageBeep(0);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return;
    }
    if (!w16_capture) SendMessage(h, WM_SETCURSOR, (WPARAM)h, MAKELPARAM(hit, base));
    if (!w16_capture && (base == WM_LBUTTONDOWN || base == WM_RBUTTONDOWN || base == WM_MBUTTONDOWN)) {
        /* USER seg1:2933: a button pressed over a child window - its client area or not - is reported
         * to each of its parents in turn, with the position in that parent's client area (an MDI
         * client activates the child under it). UNTESTED against 3.11 (the rig has no mouse input) */
        for (HWND c = h; w16_valid(c) && (c->style & WS_CHILD) && w16_valid(c->parent) && c->parent != w16_desktop;) {
            HWND par = c->parent;
            SendMessage(par, WM_PARENTNOTIFY, base, MAKELPARAM(p.x - par->rc.left, p.y - par->rc.top));
            c = par;
        }
        if (!w16_valid(h)) return;
    }
    if (base == WM_LBUTTONDOWN || base == WM_RBUTTONDOWN || base == WM_MBUTTONDOWN) {
        /* activation */
        if (!w16_capture && top != w16_active && top->parent == w16_desktop) {
            int r = (int)SendMessage(h, WM_MOUSEACTIVATE, (WPARAM)top, MAKELPARAM(hit, base));
            if (r != MA_NOACTIVATE) w16_activate(top, WA_CLICKACTIVE);
            if (r == MA_ACTIVATEANDEAT) return;
        }
        /* double clicks */
        DWORD now = GetTickCount();
        UINT dbl = w16_dblclk_time;
        if (h == last_click_hwnd && base == last_click_msg && now - last_click_time <= dbl &&
            abs(p.x - last_click_pt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
            abs(p.y - last_click_pt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 &&
            (hit != HTCLIENT || (h->cls->wc.style & CS_DBLCLKS))) {
            base += 2; /* xBUTTONDBLCLK */
            last_click_time = 0;
        } else {
            last_click_time = now; last_click_pt = p; last_click_hwnd = h; last_click_msg = base;
        }
    }
    if (hit == HTCLIENT || w16_capture) {
        POINT c = p;
        ScreenToClient(h, &c);
        enqueue(h, base, mk_flags(), MAKELPARAM(c.x, c.y), 0);
    } else if (hit != HTNOWHERE && hit != HTERROR) {
        UINT nc = base - WM_MOUSEMOVE + WM_NCMOUSEMOVE;
        enqueue(h, nc, hit, MAKELPARAM(p.x, p.y), 0);
    }
}

/* modal dialogs disable their owners; popups of the active app are fine */
static HWND modal_block(HWND h)
{
    for (HWND p = h; p && p != w16_desktop; p = p->parent)
        if (p->style & WS_DISABLED) return NULL;
    return h;
}

static void key_event(UINT msg, int vk, int scancode, int repeat)
{
    HWND h = w16_focus;
    int alt = w16_keystate[VK_MENU] & 0x80;
    LPARAM lp = 1 | ((scancode & 0xFF) << 16) | (alt ? (1 << 29) : 0) | (repeat ? (1 << 30) : 0) | (msg == WM_KEYUP ? 0xC0000000 : 0);
    if (!h) {
        h = w16_active;
        if (!h) return;
        /* no focus: keystrokes go to the active window as WM_SYSKEY* (3.1 behaviour) */
        msg = msg == WM_KEYDOWN ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    } else if (alt || vk == VK_F10 || vk == VK_MENU) {
        msg = msg == WM_KEYDOWN ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    }
    enqueue(h, msg, vk, lp, 0);
}

/* ------------------------------------------------------------------ test script driver */
static FILE *script;
static DWORD script_wait_until;
void w16_script_init(void)
{
    const char *s = getenv("W16_SCRIPT");
    if (s && *s) script = fopen(s, "r");
}

static void synth_key(const char *spec)
{
    /* e.g. alt+h, ctrl+shift+f5, a, enter */
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    int mods[3] = {0}, nm = 0, vk = 0;
    char *save, *tok = strtok_r(buf, "+", &save);
    while (tok) {
        char *next = strtok_r(NULL, "+", &save);
        int v = 0;
        if (!strcasecmp(tok, "alt")) v = VK_MENU; else if (!strcasecmp(tok, "ctrl")) v = VK_CONTROL;
        else if (!strcasecmp(tok, "shift")) v = VK_SHIFT; else if (!strcasecmp(tok, "enter")) v = VK_RETURN;
        else if (!strcasecmp(tok, "esc")) v = VK_ESCAPE; else if (!strcasecmp(tok, "tab")) v = VK_TAB;
        else if (!strcasecmp(tok, "space")) v = VK_SPACE; else if (!strcasecmp(tok, "down")) v = VK_DOWN;
        else if (!strcasecmp(tok, "up")) v = VK_UP; else if (!strcasecmp(tok, "left")) v = VK_LEFT;
        else if (!strcasecmp(tok, "right")) v = VK_RIGHT; else if (!strcasecmp(tok, "del")) v = VK_DELETE;
        else if (!strcasecmp(tok, "bs")) v = VK_BACK; else if (!strcasecmp(tok, "home")) v = VK_HOME;
        else if (!strcasecmp(tok, "end")) v = VK_END;
        else if (!strcasecmp(tok, "pageup")) v = VK_PRIOR;
        else if (!strcasecmp(tok, "pagedown")) v = VK_NEXT;
        else if (!strcasecmp(tok, "insert")) v = VK_INSERT;
        /* the numeric keypad, as DOSBox-X's AUTOTYPE names its keys */
        else if (!strcasecmp(tok, "kp_plus")) v = VK_ADD;
        else if (!strcasecmp(tok, "kp_minus")) v = VK_SUBTRACT;
        else if (!strcasecmp(tok, "kp_multiply")) v = VK_MULTIPLY;
        else if (!strcasecmp(tok, "kp_divide")) v = VK_DIVIDE;
        else if (!strcasecmp(tok, "kp_period")) v = VK_DECIMAL;
        else if (!strcasecmp(tok, "kp_enter")) v = VK_RETURN;
        else if (!strncasecmp(tok, "kp_", 3) && isdigit((unsigned char)tok[3]) && !tok[4]) v = VK_NUMPAD0 + tok[3] - '0';
        else if (!strcasecmp(tok, "semicolon")) v = 0xBA;
        else if (!strcasecmp(tok, "equals")) v = 0xBB;
        else if (!strcasecmp(tok, "comma")) v = 0xBC;
        else if (!strcasecmp(tok, "minus")) v = 0xBD;
        else if (!strcasecmp(tok, "period")) v = 0xBE;
        else if (!strcasecmp(tok, "slash")) v = 0xBF;
        else if (!strcasecmp(tok, "grave")) v = 0xC0;
        else if (!strcasecmp(tok, "lbracket")) v = 0xDB;
        else if (!strcasecmp(tok, "backslash")) v = 0xDC;
        else if (!strcasecmp(tok, "rbracket")) v = 0xDD;
        else if (!strcasecmp(tok, "quote")) v = 0xDE;
        else if ((tok[0] == 'f' || tok[0] == 'F') && isdigit((unsigned char)tok[1])) v = VK_F1 + atoi(tok + 1) - 1;
        else if (strlen(tok) == 1) v = toupper((unsigned char)tok[0]);
        if (next && nm < 3) mods[nm++] = v; else vk = v;
        tok = next;
    }
    for (int i = 0; i < nm; i++) { w16_keystate[mods[i]] |= 0x80; key_event(WM_KEYDOWN, mods[i], 0, 0); }
    w16_keystate[vk] |= 0x80;
    key_event(WM_KEYDOWN, vk, 0, 0);
    w16_keystate[vk] &= ~0x80;
    key_event(WM_KEYUP, vk, 0, 0);
    for (int i = nm - 1; i >= 0; i--) { w16_keystate[mods[i]] &= ~0x80; key_event(WM_KEYUP, mods[i], 0, 0); }
}

/* returns 1 if it produced input */
static int script_step(void)
{
    if (!script || (int)(GetTickCount() - script_wait_until) < 0) return 0;
    if (qhead || w16_any_paint_pending()) return 0; /* run only when idle, like a user would */
    char line[512];
    while (fgets(line, sizeof line, script)) {
        char *c = line;
        while (*c == ' ' || *c == '\t') c++;
        c[strcspn(c, "\r\n")] = 0;
        if (!*c || *c == '#') continue;
        char cmd[32], arg[480] = "";
        sscanf(c, "%31s %479[^\n]", cmd, arg);
        if (!strcmp(cmd, "sleep")) { script_wait_until = GetTickCount() + atoi(arg); return 0; }
        if (!strcmp(cmd, "key")) { synth_key(arg); script_wait_until = GetTickCount() + 30; return 1; }
        if (!strcmp(cmd, "type")) {
            for (char *p = arg; *p; p++) {
                HWND h = w16_focus ? w16_focus : w16_active;
                if (h) enqueue(h, WM_CHAR, (unsigned char)*p, 1, 0);
            }
            return 1;
        }
        /* mouse: move | click | dblclick | down | up X Y [shift] [ctrl]; an r or m in front (rclick,
         * rdown, mup ...) uses the right or middle button; shift / ctrl are held during the events */
        const char *mc = cmd;
        int bvk = VK_LBUTTON;
        UINT bdown = WM_LBUTTONDOWN;
        if ((cmd[0] == 'r' || cmd[0] == 'm') && (!strcmp(cmd + 1, "click") || !strcmp(cmd + 1, "dblclick") ||
                                                  !strcmp(cmd + 1, "down") || !strcmp(cmd + 1, "up"))) {
            bvk = cmd[0] == 'r' ? VK_RBUTTON : VK_MBUTTON;
            bdown = cmd[0] == 'r' ? WM_RBUTTONDOWN : WM_MBUTTONDOWN;
            mc = cmd + 1;
        }
        if (!strcmp(mc, "move") || !strcmp(mc, "click") || !strcmp(mc, "dblclick") || !strcmp(mc, "down") || !strcmp(mc, "up")) {
            int x, y, mods[2] = {0, 0};
            char m1[16] = "", m2[16] = "";
            int na = sscanf(arg, "%d %d %15s %15s", &x, &y, m1, m2);
            if (na >= 2) { w16_mouse.x = x; w16_mouse.y = y; clamp_mouse(); }
            for (int i = 0; i < 2; i++) {
                const char *m = i ? m2 : m1;
                mods[i] = !strcasecmp(m, "shift") ? VK_SHIFT : !strcasecmp(m, "ctrl") ? VK_CONTROL : 0;
                /* the key goes down as a message too, so GetKeyState still sees it when the queued
                 * click is processed (USER's list box reads Shift / Ctrl that way) */
                if (mods[i]) { w16_keystate[mods[i]] |= 0x80; key_event(WM_KEYDOWN, mods[i], 0, 0); }
            }
            mouse_event(WM_MOUSEMOVE);
            if (!strcmp(mc, "click") || !strcmp(mc, "down") || !strcmp(mc, "dblclick")) {
                w16_keystate[bvk] |= 0x80; mouse_event(bdown);
            }
            if (!strcmp(mc, "click") || !strcmp(mc, "up") || !strcmp(mc, "dblclick")) {
                w16_keystate[bvk] &= ~0x80; mouse_event(bdown + 1);
            }
            if (!strcmp(mc, "dblclick")) {
                w16_keystate[bvk] |= 0x80; mouse_event(bdown);
                w16_keystate[bvk] &= ~0x80; mouse_event(bdown + 1);
            }
            for (int i = 1; i >= 0; i--)
                if (mods[i]) { w16_keystate[mods[i]] &= ~0x80; key_event(WM_KEYUP, mods[i], 0, 0); }
            script_wait_until = GetTickCount() + 30;
            return 1;
        }
        if (!strcmp(cmd, "shot") || !strcmp(cmd, "shotcaret")) {
            /* shot: the caret hidden, so screenshots are deterministic; shotcaret: the caret shown */
            int want = !strcmp(cmd, "shotcaret") && caret.created && caret.hide == 0;
            int on = caret.on;
            if (on != want) caret_xor();
            w16_screenshot(arg);
            if (on != want) caret_xor();
            /* the active window's rectangle goes into rects.txt beside the screenshot, so
             * tools/regress.sh can compare just that window with real 3.11 */
            if (w16_active) {
                const char *slash = strrchr(arg, '/');
                char path[600];
                snprintf(path, sizeof path, "%.*srects.txt", slash ? (int)(slash - arg + 1) : 0, arg);
                FILE *rf = fopen(path, "a");
                if (rf) {
                    RECT r = w16_active->rw;
                    fprintf(rf, "%s %d %d %d %d\n", slash ? slash + 1 : arg, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
                    fclose(rf);
                }
            }
            continue;
        }
        if (!strcmp(cmd, "clip")) {
            /* TEXT onto the clipboard as CF_TEXT, as another program's Edit > Copy would put it */
            HGLOBAL g = GlobalAlloc(GHND, strlen(arg) + 1);
            char *d = GlobalLock(g);
            strcpy(d, arg);
            GlobalUnlock(g);
            OpenClipboard(NULL);
            EmptyClipboard();
            SetClipboardData(CF_TEXT, g);
            CloseClipboard();
            continue;
        }
        if (!strcmp(cmd, "quit")) exit(0);
    }
    fclose(script);
    script = NULL;
    exit(0);
}

/* ------------------------------------------------------------------ typematic
 * The host's key repeat is ignored; the last key pressed repeats at the PC/AT typematic
 * timing that KeyboardDelay / KeyboardSpeed select, like KEYBOARD.DRV SetSpeed (seg6:0020)
 * programming the 8042: delay (d+1)*250 ms; the speed indexes the driver's table (seg6:0000) for
 * the rate code B<<3|A, period (8+A)*2^B*4.17 ms. */
static int rep_vk, rep_scan;
static DWORD rep_next;

static DWORD rep_period(void)
{
    static const unsigned char rate[32] = {
        0x1F, 0x1F, 0x1F, 0x1A, 0x17, 0x14, 0x12, 0x11, 0x0F, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x09, 0x08,
        0x07, 0x06, 0x06, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
    };
    int code = rate[w16_kbd_speed & 0x1F];
    return (DWORD)((8 + (code & 7)) * (1 << ((code >> 3) & 3)) * 417 / 100);
}

static void typematic(void)
{
    if (!rep_vk) return;
    DWORD now = GetTickCount();
    if ((int)(now - rep_next) < 0) return;
    if (!(w16_keystate[rep_vk] & 0x80)) { rep_vk = 0; return; }
    key_event(WM_KEYDOWN, rep_vk, rep_scan, 1);
    rep_next += rep_period();
    if ((int)(now - rep_next) > 0) rep_next = now + rep_period(); /* fell behind: don't burst */
}

/* ------------------------------------------------------------------ SDL pump */
static void handle_sdl(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        if (w16_active) PostMessage(w16_active, WM_SYSCOMMAND, SC_CLOSE, 0);
        else exit(0);
        break;
    case SDL_MOUSEMOTION:
        w16_mouse.x = e->motion.x; w16_mouse.y = e->motion.y;
        clamp_mouse();
        mouse_event(WM_MOUSEMOVE);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        w16_mouse.x = e->button.x; w16_mouse.y = e->button.y;
        clamp_mouse();
        int down = e->type == SDL_MOUSEBUTTONDOWN;
        int vk = e->button.button == SDL_BUTTON_LEFT ? VK_LBUTTON : e->button.button == SDL_BUTTON_RIGHT ? VK_RBUTTON : VK_MBUTTON;
        if (w16_swap_buttons && vk != VK_MBUTTON) vk = vk == VK_LBUTTON ? VK_RBUTTON : VK_LBUTTON;
        UINT base = vk == VK_LBUTTON ? WM_LBUTTONDOWN : vk == VK_RBUTTON ? WM_RBUTTONDOWN : WM_MBUTTONDOWN;
        if (down) w16_keystate[vk] |= 0x80; else w16_keystate[vk] &= ~0x80;
        mouse_event(down ? base : base + 1);
        break;
    }
    case SDL_MOUSEWHEEL: {
        /* 3.1 had no wheel: map to scroll messages for the focus window */
        HWND h = w16_focus ? w16_focus : w16_active;
        while (h && !(h->style & WS_VSCROLL) && h->parent != w16_desktop) h = h->parent;
        if (h) for (int i = 0; i < 3; i++) enqueue(h, WM_VSCROLL, e->wheel.y > 0 ? SB_LINEUP : SB_LINEDOWN, 0, 0);
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int vk = sdl_to_vk(e->key.keysym.sym, e->key.keysym.scancode);
        if (!vk || e->key.repeat) break;
        int down = e->type == SDL_KEYDOWN;
        if (down) {
            w16_keystate[vk] |= 0x80; w16_keystate[vk] ^= 1;
            rep_vk = vk; rep_scan = e->key.keysym.scancode;
            rep_next = GetTickCount() + (w16_kbd_delay + 1) * 250;
        } else {
            w16_keystate[vk] &= ~0x80;
            if (vk == rep_vk) rep_vk = 0;
        }
        key_event(down ? WM_KEYDOWN : WM_KEYUP, vk, e->key.keysym.scancode, 0);
        break;
    }
    case SDL_TEXTINPUT: {
        /* characters outside the US layout table (dead keys, IME): deliver as cp1252 WM_CHAR */
        const unsigned char *s = (const unsigned char *)e->text.text;
        if (s[0] < 0x80) break;
        unsigned cp = 0;
        if ((s[0] & 0xE0) == 0xC0) cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        if (cp >= 0xA0 && cp <= 0xFF && w16_focus) enqueue(w16_focus, WM_CHAR, cp, 1, 0);
        break;
    }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED) { w16_screen_dirty = 1; w16_present(); }
        break;
    }
}

void w16_pump(int wait_ms)
{
    w16_clipboard_poll();
    if (script_step()) return;
    if (headless) {
        if (wait_ms > 0) usleep((wait_ms > 20 ? 20 : wait_ms) * 1000);
        return;
    }
    SDL_Event e;
    if (rep_vk && wait_ms > 0) {
        int left = (int)(rep_next - GetTickCount());
        if (left < wait_ms) wait_ms = left > 0 ? left : 0;
    }
    if (wait_ms > 0 && SDL_WaitEventTimeout(&e, wait_ms)) handle_sdl(&e);
    while (SDL_PollEvent(&e)) handle_sdl(&e);
    typematic();
}

void w16_delay(int ms)
{
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) { w16_present(); w16_pump(5); }
}

/* ------------------------------------------------------------------ GetMessage / PeekMessage */
static int match(const MSG *m, HWND h, UINT f, UINT l)
{
    if (h && m->hwnd != h && !IsChild(h, m->hwnd)) return 0;
    if (f || l) return m->message >= f && m->message <= l;
    return 1;
}

static int fetch(LPMSG out, HWND h, UINT first, UINT last, int remove, int *wait)
{
    if (w16_quit_posted && (!first || (WM_QUIT >= first && WM_QUIT <= last))) {
        memset(out, 0, sizeof *out);
        out->message = WM_QUIT;
        out->wParam = w16_quit_code;
        if (remove) w16_quit_posted = 0;
        return 1;
    }
    for (QMsg **p = &qhead, *prev = NULL; *p; prev = *p, p = &(*p)->next) {
        if (!match(&(*p)->m, h, first, last)) continue;
        QMsg *q = *p;
        *out = q->m;
        if (remove) {
            *p = q->next;
            if (qtail == q) qtail = prev;
            free(q);
            /* GetKeyState semantics: the key state is the one as of the last key message
             * retrieved, not the live hardware state (matters when input is queued ahead) */
            UINT km = out->message;
            if ((km == WM_KEYDOWN || km == WM_SYSKEYDOWN) && out->wParam < 256) w16_keystate[out->wParam] |= 0x80;
            else if ((km == WM_KEYUP || km == WM_SYSKEYUP) && out->wParam < 256) w16_keystate[out->wParam] &= ~0x80;
        }
        if (out->hwnd && !w16_valid(out->hwnd) && out->hwnd != w16_desktop) { if (remove) return fetch(out, h, first, last, remove, wait); }
        w16_msg_time = out->time;
        return 1;
    }
    /* paint */
    if (!first || (WM_PAINT >= first && WM_PAINT <= last)) {
        HWND ph = w16_next_to_paint(NULL);
        if (ph && (!h || ph == h || IsChild(h, ph))) {
            memset(out, 0, sizeof *out);
            out->hwnd = ph;
            out->message = WM_PAINT;
            out->time = GetTickCount();
            out->pt = w16_mouse;
            return 1;
        }
    }
    /* timers */
    DWORD now = GetTickCount();
    caret_tick(now, wait);
    if (!first || (WM_TIMER >= first && WM_TIMER <= last)) {
        Timer *t = due_timer(now, wait);
        if (t && (!h || t->h == h || IsChild(h, t->h))) {
            memset(out, 0, sizeof *out);
            out->hwnd = t->h;
            out->message = WM_TIMER;
            out->wParam = t->id;
            out->lParam = (LPARAM)t->fn;
            out->time = now;
            out->pt = w16_mouse;
            if (remove) t->due = now + t->ms;
            return 1;
        }
    }
    return 0;
}

/* the program ended with script lines left (the Control Panel closes after an applet opened by
 * name): what its windows uncovered is painted and the remaining shots are taken of the screen it
 * leaves (a wallpaper the applet set); input lines have nothing left to go to */
void w16_script_finish(void)
{
    if (!script) return;
    char line[512];
    while (fgets(line, sizeof line, script)) {
        char *c = line, cmd[32], arg[480] = "";
        while (*c == ' ' || *c == '\t') c++;
        c[strcspn(c, "\r\n")] = 0;
        if (!*c || *c == '#' || sscanf(c, "%31s %479[^\n]", cmd, arg) < 1) continue;
        if (strcmp(cmd, "shot") && strcmp(cmd, "shotcaret")) continue;
        MSG m;
        int wait = 0;
        for (int i = 0; i < 1000 && fetch(&m, NULL, WM_PAINT, WM_PAINT, 1, &wait); i++) DispatchMessage(&m);
        w16_screenshot(arg);
    }
    fclose(script);
    script = NULL;
}

BOOL GetMessage(LPMSG m, HWND h, UINT first, UINT last)
{
    for (;;) {
        int wait = 50;
        if (fetch(m, h, first, last, 1, &wait)) return m->message != WM_QUIT;
        w16_present();
        w16_pump(wait > 0 ? wait : 1);
    }
}

BOOL PeekMessage(LPMSG m, HWND h, UINT first, UINT last, UINT flags)
{
    int wait = 0;
    w16_present();
    w16_pump(0);
    return fetch(m, h, first, last, flags & PM_REMOVE, &wait);
}

BOOL WaitMessage(void)
{
    MSG m;
    int wait = 50;
    while (!fetch(&m, NULL, 0, 0, 0, &wait)) { w16_present(); w16_pump(wait > 0 ? wait : 1); wait = 50; }
    return TRUE;
}
BOOL GetInputState(void) { return qhead != NULL; }
DWORD GetMessagePos(void) { return MAKELONG(w16_mouse.x, w16_mouse.y); }
LONG GetMessageTime(void) { return (LONG)w16_msg_time; }

LRESULT DispatchMessage(const MSG *m)
{
    if (m->message == WM_TIMER && m->lParam) {
        ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, (UINT)m->wParam, GetTickCount());
        return 0;
    }
    if (!w16_valid(m->hwnd)) {
        if (m->hwnd == w16_desktop && m->hwnd) return w16_desktop->proc(m->hwnd, m->message, m->wParam, m->lParam);
        return 0;
    }
    if (m->message == WM_PAINT) {
        HWND h = m->hwnd;
        LRESULT r = SendMessage(h, w16_paint_msg(h), 0, 0);
        /* an app that does not call BeginPaint must not loop forever */
        if (w16_valid(h) && (!rgn_empty(&h->upd) || h->need_ncpaint)) {
            if (h->need_ncpaint) { h->need_ncpaint = 0; SendMessage(h, WM_NCPAINT, 1, 0); }
            rgn_clear(&h->upd);
        }
        return r;
    }
    return SendMessage(m->hwnd, m->message, m->wParam, m->lParam);
}

int w16_modal_loop_step(MSG *m)
{
    if (!GetMessage(m, NULL, 0, 0)) { PostQuitMessage((int)m->wParam); return 0; }
    TranslateMessage(m);
    DispatchMessage(m);
    return 1;
}

/* ------------------------------------------------------------------ DefWindowProc */
static HCURSOR size_cursor(int hit)
{
    switch (hit) {
    case HTLEFT: case HTRIGHT: return LoadCursor(NULL, IDC_SIZEWE);
    case HTTOP: case HTBOTTOM: return LoadCursor(NULL, IDC_SIZENS);
    case HTTOPLEFT: case HTBOTTOMRIGHT: return LoadCursor(NULL, IDC_SIZENWSE);
    case HTTOPRIGHT: case HTBOTTOMLEFT: return LoadCursor(NULL, IDC_SIZENESW);
    case HTSIZE: return LoadCursor(NULL, IDC_SIZENWSE);
    }
    return NULL;
}

static int menu_alt_pending;

LRESULT DefWindowProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        if (cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName)) { free(h->text); h->text = strdup(cs->lpszName); }
        return TRUE;
    }
    case WM_NCCALCSIZE: return 0;
    case WM_NCPAINT:
        if (IsIconic(h)) return 0;
        /* a child window's frame (an MDI child's) is drawn as its last WM_NCACTIVATE left it */
        w16_nc_paint(h, (h->style & WS_CHILD) ? h->active_frame : h->parent == w16_desktop ? (h == w16_active) : 0);
        return 0;
    case WM_NCACTIVATE:
        /* USER seg1:5CB8: WFFRAMEON follows wParam, then the caption is drawn so */
        if (h->parent == w16_desktop && !IsIconic(h)) {
            h->active_frame = wp != 0;
            w16_nc_paint(h, wp != 0);
        } else if (h->style & WS_CHILD) {
            h->active_frame = wp != 0;
            if (!IsIconic(h) && w16_has_caption(h->style) && w16_window_visible(h)) w16_nc_paint(h, wp != 0);
        }
        if (IsIconic(h)) w16_redraw_icon_title(h); /* the title shows the activation */
        return TRUE;
    case WM_ISACTIVEICON: return h->active_frame;
    case WM_CHILDACTIVATE: return 0;
    case WM_NCHITTEST: return w16_nc_hittest(h, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
    case WM_NCLBUTTONDOWN: return w16_nc_lbuttondown(h, (int)wp, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp));
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION && (h->style & WS_MAXIMIZEBOX)) SendMessage(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, lp);
        else if (wp == HTCAPTION && IsIconic(h)) SendMessage(h, WM_SYSCOMMAND, SC_RESTORE, lp);
        else if (wp == HTSYSMENU) SendMessage(h, WM_SYSCOMMAND, SC_CLOSE, lp);
        return 0;
    case WM_NCMOUSEMOVE: case WM_NCLBUTTONUP: case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: return 0;
    case WM_SETCURSOR: {
        int hit = (SHORT)LOWORD(lp);
        if (h->parent && h->parent != w16_desktop && (h->style & WS_CHILD)) {
            if (SendMessage(h->parent, WM_SETCURSOR, wp, lp)) return TRUE;
        }
        if (hit == HTERROR) { if (HIWORD(lp) == WM_LBUTTONDOWN) MessageBeep(0); return TRUE; }
        HCURSOR c = size_cursor(hit);
        if (!c && hit == HTCLIENT) c = h->cls->wc.hCursor;
        if (!c) c = LoadCursor(NULL, IDC_ARROW);
        SetCursor(c);
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        if ((h->style & WS_CHILD) && h->parent) {
            LRESULT r = SendMessage(h->parent, WM_MOUSEACTIVATE, wp, lp);
            if (r) return r;
        }
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp)) SetFocus(h);
        return 0;
    case WM_SETTEXT:
        if (IsIconic(h) && !(h->style & WS_CHILD)) w16_invalidate_icon_title(h); /* the old title's area */
        free(h->text);
        h->text = strdup(lp ? (const char *)lp : "");
        if (w16_has_caption(h->style) && w16_window_visible(h) && !IsIconic(h)) {
            HDC dc = GetWindowDC(h);
            w16_draw_caption(h, dc, w16_caption_active(h));
            ReleaseDC(h, dc);
        }
        if (IsIconic(h)) w16_redraw_icon_title(h);
        return TRUE;
    case WM_GETTEXT: {
        int n = (int)wp;
        if (n <= 0) return 0;
        snprintf((char *)lp, n, "%s", h->text);
        return strlen((char *)lp);
    }
    case WM_GETTEXTLENGTH: return strlen(h->text);
    case WM_PAINTICON:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (IsIconic(h)) w16_iconic_paint(h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HBRUSH b = h->cls->wc.hbrBackground;
        if (!b) return 0;
        RECT r;
        GetClientRect(h, &r);
        HDC dc = (HDC)wp;
        int sx = dc->brushorgx, sy = dc->brushorgy;
        FillRect(dc, &r, b);
        dc->brushorgx = sx; dc->brushorgy = sy;
        return 1;
    }
    case WM_ICONERASEBKGND: {
        /* the desktop shows through behind icons; a child's icon (an MDI child's) shows its parent's
         * background (UNTESTED against 3.11: SysEdit's children have a class icon) */
        HDC dc = (HDC)wp;
        RECT r;
        GetClientRect(h, &r);
        if ((h->style & WS_CHILD) && w16_valid(h->parent)) {
            HBRUSH b = h->parent->cls->wc.hbrBackground;
            if ((uintptr_t)b > 0 && (uintptr_t)b <= COLOR_BTNHIGHLIGHT + 1) b = w16_sys_brush((int)(uintptr_t)b - 1);
            if (b) FillRect(dc, &r, b);
            return 1;
        }
        w16_paint_desktop(dc, &r);
        return 1;
    }
    case WM_QUERYDRAGICON: return (LRESULT)h->cls->wc.hIcon;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: case WM_QUERYOPEN: return TRUE;
    case WM_SYSCOMMAND: w16_sys_command(h, (UINT)wp, (SHORT)LOWORD(lp), (SHORT)HIWORD(lp)); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_MENU) { menu_alt_pending = 1; return 0; }
        menu_alt_pending = 0;
        if (wp == VK_F4 && (HIWORD(lp) & 0x2000)) { PostMessage(w16_top_level(h), WM_SYSCOMMAND, SC_CLOSE, 0); return 0; }
        if (wp == VK_F10) { menu_alt_pending = 1; return 0; }
        if (wp == VK_TAB && (HIWORD(lp) & 0x2000)) { SendMessage(w16_top_level(h), WM_SYSCOMMAND, SC_NEXTWINDOW, 0); return 0; }
        if (wp == VK_ESCAPE && (HIWORD(lp) & 0x2000)) { SendMessage(w16_top_level(h), WM_SYSCOMMAND, SC_PREVWINDOW, 0); return 0; }
        return 0;
    case WM_SYSKEYUP:
        if ((wp == VK_MENU || wp == VK_F10) && menu_alt_pending) {
            menu_alt_pending = 0;
            HWND t = w16_top_level(h);
            SendMessage(t, WM_SYSCOMMAND, SC_KEYMENU, 0);
        }
        return 0;
    case WM_SYSCHAR:
        /* USER seg1:622C: Enter restores an icon; with Alt down a character becomes SC_KEYMENU for
         * this window (the menu code goes on up from a child, w16_sys_command), except Alt+Space in a
         * child, which goes to the parent as WM_SYSCHAR; Tab and Esc do nothing; without Alt a beep */
        menu_alt_pending = 0;
        if (wp == '\r' && IsIconic(h)) { PostMessage(h, WM_SYSCOMMAND, SC_RESTORE, 0); return 0; }
        if ((HIWORD(lp) & 0x2000) && wp) {
            if (wp == '\t' || wp == 27) return 0;
            if (wp == ' ' && (h->style & (WS_CHILD | WS_POPUP)) == WS_CHILD && w16_valid(h->parent))
                return SendMessage(h->parent, WM_SYSCHAR, wp, lp);
            SendMessage(h, WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)wp);
        } else if (wp != 27)
            MessageBeep(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F10) menu_alt_pending = 1;
        return 0;
    case WM_KEYUP:
        if (wp == VK_F10 && menu_alt_pending) { menu_alt_pending = 0; SendMessage(w16_top_level(h), WM_SYSCOMMAND, SC_KEYMENU, 0); }
        return 0;
    case WM_CTLCOLOR: {
        HDC dc = (HDC)wp;
        int type = HIWORD(lp);
        if (type == CTLCOLOR_SCROLLBAR) {
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, 0);
            return (LRESULT)w16_sys_brush(COLOR_SCROLLBAR);
        }
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)w16_sys_brush(COLOR_WINDOW);
    }
    case WM_WINDOWPOSCHANGING: {
        /* USER seg1:609A: a new size within the window's MINMAXINFO limits */
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        if (!(wpos->flags & SWP_NOSIZE)) w16_clamp_window_size(h, &wpos->cx, &wpos->cy);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        /* WM_MOVE / WM_SIZE when the client area moved / changed size (SetWindowPos's flags) */
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        RECT pr = {0, 0, 0, 0};
        if (h->parent && h->parent != w16_desktop) pr = h->parent->rc;
        if (!(wpos->flags & W16_SWP_NOCLIENTMOVE)) SendMessage(h, WM_MOVE, 0, MAKELPARAM(h->rc.left - pr.left, h->rc.top - pr.top));
        if (!(wpos->flags & W16_SWP_NOCLIENTSIZE))
            SendMessage(h, WM_SIZE, IsZoomed(h) ? SIZE_MAXIMIZED : IsIconic(h) ? SIZE_MINIMIZED : SIZE_RESTORED,
                        MAKELPARAM(h->rc.right - h->rc.left, h->rc.bottom - h->rc.top));
        return 0;
    }
    case WM_VKEYTOITEM: case WM_CHARTOITEM: return -1;
    case WM_SETREDRAW:
        h->redraw_off = !wp;
        return 0;
    case WM_SHOWWINDOW: return 0;
    case WM_VSCROLL: case WM_HSCROLL: return 0;
    case WM_DROPFILES: return 0;
    }
    return 0;
}
