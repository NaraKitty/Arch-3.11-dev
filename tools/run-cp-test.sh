#!/bin/sh
# Runs a Control Panel test script headless from a fresh, private setup state:
#   tools/run-cp-test.sh apps/control/tests/TEST.w16 [OUTDIR]
# - WIN.INI, SYSTEM.INI and CONTROL.INI are seeded from the ripped .SRC templates in a private
#   XDG_CONFIG_HOME, so parallel runs and the user's own settings are left alone; they are copied to
#   OUTDIR/ini when the run ends.
# - Drive C: is a fixture laid out like a 3.11 install: C:\WINDOWS, C:\WINDOWS\SYSTEM and C:\WINDOWS\TEMP
#   holding symbolic links to the user's ripped files (nothing is copied into the repo); A: exists.
#   With ARCH311_REF set to the reference rig (the folder with c-pristine), the files are placed in
#   WINDOWS and SYSTEM exactly as in its pristine install, so directory listings match real 3.11.
# ARCH311_SIMULATE (default 1), ARCH311_WAVEDEVS (default 0, the rig's DOSBox has no sound card) and
# ARCH311_CLOCK pass through.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
files=${ARCH311_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/arch311}/files
mkdir -p "$fx/config/arch311" "$fx/a" "$fx/c/WINDOWS/SYSTEM" "$fx/c/WINDOWS/TEMP"
ref=${ARCH311_REF:-}
if [ -n "$ref" ] && [ -d "$ref/c-pristine/WINDOWS" ]; then
    for d in "$ref"/c-pristine/WINDOWS/*/; do mkdir -p "$fx/c/WINDOWS/$(basename "$d")"; done
    for d in WINDOWS WINDOWS/SYSTEM; do
        for f in "$ref/c-pristine/$d"/*; do
            [ -f "$f" ] || continue
            n=$(basename "$f")
            u=$(printf '%s' "$n" | tr a-z A-Z)
            if [ -e "$files/$u" ]; then ln -s "$files/$u" "$fx/c/$d/$n"; fi
        done
    done
else
    for f in "$files"/*; do ln -s "$f" "$fx/c/WINDOWS/"; done
fi
printf 'A=%s\nC=%s\n' "$fx/a" "$fx/c" > "$fx/config/arch311/drives"
status=0
XDG_CONFIG_HOME="$fx/config" ARCH311_SIMULATE=${ARCH311_SIMULATE:-1} ARCH311_WAVEDEVS=${ARCH311_WAVEDEVS:-0} \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/control" "$script" "$out" || status=$?
mkdir -p "$out/ini"
cp "$fx"/config/arch311/*.INI "$out/ini/" 2>/dev/null || true
echo "INI files after the run: $out/ini"
exit $status
