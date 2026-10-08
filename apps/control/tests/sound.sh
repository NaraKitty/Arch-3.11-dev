#!/bin/sh
# Runs a Sound applet test (default tests/sound.w16) through tools/run-cp-test.sh and shows WIN.INI's
# [sounds] and Beep afterwards. ARCH311_WAVEDEVS (default 0, as on the reference machine) sets the
# wave device count.
#   apps/control/tests/sound.sh [SCRIPT.w16] [OUTDIR]
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
out=$(realpath -m "${2:-test-out}")
sh "$repo/tools/run-cp-test.sh" "${1:-$here/sound.w16}" "$out"
echo "--- WIN.INI after the test:"
grep -i -A9 '^\[sounds\]' "$out/ini/WIN.INI" || true
grep -i '^beep=' "$out/ini/WIN.INI" || true
