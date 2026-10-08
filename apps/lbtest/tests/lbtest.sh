#!/bin/sh
# Runs a list box test script headless with a private, empty configuration:
#   apps/lbtest/tests/lbtest.sh SCRIPT.w16 [OUTDIR [ARGS...]]
# ARGS are lbtest's command line. The display has 16 colours like the reference machine's VGA
# (W16_COLORS=16). lbtest writes the lists' state to OUTDIR/ini/LBTEST.INI, where the tests'
# "# ini:" lines read it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
if [ $# -ge 2 ]; then shift 2; else shift $#; fi
cfg=$(mktemp -d)
trap 'rm -rf "$cfg"' EXIT
mkdir -p "$cfg/arch311"
XDG_CONFIG_HOME="$cfg" W16_COLORS=${W16_COLORS:-16} sh "$repo/tools/run-app-test.sh" "$repo/apps/build/lbtest" "$script" "$out" "$@"
