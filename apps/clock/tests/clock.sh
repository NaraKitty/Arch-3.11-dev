#!/bin/sh
# Runs a Clock test script headless from a fresh, private setup state:
#   apps/clock/tests/clock.sh SCRIPT.w16 [OUTDIR [CLOCK.INI LINE...]]
# - WIN.INI is seeded from the ripped WIN.SRC in a private XDG_CONFIG_HOME (as tools/run-cp-test.sh
#   does), so parallel runs and the user's own settings are left alone. Lines given after OUTDIR go
#   into a CLOCK.INI [Clock] section first ("Options=1,0,0,0,0,0", "Position=0,0,300,300", ...).
# - The clock starts at ARCH311_CLOCK (default 2026-10-08 09:30:00) and runs on in real time; Clock
#   waits for the next second before it shows, so a script's first frames show HH:MM:01.
# INI files after the run are copied to OUTDIR/ini.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
if [ $# -ge 2 ]; then shift 2; else shift $#; fi
cfg=$(mktemp -d)
trap 'rm -rf "$cfg"' EXIT
mkdir -p "$cfg/arch311"
if [ $# -gt 0 ]; then
    printf '[Clock]\r\n' > "$cfg/arch311/CLOCK.INI"
    for l in "$@"; do printf '%s\r\n' "$l" >> "$cfg/arch311/CLOCK.INI"; done
fi
status=0
XDG_CONFIG_HOME="$cfg" ARCH311_CLOCK="${ARCH311_CLOCK:-2026-10-08 09:30:00}" \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/clock" "$script" "$out" || status=$?
mkdir -p "$out/ini"
cp "$cfg"/arch311/*.INI "$out/ini/" 2>/dev/null || true
exit $status
