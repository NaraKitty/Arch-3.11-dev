#!/bin/sh
# Run a ported app headless under a W16_SCRIPT and collect its screenshots.
#   tools/run-app-test.sh apps/build/notepad apps/notepad/tests/smoke.w16 [OUTDIR]
# Script commands (libw16/src/msg.c): sleep MS | key alt+f | type TEXT | click X Y | shot FILE.png | quit
# The app's C: drive is $HOME unless ~/.config/arch311/drives says otherwise.
set -e
app=$(realpath "$1"); script=$(realpath "$2"); out=${3:-test-out}
mkdir -p "$out/shots"
cd "$out"
W16_HEADLESS=1 W16_SCREEN=${W16_SCREEN:-640x480} W16_SCRIPT="$script" timeout 120 "$app"
echo "screenshots in $out/shots:"; ls shots
