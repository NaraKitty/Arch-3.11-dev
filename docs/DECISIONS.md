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
