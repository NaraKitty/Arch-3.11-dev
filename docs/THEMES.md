# Themes

arch311 is a modern Linux desktop whose whole look and behaviour is a **theme** modelled on one
version of Windows. Windows for Workgroups 3.11 is the first theme (`win311`). Planned later:
Windows 1.0 and 2.x, 95, 98, XP and 7. The user picks a theme; ported programs, the window manager,
native Linux applications and the system sounds all follow it.

This document is the plan. Only `win311` exists, and its parts are still spread through libw16
(see "Where 3.11 lives today").

## What a theme is

| Part | 3.11 theme | Later themes |
|---|---|---|
| Metrics | SM_* values, frame/caption/menu sizes measured on real 3.11 VGA | measured on each version |
| Drawing | frames, captions, caption buttons, menus, push/check/radio buttons, group boxes, scroll bars, edits, list/combo boxes, dialog frames, message boxes | 3D look (95+), Luna (XP), Aero Basic (7) |
| Colours | VGA 16-colour DAC, WIN.INI [colors] | each version's schemes |
| Fonts | System, MS Sans Serif (dialog fonts emboldened), Fixedsys | MS Sans Serif / Tahoma / Segoe UI |
| Assets | ripped by the user from their own install (`~/.local/share/arch311/files`, `res`) | one asset set per theme, ripped from that version |
| Programs | ports of the 3.11 programs | ports of that version's programs, or the 3.11 port with the theme's resources where the program barely changed |
| Shell | Program Manager, File Manager, Task List | Explorer + taskbar (95+), Start menu |
| Window manager | 3.11 frames; minimise to a desktop icon; maximise; system menu; Alt+Tab/Ctrl+Esc as 3.11 | taskbar buttons, title-bar buttons of that version |
| Linux toolkits | GTK and Qt styles drawn to match (scroll bars, buttons, menus) | per theme |
| Cursors, sounds, start-up/shut-down screens | from the theme's assets | per theme |

Each theme must stay faithful to its version. Modern features with no home in the original get
new pieces drawn in the theme's style, such as the Volume, Network and Internet applets.

## Layers

1. **libw16 theme interface** (`libw16/src/theme*.c`, planned): a `W16Theme` table with the system
   metrics, default colours and fonts, plus the drawing and layout entry points the controls and the
   non-client code call:
   - frame insets and frame drawing; caption, caption buttons and their hit-testing;
   - menu bar and popup metrics and drawing;
   - push/check/radio/group box layout (BNDrawText geometry) and drawing; focus rectangle;
   - scroll bar arrows, thumb and track;
   - text extent rules (bold overhang);
   - dialog placement and frame;
   - message-box layout and icons.
   `theme_win311.c` holds the measured 3.11 code that is in nc.c, button.c, scroll.c, menu.c,
   dialog.c and font.c today. The theme is chosen at start-up from `~/.config/arch311/theme`
   (default `win311`) or `W16_THEME`.
2. **Program variants**: each ported program keeps its logic in one place. Version-specific
   behaviour sits behind the theme name. Resources are loaded at run time from the current theme's
   ripped assets.
3. **Window manager** (planned): an X11/Wayland window manager that draws decorations for native
   Linux windows through the same libw16 theme code, so an xterm or Firefox gets the same frame,
   caption, system-menu box and min/max buttons as a ported 3.11 program, and the same behaviour:
   - double-click on the system-menu box, or Alt+F4, closes the window;
   - minimise turns the window into an icon on the desktop with its title below;
   - maximise fills the screen;
   - the system menu offers Restore, Move, Size, Minimise, Maximise, Close and Switch To.
4. **Toolkit styles**: a GTK theme (CSS + assets generated from the theme's measurements) and a Qt
   style, so scroll bars, buttons, check boxes and menus inside Linux applications match.
5. **Shell and system**: the start-up screen, Program Manager (3.11) or Explorer (95+), sounds.
   The MS-DOS Prompt item starts the built-in DOSBox; Terminal is the Linux terminal in 3.11's
   Terminal look (ADR-006).

## Light / dark preference (owner, October 2026)
Once the theme manager exists, is accurate and the other themes' colours are imported, it gets a
Light / Dark toggle. The toggle does not change any arch311 theme's look: it only sets the desktop's
colour-scheme preference that Linux applications read to pick their own light or dark appearance
(the freedesktop `org.freedesktop.appearance color-scheme` setting served by xdg-desktop-portal, the
GNOME `color-scheme` key GTK reads, and the matching Qt/KDE setting), so apps that auto-detect follow
the user's choice.

## Browsers

Internet Explorer and Netscape Navigator are theme programs: their frames, toolbars, menus,
dialogs and throbbers come from the user's ripped installs of those browsers, and the page engine
is Chromium through CEF (prebuilt). The original engines are never used.

## Where 3.11 lives today

- `libw16/src/nc.c`: frames (thick, dialog, border), captions, caption buttons, scroll bars.
- `libw16/src/button.c`: USER BNDrawText geometry, push/check/radio/group drawing.
- `libw16/src/gdi.c`: VGA palette and DAC, DrawFocusRect, dithering.
- `libw16/src/menu.c`, `dialog.c`, `font.c`: menus, dialog units and placement, text extents.
- `libw16/src/sys.c`: system metrics and WIN.INI defaults.

The refactor moves these behind the theme table without changing a pixel. The Notepad, Keyboard
and Mouse comparisons against real 3.11 (`tools/ref311`) are the regression tests.
