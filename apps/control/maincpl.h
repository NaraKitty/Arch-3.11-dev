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
void OutOfMemory(HWND hwnd);                  /* seg1:1881 */
int MyMessageBox(HWND hwnd, int idText, int idCaption, UINT flags, ...);   /* seg4:0000 */
int DoDialogBoxParam(int id, HWND hwnd, DLGPROC proc, DWORD dwHelp, LPARAM lParam); /* seg4:007E */

/* applet dialog procedures */
BOOL KeyboardDlgProc(HWND, UINT, WPARAM, LPARAM);  /* seg15:0000, dialog 5 */
void MouseRun(HWND hwnd);                          /* seg3:097F, dialog 6 (seg16) */
void NetworkDialog(HWND owner);                    /* arch311: replaces WNetDeviceMode */

#endif
