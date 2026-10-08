#!/bin/sh
# Runs a Clipboard Viewer test script headless from a fresh, private setup:
#   apps/clipbrd/tests/run.sh TEST.w16 [OUTDIR]
# - XDG_CONFIG_HOME and the shared clipboard (ARCH311_CLIPBOARD) are private folders, so parallel
#   runs, the user's settings and the user's clipboard are left alone. W16_COLORS defaults to 16,
#   the reference machine's VGA.
# - Drive C: is laid out like a 3.11 install (as tools/run-cp-test.sh makes it: with ARCH311_REF set,
#   exactly the rig's c-pristine WINDOWS and SYSTEM, so the Open dialog lists the same files), and
#   C:\WINDOWS\SYSTEM holds the test files that mkclp.py writes - where the reference runs' -Files
#   put them.
# - CLIPBRD_REF_CLP="RUN/FILE ..." also puts .CLP files that real 3.11 saved in reference runs
#   ($ARCH311_REF/run-RUN/WINDOWS/FILE) into C:\WINDOWS\SYSTEM as RUN-FILE (skipped when missing).
# - CLIPBRD_NOTEPAD=SCRIPT first runs Notepad headless with that W16 script on the same drive and
#   clipboard, so the viewer starts with what Notepad copied.
# - The .CLP files the viewer saved in C:\WINDOWS are copied to OUTDIR/files when the run ends.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
files=${ARCH311_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/arch311}/files
mkdir -p "$fx/config/arch311" "$fx/clip" "$fx/c/WINDOWS/SYSTEM" "$fx/c/WINDOWS/TEMP"
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
python3 "$here/mkclp.py" "$fx/c/WINDOWS/SYSTEM"
for rf in ${CLIPBRD_REF_CLP:-}; do
    src="$ref/run-${rf%%/*}/WINDOWS/${rf#*/}"
    [ -n "$ref" ] && [ -f "$src" ] && cp "$src" "$fx/c/WINDOWS/SYSTEM/${rf%%/*}-${rf#*/}"
done
printf 'C=%s\n' "$fx/c" > "$fx/config/arch311/drives"
export XDG_CONFIG_HOME="$fx/config" ARCH311_CLIPBOARD="$fx/clip" W16_COLORS=${W16_COLORS:-16}
export W16_HEADLESS=1 W16_SCREEN=${W16_SCREEN:-640x480}
# the programs start in C:\WINDOWS, as the rig's do (its -Cwd); shots go to OUTDIR/shots
run() {   # PROGRAM SCRIPT SHOTDIR
    mkdir -p "$3"
    sed "s#^\(shot[a-z]*\) shots/#\1 $3/#" "$2" > "$fx/script.w16"
    (cd "$fx/c/WINDOWS" && W16_SCRIPT="$fx/script.w16" timeout 120 "$repo/apps/build/$1")
}
status=0
if [ -n "${CLIPBRD_NOTEPAD:-}" ]; then
    run notepad "$(realpath "$CLIPBRD_NOTEPAD")" "$out/notepad" || status=$?
fi
run clipbrd "$script" "$out/shots" || status=$?
mkdir -p "$out/files"
for f in "$fx"/c/WINDOWS/*.CLP "$fx"/c/WINDOWS/*.clp; do [ -f "$f" ] && cp "$f" "$out/files/"; done
echo "files saved by the viewer: $out/files"
exit $status
