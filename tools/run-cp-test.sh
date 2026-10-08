#!/bin/sh
# Runs a Control Panel test script headless from a fresh, private setup state:
#   tools/run-cp-test.sh apps/control/tests/TEST.w16 [OUTDIR [APPLET NAME]]
# With an applet name ("Ports", "Date/Time") the Control Panel opens that applet at once, as
# CONTROL.EXE NAME does, and quits when it closes; scripts then need no icon navigation.
# The private config (WIN.INI, SYSTEM.INI, CONTROL.INI from the .SRC templates, copied to OUTDIR/ini
# afterwards) and the 3.11-shaped C: drive are tools/run-fixture-test.sh's; see there for
# ARCH311_REF, ARCH311_REF_INI, ARCH311_INI_DIR, ARCH311_CWD, ARCH311_SIMULATE, ARCH311_WAVEDEVS and
# ARCH311_CLOCK.
here=$(cd "$(dirname "$0")" && pwd)
exec sh "$here/run-fixture-test.sh" control "$@"
