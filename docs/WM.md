# Window manager and desktop (plan)

The owner's requirements (October 2026): native Linux applications get the same 3.11 window
borders and controls as the ported programs, with the same behaviour: minimising turns a window into
an icon on the desktop, maximising fills the screen, a window closes from its system menu, by
double-clicking the system-menu box, or with Alt+F4, and scroll bars look the same. All of it follows
the theme (docs/THEMES.md), so Windows 95, 98, XP, 7, 1 and 2 can replace 3.11 later.

Nothing here is built yet. Testing needs an X server (`xvfb`, `xdotool`, `xterm`, `x11-apps` in WSL).

## Today

Each libw16 program draws a whole virtual screen (640x480 by default, `W16_SCREEN`) with its own
desktop, and shows it in one SDL window. That is right for pixel comparisons with real 3.11, but an
operating system needs one screen shared by every program, ported or native.

## Architecture

- **Display server: X11 (Xorg).** A reparenting window manager is a few thousand lines of C against
  Xlib; a Wayland compositor would mean wlroots and a much larger code base. Wayland can follow
  later behind the same theme code.
- **`arch311-wm`**, a C program linked with libw16's theme code:
  - Native clients (GTK, Qt, xterm, the CEF browser shells) are reparented into a frame window. The
    frame (border, caption, system-menu box, minimise and maximise buttons, sizing border) is drawn
    by the same routines that draw a libw16 window's non-client area, into an off-screen bitmap that
    is copied to the frame with XPutImage. Hit-testing is libw16's too (HTCAPTION, HTSYSMENU, ...).
  - Behaviour follows USER: moving and sizing track the XOR rectangle as 3.11 does; the system menu
    (Restore, Move, Size, Minimize, Maximize, Close Alt+F4, Switch To... Ctrl+Esc) is drawn with the
    3.11 menu code; double-clicking the system-menu box or Alt+F4 closes the window
    (WM_DELETE_WINDOW, else XKillClient after the 3.11 "not responding" wait); double-clicking the
    caption maximises or restores.
  - Minimised windows become icons on the desktop - the application's icon (_NET_WM_ICON, or the
    3.11 default) with its title underneath in the 3.11 icon-title font, laid out by
    [desktop] IconSpacing / IconTitleWrap like Program Manager arranges icons. Double-clicking one
    restores the window.
  - Maximise fills the screen with the frame's borders outside it, as 3.11 does.
  - Activation: the active caption colours, click-to-activate, Alt+Tab and Alt+Esc as 3.11 switches,
    Ctrl+Esc opens the Task List (a port of TASKMAN.EXE).
  - The desktop (root window) shows WIN.INI [Desktop] Wallpaper / TileWallpaper / Pattern, drawn by
    libw16's desktop code.
- **libw16 programs** get one X window per top-level window instead of one virtual screen:
  - libw16 keeps a framebuffer the size of the X screen and draws its windows there in X screen
    coordinates (its window manager code already works in screen coordinates); each top-level window
    has an undecorated X window (_MOTIF_WM_HINTS) showing its rectangle of that framebuffer. libw16
    keeps drawing its own frames, so there is a single implementation of the 3.11 frame.
  - Move and size tracking in nc.c moves the X window; minimise asks the window manager to iconify
    (WM_CHANGE_STATE), and the desktop icon is the window manager's, as for native clients.
  - `W16_SCREEN` / the single-window mode stays for tests and pixel comparisons.
- **Scroll bars and controls inside native applications**: a GTK 3/4 theme (CSS and images generated
  from the theme's measurements) and a Qt style plugin (THEMES.md layer 4).
- **Cross-program window lists** (FindWindow, EnumWindows, DDE between ported programs, the Task List)
  use EWMH (_NET_CLIENT_LIST, WM_CLASS) to see other programs' windows.

## Milestones

1. `arch311-wm`: reparent, 3.11 frame drawn by libw16's theme code, move/size, activation, close
   (system menu double-click, Alt+F4). Tests: Xvfb + xterm + xdotool, screenshots compared with the
   same window drawn by libw16 in single-screen mode (identical frame pixels).
2. System menu, minimise to desktop icons, maximise/restore, Arrange Icons; desktop wallpaper/pattern.
3. libw16 per-window presentation on X11; ported programs managed by `arch311-wm`.
4. Task List (TASKMAN.EXE port), Alt+Tab / Alt+Esc / Ctrl+Esc, Program Manager as the shell, the
   MS-DOS Prompt item starting the built-in DOSBox and Terminal as the Linux terminal (ADR-006).
5. GTK and Qt styles.
6. Switching themes at run time.
