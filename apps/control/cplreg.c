/* The applet modules of arch311's Control Panel, in the order CONTROL.EXE loads them: MAIN.CPL
 * first (ported from 3.11; its Network entry runs arch311's Network dialog), then the other .CPL
 * files. Volume is an arch311 applet. */
#include "cpl.h"

const CplModuleDef cpl_modules[] = {
    {"MAIN.CPL", Main_CPlApplet, NULL},
    {"VOLUME.CPL", Volume_CPlApplet, NULL},
    {NULL, NULL, NULL},
};
