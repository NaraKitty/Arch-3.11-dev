# AGENTS.md - handoff notes (read first, update the log last)

## Mission (from the repo owner)
Build an Arch Linux based system themed as Windows 3.11 that also runs Windows 3.11 programs.
IE5 and Netscape Navigator become Chromium-backed apps treated as themes. Every 16-bit app should end
up running as secure 64-bit Linux-side code. Keep the 3.11 look, style, feel and animations accurate,
intended for original-era resolutions, with modern resolutions supported. Leave notes so other Claude
agents can continue.

## Hard rules
1. **Never commit Microsoft/Netscape material or anything derived from their code** (media, extracted
   binaries, decompiled/disassembled output, copied bitmaps/icons/fonts/sounds). Run `git status` before
   every commit. Policy: `docs/LEGAL.md`.
2. **Only claim what you ran.** Mark everything else `UNTESTED`. Put results (commands + outcome) in the log.
3. **Host is 64-bit only.** Never execute guest 16-bit code natively.
4. **Secure by default.** Legacy apps sandboxed, no network unless granted.
5. **Accuracy over invention.** Derive theme values from the media or observation of real 3.11; don't
   add animations/effects the original lacked.
6. Append to ADRs in `docs/DECISIONS.md`; do not silently change decisions.

## Current state
**Latest (session 3): Notepad runs end to end on Linux** (`make -C apps`, then
`tools/run-app-test.sh apps/build/notepad apps/notepad/tests/smoke.w16`). See the session 3 log below.
The session-1 notes that follow are historical; `docs/`, `tools/extract_media.py`, `make-profile.sh`
etc. are referenced but were never committed - `tools/rip` replaced the extraction tooling.

Session 1, DONE and verified:
- `tools/extract_media.py` + `tools/kwajexpand.c` expanded all 462 files from the owner's six floppy
  images (Windows for Workgroups 3.11); none left compressed.
- `tools/inventory.py` -> `docs/INVENTORY.md`: 39 NE EXEs, 207 NE DLL/drivers, 29 LE VxDs, 8 DOS MZ.
- NOTEPAD.EXE confirmed as a 16-bit NE binary after expansion.

DESIGN ONLY (UNTESTED, no code): ISO profile, theme, Win16 runtime, browsers, sandboxing.
NOTHING BOOTS YET.

## Backlog (pick the lowest open ID in your area; split tasks as needed)
| ID | Task | Notes |
|---|---|---|
| T-BUILD-01 | Build ISO from `tools/make-profile.sh`, boot in QEMU, log result | Needs real Arch/CI; the Claude chat sandbox can't reach Arch mirrors |
| T-BUILD-02 | CI workflow building the ISO in an `archlinux` container | |
| T-THEME-01 | Confirm CONTROL.SRC scheme field order; get Windows Default colours from a running 3.11 | See `theme/SPEC.md` |
| T-THEME-02 | `win31` Qt style plugin: bevels, title bar, scrollbars, buttons at 640x480 metrics | |
| T-THEME-03 | Program Manager style shell + group windows | |
| T-RT-01 | NE loader (header, segments, resources, entry table, relocations); test on NOTEPAD/CALC/WINMINE | Start here for the runtime |
| T-RT-02 | Spike: 16-bit protected-mode CPU engine (Unicorn vs own interpreter); document findings in ADR-001 | |
| T-RT-03 | Ordinal -> name tables from NE name tables of KERNEL/USER/GDI etc. | Interface facts only |
| T-RT-04 | KERNEL/USER/GDI shim enough to open Notepad | First end-to-end milestone |
| T-SB-01 | bwrap + seccomp + Landlock launcher, virtual C:\\ | |
| T-BR-01 | QtWebEngine browser with IE5 chrome; T-BR-02 Netscape chrome | ADR-003 |
| T-APP-nn | Optional clean-room native ports (Notepad, Calc, Clock, Winmine, Sol) | Respect `docs/LEGAL.md` |

## Known quirks and findings
- The media is **Windows for Workgroups 3.11** (NetWare/WINPOPUP files present), not plain 3.11.
- Install files use **KWAJ** compression (not SZDD). `libmspack` decodes it. `extract_media.py` also
  handles SZDD for other media.
- `WINVER._` expands under the name `WINVER.` (libmspack returned no extension); probably WINVER.EXE.
- `MMTASK.TSK`, `*.SCR`, `*.MOD` are NE EXEs too, so the "39 EXEs" figure includes them.
- IE5 ISO (`MSIE5`, x86 + Alpha) and Netscape 4.04 ISO (`Nav404`) are **Win32** installers: no 16-bit
  code; visual reference only. Neither has been extracted yet.
- Default colours are not in `CONTROL.SRC`; see `theme/SPEC.md`.
- Tooling available in the chat sandbox: Python, gcc, apt (Ubuntu), pip. No QEMU/Wine/Arch mirrors.

## Open questions for the owner
1. Public repo later? That decides how strict the asset policy must be (redrawn assets vs local import).
2. QtWebEngine (ADR-003) or literal Chromium-in-app-mode?
3. Is the DOS-box / VxD (386 enhanced mode) side in scope for v1?

## Handoff log (append only; newest last)
### Session 1
- Created repo scaffold, docs, tools. Ran extraction + inventory on the owner's media (results above).
- Not done: everything in the backlog. Suggested next step: T-RT-01 (pure code, testable here) or
  T-BUILD-01 (needs a real Arch machine).

### Session 2 (Claude, Oct 7 2026) - direction changed by the owner
Owner decisions: **native ports, not emulation** (decompile each 16-bit app and port it to 64-bit C);
C + SDL2; Chromium via **CEF prebuilt** for the IE/Netscape shells (1:1 UI, no original engine code);
true colour by default with the 16-colour VGA look as the default scheme; Linux apps and Wine apps get
3.11 frames from a 3.11-style window manager sharing libw16's frame code; Display Settings applet
(resolution + scaling separately); multi-monitor; PipeWire sound with the 3.11 sound events;
File Manager must refuse to delete system-critical paths. Supersedes ADR-001/003 (emulator, QtWebEngine).
Done (verified here):
- tools/rip: pure-Python FAT12 + KWAJ(LZH)/SZDD expander + NE resource decoder. All 463 files of the
  owner's 6 disks expand (NOTEPAD.EXE = 32,736 bytes, correct); 246 NE modules ripped, 0 errors.
- tools/rip/arch311rip/disasm.py: capstone-based NE disassembler resolving relocations to API names.
- Reference rig (not in repo, see below): DOSBox-X + unattended 3.11 Setup (SETUP /H with a .SHH);
  xdotool drives real 3.11 and screenshots are compared with the ports.
- libw16 (C/SDL2): USER/GDI/KERNEL subset - windows, NC frames/captions/buttons, menus (bar, popup,
  system menu, keyboard), dialogs (template loader, bold dialog font, base units), MessageBox,
  BUTTON/STATIC/EDIT/LISTBOX/COMBOBOX/SCROLLBAR, raster fonts from the ripped .FON files, regions,
  carets, timers, clipboard bridged to Linux (cp1252<->UTF-8), INI files in ~/.config/arch311.
  `make -C libw16` compiles (warnings only). NOT yet run end-to-end.
- apps/notepad/notepad.c: complete port of Notepad's logic from the disassembly (init, WndProc, all
  menu commands, load/save, find, word wrap recreate, page setup, time/date with intl, .LOG).
Measured 3.11 facts (keep): thick frame 4 px (black/2 gray/black) with notches at 22 px; caption 20
incl. borders; caption buttons are 19x18 OBM bitmaps; menu bar item = 8+text+8, 18 px high + 1 line;
popup item 18 px, text at x=16, accel column = 16+maxtext+8, right margin 13, separator 7 px, 1-px
light-gray shadow; dialog frame black+4 caption-colour; dialogs white; GRAYTEXT=C0C0C0; inversion is by
VGA palette index; Notepad uses Fixedsys (15 px lines), edit at client (8,2), no edit margins,
2-px caret, tab stops 64 px; main window owns the scroll bars (0-100 thumb via EM_GETTHUMB).
NEXT (in order): commdlg.c (GetOpen/SaveFileName from COMMDLG.DLL templates 1536/1537 with owner-drawn
folder/drive lists, FindText 1540, PrintDlg 1538/1539 via CUPS, ShellAbout from SHELL.DLL 100),
printing (NpPrintFile: port seg1:1146/1E12 header/footer codes &f &p &d &t &c &l &r), apps/Makefile,
run Notepad under W16_HEADLESS + W16_SCRIPT and pixel-diff against the DOSBox-X references
(tools/compare, palette-index compare), then Calculator, Clock, Write, Paintbrush, ...
Push: this session could not write to GitHub (repos not in its authorized set); the owner pushes a
git bundle with "Arch-3.11-dev push/push-to-github.bat" in the disks folder.

### Session 3 (Claude Code on the owner's PC, Oct 7 2026)
Build host: WSL Ubuntu 22.04 on the owner's Windows PC (gcc 11, libsdl2-dev 2.0.20). The session-2
cloud sandbox's DOSBox-X rig scripts were never committed and are lost; DOSBox-X is installed on the
owner's PC at C:\DOSBox-X for the next comparison pass.
Done and verified here (commands: `make -C libw16`, `make -C apps`, the smoke script above, headless):
- libw16 builds and links on Linux. Fixed: strcasestr without _GNU_SOURCE (64-bit pointer truncation),
  menu loop never called TranslateMessage (menu mnemonics dead for real keyboards too), key state
  updated before queued key messages ran (Ctrl+Home etc. seen as Home), dialogs focusing a control
  WM_INITDIALOG disabled, disabled push-button text invisible (now stippled as 3.1 does when
  GRAYTEXT == BTNFACE), combo boxes drawing their template WS_VSCROLL as a scroll bar, parent paint
  erasing a child that UpdateWindow had already validated (Notepad's text vanished after File>Open).
- New: libw16/src/commdlg.c (GetOpen/SaveFileName, FindText/ReplaceText, PrintDlg + Print Setup with
  CUPS printers via lpstat, GetFileTitle, CommDlgExtendedError) on the ripped COMMDLG.DLL templates
  1536-1541, strings and folder/drive bitmap 576; libw16/src/shell.c (ShellAbout on SHELL.DLL 100);
  per-drive current directory (w16_chdir/w16_getcwd/w16_drive_root); GDI Escape stub.
- New: apps/notepad/print.c, ported from seg1:0FC6-21B6 (AbortProc, abort dialog, margins->device
  units, &f &p[+n] &t &d &l &c &r headers/footers, tab expansion, page loop); apps/Makefile.
- Smoke test passes: typing, menus, Find (match highlighted), Save As writes ~/hello.txt with CRLF and
  the tab byte intact, Open lists and reloads it, Print Setup, About.
- Decision: names that do not exist yet are created lower case on Linux (DOS apps pass upper case);
  existing files still match case-insensitively.
UNTESTED: real printing (printer DCs are info-only - T-PRN-01; no CUPS in WSL), the Help buttons,
Replace dialog, OFN hooks/templates, layout constants marked MEASURE in commdlg.c, ShellAbout texts
(101/112/115) and the stipple phase - all need the DOSBox-X comparison.
Known: Print Setup opened via Alt+F,R shows focus on "Portrait" (likely the 'r' mnemonic reaching the
dialog) - check against real 3.11.
NEXT: rebuild the DOSBox-X reference rig on the owner's PC and pixel-compare Notepad + the common
dialogs; then T-PRN-01 (printer DC -> PDF -> CUPS); then Calculator, Clock.

### Session 4 (Claude Code on the owner's PC, Oct 7 2026)
Reference rig (tools/ref311, committed): WfW 3.11 installs unattended in DOSBox-X from the owner's
floppies (ARCH311.SHH, ~1 min) into %USERPROFILE%\arch311-ref\c-pristine; ref-run.ps1 copies it,
sets the app as SYSTEM.INI shell=, types AUTOTYPE keys and records every stable screen through
PrintWindow (no focus needed); compare.py diffs/measures; scenario.py drives both sides from one file.
Known rig limits: long AUTOTYPE lines were unreliable (an 845-char scenario typed nothing; probe
results inconsistent); posting WM_KEYDOWN to the DOSBox-X window does not reach the guest. Short
scenarios work. Next agent: split scenarios or find a reliable host->guest key path.
Accuracy work, each measured against real 3.11 screenshots (Notepad main window now differs only by
the mouse cursor, which VGA draws into the framebuffer):
- VGA monitor colours from VGA.DRV seg4:00F0 (attribute table + DAC 7/15), applied on output only.
- CW_USEDEFAULT placement ported from USER seg6:01B4 (+ CreateWindow's overlapped-window part).
- WM_SIZE/WM_MOVE deferred to the first ShowWindow for hidden windows (WFSENDSIZEMOVE).
- Menu bar spans the scroll-bar column; CS_BYTEALIGNCLIENT windows round item widths to bytes.
- Borderless multiline edits have no format inset; launch dir becomes the DOS current directory.
Control Panel: apps/control/control.c is a port of CONTROL.EXE (all of seg1: module loading through
the CPlApplet protocol, the owner-drawn "lb" icon list, "Text" status line, Settings menu, F1/About,
CONTROL.INI [MMCPL]/[don't load], "control NAME"). Applets are native modules (cplreg.c). New
arch311 applets in 3.11 style: Volume (volcpl.c: speaker/microphone level + mute, output device,
Test plays the ripped DING.WAV; PipeWire via wpctl, audio.c) and Network (netcpl.c: computer name,
connections, Wi-Fi password, TCP/IP settings; NetworkManager via nmcli, net.c; WfW's Network icon
from the ripped MAIN.CPL #34). System tools run through sysexec.c (fork/execvp, no shell; Wi-Fi
password on stdin). ARCH311_SIMULATE=1 uses sample data; apps/control/tests/smoke.w16 passes with it.
libw16: dialog templates from code (dlgtmpl.c), CreateIcon, SystemParametersInfo, GetWindowPlacement,
W16_CMD_LPARAM, CTLCOLOR_* public, EndDialog re-enables the owner before hiding (focus came back
nowhere), hiding the active window activates its owner, group-box mnemonics focus the next control.
UNTESTED: the real back ends (no PipeWire/NetworkManager in WSL; nmcli --ask with a piped password in
particular), hostnamectl permissions (polkit), Help (no help viewer yet).
NEXT: port MAIN.CPL (Color, Fonts, Ports, Mouse, Desktop, Keyboard, Printers, International,
Date/Time) into apps/control; test Volume/Network on a real Arch install; T-PRN-01; Calculator, Clock.
