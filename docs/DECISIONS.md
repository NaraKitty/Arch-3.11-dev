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

Addendum to ADR-003 (session 7, owner): Chromium stays updatable for security fixes. The CEF build the
browsers use is named in one place (a version file with its URL and SHA-256/SHA-1 checksum, read by the
build). An owner-run update script fetches a newer CEF release, verifies its published checksum, and
rebuilds both browsers. The browser shells use only CEF's stable public API, so a CEF update needs no
port change, and they never patch Chromium. When a CEF update breaks the shell, that is a bug to fix
in the shell, not a reason to pin an old Chromium.

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

Addendum to ADR-007 (session 7, owner): the owner supplied the two players. WinPlay3 v2.3b5 (Fraunhofer
IIS, the installer wp3v23b5.exe) is the MP3 player; XingMPEG Player 1.3 (xing_31.zip) is the video
player. Both are installed from the user's copies in the reference rig and reproduced as libVLC shells.
XingMPEG's licence text forbids reverse engineering, so its UI is reproduced from observation of the
running program (screenshots, dialogs and menus as resources) unless the owner decides otherwise.

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

## ADR-010 (session 7, owner) - Modern dates; the clock syncs over the internet
- Dates work for today's and future computer dates: the ported date code (Calendar, the Date/Time
  applet, File Manager, Clock, DOS file times through libw16) uses 64-bit host time. Wherever 3.1
  stops at a year (for example Calendar's 1980-2099 range), the range is widened to what the host
  time supports, and the rest of the behaviour stays as in 3.1. These widened limits are the only
  intended deviations; each is marked in code with the 3.1 limit it replaces.
- The system clock is set from internet time (NTP, through systemd-timesyncd) by default. The
  Control Panel Date/Time dialog gets a control to turn that sync on or off, drawn in the theme's
  style. This is the "internet settings in Control Panel" rule. The sync is a system service: no
  legacy program gets network access through it (hard rule 4).

## ADR-011 (session 7, owner) - Calmira II is an optional Windows 95-style shell
Calmira II (GPL v2 or later, Delphi 1 source supplied by the owner as calsrc.zip) becomes an optional
shell for users who find 3.11's Program Manager layout hard. Its start menu, taskbar, desktop icons and
explorer give a 95-like layout on top of the 3.11 theme. It never starts automatically: the user
launches it from Program Manager, and it is not the default shell. It is ported to C on libw16 like
the other programs, but because its source is GPL, the port and its own art live in their own
directory under GPL v2+, with the original copyright and licence notices kept. Its source text
(comments, strings, identifiers) is English; anything that is not English is translated when ported.
Its bitmaps are Calmira's own. Any bitmap that copies Windows 95 art is not committed and is redrawn
in the theme's style instead.

## ADR-012 (session 7, owner) - Power on straight into the 3.11 desktop
arch311 boots like a modern OS: there is no DOS prompt, and nobody types `C:`, `cd windows`, `win`.
The machine boots Linux and goes straight to the 3.11 desktop. Any Linux boot text stays hidden
behind a quiet boot. The session starts X with arch311-wm and Program Manager as the shell (the
SYSTEM.INI [boot] shell= program, as on 3.1, so a user can choose File Manager or Calmira, ADR-011).
A single-user install logs in automatically, like 3.11 with no network logon. With several accounts
or a password, a logon dialog in 3.11's look (WfW's Logon dialog) asks first. Exit Windows ends the
session and powers the machine off or restarts it. 3.1's "exit to DOS" has no DOS to return to. The
only splash that may be shown is the one real 3.11 shows (the Windows logo screen from the user's
own ripped files), for as long as start-up takes, never longer (hard rule 5). Typing `win` stays
possible from the MS-DOS Prompt's DOSBox only as the emulated DOS world's own business (ADR-006).
