#!/bin/sh
# Runs a test script of a ported program headless from a fresh, private setup state:
#   tools/run-fixture-test.sh APP SCRIPT.w16 [OUTDIR [ARGS...]]
# APP is a program built in apps/build ("notepad", "control") or the path of one; ARGS are its
# command line (an applet name for the Control Panel, as tools/run-cp-test.sh passes it).
# - WIN.INI, SYSTEM.INI and CONTROL.INI are seeded from the ripped .SRC templates in a private
#   XDG_CONFIG_HOME, so parallel runs and the user's own settings are left alone; they are copied to
#   OUTDIR/ini when the run ends.
# - Drive C: is a fixture laid out like a 3.11 install: C:\WINDOWS, C:\WINDOWS\SYSTEM and C:\WINDOWS\TEMP
#   holding symbolic links to the user's ripped files (nothing is copied into the repo); A: exists (an
#   empty floppy). With ARCH311_REF set to the reference rig (the folder with c-pristine), the files
#   are placed in WINDOWS and SYSTEM exactly as in its pristine install, so directory listings match
#   real 3.11, and the drives are the rig's: A:, C: labelled C_DRIVE and DOSBox-X's own Z: labelled
#   DOSBOX-X (libw16 gives a drive the name of its folder as volume label); ARCH311_REF_INI=1 also
#   starts from that install's WIN.INI, SYSTEM.INI and CONTROL.INI, so the files after the run compare
#   byte for byte with a reference run's (arch311-ref/run-NAME).
# - The program starts in C:\WINDOWS, as the rig starts Windows from there (ref-run.ps1 -Cwd);
#   ARCH311_CWD=<DOS directory> picks another one.
# - ARCH311_INI_DIR=<folder>: its *.INI files are the starting ones (after the above), e.g. a WIN.INI
#   changed the way a reference run's -WinIni changes it.
# ARCH311_SIMULATE (default 1), ARCH311_WAVEDEVS (default 0, the rig's DOSBox has no sound card) and
# ARCH311_CLOCK pass through.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
app=$1
case $app in */*) app=$(realpath "$app") ;; *) app="$repo/apps/build/$app" ;; esac
script=$(realpath "$2")
out=$(realpath -m "${3:-test-out}")
if [ $# -ge 3 ]; then shift 3; else shift $#; fi
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
files=${ARCH311_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/arch311}/files
c="$fx/C_DRIVE"
mkdir -p "$fx/config/arch311" "$fx/a" "$c/WINDOWS/SYSTEM" "$c/WINDOWS/TEMP"
ref=${ARCH311_REF:-}
if [ -n "$ref" ] && [ -d "$ref/c-pristine/WINDOWS" ]; then
    for d in "$ref"/c-pristine/WINDOWS/*/; do mkdir -p "$c/WINDOWS/$(basename "$d")"; done
    for d in WINDOWS WINDOWS/SYSTEM; do
        for f in "$ref/c-pristine/$d"/*; do
            [ -f "$f" ] || continue
            n=$(basename "$f")
            u=$(printf '%s' "$n" | tr a-z A-Z)
            if [ -e "$files/$u" ]; then ln -s "$files/$u" "$c/$d/$n"; fi
        done
    done
    if [ "${ARCH311_REF_INI:-0}" = 1 ]; then
        for n in WIN SYSTEM CONTROL; do cp "$ref/c-pristine/WINDOWS/$n.INI" "$fx/config/arch311/$n.INI"; done
    fi
    mkdir -p "$fx/DOSBOX-X"
    printf 'A=%s\nC=%s\nZ=%s\n' "$fx/a" "$c" "$fx/DOSBOX-X" > "$fx/config/arch311/drives"
else
    for f in "$files"/*; do ln -s "$f" "$c/WINDOWS/"; done
    printf 'A=%s\nC=%s\n' "$fx/a" "$c" > "$fx/config/arch311/drives"
fi
if [ -n "${ARCH311_INI_DIR:-}" ]; then
    for f in "$ARCH311_INI_DIR"/*.INI; do [ -f "$f" ] && cp "$f" "$fx/config/arch311/"; done
fi
status=0
XDG_CONFIG_HOME="$fx/config" ARCH311_SIMULATE=${ARCH311_SIMULATE:-1} ARCH311_WAVEDEVS=${ARCH311_WAVEDEVS:-0} \
    W16_DOS_CWD="${ARCH311_CWD:-C:\\WINDOWS}" \
    sh "$repo/tools/run-app-test.sh" "$app" "$script" "$out" "$@" || status=$?
mkdir -p "$out/ini"
cp "$fx"/config/arch311/*.INI "$out/ini/" 2>/dev/null || true
echo "INI files after the run: $out/ini"
exit $status
