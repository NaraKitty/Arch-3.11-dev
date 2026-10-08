/* libw16 internal declarations */
#ifndef W16INT_H
#define W16INT_H

#include "w16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W16_LOG(...) do { if (w16_debug) fprintf(stderr, "[w16] " __VA_ARGS__); } while (0)
extern int w16_debug;

/* ------------------------------------------------------------------ regions (rect lists) */
typedef struct {
    int n, cap;
    RECT *r;
} Region;
void rgn_init(Region *g);
void rgn_free(Region *g);
void rgn_clear(Region *g);
void rgn_set(Region *g, const RECT *r);
void rgn_copy(Region *d, const Region *s);
void rgn_add(Region *g, const RECT *r);          /* union */
void rgn_sub(Region *g, const RECT *r);          /* subtract */
void rgn_and(Region *g, const RECT *r);          /* intersect with rect */
void rgn_and_rgn(Region *g, const Region *o);
void rgn_offset(Region *g, int dx, int dy);
int rgn_empty(const Region *g);
void rgn_bounds(const Region *g, RECT *out);
int rgn_contains(const Region *g, int x, int y);

/* ------------------------------------------------------------------ modules / resources */
typedef struct {
    int type_id;          /* integer type, or 0 if named */
    char type_name[32];
    int name_id;          /* integer name, or 0 if named */
    char name[64];
    uint32_t off, len;
} W16Res;

struct W16Module {
    char name[32];        /* module name from the NE header, e.g. NOTEPAD */
    char file[32];        /* file name, e.g. NOTEPAD.EXE */
    uint8_t *data;
    size_t size;
    W16Res *res;
    int nres;
    struct W16Module *next;
};
HINSTANCE w16_module_open(const char *file); /* from assets/files, cached */
const W16Res *w16_find_res(HINSTANCE m, LPCSTR name, LPCSTR type);
const uint8_t *w16_res_data(HINSTANCE m, const W16Res *r);
HINSTANCE w16_system_module(const char *file); /* USER.EXE, VGA.DRV, ... (NULL if absent) */
int w16_wildmatch(const char *pat, const char *s); /* DOS wildcards, case-insensitive */

/* ------------------------------------------------------------------ GDI */
enum { OBJ_PEN = 1, OBJ_BRUSH, OBJ_FONT, OBJ_BITMAP, OBJ_RGN, OBJ_PAL };

typedef struct W16Font W16Font;

typedef struct W16Bitmap {
    int w, h;
    int mono;            /* 1bpp semantics: pixels are 0x000000 or 0xFFFFFF */
    uint32_t *px;        /* 0x00RRGGBB */
} W16Bitmap;

struct W16GdiObj {
    int kind;
    int stock;
    int refs_selected;
    union {
        struct { int style, width; COLORREF color; } pen;
        struct { int style, hatch; COLORREF color; uint32_t pat[64]; int has_pat; } brush;
        struct { LOGFONT lf; W16Font *f; } font;
        W16Bitmap bmp;
        Region rgn;
    } u;
};

struct W16DC {
    int is_mem, is_info;
    HWND hwnd;           /* window this DC paints, or NULL */
    W16Bitmap *target;   /* NULL = screen */
    HBITMAP sel_bitmap;
    int ox, oy;          /* device origin of the DC (screen coordinates of client/window 0,0) */
    Region vis;          /* visible region, device coords */
    Region clip;         /* user clip (device coords); empty+use_clip=0 means none */
    int use_clip;
    HPEN pen;
    HBRUSH brush;
    HFONT font;
    COLORREF text, bk;
    int bkmode, rop2;
    UINT align;
    int curx, cury;
    int mapmode;
    int worgx, worgy, vorgx, vorgy, wextx, wexty, vextx, vexty;
    int brushorgx, brushorgy;
    int charextra;
    int saved_depth;
    struct W16DC *saved;
};

/* the screen */
extern W16Bitmap w16_screen;
extern int w16_screen_dirty;
void w16_screen_init(int w, int h);
void w16_present(void);

/* drawing helpers used throughout USER (device coordinates, clipped to dc->vis/clip) */
uint32_t w16_display_px(uint32_t p);         /* logical pixel -> what the VGA monitor shows */
const uint32_t *w16_display_frame(void);     /* the screen converted with w16_display_px */
uint32_t w16_rgb(COLORREF c);              /* COLORREF -> 0xRRGGBB (nearest VGA colour) */
uint32_t w16_dither(COLORREF c, int x, int y);
uint32_t w16_invert_px(uint32_t p);
void w16_fill_rect_dev(HDC dc, const RECT *r, HBRUSH b);
void w16_fill_solid_dev(HDC dc, const RECT *r, uint32_t rgb);
void w16_invert_dev(HDC dc, const RECT *r);
void w16_hline(HDC dc, int x1, int x2, int y, uint32_t rgb);
void w16_vline(HDC dc, int x, int y1, int y2, uint32_t rgb);
void w16_blit_bitmap(HDC dc, int x, int y, const W16Bitmap *bm, int sx, int sy, int w, int h, DWORD rop);
void w16_lp_to_dp(HDC dc, int *x, int *y);
HDC w16_make_dc(HWND h, int window_dc);
void w16_dc_clip_iter_begin(HDC dc, Region *out);
W16Bitmap *w16_bitmap_of(HBITMAP h);
HBRUSH w16_sys_brush(int color_index);
HPEN w16_sys_pen(int color_index);

/* OEM bitmaps from the user's display driver (VGA.DRV) */
W16Bitmap *w16_obm(int id);

/* ------------------------------------------------------------------ fonts */
struct W16Font {
    char face[LF_FACESIZE];
    int points, height, ascent, ileading, eleading, avgw, maxw, weight;
    int italic, underline, strike, charset, pitchfam;
    int first, last, defchar, breakchar;
    int bold_sim;        /* emboldened by overstrike: +1 px per glyph */
    int widths[256];
    const uint8_t *fnt;  /* raw FNT (inside module data) */
    int v3;
    struct W16Font *next;
};
W16Font *w16_font_realize(const LOGFONT *lf);
W16Font *w16_font_system(void);
int w16_text_width(W16Font *f, const char *s, int n);
int w16_ave_char_width(HDC dc, TEXTMETRIC *tm); /* USER's average width (GetDialogBaseUnits) */
void w16_draw_text_dev(HDC dc, W16Font *f, int x, int y, const char *s, int n, uint32_t fg,
                       const int *dx, int charextra);
void w16_fonts_init(void);
W16Font *w16_dc_font(HDC dc);

/* ------------------------------------------------------------------ USER: windows */
#define W16_WND_MAGIC 0x57314E44u
#define W16_ES_COMBOBOX 0x0200L  /* USER's internal edit style: the edit of a combo box */
typedef struct W16Class {
    WNDCLASS wc;
    char name[64];
    struct W16Class *next;
    int system;
} W16Class;

typedef struct {
    int min, max, pos;
    int disabled;        /* ESB_ flags */
    int shown;
} W16Scroll;

typedef struct W16Prop {
    char name[64];
    HANDLE val;
    struct W16Prop *next;
} W16Prop;

struct W16Window {
    uint32_t magic;
    W16Class *cls;
    WNDPROC proc;
    char *text;
    DWORD style, exstyle;
    RECT rw;            /* window rect, screen coords */
    RECT rc;            /* client rect, screen coords */
    RECT restore;       /* normal position (for min/max) */
    POINT iconpos;
    int has_iconpos;
    HWND parent, owner;
    HWND child;         /* topmost child */
    HWND next;          /* next sibling below */
    HMENU menu;         /* menu bar, or child id when WS_CHILD */
    UINT id;
    HINSTANCE inst;
    uint8_t *extra;
    int cbextra;
    W16Scroll sb[2];
    Region upd;         /* update region (screen coords) */
    int need_erase, need_ncpaint, internal_paint;
    int destroyed, in_destroy;
    int active_frame;   /* caption drawn active */
    HMENU sysmenu;
    W16Prop *props;
    HFONT font;         /* for controls */
    void *ctl;          /* control private data */
    int is_dialog;
    int visible_cache;
    int show_state;     /* SW_ state last applied */
    int redraw_off;
    int send_sizemove;   /* created hidden: WM_SIZE + WM_MOVE are sent on the first ShowWindow */
    HICON icon_cache;
};

extern HWND w16_desktop;
extern HWND w16_focus, w16_active, w16_capture;
int w16_valid(HWND h);
W16Class *w16_find_class(LPCSTR name, HINSTANCE inst);
void w16_register_system_classes(void);
void w16_calc_visrgn(HWND h, int window, int clipchildren, Region *out);
void w16_invalidate_screen_rect(const RECT *r);   /* repaint everything under r */
void w16_invalidate_window(HWND h, const RECT *screen_r, int erase, int nc);
HWND w16_next_to_paint(HWND root);
int w16_any_paint_pending(void);
void w16_set_window_rect(HWND h, const RECT *rw, UINT swp);
void w16_get_minmax_info(HWND h, MINMAXINFO *mm);       /* USER seg6:1A4F (sends WM_GETMINMAXINFO) */
void w16_clamp_window_size(HWND h, int *cx, int *cy);    /* USER seg1:0000 */
HWND w16_top_level(HWND h);
HWND w16_window_under(HWND h, POINT pt); /* hit testing past an HTTRANSPARENT window */
int w16_window_visible(HWND h); /* visible including ancestors */
void w16_activate(HWND h, int how);
void w16_send_paint_cascade(HWND h); /* UpdateWindow semantics */
UINT w16_paint_msg(HWND h);          /* WM_PAINT, or WM_PAINTICON for an icon with a class icon */
void w16_destroy_children(HWND h);

/* non-client */
void w16_nc_calc(HWND h, const RECT *rw, RECT *rc);
void w16_nc_paint(HWND h, int active);
int w16_nc_hittest(HWND h, int x, int y);
LRESULT w16_nc_lbuttondown(HWND h, int hit, int x, int y);
void w16_sys_command(HWND h, UINT cmd, int x, int y);
void w16_draw_caption(HWND h, HDC dc, int active);
int w16_has_caption(DWORD style);
void w16_draw_sb(HWND h, HDC dc, int bar, int pressed_part);
void w16_get_sb_rect(HWND h, int bar, RECT *r); /* window-relative */
void w16_track_sb(HWND h, HWND notify, int bar, int x, int y, int ctl);
void w16_draw_sb_ctl(HDC dc, const RECT *r, int vert, W16Scroll *s, int pressed, int enabled_win, HWND bg);
int w16_sb_hit(const RECT *r, int vert, W16Scroll *s, int x, int y, RECT *part);
void w16_iconic_paint(HWND h);
/* USER's internal WINDOWPOS flags (WM_WINDOWPOSCHANGED): the client area kept its size / place */
#define W16_SWP_NOCLIENTSIZE 0x0800
#define W16_SWP_NOCLIENTMOVE 0x1000
void w16_minimize(HWND h);
void w16_maximize(HWND h);
void w16_restore(HWND h);

/* messages */
void w16_post(HWND h, UINT m, WPARAM wp, LPARAM lp);
void w16_pump(int wait_ms);        /* read SDL events into the queue */
int w16_modal_loop_step(MSG *m);   /* one GetMessage+dispatch for internal modal loops */
extern POINT w16_mouse;
extern DWORD w16_msg_time;
extern int w16_quit_posted, w16_quit_code;
void w16_caret_hide_for_paint(HWND h);
void w16_caret_restore_after_paint(HWND h);
void w16_caret_on_destroy(HWND h);
void w16_timers_on_destroy(HWND h);
extern int w16_keystate[256];
void w16_cursor_set_from_hit(HWND h, int hit);
void w16_xor_frame_rect(const RECT *r, int thick); /* drag outline */
void w16_delay(int ms);

/* menus */
struct W16MenuItem {
    UINT flags;
    UINT id;
    HMENU sub;
    char *text;
    RECT rc;            /* relative to menu bar window or popup */
};
struct W16Menu {
    int n, cap;
    struct W16MenuItem *it;
    int is_popup;
    int is_sys;
    int height;         /* bar height cache */
    HWND owner;
};
int w16_menubar_height(HWND h, int width);
void w16_draw_menubar(HWND h, HDC wdc);
void w16_menu_track_bar(HWND h, int x, int y, int key_char, int by_key);
void w16_menu_track_sys(HWND h, int by_key);
HMENU w16_default_sysmenu(HWND h);

/* dialogs */
typedef struct {
    DLGPROC proc;
    int result;
    int ended;
    HWND focus;
    int defid;
    LRESULT msgresult;
    LPARAM user;
    HFONT font;
    int ownfont;
    int cxchar, cychar;
    HWND owner;
    HWND disabled_owner; /* top-level owner the modal loop disabled (re-enabled by EndDialog) */
} W16Dialog;
W16Dialog *w16_dlg(HWND h);
LRESULT w16_dialog_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp);

/* controls */
LRESULT w16_button_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_static_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_edit_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_listbox_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_combobox_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_scrollbar_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT w16_desktop_proc(HWND, UINT, WPARAM, LPARAM);
/* the desktop (window.c): USER's SetDeskPattern / SetDeskWallpaper ((LPCSTR)-1 reads WIN.INI) */
BOOL w16_set_desk_pattern(LPCSTR pattern);
BOOL w16_set_desk_wallpaper(LPCSTR file);
HBRUSH w16_desktop_pattern_brush(void);  /* COLOR_BACKGROUND's brush while there is a pattern */
void w16_desktop_redraw(void);           /* the desktop and every window again */
void w16_paint_desktop(HDC dc, const RECT *r); /* the desktop's picture behind a window */
void w16_border_changed(int old);        /* SPI_SETBORDER: sizable frames grow or shrink */
LRESULT w16_combolbox_proc(HWND, UINT, WPARAM, LPARAM);
HBRUSH w16_ctl_color(HWND ctl, HDC dc, int type);
void w16_draw_prefix_text(HDC dc, int x, int y, const char *s, int n, int noprefix);
int w16_prefix_text_width(HDC dc, const char *s, int n);
char w16_mnemonic(const char *s);
void w16_draw_gray_text(HDC dc, int x, int y, const char *s, int n, int noprefix);
void w16_draw_stippled_text(HDC dc, int x, int y, const char *s, int n, int noprefix, COLORREF fg);
void w16_notify_parent(HWND h, int code);

/* system parameters */
extern COLORREF w16_syscolor[W16_NUM_SYSCOLORS];
extern int w16_metric[SM_CMETRICS];
void w16_sys_init(void);
const char *w16_config_dir(void);
extern int w16_border_width;
/* the next five are kept, reported and written as USER does; nothing acts on them yet (TODO,
 * UNTESTED: icon arrangement and title wrapping, the sizing grid when moving and sizing, Alt+Tab
 * switching, starting the screen saver after the timeout) */
extern int w16_icon_spacing;            /* SPI_ICONHORIZONTALSPACING (USER [0xc0]) */
extern int w16_icon_title_wrap;         /* SPI_GET/SETICONTITLEWRAP ([0x1b2]) */
extern int w16_grid;                    /* sizing grid in pixels, 8 * GridGranularity, at least 1 ([0x6e8]) */
extern int w16_fast_switch;             /* CoolSwitch ([0x1ae]) */
extern int w16_screen_save;             /* ScreenSaveTimeOut in seconds, negative while not active ([0xe8]) */
extern int w16_kbd_speed, w16_kbd_delay; /* typematic: speed 0..31, delay 0..3 (WIN.INI [windows]) */
extern UINT w16_dblclk_time;            /* DoubleClickSpeed */
extern int w16_swap_buttons;            /* SwapMouseButtons */
extern int w16_mouse_params[3];         /* MouseThreshold1, MouseThreshold2, MouseSpeed */
void w16_trails_init(void);             /* mouse trails (msg.c): VGA.DRV's MOUSETRAILS escape */
int w16_trails_query(void);
int w16_trails_escape(int n);
uint32_t w16_invert_display_px(uint32_t d);
void w16_invert_dev_gray(HDC dc, const RECT *r);
int w16_sb_thumb(const RECT *r, int vert, W16Scroll *s);  /* thumb offset in the bar, -1 if none (nc.c) */
int w16_dir_add(HWND lb, UINT attr, LPCSTR spec, int combo); /* LB_DIR / CB_DIR (dialog.c) */
int w16_mount_children(LPCSTR dosdir, char names[][64], int max); /* mount points inside a DOS dir (sys.c) */
extern BOOL w16_beep; /* WIN.INI Beep (sys.c), SPI_GETBEEP/SETBEEP */
#endif
