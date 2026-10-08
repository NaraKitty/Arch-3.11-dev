#!/bin/bash
# Regression run of the headless tests against real 3.11:
#   tools/regress.sh [-j JOBS] [-o OUTDIR] [-n] [TEST.w16 ...]
# Runs every test (default: apps/*/tests/*.w16) that has a "# regress:" line - the command that runs
# it, with "$TEST" the script and "$OUT" its output folder (quoted: paths may hold spaces) - in
# parallel after building the apps (-n: no build), then checks the test's "# compare:" lines:
#   # compare: REF.png SHOT.png [active | x0 y0 x1 y1] [ignore x0 y0 x1 y1]... [max N]
# REF is a frame of the real-3.11 reference rig ($ARCH311_REF/shots/REF.png), SHOT a screenshot of
# the test ($OUT/shots/SHOT.png). They are compared inside the box (screen coordinates, right and
# bottom exclusive; "active" is the rectangle of the window that was active when the shot was taken,
# from shots/rects.txt; default the whole screen), leaving out the ignored rectangles, and may differ
# in at most N pixels (default 0). Reference frames show the mouse pointer (the rig leaves it at
# 320,240: "ignore 320 240 332 260") and port frames never do. "# ini: FILE SECTION KEY=VALUE" lines
# check what the run left in $OUT/ini/FILE (tools/run-cp-test.sh copies the INI files there) against
# what real 3.11 wrote; a SECTION with spaces goes in brackets ("# ini: CONTROL.INI [Custom Colors]
# ColorA=12C2C2"), and "KEY=" checks that the key is absent or empty. Prints a PASS, FAIL or SKIP
# (no reference frame on this machine) line per check and exits with status 1 if anything failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
jobs=4 out=${TMPDIR:-/tmp}/arch311-regress build=1
while getopts j:o:n opt; do
    case $opt in j) jobs=$OPTARG ;; o) out=$OPTARG ;; n) build=0 ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
ref=${ARCH311_REF:?set ARCH311_REF to the reference rig folder (the one holding shots/)}
export ARCH311_REF
cmp="$repo/tools/ref311/compare.py"
[ $# -eq 0 ] && set -- "$repo"/apps/*/tests/*.w16
if [ $build = 1 ]; then make -s -C "$repo/apps" > "$out.build.log" 2>&1 || { echo "build failed: $out.build.log"; exit 1; }; fi
mkdir -p "$out"

# apps/control/tests/ports.w16 -> control-tests-ports
name_of() { local t; t=$(realpath --relative-to="$repo" "$1"); t=${t#apps/}; t=${t%.w16}; printf '%s' "${t//\//-}"; }

run_one() {
    local t o cmd
    t=$(realpath "$1") o="$out/$(name_of "$1")"
    cmd=$(sed -n 's/^# regress: //p' "$t" | head -1)
    rm -rf "$o"; mkdir -p "$o"
    (cd "$repo" && TEST=$t OUT=$o && eval "$cmd") > "$o.log" 2>&1
    echo $? > "$o.status"
}

tests=()
for t in "$@"; do grep -q '^# regress: ' "$t" && tests+=("$t"); done
running=0
for t in "${tests[@]}"; do
    run_one "$t" &
    running=$((running + 1))
    if [ $running -ge "$jobs" ]; then wait -n; running=$((running - 1)); fi
done
wait

fail=0
for t in "${tests[@]}"; do
    n=$(name_of "$t") o="$out/$(name_of "$t")"
    st=$(cat "$o.status" 2>/dev/null || echo 1)
    if [ "$st" != 0 ]; then echo "FAIL $n: the run ended with status $st (log: $o.log)"; fail=1; fi
    while read -r refp shot rest; do
        set -- $rest
        box="0 0 100000 100000" max=0 ign=()
        if [ "${1:-}" = active ]; then
            box=$(awk -v s="$shot" '$1 == s {print $2, $3, $4, $5}' "$o/shots/rects.txt" 2>/dev/null | tail -1)
            shift
            if [ -z "$box" ]; then echo "FAIL $n: no active window recorded for $shot"; fail=1; continue; fi
        elif [ $# -ge 4 ] && [ "$1" != max ] && [ "$1" != ignore ]; then
            box="$1 $2 $3 $4"; shift 4
        fi
        set -- $box "$@"
        bx=$1 by=$2; shift 4
        while [ $# -gt 0 ]; do
            case $1 in
            max) max=$2; shift 2 ;;
            ignore) ign+=(--ignore "$(($2 - bx)),$(($3 - by)),$(($4 - bx)),$(($5 - by))"); shift 5 ;;
            *) echo "FAIL $n: cannot read '# compare: $refp $shot $rest'"; fail=1; continue 2 ;;
            esac
        done
        r="$ref/shots/$refp" p="$o/shots/$shot" d="$o/diff-${shot%.png}"
        if [ ! -f "$r" ]; then echo "SKIP $n: no reference frame $refp"; continue; fi
        if [ ! -f "$p" ]; then echo "FAIL $n: no screenshot $shot"; fail=1; continue; fi
        python3 "$cmp" crop "$r" $box 1 "$d-ref.png" && python3 "$cmp" crop "$p" $box 1 "$d-port.png" || { fail=1; continue; }
        cnt=$(python3 "$cmp" diff "$d-ref.png" "$d-port.png" "$d.png" "${ign[@]}" | head -1 | awk '{print $1}')
        if [ -n "$cnt" ] && [ "$cnt" -le "$max" ] 2>/dev/null; then
            echo "PASS $n: $refp ~ $shot ($cnt px)"
        else
            echo "FAIL $n: $refp ~ $shot: ${cnt:-?} px differ (max $max), see $d.png"; fail=1
        fi
    done < <(sed -n 's/^# compare: //p' "$t")
    while read -r file sect kv; do
        case $sect in \[*) sect="${sect#\[} $kv"; kv=${sect#*\] }; sect=${sect%%\]*} ;; esac
        key=${kv%%=*} val=${kv#*=}
        # a section header ends at its "]" (3.11's SYSTEM.INI has "[boot]" followed by spaces)
        got=$(awk -v s="$sect" -v k="$key" '{ sub(/\r$/, "") }
            /^\[/ { h = $0; sub(/\].*$/, "]", h); ins = tolower(h) == "[" tolower(s) "]"; next }
            ins { i = index($0, "="); if (i && tolower(substr($0, 1, i - 1)) == tolower(k)) { print substr($0, i + 1); exit } }' \
            "$o/ini/$file" 2>/dev/null)
        if [ "$got" = "$val" ]; then echo "PASS $n: $file [$sect] $key=$val"
        else echo "FAIL $n: $file [$sect] $key is '$got', real 3.11 wrote '$val'"; fail=1; fi
    done < <(sed -n 's/^# ini: //p' "$t")
done
exit $fail
