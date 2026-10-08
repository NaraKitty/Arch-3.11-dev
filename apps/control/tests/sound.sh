#!/bin/sh
# Runs a Sound applet test (default tests/sound.w16) with a C: drive laid out like the reference
# install in arch311-ref: C:\WINDOWS holds the three 3.11 .wav files (copied from the user's rip
# while the test runs, never committed) and the SYSTEM and TEMP directories, and A: exists.
# ARCH311_WAVEDEVS (default 0, as on the reference machine) sets the wave device count.
#   apps/control/tests/sound.sh [SCRIPT.w16] [OUTDIR]
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "${1:-$here/sound.w16}")
out=$(realpath -m "${2:-test-out}")
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
mkdir -p "$fx/config/arch311" "$fx/a" "$fx/c/WINDOWS/SYSTEM" "$fx/c/WINDOWS/TEMP"
files=${ARCH311_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/arch311}/files
for w in CHIMES CHORD DING; do cp "$files/$w.WAV" "$fx/c/WINDOWS/"; done
printf 'A=%s\nC=%s\n' "$fx/a" "$fx/c" > "$fx/config/arch311/drives"
XDG_CONFIG_HOME="$fx/config" ARCH311_WAVEDEVS=${ARCH311_WAVEDEVS:-0} ARCH311_SIMULATE=1 \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/control" "$script" "$out"
echo "--- WIN.INI after the test:"
grep -i -A9 '^\[sounds\]' "$fx/config/arch311/WIN.INI" || true
grep -i '^beep=' "$fx/config/arch311/WIN.INI" || true
