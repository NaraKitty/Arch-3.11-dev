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

## Current state (end of session 1)
DONE and verified:
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
