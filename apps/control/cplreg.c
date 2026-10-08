/* The applet modules of arch311's Control Panel, in the order CONTROL.EXE loads them: MAIN.CPL first
 * (TODO: port MAIN.CPL - Color, Fonts, Ports, Mouse, Desktop, Keyboard, Printers, International,
 * Date/Time), then the other .CPL files. Network and Volume are arch311 applets. */
#include "cpl.h"

const CplModuleDef cpl_modules[] = {
    {"NETWORK.CPL", Network_CPlApplet, NULL},
    {"VOLUME.CPL", Volume_CPlApplet, NULL},
    {NULL, NULL, NULL},
};
