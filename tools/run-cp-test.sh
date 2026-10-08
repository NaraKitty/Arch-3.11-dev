#!/bin/sh
# Runs a Control Panel test script headless from a fresh, private setup state:
#   tools/run-cp-test.sh apps/control/tests/TEST.w16 [OUTDIR [APPLET NAME]]
# With an applet name ("Ports", "Date/Time") the Control Panel opens that applet at once, as
# CONTROL.EXE NAME does, and quits when it closes; scripts then need no icon navigation.
# - WIN.INI, SYSTEM.INI and CONTROL.INI are seeded from the ripped .SRC templates in a private
#   XDG_CONFIG_HOME, so parallel runs and the user's own settings are left alone; they are copied to
#   OUTDIR/ini when the run ends.
# - Drive C: is a fixture laid out like a 3.11 install: C:\WINDOWS, C:\WINDOWS\SYSTEM and C:\WINDOWS\TEMP
#   holding symbolic links to the user's ripped files (nothing is copied into the repo); A: exists.
#   With ARCH311_REF set to the reference rig (the folder with c-pristine), the files are placed in
#   WINDOWS and SYSTEM exactly as in its pristine install, so directory listings match real 3.11;
#   ARCH311_REF_INI=1 also starts from that install's WIN.INI, SYSTEM.INI and CONTROL.INI, so the
#   files after the run compare byte for byte with a reference run's (arch311-ref/run-NAME).
# - ARCH311_INI_DIR=<folder>: its *.INI files are the starting ones (after the above), e.g. a WIN.INI
#   changed the way a reference run's -WinIni changes it.
# - ARCH311_WININI='section/key=value;...' changes the starting WIN.INI exactly as ref-run.ps1 -WinIni
#   does (Set-IniKey: the key's line replaced in its section, else added at the section's top, a new
#   section added at the end; a leading '+' always adds), so a run starts from a reference run's file.
# ARCH311_SIMULATE (default 1), ARCH311_WAVEDEVS (default 0, the rig's DOSBox has no sound card),
# ARCH311_CLOCK and the applets' ARCH311_SIM_* settings pass through.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
script=$(realpath "$1")
out=$(realpath -m "${2:-test-out}")
if [ $# -ge 2 ]; then shift 2; else set --; fi
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
    if [ "${ARCH311_REF_INI:-0}" = 1 ]; then
        for n in WIN SYSTEM CONTROL; do cp "$ref/c-pristine/WINDOWS/$n.INI" "$fx/config/arch311/$n.INI"; done
    fi
else
    for f in "$files"/*; do ln -s "$f" "$fx/c/WINDOWS/"; done
fi
if [ -n "${ARCH311_INI_DIR:-}" ]; then
    for f in "$ARCH311_INI_DIR"/*.INI; do [ -f "$f" ] && cp "$f" "$fx/config/arch311/"; done
fi
if [ -n "${ARCH311_WININI:-}" ]; then
    w="$fx/config/arch311/WIN.INI"
    [ -f "$w" ] || cp "$files/WIN.SRC" "$w"
    printf '%s\n' "$ARCH311_WININI" | tr ';' '\n' | while IFS= read -r kv; do
        [ -n "$kv" ] || continue
        awk -v kv="$kv" '
            BEGIN { add = substr(kv, 1, 1) == "+"; if (add) kv = substr(kv, 2)
                    i = index(kv, "/"); secname = substr(kv, 1, i - 1); kv = substr(kv, i + 1)
                    i = index(kv, "="); key = substr(kv, 1, i - 1); val = substr(kv, i + 1) }
            { sub(/\r$/, ""); line[++n] = $0 }
            END {
                at = 0; inside = 0; done = 0
                for (i = 1; i <= n && !done; i++) {
                    if (line[i] ~ /^\[.*\]/) { h = line[i]; sub(/^\[/, "", h); sub(/\].*$/, "", h)
                                              inside = tolower(h) == tolower(secname); if (inside) at = i; continue }
                    if (inside && !add && tolower(substr(line[i], 1, length(key))) == tolower(key) &&
                        substr(line[i], length(key) + 1) ~ /^[ \t]*=/) { line[i] = key "=" val; done = 1 }
                }
                if (!done) {
                    if (!at) { line[++n] = "[" secname "]"; at = n }
                    for (i = n; i > at; i--) line[i + 1] = line[i]
                    line[at + 1] = key "=" val; n++
                }
                for (i = 1; i <= n; i++) printf "%s\r\n", line[i]
            }' "$w" > "$w.new"
        mv "$w.new" "$w"
    done
fi
printf 'A=%s\nC=%s\n' "$fx/a" "$fx/c" > "$fx/config/arch311/drives"
status=0
XDG_CONFIG_HOME="$fx/config" ARCH311_SIMULATE=${ARCH311_SIMULATE:-1} ARCH311_WAVEDEVS=${ARCH311_WAVEDEVS:-0} \
    sh "$repo/tools/run-app-test.sh" "$repo/apps/build/control" "$script" "$out" "$*" || status=$?
mkdir -p "$out/ini"
cp "$fx"/config/arch311/*.INI "$out/ini/" 2>/dev/null || true
echo "INI files after the run: $out/ini"
exit $status
