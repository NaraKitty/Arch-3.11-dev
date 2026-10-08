#!/bin/sh
# Runs a Minesweeper test script headless from a fresh, private setup:
#   apps/winmine/tests/run.sh TEST.w16 [OUTDIR]
# - XDG_CONFIG_HOME is a private folder, so parallel runs and the user's settings are left alone:
#   WIN.INI and SYSTEM.INI are seeded from the ripped .SRC templates on first use and WINMINE.INI
#   starts absent, as on a fresh 3.11, unless WINMINE_INI names a file to start from. The INI files
#   are copied to OUTDIR/ini when the run ends.
# - WINMINE_SEED fixes the mine layout (default 1; set it empty for the tick count as in 3.1).
# - W16_COLORS defaults to 16, the reference machine's VGA. (In libw16's true-colour mode
#   GetDeviceCaps(NUMCOLORS) is -1, and Minesweeper takes its monochrome bitmaps as 3.1 would.)
# - ARCH311_WAVEDEVS=1 logs the tunes of WINMINE.INI Sound=3 (headless runs never play them).
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
cfg=$(mktemp -d)
trap 'rm -rf "$cfg"' EXIT
mkdir -p "$cfg/arch311"
if [ -n "$WINMINE_INI" ]; then cp "$WINMINE_INI" "$cfg/arch311/WINMINE.INI"; fi
status=0
XDG_CONFIG_HOME="$cfg" WINMINE_SEED=${WINMINE_SEED-1} W16_COLORS=${W16_COLORS:-16} \
    ARCH311_WAVEDEVS=${ARCH311_WAVEDEVS:-0} \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/winmine" "$script" "$out" || status=$?
mkdir -p "$out/ini"
cp "$cfg"/arch311/*.INI "$out/ini/" 2>/dev/null || true
echo "INI files after the run: $out/ini"
exit $status
