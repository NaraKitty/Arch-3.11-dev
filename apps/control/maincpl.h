/* Shared state and helpers of the MAIN.CPL port (main_cpl.c) and its applets. */
#ifndef ARCH311_MAINCPL_H
#define ARCH311_MAINCPL_H
#include "cpl.h"

#define IDD_HELP 119            /* the "&Help" button of every MAIN.CPL dialog */

extern HINSTANCE hInstMain;     /* the user's ripped MAIN.CPL: dialogs, icons, strings */
extern DWORD dwContext;         /* help context of the running applet (5000 + applet id) */
extern UINT wHelpMessage;       /* RegisterWindowMessage("ShellHelp") */

void HourGlass(BOOL fOn);                     /* seg1:19D7 */
void BroadcastWinIniChange(int section);      /* seg4:0283 */
void CPHelp(HWND hwnd);                       /* seg3:09DD */

/* applet dialog procedures */
BOOL KeyboardDlgProc(HWND, UINT, WPARAM, LPARAM);  /* seg15:0000, dialog 5 */
void NetworkDialog(HWND owner);                    /* arch311: replaces WNetDeviceMode */

#endif
