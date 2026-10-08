#!/bin/sh
# Runs a SysEdit test script headless from a private, fresh setup:
#   apps/sysedit/tests/sysedit.sh SCRIPT.w16 [OUTDIR [REFRUN]]
# - XDG_CONFIG_HOME is private. With ARCH311_REF set to the reference rig, WIN.INI and SYSTEM.INI are
#   the ones real SysEdit opened in the reference run run-mdi-tile (the rig wrote shell=sysedit.exe
#   into SYSTEM.INI with a lone LF, which the edit window shows); without it libw16 seeds them from
#   the ripped WIN.SRC / SYSTEM.SRC.
# - Drive C: is a fixture: C:\AUTOEXEC.BAT and C:\CONFIG.SYS as the reference runs' DOS commands wrote
#   them, C:\WINDOWS holding links to the ripped files (so SysEdit's .SYD backups stay in the fixture).
# - The display has 16 colours like the reference machine's VGA.
# - Afterwards OUTDIR/ini holds the INI files and OUTDIR/c the files left on C: (backups included).
#   With REFRUN (e.g. mdi-edit) they must equal byte for byte what real 3.11 left in
#   $ARCH311_REF/run-REFRUN (AUTOEXEC.BAT, AUTOEXEC.SYD, CONFIG.SYS, WINDOWS\WIN.INI, WIN.SYD,
#   SYSTEM.INI, SYSTEM.SYD where they exist), and no temporary file may be left over.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
refrun=${3:-}
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
files=${ARCH311_ASSETS:-${XDG_DATA_HOME:-$HOME/.local/share}/arch311}/files
mkdir -p "$fx/config/arch311" "$fx/c/WINDOWS"
for f in "$files"/*; do ln -s "$f" "$fx/c/WINDOWS/"; done
printf 'PATH C:\\WINDOWS\r\nSET TEMP=C:\\WINDOWS\\TEMP\r\n' > "$fx/c/AUTOEXEC.BAT"
printf 'FILES=30\r\nBUFFERS=20\r\n' > "$fx/c/CONFIG.SYS"
ref=${ARCH311_REF:-}
if [ -n "$ref" ] && [ -f "$ref/run-mdi-tile/WINDOWS/SYSTEM.INI" ]; then
    cp "$ref/run-mdi-tile/WINDOWS/WIN.INI" "$ref/run-mdi-tile/WINDOWS/SYSTEM.INI" "$fx/config/arch311/"
fi
printf 'C=%s\n' "$fx/c" > "$fx/config/arch311/drives"
status=0
XDG_CONFIG_HOME="$fx/config" W16_COLORS=${W16_COLORS:-16} \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/sysedit" "$script" "$out" || status=$?
mkdir -p "$out/ini" "$out/c"
cp "$fx"/config/arch311/*.INI "$out/ini/" 2>/dev/null || true
for f in "$fx"/c/* "$fx"/c/WINDOWS/*.[Ss][Yy][Dd]; do [ -f "$f" ] && [ ! -L "$f" ] && cp "$f" "$out/c/"; done
if [ -n "$refrun" ] && [ -n "$ref" ]; then
    r="$ref/run-$refrun"
    for n in AUTOEXEC.BAT AUTOEXEC.SYD CONFIG.SYS CONFIG.SYD; do
        p=$(ls "$out/c" | grep -i "^$n\$" || true)
        if [ -f "$r/$n" ]; then
            if [ -z "$p" ] || ! cmp -s "$r/$n" "$out/c/$p"; then echo "C:\\$n differs from real 3.11's"; status=1; fi
        elif [ -n "$p" ]; then echo "C:\\$n was written, real 3.11 left none"; status=1; fi
    done
    for n in WIN.INI SYSTEM.INI; do
        if ! cmp -s "$r/WINDOWS/$n" "$out/ini/$n"; then echo "C:\\WINDOWS\\$n differs from real 3.11's"; status=1; fi
    done
    for n in WIN.SYD SYSTEM.SYD; do
        p=$(ls "$out/c" | grep -i "^$n\$" || true)
        if [ -f "$r/WINDOWS/$n" ]; then
            if [ -z "$p" ] || ! cmp -s "$r/WINDOWS/$n" "$out/c/$p"; then echo "C:\\WINDOWS\\$n differs from real 3.11's"; status=1; fi
        elif [ -n "$p" ]; then echo "C:\\WINDOWS\\$n was written, real 3.11 left none"; status=1; fi
    done
    if ls "$out/c" | grep -qi '\.tmp$'; then echo "a temporary file was left on C:"; status=1; fi
fi
exit $status
