/* The applet modules of arch311's Control Panel, in the order CONTROL.EXE loads them: MAIN.CPL
 * first (ported from 3.11; its Network entry runs arch311's Network dialog), then the other .CPL
 * files as 3.11 Setup installs them (CPWIN386 is not ported yet; DRIVERS and SND are). Volume is an
 * arch311 applet. */
#include "cpl.h"

const CplModuleDef cpl_modules[] = {
    {"MAIN.CPL", Main_CPlApplet, NULL},
    {"DRIVERS.CPL", Drivers_CPlApplet, NULL},
    {"SND.CPL", Sound_CPlApplet, NULL},
    {"VOLUME.CPL", Volume_CPlApplet, NULL},
    {NULL, NULL, NULL},
};
