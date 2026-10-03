# arch311

An Arch Linux based system that looks, feels and behaves like **Windows 3.11 (for Workgroups)**,
runs the original 16-bit programs on a 64-bit host, and ships "Internet Explorer 5" and
"Netscape Navigator" as themes on top of a modern Chromium engine.

> **Status: design + first tooling only. Nothing here boots yet.** See `AGENTS.md` for the exact
> state, the backlog and the handoff log. This repo is meant to be continued by other Claude
> agents (and humans): read `AGENTS.md` first, update its handoff log last.

## Goals
- Win 3.11 look, feel and behaviour, authored for the original resolutions (640x480, 800x600, 1024x768),
  with integer scaling / larger logical desktops for modern displays.
- Run the original 16-bit programs without ever executing 16-bit code natively: they are loaded and
  interpreted inside a sandbox, with the Win16 API implemented as 64-bit host code.
- IE5 / Netscape appearance over Chromium's engine, so web content stays modern and patched.
- Secure by default: per-app sandboxes, no network for legacy apps unless granted.

## Layout
| Path | Purpose |
|---|---|
| `AGENTS.md` | Handoff notes, rules, backlog, log. **Start here.** |
| `docs/` | Architecture, decisions, legal policy, build steps, generated inventory |
| `tools/` | Media extraction (KWAJ/SZDD), format inventory, profile builder |
| `runtime/` | Win16 runtime (NE loader, CPU sandbox, API layer) - not started |
| `theme/` | Win 3.11 theme spec and assets pipeline - not started |
| `browsers/` | Chromium-based IE5/Netscape themed apps - not started |
| `profile/` | archiso overlay (packages, airootfs) - untested |

You supply your own Windows 3.11 install media; none is included. See `docs/LEGAL.md`.
