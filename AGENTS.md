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
**Latest (session 6): Notepad and the Control Panel run natively on libw16 (C + SDL2)** - CONTROL.EXE
with MAIN.CPL's Mouse, Keyboard, Date & Time and Ports, SND.CPL's Sound, and arch311's own Network and
Volume applets - pixel-identical to real 3.11 wherever compared. Build: `make -C apps`. Regression
against the real-3.11 reference rig (see session 6): `ARCH311_REF=<rig folder> tools/regress.sh` runs
every test with a `# regress:` line and checks its `# compare:` frames and `# ini:` values (~30 s).
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

### Session 5 (Claude Code on the owner's PC, Oct 7-8 2026)
MAIN.CPL port started (apps/control/main_cpl.c = seg3:03A2 CPlApplet with the ds:00CE applet table,
HourGlass seg1:19D7, BroadcastWinIniChange seg4:0283, CPHelp seg3:09DD; dialogs/icons/strings load
from the user's MAIN.CPL at run time). Applets not ported yet are left out of GETCOUNT/INQUIRE
(`ported` flag in the table). Done: Keyboard (kbdcpl.c, seg15:0000, dialog 5). Network is now
MAIN.CPL's entry 10 (id 10 ran WNetDeviceMode; it runs arch311's Network dialog); cplreg.c loads
MAIN.CPL then VOLUME.CPL. Messages 100/101 (Print Manager / Setup entry points) and the MOUSE-driver
CplApplet override are not ported (noted in code).
Verified against real 3.11 (tools/ref311, scenario %USERPROFILE%\arch311-ref\scn\keyboard.scn vs
apps/control/tests/keyboard.w16): the Keyboard dialog with the Test box filled is pixel-identical to
real 3.11; KeyboardDelay/KeyboardSpeed written to WIN.INI match the values the same keys give there.
Notepad's main window still matches (hello test); Control Panel smoke test passes.
libw16 fixes found by that comparison (first dialogs ever measured - Notepad's were never compared):
- Dialog template x,y place the CLIENT area in the owner's client coordinates; frame/caption go
  around it (was: window origin). Empty menu name no longer adds a menu bar's height.
- Dialog class is CS_DBLCLKS|CS_SAVEBITS|CS_BYTEALIGNWINDOW (USER seg3:1547, style 0x2808);
  CreateWindow byte-aligns window x (CS_BYTEALIGNWINDOW) or client x (CS_BYTEALIGNCLIENT) to the
  nearest 8 px, ported from USER seg13:0E34 (called from CreateWindow seg8:06FC). Not yet applied in
  SetWindowPos / move tracking (seg6:12D3 calls it too) - TODO.
- Dialog frame inset 5 (border + 4), caption inside a white line; controls convert x, y, cx, cy
  separately with rounding (a 14-unit button is 23 px wherever it sits).
- GetTextExtent/DrawText/prefix widths include the overhang of simulated bold once per string
  (dialog fonts): underlines, centred button text and right-aligned statics now land right; push
  button text centres on the face less the 2-px right shadow; group-box title cell 3 px below the
  control top with the line broken from 2 px before to 2 px after it (measured on MS Sans Serif 8
  only); scroll-bar arrows stretch to the control thickness (centre-sampled), thumb position rounds.
- RestoreDC decremented the save depth twice, so the second DrawText on a DC left its clip rectangle
  behind (Control Panel's third icon was never drawn). Edit controls no longer send EN_UPDATE/
  EN_CHANGE for their creation text (the dialog proc saw WM_COMMAND before WM_INITDIALOG).
- Config dir is created with its parents (~/.config may not exist). Keyboard SPI get/set
  (SPI_*KEYBOARDSPEED/DELAY, SPIF_UPDATEINIFILE writes WIN.INI), HWND_BROADCAST.
- Key repeat: host auto-repeat is ignored; libw16 repeats the last key with PC/AT typematic timing
  from KeyboardDelay/KeyboardSpeed (delay (d+1)*250 ms, period (8+A)*2^B*4.17 ms, rate code from
  KEYBOARD.DRV's speed table seg6:0000 - not 31-speed).
UNTESTED: the typematic timing on a real keyboard (the headless script driver bypasses it); Help
buttons (no help viewer yet); no-caption dialog-frame top inset (assumed 5 like the sides).
Owner direction (Oct 8): arch311 is a modern OS mimicking 3.11 accurately to a degree - once the
Control Panel is complete, internet settings must be in it (Network LAN/Wi-Fi is there now; add an
Internet applet in 3.11 style, e.g. proxy/DNS/default browser for the IE/Netscape CEF shells).
Rig note: the blinking focus thumb made the reference recorder drop the in-dialog frames; only the
frame with focus in the Test edit (caret within tolerance) was captured. Use scenarios that end each
step with focus on an edit or button.
Later the same session - Mouse applet (mousecpl.c = seg3:097F + seg16, dialog 6) and MAIN.CPL helpers
(MyMessageBox seg4:0000, DoDialogBoxParam seg4:007E, OutOfMemory seg1:1881). Verified against real 3.11
(scn/mouse.scn vs apps/control/tests/mouse.w16): the dialog after tracking +2, double-click -3 lines is
pixel-identical, and WIN.INI after Swap + OK matches real exactly (DoubleClickSpeed=500,
SwapMouseButtons=yes, MouseThreshold1=4, MouseThreshold2=9, MouseSpeed=2).
libw16: SetDoubleClickTime/GetDoubleClickTime/SwapMouseButton (buttons swapped as SDL reports them),
SPI_GET/SETMOUSE, SPI_SETDOUBLECLICKTIME, SPI_SETMOUSEBUTTONSWAP; USER init defaults (DoubleClickSpeed 0 =
500 ms, MouseThreshold1 = MOUSE.DRV's X threshold 2, MouseThreshold2 10, MouseSpeed 1); Escape moved to
w16.h with QUERYESCSUPPORT; VGA.DRV's MOUSETRAILS escape (seg5:00C7, 1-7 pointer images, WIN.INI
"MouseTrails= n" / "-n") with the trail drawn into the presented frame (UNTESTED on a display: headless
shots have no pointer). Pointer acceleration from the thresholds is stored but not applied (the host
moves the pointer) - TODO when arch311 owns the pointer.
Button text now follows USER BNDrawText exactly (seg25:102E style->layout table, seg25:1097 text rects,
seg25:1366): text centred vertically on tmAscent (not tmHeight), check-box text at the OBM_CHECKBOXES
cell width (bitmap/4 = 14) + 4, group-box title rect = system char width - 1, extent + 4 wide and high
(replaces the measured constants), pressed push buttons shift the text 1 px (was 2), focus rectangle
text-2/text-1 .. +extent+4/+height+3 (push buttons kept 3 px off the top and 4 off the bottom on
displays over 300 lines). DrawFocusRect ported from seg1:2069/1FAA: four full-length PATINVERT strips
(corners inverted twice), inverting odd x+y.
KEYBOARD.DRV SetSpeed (seg6:0020) maps KeyboardSpeed through a 32-entry table to the 8042 rate code;
libw16's typematic uses that table now.
Rig: ref-run.ps1 -Tolerance (default 80; 300 records frames with a blinking focused scroll bar);
runcp.sh-style tests should start from a fresh WIN.INI (re-seeded from WIN.SRC) like the reference.
Owner direction (Oct 8, later): the look is a THEME - Windows 3.11 now, Windows 1, 2, 95, 98, XP and 7 later;
programs must be able to switch with the theme. Keep look-specific code (metrics, frames, captions,
buttons, scroll bars, menus, fonts, colours, cursors, sounds, ripped assets, WM decorations, GTK/Qt styling)
behind a theme layer with win311 as the first theme. Native Linux apps get the theme's frames, scroll
bars, min/max/close behaviour through the WM (3.11: minimise to a desktop icon, maximise to full screen,
close from the system menu / double click / Alt+F4). MS-DOS Prompt just launches the Linux terminal
emulator. IE and Netscape (Chromium/CEF shells) are essential - de-risk early.
Date & Time applet (datetime.c = seg8 + the DOS clock calls seg1:189B-1915; arrow.c = the "cpArrow" spin
control, seg2, also used by Desktop). Verified against real 3.11 (scn/datetime.scn with ref-run
-Dos 'time 09:30:00', port test datetime.w16 with ARCH311_CLOCK): both frames are pixel-identical except
the caret (screenshots hide it) and the ticking seconds. Three probe runs with odd WIN.INI [intl] values
(ref-run -WinIni) pinned down the layout rules. The dialog keeps its edits as an offset to the clock and
sets the Linux clock once on OK (timedatectl set-ntp false + set-time; UNTESTED outside simulation).
MAIN.CPL counts 2000 as a common year (DaysInMonth seg8:064E) - ported as is.
libw16 findings from it:
- VGA.DRV GetCharWidth (seg1:17DC) returns widths + 2 for simulated bold on a 386 in protected mode
  (text still advances + 1): GetCharWidth now does the same.
- ES_CENTER / ES_RIGHT align each line of MULTILINE edits only (was: single-line only, backwards);
  line measured with the overhang, centring offset (fw - w + 1) / 2.
- DrawText DT_CENTER floors a negative offset (text wider than its rectangle).
- Polygon fill: pixels with integer centres inside, then the pen outline (was sampled at y + .5).
- MulDiv added; mapping-mode transforms round (MulDiv) instead of truncating; GetNearestColor.
- AdjustArrowWidth (seg2:0000) makes the spin control's WIDTH odd (13), not its height.
Rig: ref-run.ps1 -Dos 'cmd', ... (DOS commands before win; DOSBox-X's `date` did not take, `time` did)
and -WinIni 'section/key=value', ...; PowerShell jobs do not survive between tool calls - run captures
with the tool's background mode instead.
docs/THEMES.md: the theme architecture (owner direction: 3.11 first, Win 1/2/95/98/XP/7 later).
NEXT: Desktop (8, seg18:1419), Color (100, seg6:0DC8 modeless), International (3, seg12:194D), Fonts (2,
seg9:0CBC), Ports (4, seg19:062E), Printers (1, seg20:1302); Internet applet; CEF spike for the browsers
(download needs the owner's OK: cef_binary_154.0.34+g14c5a08+chromium-154.0.8037.98_linux64_minimal,
326 MB); theme layer refactor; WM.
Scroll-bar controls now show USER's focus caret (seg18:0A48/06F4: a gray caret, 2 px inside the thumb,
following it); gray carets (CreateCaret bitmap 1) invert odd x + y. The Mouse dialog's first frame with
the focused, blinking thumb matches real 3.11. Test scripts: `shotcaret` keeps the caret in the shot.

### Session 6 (Oct 8) - Sound applet, MMSYSTEM sounds, LB_DIR, C:\WINDOWS, list box geometry
Sound applet (apps/control/sndcpl.c = SND.CPL seg1 CPlApplet + seg2 dialog 42), registered after
MAIN.CPL. Verified against real 3.11 in two configurations (scn/sound.scn, scn/sound2.scn vs
apps/control/tests/sound.sh + sound.w16 / sound2.w16):
- no wave device (the rig's default, ARCH311_WAVEDEVS=0): lists and Test disabled, focus OK -> Cancel ->
  Help -> check box: all four dialog frames pixel-identical (only the mouse pointer differs);
- Sound Blaster 1.5 (sndblst2.drv + vsbd.386 from the rip, DOSBox-X sbtype=sb2, nosound): open, Critical
  Stop, Files focused, chimes.wav picked: all pixel-identical; after OK, WIN.INI [sounds] is exactly what
  real 3.11 wrote ([Sounds] removed, "[sounds]" appended at the end in Events-list order, the changed
  event as "SystemHand=C:\WINDOWS\CHIMES.WAV,Critical Stop", the others re-written without the blank).
- Test passes the selected file to sndPlaySound (tests/sound3.w16, headless log). UNTESTED: real audio
  (WSL has no sound server; pw-play/paplay/aplay are found in PATH on Arch).
libw16:
- mmsystem.c: sndPlaySound with MMSYSTEM's rules (seg3:0000/0090/0229/05B1: [sounds] name or file name,
  OpenFile search, RIFF WAVE check, SystemDefault fallback unless SND_NODEFAULT, NULL stops, "" succeeds,
  SND_NOSTOP/LOOP/MEMORY); one player process (pw-play, paplay or aplay via posix_spawnp, no shell) at a
  time. waveOutGetNumDevs = 1 when a player exists (ARCH311_WAVEDEVS overrides). Headless runs log the
  file instead of playing. MessageBeep plays SystemDefault/Hand/Question/Exclamation/Asterisk as
  MMSOUND.DRV's DoBeep (seg1:000A) does, only while WIN.INI Beep is on (SPI_GET/SETBEEP, w16_beep).
- C:\WINDOWS and C:\WINDOWS\SYSTEM are mounted on the ripped files (unless the C: folder has a WINDOWS
  directory or the drives file has a "C:\WINDOWS=/path" line); listings show mount points as directories.
  DOS paths resolve "." / ".." and trailing dots in DOS terms; OpenFile searches the current, Windows and
  system directories for bare names and returns the full upper-case path; OemToAnsi/AnsiToOem copy;
  SetErrorMode; DDL_* constants.
- LB_DIR (was a stub) and DlgDirList share w16_dir_add: sorted lists keep USER's directory order (files,
  then [dirs] with [..] first, then [-x-] drives - as on 3.11); DlgDirList makes the path's directory
  current; DlgDirSelect appends "." to extensionless names.
- List boxes: a WS_BORDER list's border goes around the rectangle it was given (measured: SND.CPL's lists
  are 1 px larger on every side than their template rects); whole-item height is redone when the font
  changes (8 x 13 px from a 114 px template); a disabled list grays the selected item too; its own scroll
  bar stays live (3.11 keeps the Files list's arrows and thumb); LBS_DISABLENOSCROLL uses ESB_DISABLE_BOTH.
- A scroll bar with both arrows disabled fills its trough with the window's class background / COLOR_WINDOW
  (USER seg18:02DA + seg1:6355), not COLOR_SCROLLBAR.
- The gray halftone (DrawFocusRect, gray carets) follows the DC origin: SND.CPL's OK button at (380,85)
  settles the screen-vs-window question left open in session 5.
- Dialogs whose WM_INITDIALOG returns FALSE without setting focus get it on the first tab stop enabled by
  then (Sound disables its lists there; OK gets the focus on 3.11).
- List/combo item data is pointer-sized (ULONG_PTR, as Win32) - ports keep pointers there.
- Note: LocalAlloc returns a MemH handle even for LMEM_FIXED, not the memory: ports that rely on fixed
  local handles being pointers must LocalLock (SND.CPL's item strings use malloc instead).
Regression: Keyboard, Mouse (0 differing pixels) and Date/Time dialogs unchanged.
Rig: ref-run.ps1 -SysIni (and '+section/key=value' to add a line), -Files (copied into WINDOWS\SYSTEM),
-Dosbox (extra DOSBox-X lines); each -Name runs in its own run-<Name> folder, so captures can run in
parallel and the drive is kept for checking INI files. Run the captures with pwsh 7 (WinCap.cs needs
.NET 6); a pwsh started from the PowerShell tool's background mode hung once - use Bash + pwsh.exe.
Decodes of the remaining MAIN.CPL applets (Desktop, International, Ports + Fonts, Printers, Color) were
written to the session scratchpad (derived from the disassembly: never commit them).
NEXT: port Desktop, Color, International, Ports, Fonts, Printers from those decodes; then DRIVERS.CPL,
CPWIN386.CPL and the Internet applet; CEF spike (download still needs the owner's OK).
Tests: tools/run-cp-test.sh SCRIPT.w16 [OUT] runs a Control Panel script with a private XDG_CONFIG_HOME
(fresh WIN.INI/SYSTEM.INI/CONTROL.INI from the .SRC templates, copied to OUT/ini afterwards) and a C:
fixture shaped like a 3.11 install (links to the rip; with ARCH311_REF=<rig folder> exactly as in
c-pristine). Use it for every applet test: runs can go in parallel and never touch ~/.config/arch311.
Ports applet (apps/control/portscpl.c = MAIN.CPL seg19 + restart dialog seg9:05A9, shared with Fonts).
Verified against real 3.11 (scn/ports-a.scn COM2 settings, scn/ports-b.scn COM1 Advanced + restart
prompt; port tests ports.w16 / ports2.w16 run as `tools/run-cp-test.sh TEST OUT Ports`): every frame
of the Ports, Settings, Advanced and System Setting Change dialogs is pixel-identical except the selected
text of combo-box edits (below), and WIN.INI [ports] / SYSTEM.INI [386Enh] COM1Irq=3, COM1Base=03F8
match the real run byte for byte. EscapeCommFunction(GETBASEIRQ) follows COMM.DRV seg2:0BD1 on Linux's
ttyS ports (BIOS table = ports where Linux found a UART; ARCH311_SIMULATE = the reference PC's 3F8/2F8).
"Restart Now" calls ExitWindows, which ends the program (restarting the arch311 session: TODO).
libw16 findings:
- Combo boxes laid out by USER seg34:02AC, again on WM_SETFONT: field = font height + min(it, system
  font height)/4 + 4 borders (20 px for 8 pt Helv; was 22 from the system font), button = right 17 px,
  drop-down-list field shares the button's left border, drop-down edit ends a system character (8) short
  of it, the list is indented by 8 except under drop-down lists. Button bevel: shadow first, highlight on
  top. Focused drop-down-list field (seg33:0E77): control colour 1 px inside the frame, highlight cell
  inside that, opaque text 1 px in, focus rectangle around the cell.
- DrawFocusRect's gray brush is a monochrome pattern: GDI colours it with the DC's text colour (0 bits)
  and background colour (1 bits) - black/white inverts every other pixel, the highlight colours give the
  black-and-yellow dots of a focused combo field (measured).
- Tab/arrow navigation from a control's child window (a combo's edit) moves on from the control itself.
- Owner-drawn buttons get a real DRAWITEMSTRUCT and ODA_FOCUS / ODA_SELECT notifications.
- Icon statics fall back to system icons (dialog 37's IDI_EXCLAMATION).
- (Fixed below: combo-box edit selection and caret.)
Tests: run-cp-test.sh/run-app-test.sh pass an applet name as the Control Panel's command line
("control NAME" opens it, seg1:10A7), so applet tests no longer depend on the icon order; smoke.w16
reaches Volume with End. Rig: DOSBox-X AUTOTYPE has no key combinations - `key alt+x` in a .scn aborts the
rest of the run; navigate with tab/space instead.

Edit control geometry from USER (libw16/src/edit.c; seg26-seg30). A WS_BORDER edit takes the style off at
WM_NCCREATE and draws a one-pixel DF_WINDOWFRAME frame inside its client area (GetWindowLong shows no
WS_BORDER, as in 3.1). Every edit gets the system font first, which fixes the "system" average width and
height (8 and 16 on VGA). Average width = USER's routine behind GetDialogBaseUnits (now
w16_ave_char_width). Single-line: formatting rect = client inset by min(avg, 8)/2 and min(height, 16)/4,
at most one line tall; drawing clipped to the inset client; each selection run spans its GetTextExtent
(overhang included) and is filled a pixel taller above and below; caret 1 px wide when the font's average
width is under the system font's (else 2), line height + 1 tall, at the extent less the overhang, kept
inside the rect; ES_AUTOHSCROLL scrolls by first visible character (quarter width left, three quarters
right); mouse hit test uses half the average width. Multi-line: inset 4/4 (system font) and a whole number
of lines tall (no partial line at the bottom), frame drawn round the window rect. WM_SETFONT on a focused
edit makes a 2 x line-height caret (3.1 does this for single-line edits too). Verified: Ports (both
scenarios) now pixel-identical in every dialog frame including the combo edits' selection and caret;
Keyboard, Mouse, Sound (with and without a wave device) unchanged; Notepad hello unchanged (cursor and
caret-blink pixels only). UNTESTED against 3.11: bordered multi-line edits, password edits, scrolling of
long single-line text, the caret at the start of an empty edit (GetTextExtent of 0 characters returns 0
in libw16; 3.1's GDI value unknown).
Tests: keyboard/mouse/datetime/sound*.w16 now open their applet by name (third argument of
run-cp-test.sh; sound.sh passes Sound) instead of counting icons, which broke when Ports was ported;
ports2.w16 shoots the caret where the reference caught it. The Date & Time references (shots/datetime*)
show the rig's own clock (e.g. 10/8/26 9:30:26), not 11/8/93, so only their layout compares with the
port's ARCH311_CLOCK run; the layout is unchanged.
Combo-box edits as 3.1 creates them (seg34): ES_NOHIDESEL plus USER's internal 0x200 (W16_ES_COMBOBOX),
ES_AUTOHSCROLL / ES_OEMCONVERT only from CBS_AUTOHSCROLL / CBS_OEMCONVERT; the combo clears the edit's
selection when the focus leaves (seg33 kill-focus helper). SLKeyDown (seg28:0A93): a combo's edit sends
F4, Page Up/Down and Up/Down to the combo; other single-line edits move Up/Down as Left/Right and ignore
Page Up/Down. SLInsertText (seg28:0719): without ES_AUTOHSCROLL a single-line edit accepts only what fits
beside the rest of the text (EN_MAXTEXT for the rest). Ports frames unchanged (pixel-identical dialogs).
tools/regress.sh: one command for the whole regression against real 3.11 (WSL: `ARCH311_REF=/mnt/c/Users/
pikac/arch311-ref tools/regress.sh [-j N] [-o OUT] [-n] [TESTS...]`). A test opts in with a `# regress:`
line (its command, "$TEST" and "$OUT" quoted), names reference frames with `# compare: REF.png SHOT.png
[active | x0 y0 x1 y1] [ignore x0 y0 x1 y1]... [max N]` and INI results with `# ini: FILE SECTION
KEY=VALUE`. `shot`/`shotcaret` now also append the active window's rectangle to shots/rects.txt, so
"active" crops both frames to the window under test (the Control Panel behind it and the rig's mouse
pointer, which sits at 320,240, stay out). First run: 30 checks in 30 s, all PASS (Ports both
scenarios, Sound with and without a wave device, Keyboard, Mouse, Notepad hello; INI values for Ports
and Sound). New applet tests should carry these lines; I merge branches only with the run green.

### Session 7 (Oct 8, Color applet worktree) - MAIN.CPL Color, VGA colour matching, MessageBox
Color applet (apps/control/colorcpl.c = MAIN.CPL seg6/seg7 with seg1:191B, seg4:00E5, seg23:07E1;
seg3:0756 runs it with the private loop seg3:06AE): dialog 100 with the sample screen and the palette
half, Save Scheme (27), the modeless Custom Color Selector (26). Windows Default and the basic colours
come from VGA.DRV's OEMBIN #1/#2, MAIN.CPL's fallback colours from its own data segment (ds:02CE) - all
read at run time. Color is the first icon, so its tests open it with Enter like the real scenarios.
Verified in 16 colours (W16_COLORS=16) with tools/regress.sh, tests apps/control/tests/color*.w16 vs
arch311-ref/scn/color-*.scn: opened, every scheme (23 frames), palette, custom colour, OK (Control
Panel repainted in Arizona; WIN.INI [colors] and CONTROL.INI [current]/[Custom Colors] equal to real
3.11's), Save Scheme (CONTROL.INI), Remove Scheme and three confirmation boxes - all PASS, as do the
other applets' and Notepad's tests. UNTESTED: every mouse path (the rig types keys only), Help, the
selector's own arrow keys, Color|Solid, Save over an existing name, true-colour drawing.
libw16 (each from the 3.1 code named):
- 16-colour mode matches VGA.DRV, tables read from the user's VGA.DRV: nearest colour seg1:1956,
  dither seg1:24A1, RealizeObject seg1:292A (white/black and driver colours solid, E0E0E0+0x10 = the
  scroll bar's 50% pattern, others dithered); brush patterns start at the DC origin + brush origin.
- SetSysColors (seg41:0C06) and USER's start-up (seg3:0748, its reader 06D7) make the text-like colours
  solid (GetNearestColor) and flag E0E0E0 scroll bars; SetSysColors broadcasts WM_SYSCOLORCHANGE and
  redraws everything. nc.c fills frames and captions with brushes, so e.g. Arizona's caption dithers.
- Dialog manager: CheckDefPushButton (seg25:0B5B), ClearDefaults (0AB2), Save/RestoreDlgFocus
  (03A8/03E3), DlgSetFocus (0000); the default button changes only on the focus moves IsDialogMessage
  makes (Tab, arrows, mnemonics, clicks), activation and DM_SETDEFID; a disabled default push button
  draws plain (seg25:18BC).
- MessageBox = USER seg42:04F5 + MB_DlgProc seg42:0101: an in-memory template in system-font units,
  buttons 2 x "0" wider than the longest label (seg3:23BE), CS_BYTEALIGNWINDOW then places it (that
  explains real boxes 1 px right of the arithmetic). No MessageBeep - 3.1's MessageBox plays none.
- DrawText = USER seg6:0571 (wrapped lines keep their trailing blank for centring and for
  DT_CALCRECT); GrayString's stipple follows the text origin; owner-draw drop-down lists (seg33/34);
  CombineRgn/PtInRegion/FillRgn, ClipCursor, w16_module_data (a module's segment bytes).
- tools/regress.sh: `# ini:` takes a bracketed section with spaces ([Custom Colors]).
Rig notes: the frame recorder drops a shot identical to the previous one, so count frames by what
changed; a letter typed while a control wants characters (the colour grids) is no mnemonic; after a
message box closed with "n", real 3.11 leaves one black pixel at the Color dialog's client origin
(both runs, not understood; ignored in color-mbox.w16).
