/*
 * w16.h - libw16: a native 64-bit reimplementation of the Windows 3.1 USER/GDI/KERNEL
 * programming interface on SDL2, used by the native ports of the 3.11 applications.
 *
 * Names and constant values follow the documented Windows 3.1 SDK so that ported code
 * reads like the original. Types are widened for 64-bit: handles are pointers,
 * WPARAM/LPARAM are pointer-sized. Strings are 8-bit Windows-1252 ("ANSI"), as in 3.1.
 *
 * Resources (menus, dialogs, strings, icons, bitmaps, fonts) are read at run time from
 * the user's own ripped 3.11 files (see tools/rip). Nothing Microsoft-made is compiled in.
 */
#ifndef W16_H
#define W16_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ basic types */
typedef int BOOL;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int16_t SHORT;
typedef int32_t LONG;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
/* list and combo box item data: a DWORD in 3.1, pointer-sized here (Win32's ULONG_PTR) so ports can
 * keep pointers there as 3.1 programs kept near pointers and handles */
typedef uintptr_t ULONG_PTR;
typedef uint32_t COLORREF;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef void *LPVOID;
typedef uint16_t ATOM;
typedef uintptr_t UINT_PTR_W16; /* menu item id or HMENU for MF_POPUP */

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define CALLBACK
#define WINAPI
#define FAR
#define NEAR
#define PASCAL

typedef struct W16Window *HWND;
typedef struct W16DC *HDC;
typedef struct W16GdiObj *HGDIOBJ;
typedef struct W16GdiObj *HPEN;
typedef struct W16GdiObj *HBRUSH;
typedef struct W16GdiObj *HFONT;
typedef struct W16GdiObj *HBITMAP;
typedef struct W16GdiObj *HRGN;
typedef struct W16GdiObj *HPALETTE;
typedef struct W16Menu *HMENU;
typedef struct W16Module *HINSTANCE;
typedef struct W16Module *HMODULE;
typedef struct W16Icon *HICON;
typedef struct W16Icon *HCURSOR;
typedef struct W16Accel *HACCEL;
typedef void *HGLOBAL;
typedef void *HLOCAL;
typedef void *HANDLE;
typedef int HFILE;
#define HFILE_ERROR (-1)

typedef struct { int x, y; } POINT, *LPPOINT;
typedef struct { int cx, cy; } SIZE, *LPSIZE;
typedef struct { int left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (*TIMERPROC)(HWND, UINT, UINT, DWORD);
typedef void *FARPROC;

#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xFFFF))
#define LOBYTE(w) ((BYTE)(w))
#define HIBYTE(w) ((BYTE)(((WORD)(w) >> 8) & 0xFF))
#define MAKELONG(lo, hi) ((LONG)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))
#define MAKELPARAM(lo, hi) ((LPARAM)MAKELONG(lo, hi))
#define MAKEPOINT(l) (*(POINT *)&(l))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#define MAKEINTRESOURCE(i) ((LPCSTR)(uintptr_t)(WORD)(i))
#define IS_INTRESOURCE(p) (((uintptr_t)(p) >> 16) == 0)
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
/* WM_COMMAND packing in Win16: wParam = id, lParam = MAKELONG(hwndCtl, notify).
 * hwnd does not fit in 16 bits on a 64-bit host, so libw16 passes the control id in
 * wParam, the notification code in HIWORD(lParam) and keeps the control handle
 * retrievable with W16_CMD_HWND(lParam). */
#define GET_WM_COMMAND_ID(wp, lp) ((UINT)(wp))
#define GET_WM_COMMAND_CMD(wp, lp) HIWORD(lp)
HWND W16_CMD_HWND(LPARAM lp);
/* the WM_COMMAND lParam a control sends its parent: MAKELONG(hwnd slot, notification) */
LPARAM W16_CMD_LPARAM(HWND ctl, int code);

/* ------------------------------------------------------------------ messages */
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_QUERYOPEN 0x0013
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_ENDSESSION 0x0016
#define WM_SHOWWINDOW 0x0018
#define WM_CTLCOLOR 0x0019
/* HIWORD(lParam) of WM_CTLCOLOR */
#define CTLCOLOR_MSGBOX 0
#define CTLCOLOR_EDIT 1
#define CTLCOLOR_LISTBOX 2
#define CTLCOLOR_BTN 3
#define CTLCOLOR_DLG 4
#define CTLCOLOR_SCROLLBAR 5
#define CTLCOLOR_STATIC 6
#define WM_WININICHANGE 0x001A
#define WM_ACTIVATEAPP 0x001C
#define WM_FONTCHANGE 0x001D
#define WM_TIMECHANGE 0x001E
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_CHILDACTIVATE 0x0022
#define WM_GETMINMAXINFO 0x0024
#define WM_PAINTICON 0x0026
#define WM_ICONERASEBKGND 0x0027
#define WM_NEXTDLGCTL 0x0028
#define WM_DRAWITEM 0x002B
#define WM_MEASUREITEM 0x002C
#define WM_DELETEITEM 0x002D
#define WM_VKEYTOITEM 0x002E
#define WM_CHARTOITEM 0x002F
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_QUERYDRAGICON 0x0037
#define WM_COMPAREITEM 0x0039
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCLBUTTONUP 0x00A2
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_NCRBUTTONDOWN 0x00A4
#define WM_NCRBUTTONUP 0x00A5
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_KEYLAST 0x0108
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT 0x011F
#define WM_MENUCHAR 0x0120
#define WM_ENTERIDLE 0x0121
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSELAST 0x0209
#define WM_PARENTNOTIFY 0x0210
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_MDICREATE 0x0220
#define WM_MDIDESTROY 0x0221
#define WM_MDIACTIVATE 0x0222
#define WM_MDIRESTORE 0x0223
#define WM_MDINEXT 0x0224
#define WM_MDIMAXIMIZE 0x0225
#define WM_MDITILE 0x0226
#define WM_MDICASCADE 0x0227
#define WM_MDIICONARRANGE 0x0228
#define WM_MDIGETACTIVE 0x0229
#define WM_MDISETMENU 0x0230
#define WM_DROPFILES 0x0233
#define WM_CUT 0x0300
#define WM_COPY 0x0301
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define WM_RENDERFORMAT 0x0305
#define WM_RENDERALLFORMATS 0x0306
#define WM_DESTROYCLIPBOARD 0x0307
#define WM_DRAWCLIPBOARD 0x0308
#define WM_PAINTCLIPBOARD 0x0309
#define WM_CHANGECBCHAIN 0x030D
#define WM_QUERYNEWPALETTE 0x030F
#define WM_PALETTECHANGED 0x0311
#define WM_USER 0x0400

/* ------------------------------------------------------------------ window styles */
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_POPUPWINDOW (WS_POPUP | WS_BORDER | WS_SYSMENU)
#define WS_CHILDWINDOW WS_CHILD
#define WS_ICONIC WS_MINIMIZE
#define WS_TILEDWINDOW WS_OVERLAPPEDWINDOW

#define WS_EX_DLGMODALFRAME 0x0001L
#define WS_EX_NOPARENTNOTIFY 0x0004L
#define WS_EX_TOPMOST 0x0008L
#define WS_EX_ACCEPTFILES 0x0010L
#define WS_EX_TRANSPARENT 0x0020L

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_KEYCVTWINDOW 0x0004
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040
#define CS_PARENTDC 0x0080
#define CS_NOCLOSE 0x0200
#define CS_SAVEBITS 0x0800
#define CS_BYTEALIGNCLIENT 0x1000
#define CS_BYTEALIGNWINDOW 0x2000
#define CS_GLOBALCLASS 0x4000

#define CW_USEDEFAULT ((int)0x8000)

typedef struct {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASS, *LPWNDCLASS;

typedef struct {
    void *lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCSTR lpszName;
    LPCSTR lpszClass;
    DWORD dwExStyle;
} CREATESTRUCT, *LPCREATESTRUCT;

typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;

typedef struct {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore, fIncUpdate;
    BYTE rgbReserved[16];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct {
    POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} MINMAXINFO;

typedef struct {
    HWND hwnd, hwndInsertAfter;
    int x, y, cx, cy;
    UINT flags;
} WINDOWPOS;

/* ShowWindow */
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9

/* SetWindowPos */
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
#define HWND_DESKTOP ((HWND)0)
#define HWND_BROADCAST ((HWND)0xFFFF)

/* GetWindowLong / Word */
#define GWL_WNDPROC (-4)
#define GWW_HINSTANCE (-6)
#define GWL_HINSTANCE (-6)
#define GWW_HWNDPARENT (-8)
#define GWW_ID (-12)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8
#define GCW_HBRBACKGROUND (-10)
#define GCW_HCURSOR (-12)
#define GCW_HICON (-14)

/* GetWindow */
#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5

/* hit test */
#define HTERROR (-2)
#define HTTRANSPARENT (-1)
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTGROWBOX 4
#define HTSIZE HTGROWBOX
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTMINBUTTON 8
#define HTREDUCE HTMINBUTTON
#define HTMAXBUTTON 9
#define HTZOOM HTMAXBUTTON
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTBORDER 18

/* WM_SIZE */
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
/* WM_ACTIVATE */
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
/* WM_MOUSEACTIVATE */
#define MA_ACTIVATE 1
#define MA_ACTIVATEANDEAT 2
#define MA_NOACTIVATE 3

/* system commands */
#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_ICON SC_MINIMIZE
#define SC_MAXIMIZE 0xF030
#define SC_ZOOM SC_MAXIMIZE
#define SC_NEXTWINDOW 0xF040
#define SC_PREVWINDOW 0xF050
#define SC_CLOSE 0xF060
#define SC_VSCROLL 0xF070
#define SC_HSCROLL 0xF080
#define SC_MOUSEMENU 0xF090
#define SC_KEYMENU 0xF100
#define SC_ARRANGE 0xF110
#define SC_RESTORE 0xF120
#define SC_TASKLIST 0xF130
#define SC_SCREENSAVE 0xF140

/* key state / mouse */
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010

/* virtual keys */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91

/* ------------------------------------------------------------------ menus */
#define MF_INSERT 0x0000
#define MF_CHANGE 0x0080
#define MF_APPEND 0x0100
#define MF_DELETE 0x0200
#define MF_REMOVE 0x1000
#define MF_BYCOMMAND 0x0000
#define MF_BYPOSITION 0x0400
#define MF_SEPARATOR 0x0800
#define MF_ENABLED 0x0000
#define MF_GRAYED 0x0001
#define MF_DISABLED 0x0002
#define MF_UNCHECKED 0x0000
#define MF_CHECKED 0x0008
#define MF_USECHECKBITMAPS 0x0200
#define MF_STRING 0x0000
#define MF_BITMAP 0x0004
#define MF_OWNERDRAW 0x0100
#define MF_POPUP 0x0010
#define MF_MENUBARBREAK 0x0020
#define MF_MENUBREAK 0x0040
#define MF_UNHILITE 0x0000
#define MF_HILITE 0x0080
#define MF_SYSMENU 0x2000
#define MF_HELP 0x4000
#define MF_MOUSESELECT 0x8000
#define MF_END 0x0080
#define TPM_LEFTALIGN 0x0000
#define TPM_CENTERALIGN 0x0004
#define TPM_RIGHTALIGN 0x0008
#define TPM_LEFTBUTTON 0x0000
#define TPM_RIGHTBUTTON 0x0002

/* ------------------------------------------------------------------ dialogs / controls */
#define DS_ABSALIGN 0x01L
#define DS_SYSMODAL 0x02L
#define DS_LOCALEDIT 0x20L
#define DS_SETFONT 0x40L
#define DS_MODALFRAME 0x80L
#define DS_NOIDLEMSG 0x100L

#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

#define DM_GETDEFID (WM_USER + 0)
#define DM_SETDEFID (WM_USER + 1)
#define DC_HASDEFID 0x534B

#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB 0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_WANTMESSAGE 0x0004
#define DLGC_HASSETSEL 0x0008
#define DLGC_DEFPUSHBUTTON 0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON 0x0040
#define DLGC_WANTCHARS 0x0080
#define DLGC_STATIC 0x0100
#define DLGC_BUTTON 0x2000

/* buttons */
#define BS_PUSHBUTTON 0x00L
#define BS_DEFPUSHBUTTON 0x01L
#define BS_CHECKBOX 0x02L
#define BS_AUTOCHECKBOX 0x03L
#define BS_RADIOBUTTON 0x04L
#define BS_3STATE 0x05L
#define BS_AUTO3STATE 0x06L
#define BS_GROUPBOX 0x07L
#define BS_USERBUTTON 0x08L
#define BS_AUTORADIOBUTTON 0x09L
#define BS_OWNERDRAW 0x0BL
#define BS_LEFTTEXT 0x20L
#define BM_GETCHECK (WM_USER + 0)
#define BM_SETCHECK (WM_USER + 1)
#define BM_GETSTATE (WM_USER + 2)
#define BM_SETSTATE (WM_USER + 3)
#define BM_SETSTYLE (WM_USER + 4)
#define BN_CLICKED 0
#define BN_PAINT 1
#define BN_HILITE 2
#define BN_UNHILITE 3
#define BN_DISABLE 4
#define BN_DOUBLECLICKED 5

/* static */
#define SS_LEFT 0x00L
#define SS_CENTER 0x01L
#define SS_RIGHT 0x02L
#define SS_ICON 0x03L
#define SS_BLACKRECT 0x04L
#define SS_GRAYRECT 0x05L
#define SS_WHITERECT 0x06L
#define SS_BLACKFRAME 0x07L
#define SS_GRAYFRAME 0x08L
#define SS_WHITEFRAME 0x09L
#define SS_SIMPLE 0x0BL
#define SS_LEFTNOWORDWRAP 0x0CL
#define SS_NOPREFIX 0x80L
#define STM_SETICON (WM_USER + 0)
#define STM_GETICON (WM_USER + 1)

/* edit */
#define ES_LEFT 0x0000L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_UPPERCASE 0x0008L
#define ES_LOWERCASE 0x0010L
#define ES_PASSWORD 0x0020L
#define ES_AUTOVSCROLL 0x0040L
#define ES_AUTOHSCROLL 0x0080L
#define ES_NOHIDESEL 0x0100L
#define ES_OEMCONVERT 0x0400L
#define ES_READONLY 0x0800L
#define ES_WANTRETURN 0x1000L
#define EM_GETSEL (WM_USER + 0)
#define EM_SETSEL (WM_USER + 1)
#define EM_GETRECT (WM_USER + 2)
#define EM_SETRECT (WM_USER + 3)
#define EM_SETRECTNP (WM_USER + 4)
#define EM_SCROLL (WM_USER + 5)
#define EM_LINESCROLL (WM_USER + 6)
#define EM_GETMODIFY (WM_USER + 8)
#define EM_SETMODIFY (WM_USER + 9)
#define EM_GETLINECOUNT (WM_USER + 10)
#define EM_LINEINDEX (WM_USER + 11)
#define EM_SETHANDLE (WM_USER + 12)
#define EM_GETHANDLE (WM_USER + 13)
#define EM_GETTHUMB (WM_USER + 14)
#define EM_LINELENGTH (WM_USER + 17)
#define EM_REPLACESEL (WM_USER + 18)
#define EM_SETFONT (WM_USER + 19)
#define EM_GETLINE (WM_USER + 20)
#define EM_LIMITTEXT (WM_USER + 21)
#define EM_CANUNDO (WM_USER + 22)
#define EM_UNDO (WM_USER + 23)
#define EM_FMTLINES (WM_USER + 24)
#define EM_LINEFROMCHAR (WM_USER + 25)
#define EM_SETWORDBREAK (WM_USER + 26)
#define EM_SETTABSTOPS (WM_USER + 27)
#define EM_SETPASSWORDCHAR (WM_USER + 28)
#define EM_EMPTYUNDOBUFFER (WM_USER + 29)
#define EM_GETFIRSTVISIBLELINE (WM_USER + 30)
#define EM_SETREADONLY (WM_USER + 31)
#define EM_SETWORDBREAKPROC (WM_USER + 32)
#define EM_GETWORDBREAKPROC (WM_USER + 33)
#define EM_GETPASSWORDCHAR (WM_USER + 34)
#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
#define EN_UPDATE 0x0400
#define EN_ERRSPACE 0x0500
#define EN_MAXTEXT 0x0501
#define EN_HSCROLL 0x0601
#define EN_VSCROLL 0x0602

/* listbox */
#define LBS_NOTIFY 0x0001L
#define LBS_SORT 0x0002L
#define LBS_NOREDRAW 0x0004L
#define LBS_MULTIPLESEL 0x0008L
#define LBS_OWNERDRAWFIXED 0x0010L
#define LBS_OWNERDRAWVARIABLE 0x0020L
#define LBS_HASSTRINGS 0x0040L
#define LBS_USETABSTOPS 0x0080L
#define LBS_NOINTEGRALHEIGHT 0x0100L
#define LBS_MULTICOLUMN 0x0200L
#define LBS_WANTKEYBOARDINPUT 0x0400L
#define LBS_EXTENDEDSEL 0x0800L
#define LBS_DISABLENOSCROLL 0x1000L
#define LBS_STANDARD (LBS_NOTIFY | LBS_SORT | WS_VSCROLL | WS_BORDER)
#define LB_ADDSTRING (WM_USER + 1)
#define LB_INSERTSTRING (WM_USER + 2)
#define LB_DELETESTRING (WM_USER + 3)
#define LB_RESETCONTENT (WM_USER + 5)
#define LB_SETSEL (WM_USER + 6)
#define LB_SETCURSEL (WM_USER + 7)
#define LB_GETSEL (WM_USER + 8)
#define LB_GETCURSEL (WM_USER + 9)
#define LB_GETTEXT (WM_USER + 10)
#define LB_GETTEXTLEN (WM_USER + 11)
#define LB_GETCOUNT (WM_USER + 12)
#define LB_SELECTSTRING (WM_USER + 13)
#define LB_DIR (WM_USER + 14)
#define LB_GETTOPINDEX (WM_USER + 15)
#define LB_FINDSTRING (WM_USER + 16)
#define LB_GETSELCOUNT (WM_USER + 17)
#define LB_GETSELITEMS (WM_USER + 18)
#define LB_SETTABSTOPS (WM_USER + 19)
#define LB_GETHORIZONTALEXTENT (WM_USER + 20)
#define LB_SETHORIZONTALEXTENT (WM_USER + 21)
#define LB_SETCOLUMNWIDTH (WM_USER + 22)
#define LB_SETTOPINDEX (WM_USER + 24)
#define LB_GETITEMRECT (WM_USER + 25)
#define LB_GETITEMDATA (WM_USER + 26)
#define LB_SETITEMDATA (WM_USER + 27)
#define LB_SELITEMRANGE (WM_USER + 28)
#define LB_SETCARETINDEX (WM_USER + 31)
#define LB_GETCARETINDEX (WM_USER + 32)
#define LB_SETITEMHEIGHT (WM_USER + 33)
#define LB_GETITEMHEIGHT (WM_USER + 34)
#define LB_FINDSTRINGEXACT (WM_USER + 35)
#define LB_ERR (-1)
#define LB_ERRSPACE (-2)
#define LBN_ERRSPACE (-2)
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define LBN_SELCANCEL 3
#define LBN_SETFOCUS 4
#define LBN_KILLFOCUS 5

/* combobox */
#define CBS_SIMPLE 0x0001L
#define CBS_DROPDOWN 0x0002L
#define CBS_DROPDOWNLIST 0x0003L
#define CBS_OWNERDRAWFIXED 0x0010L
#define CBS_OWNERDRAWVARIABLE 0x0020L
#define CBS_AUTOHSCROLL 0x0040L
#define CBS_OEMCONVERT 0x0080L
#define CBS_SORT 0x0100L
#define CBS_HASSTRINGS 0x0200L
#define CBS_NOINTEGRALHEIGHT 0x0400L
#define CBS_DISABLENOSCROLL 0x0800L
#define CB_GETEDITSEL (WM_USER + 0)
#define CB_LIMITTEXT (WM_USER + 1)
#define CB_SETEDITSEL (WM_USER + 2)
#define CB_ADDSTRING (WM_USER + 3)
#define CB_DELETESTRING (WM_USER + 4)
#define CB_DIR (WM_USER + 5)
#define CB_GETCOUNT (WM_USER + 6)
#define CB_GETCURSEL (WM_USER + 7)
#define CB_GETLBTEXT (WM_USER + 8)
#define CB_GETLBTEXTLEN (WM_USER + 9)
#define CB_INSERTSTRING (WM_USER + 10)
#define CB_RESETCONTENT (WM_USER + 11)
#define CB_FINDSTRING (WM_USER + 12)
#define CB_SELECTSTRING (WM_USER + 13)
#define CB_SETCURSEL (WM_USER + 14)
#define CB_SHOWDROPDOWN (WM_USER + 15)
#define CB_GETITEMDATA (WM_USER + 16)
#define CB_SETITEMDATA (WM_USER + 17)
#define CB_GETDROPPEDCONTROLRECT (WM_USER + 18)
#define CB_SETITEMHEIGHT (WM_USER + 19)
#define CB_GETITEMHEIGHT (WM_USER + 20)
#define CB_SETEXTENDEDUI (WM_USER + 21)
#define CB_GETEXTENDEDUI (WM_USER + 22)
#define CB_GETDROPPEDSTATE (WM_USER + 23)
#define CB_FINDSTRINGEXACT (WM_USER + 24)
#define CB_ERR (-1)
#define CB_ERRSPACE (-2)
#define CBN_ERRSPACE (-1)
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_EDITUPDATE 6
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CBN_SELENDCANCEL 10

/* scroll bars */
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINELEFT 0
#define SB_LINEDOWN 1
#define SB_LINERIGHT 1
#define SB_PAGEUP 2
#define SB_PAGELEFT 2
#define SB_PAGEDOWN 3
#define SB_PAGERIGHT 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_LEFT 6
#define SB_BOTTOM 7
#define SB_RIGHT 7
#define SB_ENDSCROLL 8
#define SBS_HORZ 0x0000L
#define SBS_VERT 0x0001L
#define SBS_TOPALIGN 0x0002L
#define SBS_LEFTALIGN 0x0002L
#define SBS_BOTTOMALIGN 0x0004L
#define SBS_RIGHTALIGN 0x0004L
#define SBS_SIZEBOX 0x0008L
#define ESB_ENABLE_BOTH 0x0000
#define ESB_DISABLE_BOTH 0x0003

/* MessageBox */
#define MB_OK 0x0000
#define MB_OKCANCEL 0x0001
#define MB_ABORTRETRYIGNORE 0x0002
#define MB_YESNOCANCEL 0x0003
#define MB_YESNO 0x0004
#define MB_RETRYCANCEL 0x0005
#define MB_TYPEMASK 0x000F
#define MB_ICONHAND 0x0010
#define MB_ICONSTOP MB_ICONHAND
#define MB_ICONQUESTION 0x0020
#define MB_ICONEXCLAMATION 0x0030
#define MB_ICONASTERISK 0x0040
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_ICONMASK 0x00F0
#define MB_DEFBUTTON1 0x0000
#define MB_DEFBUTTON2 0x0100
#define MB_DEFBUTTON3 0x0200
#define MB_DEFMASK 0x0F00
#define MB_APPLMODAL 0x0000
#define MB_SYSTEMMODAL 0x1000
#define MB_TASKMODAL 0x2000
#define MB_NOFOCUS 0x8000

/* ------------------------------------------------------------------ GDI */
#define R2_BLACK 1
#define R2_NOTMERGEPEN 2
#define R2_MASKNOTPEN 3
#define R2_NOTCOPYPEN 4
#define R2_MASKPENNOT 5
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOTMASKPEN 8
#define R2_MASKPEN 9
#define R2_NOTXORPEN 10
#define R2_NOP 11
#define R2_MERGENOTPEN 12
#define R2_COPYPEN 13
#define R2_MERGEPENNOT 14
#define R2_MERGEPEN 15
#define R2_WHITE 16

#define SRCCOPY 0x00CC0020L
#define SRCPAINT 0x00EE0086L
#define SRCAND 0x008800C6L
#define SRCINVERT 0x00660046L
#define SRCERASE 0x00440328L
#define NOTSRCCOPY 0x00330008L
#define NOTSRCERASE 0x001100A6L
#define MERGECOPY 0x00C000CAL
#define MERGEPAINT 0x00BB0226L
#define PATCOPY 0x00F00021L
#define PATPAINT 0x00FB0A09L
#define PATINVERT 0x005A0049L
#define DSTINVERT 0x00550009L
#define BLACKNESS 0x00000042L
#define WHITENESS 0x00FF0062L

#define TRANSPARENT 1
#define OPAQUE 2

#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define HOLLOW_BRUSH NULL_BRUSH
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define OEM_FIXED_FONT 10
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define SYSTEM_FONT 13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE 15
#define SYSTEM_FIXED_FONT 16

#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL 5
#define PS_INSIDEFRAME 6
#define BS_SOLID 0
#define BS_NULL 1
#define BS_HOLLOW BS_NULL
#define BS_HATCHED 2
#define BS_PATTERN 3
#define HS_HORIZONTAL 0
#define HS_VERTICAL 1
#define HS_FDIAGONAL 2
#define HS_BDIAGONAL 3
#define HS_CROSS 4
#define HS_DIAGCROSS 5

typedef struct { UINT lbStyle; COLORREF lbColor; int lbHatch; } LOGBRUSH;

#define LF_FACESIZE 32
typedef struct {
    int lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision,
        lfQuality, lfPitchAndFamily;
    char lfFaceName[LF_FACESIZE];
} LOGFONT, *LPLOGFONT;
#define FW_DONTCARE 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define ANSI_CHARSET 0
#define SYMBOL_CHARSET 2
#define OEM_CHARSET 255
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE 0x00
#define FF_ROMAN 0x10
#define FF_SWISS 0x20
#define FF_MODERN 0x30
#define FF_SCRIPT 0x40
#define FF_DECORATIVE 0x50

typedef struct {
    int tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    int tmAveCharWidth, tmMaxCharWidth, tmWeight;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmFirstChar, tmLastChar, tmDefaultChar,
        tmBreakChar, tmPitchAndFamily, tmCharSet;
    int tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
} TEXTMETRIC, *LPTEXTMETRIC;

typedef struct {
    int bmType, bmWidth, bmHeight, bmWidthBytes;
    BYTE bmPlanes, bmBitsPixel;
    void *bmBits;
} BITMAP;

#define TA_NOUPDATECP 0
#define TA_UPDATECP 1
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define ETO_GRAYED 1
#define ETO_OPAQUE 2
#define ETO_CLIPPED 4

#define DT_TOP 0x0000
#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_BOTTOM 0x0008
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_TABSTOP 0x0080
#define DT_NOCLIP 0x0100
#define DT_EXTERNALLEADING 0x0200
#define DT_CALCRECT 0x0400
#define DT_NOPREFIX 0x0800
#define DT_INTERNAL 0x1000

#define MM_TEXT 1
#define MM_LOMETRIC 2
#define MM_HIMETRIC 3
#define MM_LOENGLISH 4
#define MM_HIENGLISH 5
#define MM_TWIPS 6
#define MM_ISOTROPIC 7
#define MM_ANISOTROPIC 8

/* GetDeviceCaps */
#define DRIVERVERSION 0
#define TECHNOLOGY 2
#define HORZSIZE 4
#define VERTSIZE 6
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
#define NUMBRUSHES 16
#define NUMPENS 18
#define NUMFONTS 22
#define NUMCOLORS 24
#define ASPECTX 40
#define ASPECTY 42
#define ASPECTXY 44
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define RASTERCAPS 38
#define DT_RASDISPLAY 1

/* system colours */
#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_INACTIVECAPTION 3
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_CAPTIONTEXT 9
#define COLOR_ACTIVEBORDER 10
#define COLOR_INACTIVEBORDER 11
#define COLOR_APPWORKSPACE 12
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNFACE 15
#define COLOR_BTNSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18
#define COLOR_INACTIVECAPTIONTEXT 19
#define COLOR_BTNHIGHLIGHT 20
#define W16_NUM_SYSCOLORS 21

/* system metrics */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB 9
#define SM_CXHTHUMB 10
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CYMENU 15
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_CYKANJIWINDOW 18
#define SM_MOUSEPRESENT 19
#define SM_CYVSCROLL 20
#define SM_CXHSCROLL 21
#define SM_DEBUG 22
#define SM_SWAPBUTTON 23
#define SM_CXMIN 28
#define SM_CYMIN 29
#define SM_CXSIZE 30
#define SM_CYSIZE 31
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_CXICONSPACING 38
#define SM_CYICONSPACING 39
#define SM_MENUDROPALIGNMENT 40
#define SM_PENWINDOWS 41
#define SM_DBCSENABLED 42
#define SM_CMETRICS 43

/* resources */
#define RT_CURSOR MAKEINTRESOURCE(1)
#define RT_BITMAP MAKEINTRESOURCE(2)
#define RT_ICON MAKEINTRESOURCE(3)
#define RT_MENU MAKEINTRESOURCE(4)
#define RT_DIALOG MAKEINTRESOURCE(5)
#define RT_STRING MAKEINTRESOURCE(6)
#define RT_FONTDIR MAKEINTRESOURCE(7)
#define RT_FONT MAKEINTRESOURCE(8)
#define RT_ACCELERATOR MAKEINTRESOURCE(9)
#define RT_RCDATA MAKEINTRESOURCE(10)
#define RT_GROUP_CURSOR MAKEINTRESOURCE(12)
#define RT_GROUP_ICON MAKEINTRESOURCE(14)

#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_IBEAM MAKEINTRESOURCE(32513)
#define IDC_WAIT MAKEINTRESOURCE(32514)
#define IDC_CROSS MAKEINTRESOURCE(32515)
#define IDC_UPARROW MAKEINTRESOURCE(32516)
#define IDC_SIZE MAKEINTRESOURCE(32640)
#define IDC_ICON MAKEINTRESOURCE(32641)
#define IDC_SIZENWSE MAKEINTRESOURCE(32642)
#define IDC_SIZENESW MAKEINTRESOURCE(32643)
#define IDC_SIZEWE MAKEINTRESOURCE(32644)
#define IDC_SIZENS MAKEINTRESOURCE(32645)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define IDI_HAND MAKEINTRESOURCE(32513)
#define IDI_QUESTION MAKEINTRESOURCE(32514)
#define IDI_EXCLAMATION MAKEINTRESOURCE(32515)
#define IDI_ASTERISK MAKEINTRESOURCE(32516)

/* OEM bitmaps (from the display driver) */
#define OBM_CLOSE 32754
#define OBM_UPARROW 32753
#define OBM_DNARROW 32752
#define OBM_RGARROW 32751
#define OBM_LFARROW 32750
#define OBM_REDUCE 32749
#define OBM_ZOOM 32748
#define OBM_RESTORE 32747
#define OBM_REDUCED 32746
#define OBM_ZOOMD 32745
#define OBM_RESTORED 32744
#define OBM_UPARROWD 32743
#define OBM_DNARROWD 32742
#define OBM_RGARROWD 32741
#define OBM_LFARROWD 32740
#define OBM_MNARROW 32739
#define OBM_COMBO 32738
#define OBM_UPARROWI 32737
#define OBM_DNARROWI 32736
#define OBM_RGARROWI 32735
#define OBM_LFARROWI 32734
#define OBM_OLD_CLOSE 32767
#define OBM_SIZE 32766
#define OBM_OLD_UPARROW 32765
#define OBM_OLD_DNARROW 32764
#define OBM_OLD_RGARROW 32763
#define OBM_OLD_LFARROW 32762
#define OBM_BTSIZE 32761
#define OBM_CHECK 32760
#define OBM_CHECKBOXES 32759
#define OBM_BTNCORNERS 32758
#define OBM_OLD_REDUCE 32757
#define OBM_OLD_ZOOM 32756
#define OBM_OLD_RESTORE 32755

/* accelerator flags */
#define FVIRTKEY 0x01
#define FNOINVERT 0x02
#define FSHIFT 0x04
#define FCONTROL 0x08
#define FALT 0x10

/* clipboard */
#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_METAFILEPICT 3
#define CF_SYLK 4
#define CF_DIF 5
#define CF_TIFF 6
#define CF_OEMTEXT 7
#define CF_DIB 8
#define CF_PALETTE 9

/* memory */
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_DDESHARE 0x2000
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define LMEM_FIXED 0x0000
#define LMEM_MOVEABLE 0x0002
#define LMEM_ZEROINIT 0x0040
#define LHND (LMEM_MOVEABLE | LMEM_ZEROINIT)
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)

/* WinHelp */
#define HELP_CONTEXT 0x0001
#define HELP_QUIT 0x0002
#define HELP_INDEX 0x0003
#define HELP_CONTENTS 0x0003
#define HELP_HELPONHELP 0x0004
#define HELP_SETINDEX 0x0005
#define HELP_KEY 0x0101
#define HELP_PARTIALKEY 0x0105

/* owner draw */
#define ODT_MENU 1
#define ODT_LISTBOX 2
#define ODT_COMBOBOX 3
#define ODT_BUTTON 4
#define ODA_DRAWENTIRE 0x0001
#define ODA_SELECT 0x0002
#define ODA_FOCUS 0x0004
#define ODS_SELECTED 0x0001
#define ODS_GRAYED 0x0002
#define ODS_DISABLED 0x0004
#define ODS_CHECKED 0x0008
#define ODS_FOCUS 0x0010
typedef struct {
    UINT CtlType, CtlID, itemID, itemAction, itemState;
    HWND hwndItem;
    HDC hDC;
    RECT rcItem;
    ULONG_PTR itemData;
} DRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;
typedef struct {
    UINT CtlType, CtlID, itemID, itemWidth, itemHeight;
    ULONG_PTR itemData;
} MEASUREITEMSTRUCT, *LPMEASUREITEMSTRUCT;
typedef struct {
    UINT CtlType, CtlID, itemID;
    HWND hwndItem;
    ULONG_PTR itemData;
} DELETEITEMSTRUCT;
typedef struct {
    UINT CtlType, CtlID;
    HWND hwndItem;
    UINT itemID1;
    ULONG_PTR itemData1;
    UINT itemID2;
    ULONG_PTR itemData2;
} COMPAREITEMSTRUCT;

/* ------------------------------------------------------------------ functions */
/* process / startup: the port provides WinMain; libw16 provides main() */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);
/* Each port names the original module it takes its resources from, e.g. "NOTEPAD.EXE". */
extern const char *w16_app_module;

/* KERNEL */
HINSTANCE GetModuleHandle(LPCSTR name);
HINSTANCE w16_load_module(LPCSTR filename);
/* the file contents of segment seg (0 = the automatic data segment) of a loaded NE module, for
 * read-only tables; NULL if absent */
const void *w16_module_data(HINSTANCE m, int seg, unsigned *len);
DWORD GetTickCount(void);
DWORD GetCurrentTime(void);
int GetProfileInt(LPCSTR app, LPCSTR key, int def);
int GetProfileString(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, int cb);
BOOL WriteProfileString(LPCSTR app, LPCSTR key, LPCSTR val);
int GetPrivateProfileInt(LPCSTR app, LPCSTR key, int def, LPCSTR file);
int GetPrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, int cb, LPCSTR file);
BOOL WritePrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file);
UINT GetWindowsDirectory(LPSTR buf, UINT cb);
UINT GetSystemDirectory(LPSTR buf, UINT cb);
HGLOBAL GlobalAlloc(UINT flags, DWORD bytes);
HGLOBAL GlobalReAlloc(HGLOBAL h, DWORD bytes, UINT flags);
void *GlobalLock(HGLOBAL h);
BOOL GlobalUnlock(HGLOBAL h);
HGLOBAL GlobalFree(HGLOBAL h);
DWORD GlobalSize(HGLOBAL h);
HLOCAL LocalAlloc(UINT flags, UINT bytes);
HLOCAL LocalReAlloc(HLOCAL h, UINT bytes, UINT flags);
void *LocalLock(HLOCAL h);
BOOL LocalUnlock(HLOCAL h);
HLOCAL LocalFree(HLOCAL h);
UINT LocalSize(HLOCAL h);
int lstrlen(LPCSTR s);
LPSTR lstrcpy(LPSTR d, LPCSTR s);
LPSTR lstrcat(LPSTR d, LPCSTR s);
int lstrcmp(LPCSTR a, LPCSTR b);
int lstrcmpi(LPCSTR a, LPCSTR b);
LPSTR AnsiUpper(LPSTR s);
LPSTR AnsiLower(LPSTR s);
LPSTR AnsiNext(LPCSTR s);
LPSTR AnsiPrev(LPCSTR start, LPCSTR s);
/* KEYBOARD: OEM <-> ANSI. libw16's DOS layer already hands out ANSI names (OpenFile, DlgDirList),
 * so these copy the string */
void OemToAnsi(LPCSTR oem, LPSTR ansi);
void AnsiToOem(LPCSTR ansi, LPSTR oem);
BOOL IsCharAlpha(char c);
BOOL IsCharAlphaNumeric(char c);
BOOL IsCharUpper(char c);
BOOL IsCharLower(char c);
int wsprintf(LPSTR buf, LPCSTR fmt, ...);
int wvsprintf(LPSTR buf, LPCSTR fmt, va_list ap);
void OutputDebugString(LPCSTR s);
FARPROC MakeProcInstance(FARPROC p, HINSTANCE h);
void FreeProcInstance(FARPROC p);
/* files: DOS-style paths are mapped onto the Linux file system (see w16_dos_to_host) */
#define OF_READ 0x0000
#define OF_WRITE 0x0001
#define OF_READWRITE 0x0002
#define OF_SHARE_COMPAT 0x0000
#define OF_SHARE_EXCLUSIVE 0x0010
#define OF_SHARE_DENY_WRITE 0x0020
#define OF_SHARE_DENY_READ 0x0030
#define OF_SHARE_DENY_NONE 0x0040
#define OF_PARSE 0x0100
#define OF_DELETE 0x0200
#define OF_VERIFY 0x0400
#define OF_SEARCH 0x0400
#define OF_CANCEL 0x0800
#define OF_CREATE 0x1000
#define OF_PROMPT 0x2000
#define OF_EXIST 0x4000
#define OF_REOPEN 0x8000
typedef struct {
    BYTE cBytes, fFixedDisk;
    WORD nErrCode;
    BYTE reserved[4];
    char szPathName[260];
} OFSTRUCT, *LPOFSTRUCT;
HFILE OpenFile(LPCSTR name, OFSTRUCT *of, UINT style);
HFILE _lopen(LPCSTR name, int mode);
HFILE _lcreat(LPCSTR name, int attr);
UINT _lread(HFILE f, void *buf, UINT n);
UINT _lwrite(HFILE f, const void *buf, UINT n);
LONG _llseek(HFILE f, LONG off, int origin);
HFILE _lclose(HFILE f);
/* "C:\\FOO\\BAR.TXT" <-> "/home/user/FOO/BAR.TXT" (drive map in ~/.config/arch311/drives) */
int w16_dos_to_host(LPCSTR dos, char *host, size_t cb);
int w16_host_to_dos(const char *host, LPSTR dos, size_t cb);
int w16_drive_root(char letter, char *root, size_t cb); /* 0 if the drive letter is mapped */
void w16_dos_fullpath(LPCSTR dos, LPSTR out, size_t cb); /* "..\\X" -> "C:\\X": full, upper case, dots resolved */
void w16_getcwd(LPSTR dos, size_t cb);                  /* "C:\\WINDOWS" */
int w16_chdir(LPCSTR dos);                              /* "X:", "..", "X:\\DIR": 0, -1 no path, -2 no drive */
/* COMM (comm.c): COM1..4 are Linux's ttyS0..3 */
#define SETXOFF 1
#define SETXON 2
#define SETRTS 3
#define CLRRTS 4
#define SETDTR 5
#define CLRDTR 6
#define RESETDEV 7
#define GETMAXLPT 8
#define GETMAXCOM 9
#define GETBASEIRQ 10
LONG EscapeCommFunction(int cid, int func);
/* ExitWindows: the program's windows are asked (WM_QUERYENDSESSION) and told (WM_ENDSESSION), then
 * the program ends; restarting or rebooting the arch311 session is not wired up yet (UNTESTED) */
#define EW_RESTARTWINDOWS 0x42
#define EW_REBOOTSYSTEM 0x43
#define EW_EXITANDEXECAPP 0x44
BOOL ExitWindows(DWORD code, UINT reserved);
/* there are no critical-error boxes to suppress; the mode is kept for callers that restore it */
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000
UINT SetErrorMode(UINT mode);

/* USER: classes / windows */
ATOM RegisterClass(const WNDCLASS *wc);
BOOL UnregisterClass(LPCSTR name, HINSTANCE h);
BOOL GetClassInfo(HINSTANCE h, LPCSTR name, WNDCLASS *wc);
int GetClassName(HWND h, LPSTR buf, int cb);
HWND CreateWindow(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy,
                  HWND parent, HMENU menu, HINSTANCE inst, void *param);
HWND CreateWindowEx(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx,
                    int cy, HWND parent, HMENU menu, HINSTANCE inst, void *param);
BOOL DestroyWindow(HWND h);
BOOL IsWindow(HWND h);
BOOL IsWindowVisible(HWND h);
BOOL IsWindowEnabled(HWND h);
BOOL IsIconic(HWND h);
BOOL IsZoomed(HWND h);
BOOL IsChild(HWND parent, HWND h);
BOOL ShowWindow(HWND h, int cmd);
BOOL UpdateWindow(HWND h);
BOOL EnableWindow(HWND h, BOOL en);
BOOL MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint);
BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL BringWindowToTop(HWND h);
HWND GetParent(HWND h);
HWND SetParent(HWND h, HWND parent);
HWND GetWindow(HWND h, UINT cmd);
HWND GetTopWindow(HWND h);
HWND GetDesktopWindow(void);
HWND GetActiveWindow(void);
HWND SetActiveWindow(HWND h);
HWND GetFocus(void);
HWND SetFocus(HWND h);
HWND GetCapture(void);
HWND SetCapture(HWND h);
void ReleaseCapture(void);
HWND WindowFromPoint(POINT pt);
HWND ChildWindowFromPoint(HWND parent, POINT pt);
HWND FindWindow(LPCSTR cls, LPCSTR title);
LONG GetWindowLong(HWND h, int idx);
LONG SetWindowLong(HWND h, int idx, LONG v);
WORD GetWindowWord(HWND h, int idx);
WORD SetWindowWord(HWND h, int idx, WORD v);
intptr_t w16_GetWindowPtr(HWND h, int idx);
intptr_t w16_SetWindowPtr(HWND h, int idx, intptr_t v);
LONG GetClassLong(HWND h, int idx);
WORD GetClassWord(HWND h, int idx);
WORD SetClassWord(HWND h, int idx, WORD v);
intptr_t w16_SetClassPtr(HWND h, int idx, intptr_t v);
int GetWindowText(HWND h, LPSTR buf, int cb);
int GetWindowTextLength(HWND h);
void SetWindowText(HWND h, LPCSTR s);
void GetWindowRect(HWND h, LPRECT r);
void GetClientRect(HWND h, LPRECT r);
void ClientToScreen(HWND h, LPPOINT p);
void ScreenToClient(HWND h, LPPOINT p);
void MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n);
void AdjustWindowRect(LPRECT r, DWORD style, BOOL menu);
void AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex);
BOOL OpenIcon(HWND h);
BOOL CloseWindow(HWND h);
void DragAcceptFiles(HWND h, BOOL accept);
BOOL SetProp(HWND h, LPCSTR name, HANDLE v);
HANDLE GetProp(HWND h, LPCSTR name);
HANDLE RemoveProp(HWND h, LPCSTR name);
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
BOOL EnumChildWindows(HWND parent, WNDENUMPROC fn, LPARAM lp);
BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp);
BOOL FlashWindow(HWND h, BOOL invert);

/* USER: messages */
LRESULT SendMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp);
BOOL PostMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void PostQuitMessage(int code);
BOOL GetMessage(LPMSG m, HWND h, UINT first, UINT last);
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002
BOOL PeekMessage(LPMSG m, HWND h, UINT first, UINT last, UINT flags);
BOOL TranslateMessage(const MSG *m);
LRESULT DispatchMessage(const MSG *m);
BOOL WaitMessage(void);
DWORD GetMessagePos(void);
LONG GetMessageTime(void);
LRESULT DefWindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CallWindowProc(WNDPROC p, HWND h, UINT msg, WPARAM wp, LPARAM lp);
UINT RegisterWindowMessage(LPCSTR name);
BOOL InSendMessage(void);
UINT SetTimer(HWND h, UINT id, UINT ms, TIMERPROC fn);
BOOL KillTimer(HWND h, UINT id);
int GetKeyState(int vk);
int GetAsyncKeyState(int vk);
void GetCursorPos(LPPOINT p);
void SetCursorPos(int x, int y);
void ClipCursor(LPCRECT r);         /* screen rectangle the pointer is kept in, NULL = whole screen */
void GetClipCursor(LPRECT r);
HCURSOR SetCursor(HCURSOR c);
int ShowCursor(BOOL show);
void MessageBeep(UINT t);
/* MMSYSTEM: waveform sounds, played by the Linux sound server (mmsystem.c) */
#define SND_SYNC 0x0000
#define SND_ASYNC 0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY 0x0004
#define SND_LOOP 0x0008
#define SND_NOSTOP 0x0010
BOOL sndPlaySound(LPCSTR sound, UINT flags);
UINT waveOutGetNumDevs(void);
BOOL Yield(void);
BOOL GetInputState(void);

/* USER: painting */
HDC BeginPaint(HWND h, LPPAINTSTRUCT ps);
void EndPaint(HWND h, const PAINTSTRUCT *ps);
HDC GetDC(HWND h);
HDC GetWindowDC(HWND h);
int ReleaseDC(HWND h, HDC dc);
void InvalidateRect(HWND h, LPCRECT r, BOOL erase);
void ValidateRect(HWND h, LPCRECT r);
void InvalidateRgn(HWND h, HRGN rgn, BOOL erase);
BOOL GetUpdateRect(HWND h, LPRECT r, BOOL erase);
void ScrollWindow(HWND h, int dx, int dy, LPCRECT scroll, LPCRECT clip);
BOOL ScrollDC(HDC dc, int dx, int dy, LPCRECT scroll, LPCRECT clip, HRGN upd, LPRECT rcupd);
int FillRect(HDC dc, LPCRECT r, HBRUSH b);
int FrameRect(HDC dc, LPCRECT r, HBRUSH b);
void InvertRect(HDC dc, LPCRECT r);
void DrawFocusRect(HDC dc, LPCRECT r);
BOOL DrawIcon(HDC dc, int x, int y, HICON i);
int DrawText(HDC dc, LPCSTR s, int n, LPRECT r, UINT fmt);
BOOL GrayString(HDC dc, HBRUSH b, void *fn, LPARAM data, int n, int x, int y, int cx, int cy);
LONG TabbedTextOut(HDC dc, int x, int y, LPCSTR s, int n, int ntabs, const int *tabs, int origin);
DWORD GetTabbedTextExtent(HDC dc, LPCSTR s, int n, int ntabs, const int *tabs);
COLORREF GetSysColor(int idx);
void SetSysColors(int n, const int *idx, const COLORREF *vals);
int GetSystemMetrics(int idx);
DWORD GetDialogBaseUnits(void);
void SetRect(LPRECT r, int l, int t, int rt, int b);
void SetRectEmpty(LPRECT r);
void CopyRect(LPRECT d, LPCRECT s);
BOOL IsRectEmpty(LPCRECT r);
BOOL PtInRect(LPCRECT r, POINT p);
void OffsetRect(LPRECT r, int dx, int dy);
void InflateRect(LPRECT r, int dx, int dy);
BOOL IntersectRect(LPRECT d, LPCRECT a, LPCRECT b);
BOOL UnionRect(LPRECT d, LPCRECT a, LPCRECT b);
BOOL EqualRect(LPCRECT a, LPCRECT b);
HCURSOR LoadCursor(HINSTANCE h, LPCSTR name);
HICON LoadIcon(HINSTANCE h, LPCSTR name);
/* planes = 1; bpp 1 (mono) or 4 (the standard 16-colour order); rows are WORD aligned as in Win16 */
HICON CreateIcon(HINSTANCE inst, int w, int h, BYTE planes, BYTE bpp, const void *andbits, const void *xorbits);

/* SystemParametersInfo (3.1 subset) */
#define SPI_GETBEEP 0x0001
#define SPI_SETBEEP 0x0002
#define SPI_GETMOUSE 0x0003
#define SPI_SETMOUSE 0x0004
#define SPI_GETBORDER 0x0005
#define SPI_SETBORDER 0x0006
#define SPI_GETKEYBOARDSPEED 0x000A
#define SPI_SETKEYBOARDSPEED 0x000B
#define SPI_ICONHORIZONTALSPACING 0x000D
#define SPI_GETKEYBOARDDELAY 0x0016
#define SPI_SETKEYBOARDDELAY 0x0017
#define SPI_ICONVERTICALSPACING 0x0018
#define SPI_SETDOUBLECLKWIDTH 0x001D
#define SPI_SETDOUBLECLKHEIGHT 0x001E
#define SPI_GETICONTITLELOGFONT 0x001F
#define SPI_SETDOUBLECLICKTIME 0x0020
#define SPI_SETMOUSEBUTTONSWAP 0x0021
#define SPIF_UPDATEINIFILE 0x0001
#define SPIF_SENDWININICHANGE 0x0002
BOOL SystemParametersInfo(UINT action, UINT param, void *pv, UINT winini);
BOOL SetDoubleClickTime(UINT ms);
UINT GetDoubleClickTime(void);
BOOL SwapMouseButton(BOOL swap);
/* GDI Escape (printing escapes are in commdlg.h) */
#define QUERYESCSUPPORT 8
#define MOUSETRAILS 39 /* display driver: mouse trails (not supported: QUERYESCSUPPORT says 0) */
int Escape(HDC dc, int esc, int cb, LPCSTR in, void *out);

typedef struct {
    UINT length, flags, showCmd;
    POINT ptMinPosition, ptMaxPosition;
    RECT rcNormalPosition;
} WINDOWPLACEMENT;
BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *wp);

/* dialog templates built in code (Win16 DIALOG format; use with DialogBoxIndirectParam) */
typedef struct W16DlgTemplate W16DlgTemplate;
W16DlgTemplate *w16_dlgt_new(DWORD style, int x, int y, int cx, int cy, LPCSTR caption, int pt, LPCSTR face);
void w16_dlgt_add(W16DlgTemplate *t, LPCSTR cls, LPCSTR text, int id, DWORD style, int x, int y, int cx, int cy);
const void *w16_dlgt_data(W16DlgTemplate *t);
void w16_dlgt_free(W16DlgTemplate *t);
BOOL DestroyIcon(HICON i);
HICON w16_icon_for_size(HICON i, int w, int h);

/* USER: carets */
BOOL CreateCaret(HWND h, HBITMAP bm, int w, int ht);
void DestroyCaret(void);
void ShowCaret(HWND h);
void HideCaret(HWND h);
void SetCaretPos(int x, int y);
void GetCaretPos(LPPOINT p);
void SetCaretBlinkTime(UINT ms);
UINT GetCaretBlinkTime(void);

/* USER: menus */
HMENU LoadMenu(HINSTANCE h, LPCSTR name);
HMENU LoadMenuIndirect(const void *tmpl);
HMENU CreateMenu(void);
HMENU CreatePopupMenu(void);
BOOL DestroyMenu(HMENU m);
HMENU GetMenu(HWND h);
BOOL SetMenu(HWND h, HMENU m);
HMENU GetSubMenu(HMENU m, int pos);
HMENU GetSystemMenu(HWND h, BOOL revert);
BOOL AppendMenu(HMENU m, UINT flags, UINT_PTR_W16 id, LPCSTR text);
BOOL InsertMenu(HMENU m, UINT pos, UINT flags, UINT_PTR_W16 id, LPCSTR text);
BOOL ModifyMenu(HMENU m, UINT pos, UINT flags, UINT_PTR_W16 id, LPCSTR text);
BOOL DeleteMenu(HMENU m, UINT pos, UINT flags);
BOOL RemoveMenu(HMENU m, UINT pos, UINT flags);
BOOL ChangeMenu(HMENU m, UINT cmd, LPCSTR text, UINT id, UINT flags);
BOOL EnableMenuItem(HMENU m, UINT id, UINT flags);
DWORD CheckMenuItem(HMENU m, UINT id, UINT flags);
BOOL HiliteMenuItem(HWND h, HMENU m, UINT id, UINT flags);
UINT GetMenuState(HMENU m, UINT id, UINT flags);
int GetMenuItemCount(HMENU m);
UINT GetMenuItemID(HMENU m, int pos);
int GetMenuString(HMENU m, UINT id, LPSTR buf, int cb, UINT flags);
void DrawMenuBar(HWND h);
BOOL TrackPopupMenu(HMENU m, UINT flags, int x, int y, int r, HWND h, LPCRECT rc);
DWORD GetMenuCheckMarkDimensions(void);
HACCEL LoadAccelerators(HINSTANCE h, LPCSTR name);
int TranslateAccelerator(HWND h, HACCEL a, LPMSG m);

/* USER: dialogs */
int DialogBox(HINSTANCE h, LPCSTR tmpl, HWND owner, DLGPROC proc);
int DialogBoxParam(HINSTANCE h, LPCSTR tmpl, HWND owner, DLGPROC proc, LPARAM lp);
int DialogBoxIndirectParam(HINSTANCE h, const void *tmpl, HWND owner, DLGPROC proc, LPARAM lp);
HWND CreateDialog(HINSTANCE h, LPCSTR tmpl, HWND owner, DLGPROC proc);
HWND CreateDialogParam(HINSTANCE h, LPCSTR tmpl, HWND owner, DLGPROC proc, LPARAM lp);
HWND CreateDialogIndirectParam(HINSTANCE h, const void *tmpl, HWND owner, DLGPROC proc, LPARAM lp);
void EndDialog(HWND h, int result);
HWND GetDlgItem(HWND h, int id);
int GetDlgCtrlID(HWND h);
UINT GetDlgItemText(HWND h, int id, LPSTR buf, int cb);
void SetDlgItemText(HWND h, int id, LPCSTR s);
UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign);
void SetDlgItemInt(HWND h, int id, UINT v, BOOL sign);
LRESULT SendDlgItemMessage(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp);
void CheckDlgButton(HWND h, int id, UINT check);
void CheckRadioButton(HWND h, int first, int last, int check);
UINT IsDlgButtonChecked(HWND h, int id);
BOOL IsDialogMessage(HWND h, LPMSG m);
HWND GetNextDlgGroupItem(HWND dlg, HWND ctl, BOOL prev);
HWND GetNextDlgTabItem(HWND dlg, HWND ctl, BOOL prev);
void MapDialogRect(HWND h, LPRECT r);
LRESULT DefDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
int MessageBox(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int DlgDirList(HWND dlg, LPSTR path, int idlist, int idstatic, UINT attr);
BOOL DlgDirSelect(HWND dlg, LPSTR buf, int idlist);
int DlgDirListComboBox(HWND dlg, LPSTR path, int idcombo, int idstatic, UINT attr);
BOOL DlgDirSelectComboBox(HWND dlg, LPSTR buf, int idcombo);
#define DDL_READWRITE 0x0000
#define DDL_READONLY 0x0001
#define DDL_HIDDEN 0x0002
#define DDL_SYSTEM 0x0004
#define DDL_DIRECTORY 0x0010
#define DDL_ARCHIVE 0x0020
#define DDL_POSTMSGS 0x2000
#define DDL_DRIVES 0x4000
#define DDL_EXCLUSIVE 0x8000

/* USER: scroll bars */
int SetScrollPos(HWND h, int bar, int pos, BOOL redraw);
int GetScrollPos(HWND h, int bar);
void SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw);
void GetScrollRange(HWND h, int bar, int *mn, int *mx);
void ShowScrollBar(HWND h, int bar, BOOL show);
BOOL EnableScrollBar(HWND h, int bar, UINT flags);

/* USER: strings / resources */
int LoadString(HINSTANCE h, UINT id, LPSTR buf, int cb);
HANDLE FindResource(HINSTANCE h, LPCSTR name, LPCSTR type);
HGLOBAL LoadResource(HINSTANCE h, HANDLE res);
void *LockResource(HGLOBAL h);
BOOL FreeResource(HGLOBAL h);
DWORD SizeofResource(HINSTANCE h, HANDLE res);

/* USER: clipboard */
BOOL OpenClipboard(HWND h);
BOOL CloseClipboard(void);
BOOL EmptyClipboard(void);
HANDLE SetClipboardData(UINT fmt, HANDLE data);
HANDLE GetClipboardData(UINT fmt);
BOOL IsClipboardFormatAvailable(UINT fmt);
UINT EnumClipboardFormats(UINT fmt);
int CountClipboardFormats(void);
HWND SetClipboardViewer(HWND h);
BOOL ChangeClipboardChain(HWND h, HWND next);

/* USER: help */
BOOL WinHelp(HWND h, LPCSTR file, UINT cmd, DWORD data);

/* GDI */
HGDIOBJ GetStockObject(int i);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
BOOL DeleteObject(HGDIOBJ o);
int GetObject(HGDIOBJ o, int cb, void *out);
HPEN CreatePen(int style, int width, COLORREF c);
HBRUSH CreateSolidBrush(COLORREF c);
HBRUSH CreateHatchBrush(int style, COLORREF c);
HBRUSH CreatePatternBrush(HBITMAP bm);
HBRUSH CreateBrushIndirect(const LOGBRUSH *lb);
HFONT CreateFont(int h, int w, int esc, int orient, int weight, BYTE italic, BYTE underline,
                 BYTE strike, BYTE charset, BYTE outprec, BYTE clipprec, BYTE quality,
                 BYTE pitchfam, LPCSTR face);
HFONT CreateFontIndirect(const LOGFONT *lf);
HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits);
HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h);
HBITMAP CreateDiscardableBitmap(HDC dc, int w, int h);
HBITMAP LoadBitmap(HINSTANCE h, LPCSTR name);
HDC CreateCompatibleDC(HDC dc);
HDC CreateDC(LPCSTR driver, LPCSTR device, LPCSTR port, const void *init);
HDC CreateIC(LPCSTR driver, LPCSTR device, LPCSTR port, const void *init);
BOOL DeleteDC(HDC dc);
int SaveDC(HDC dc);
BOOL RestoreDC(HDC dc, int n);
COLORREF SetTextColor(HDC dc, COLORREF c);
COLORREF GetTextColor(HDC dc);
COLORREF SetBkColor(HDC dc, COLORREF c);
COLORREF GetBkColor(HDC dc);
int SetBkMode(HDC dc, int m);
int GetBkMode(HDC dc);
int SetROP2(HDC dc, int r);
UINT SetTextAlign(HDC dc, UINT a);
int SetMapMode(HDC dc, int m);
int GetMapMode(HDC dc);
DWORD SetWindowOrg(HDC dc, int x, int y);
DWORD SetViewportOrg(HDC dc, int x, int y);
DWORD SetWindowExt(HDC dc, int x, int y);
DWORD SetViewportExt(HDC dc, int x, int y);
DWORD GetWindowOrg(HDC dc);
DWORD GetViewportOrg(HDC dc);
BOOL DPtoLP(HDC dc, LPPOINT p, int n);
BOOL LPtoDP(HDC dc, LPPOINT p, int n);
int GetDeviceCaps(HDC dc, int idx);
DWORD MoveTo(HDC dc, int x, int y);
BOOL LineTo(HDC dc, int x, int y);
BOOL Rectangle(HDC dc, int l, int t, int r, int b);
BOOL RoundRect(HDC dc, int l, int t, int r, int b, int w, int h);
BOOL Ellipse(HDC dc, int l, int t, int r, int b);
BOOL Polygon(HDC dc, const POINT *p, int n);
COLORREF GetNearestColor(HDC dc, COLORREF c);
int MulDiv(int a, int b, int c);
BOOL Polyline(HDC dc, const POINT *p, int n);
BOOL Arc(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2);
BOOL Pie(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2);
BOOL Chord(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2);
COLORREF SetPixel(HDC dc, int x, int y, COLORREF c);
COLORREF GetPixel(HDC dc, int x, int y);
BOOL FloodFill(HDC dc, int x, int y, COLORREF c);
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop);
BOOL BitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
BOOL StretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop);
BOOL TextOut(HDC dc, int x, int y, LPCSTR s, int n);
BOOL ExtTextOut(HDC dc, int x, int y, UINT opt, LPCRECT r, LPCSTR s, UINT n, const int *dx);
DWORD GetTextExtent(HDC dc, LPCSTR s, int n);
BOOL GetTextExtentPoint(HDC dc, LPCSTR s, int n, LPSIZE sz);
BOOL GetTextMetrics(HDC dc, LPTEXTMETRIC tm);
int GetTextFace(HDC dc, int cb, LPSTR buf);
BOOL GetCharWidth(HDC dc, UINT first, UINT last, int *out);
int SetTextCharacterExtra(HDC dc, int extra);
int IntersectClipRect(HDC dc, int l, int t, int r, int b);
int ExcludeClipRect(HDC dc, int l, int t, int r, int b);
int SelectClipRgn(HDC dc, HRGN r);
int GetClipBox(HDC dc, LPRECT r);
BOOL PtVisible(HDC dc, int x, int y);
BOOL RectVisible(HDC dc, LPCRECT r);
HRGN CreateRectRgn(int l, int t, int r, int b);
HRGN CreateRectRgnIndirect(LPCRECT r);
/* CombineRgn modes; regions are rectangle lists (region.c) */
#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5
int CombineRgn(HRGN dst, HRGN a, HRGN b, int mode);
BOOL PtInRegion(HRGN r, int x, int y);
BOOL FillRgn(HDC dc, HRGN r, HBRUSH b);
DWORD SetBrushOrg(HDC dc, int x, int y);
BOOL UnrealizeObject(HGDIOBJ o);
int AddFontResource(LPCSTR file);
#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#define ERROR 0

/* SHELL */
UINT DragQueryFile(HANDLE drop, UINT i, LPSTR buf, UINT cb);
void DragFinish(HANDLE drop);
HINSTANCE ShellExecute(HWND h, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show);

/* ------------------------------------------------------------------ libw16 specifics */
/* where ripped assets live (default ~/.local/share/arch311, env ARCH311_ASSETS) */
const char *w16_assets_dir(void);
/* exit with a 3.11-style message if the user has not ripped their media yet */
void w16_require_assets(void);
/* run screen: W16_DESKTOP=1 shows a 640x480 3.11 desktop around the app (used for
 * pixel comparisons with real 3.11); otherwise top-level windows are native windows. */
/* save a PNG of the virtual screen (used by tests) */
int w16_screenshot(const char *path);

#ifdef __cplusplus
}
#endif
#endif
