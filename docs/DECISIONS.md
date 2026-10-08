# Decisions (ADRs)

Append new decisions at the end; never rewrite an old one - supersede it with a new entry. The
earlier session notes referred to this file before it existed; ADR-001 to ADR-005 record the
decisions those sessions and the owner made (see AGENTS.md logs), with their dates.

## ADR-001 (session 2/3, owner) - Native ports, no 16-bit emulation
3.11 programs are decompiled (disassembled and understood) and rewritten as native 64-bit C against
libw16, a Win16 API implemented on SDL2. No 16-bit code is ever executed natively; emulators such as
DOSBox-X are only used to run real 3.11 as the reference for comparisons. Supersedes the session-1
plan of a 16-bit CPU engine (T-RT-02).

## ADR-002 (session 1, owner) - The user rips their own assets
Nothing from Microsoft or Netscape is distributed: the user rips their own installation media
(`tools/rip`) into `~/.local/share/arch311`, and programs load menus, dialogs, strings, icons, fonts
and sounds from there at run time. Disassembly and decode notes stay outside the repository.

## ADR-003 (session 2, owner) - Browsers are CEF shells
Internet Explorer and Netscape Navigator are rebuilt as shells around Chromium (CEF, prebuilt
minimal distribution); their user interfaces come from the user's ripped installs, the original
page engines are never used. The CEF download waits for the owner's approval.

## ADR-004 (session 5, owner) - The look is a theme
Windows 3.11 is the first theme; Windows 1.0/2.x, 95, 98, XP and 7 follow. Look-specific code sits
behind a theme layer (docs/THEMES.md), and programs switch with the theme.

## ADR-005 (session 6) - X11 reparenting window manager; libw16 draws its own frames
Native Linux applications are framed by `arch311-wm`, an X11 reparenting window manager that draws
3.11 frames with libw16's theme code and implements USER's window behaviour (docs/WM.md). Ported
programs keep drawing their own frames in undecorated X windows, so the 3.11 frame has one
implementation. X11 over Wayland because a reparenting window manager is far smaller than a
compositor; Wayland can follow behind the same theme code.

## ADR-006 (session 7, owner) - MS-DOS Prompt is a DOSBox; Terminal is the Linux terminal
Supersedes the earlier plan that the MS-DOS Prompt item opens the Linux terminal emulator. The MS-DOS
Prompt starts arch311's built-in DOS emulator (DOSBox, a packaged dependency - DOSBox-X like the
reference rig unless a later ADR picks another), so DOS programs and games run emulated, never natively
(hard rule 3 still holds: no guest code runs on the host CPU). The Linux terminal is a port of 3.11's
Terminal (TERMINAL.EXE): its windows, menus, dialogs and VT-100/VT-52/TTY emulation are kept and look as
in 3.11, but it talks to a shell on a pseudo-terminal instead of a modem; dial-up features (phone
number, modem commands) have no modem to drive. Windows programs stay native ports (ADR-001).

## ADR-007 (session 7, owner) - Media players are shells over libVLC
The owner will supply a Windows 3.11 MP3 player and an MP4 (video) player. Like the browsers (ADR-003),
they become separate arch311 programs whose windows, menus, dialogs and behaviour are decompiled and
reproduced 1:1 from the originals (enough of the original code is decoded to make them accurate), while
decoding and playback use libVLC (VideoLAN's VLC engine, a packaged dependency) instead of the
originals' code. The same engine may later stand behind MCI for 3.11's own Media Player and Sound
Recorder. Their assets come from the user's own copies, as with every program.
Addendum (owner): the players open everything libVLC can read and play, not only MP3/MP4.

## ADR-008 (session 7, owner) - Program Manager groups for Linux apps; preferred applications
- Krita is installed as an item in Accessories next to Paintbrush, with a 3.11-style icon (drawn in
  the theme's style, never a copied Microsoft icon).
- A new "Media" group holds the MP3 player and the video player (ADR-007).
- Files open with a preferred application, as on a modern OS: 3.1's own association mechanism
  (WIN.INI [Extensions], File Manager's Associate dialog, ShellExecute/FindExecutable) is kept and
  extended so that the media players are the default for the formats they play, and Linux defaults
  (xdg-mime / mimeapps.list) and 3.1's [Extensions] agree in both directions.

## ADR-009 (session 7) - Long Linux names reach 3.1 programs as 8.3 aliases
Ported 3.1 code (COMMDLG's ParseFile, File Manager, every program's own checks) refuses names that are
not 8.3, but Linux folders hold long names. Rather than relaxing each ported check (inaccurate), the
libw16 DOS path layer (sys.c) gives every long or non-8.3 name a stable 8.3 alias the way VFAT does
(NAME~1.EXT, upper case, unique per directory) and maps it back on open/create/rename; File Manager
also shows the long names through WfW's own long-file-name layer (seg19) where 3.11 would.
