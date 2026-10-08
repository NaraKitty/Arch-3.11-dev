#!/bin/sh
# Run a ported app headless under a W16_SCRIPT and collect its screenshots.
#   tools/run-app-test.sh apps/build/notepad apps/notepad/tests/smoke.w16 [OUTDIR [ARGS...]]
# ARGS are the program's command line (e.g. an applet name for the Control Panel).
# Script commands (libw16/src/msg.c): sleep MS | key alt+f | type TEXT | click X Y | shot FILE.png
# (caret hidden) | shotcaret FILE.png (caret shown) | clip TEXT (onto the clipboard) | quit
# Key names include kp_plus, kp_minus, kp_multiply, kp_divide, kp_period, kp_enter, kp_0..kp_9.
# Shots left in the script when the program ends are taken of the screen it leaves.
# The app's C: drive is $HOME unless ~/.config/arch311/drives says otherwise.
# The clipboard the arch311 programs share (libw16/src/clipbrd.c) is the run's own, OUTDIR/clipboard,
# so tests running side by side do not paste each other's text; set ARCH311_CLIPBOARD to share one.
set -e
app=$(realpath "$1"); script=$(realpath "$2"); out=${3:-test-out}
if [ $# -ge 3 ]; then shift 3; else set --; fi
mkdir -p "$out/shots"
cd "$out"
if [ -z "${ARCH311_CLIPBOARD:-}" ]; then ARCH311_CLIPBOARD=$(pwd)/clipboard; rm -rf "$ARCH311_CLIPBOARD"; fi
export ARCH311_CLIPBOARD
W16_HEADLESS=1 W16_SCREEN=${W16_SCREEN:-640x480} W16_SCRIPT="$script" timeout 120 "$app" "$@"
echo "screenshots in $out/shots:"; ls shots
