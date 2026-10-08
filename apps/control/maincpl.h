/* Shared state and helpers of the MAIN.CPL port (main_cpl.c) and its applets. */
#ifndef ARCH311_MAINCPL_H
#define ARCH311_MAINCPL_H
#include "cpl.h"

#define IDD_HELP 119            /* the "&Help" button of every MAIN.CPL dialog */

extern HINSTANCE hInstMain;     /* the user's ripped MAIN.CPL: dialogs, icons, strings */
extern DWORD dwContext;         /* help context of the running applet (5000 + applet id) */
extern UINT wHelpMessage;       /* RegisterWindowMessage("ShellHelp") */
extern char szCaption[];        /* [0x1f6a] string 1, "Control Panel" */
extern char szClose[];          /* [0x1730] string 9, "Close": Cancel buttons say it once a change is saved */
extern char szWinDir[];         /* [0x1fca] the Windows directory with a backslash */
extern char szSysDir[];         /* [0x102a] the system directory with a backslash */
extern char szControlIni[];     /* [0xe7c] "<Windows directory>\control.ini" */

void HourGlass(BOOL fOn);                     /* seg1:19D7 */
void BroadcastWinIniChange(int section);      /* seg4:0283 */
void CPHelp(HWND hwnd);                       /* seg3:09DD */
void OutOfMemory(HWND hwnd);                  /* seg1:1881 */
int MyMessageBox(HWND hwnd, int idText, int idCaption, UINT flags, ...);   /* seg4:0000 */
int DoDialogBoxParam(int id, HWND hwnd, DLGPROC proc, DWORD dwHelp, LPARAM lParam); /* seg4:007E */
void IntToStr(int n, LPSTR p);                /* seg4:0210 */
void TrimSpaces(LPSTR s);                     /* seg4:019E */
int StrNCmpPrefix(LPCSTR a, LPCSTR b, int n); /* seg4:02B4 */
int StrIndex(LPCSTR s, char ch);              /* seg4:031D */
void AddBackslash(LPSTR s);                   /* seg4:0341 */
LPSTR StrStrI(LPCSTR s, LPCSTR sub);          /* seg1:15C9 */
LPSTR FindIniKeyByValue(LPCSTR file, LPCSTR section, LPCSTR value); /* seg1:1C0B; free() the key */
BOOL ConfirmRemove(HWND hwnd, LPCSTR name, int idFormat);          /* seg6:0000 */
HFILE OpenFileFromWinDir(LPCSTR file, OFSTRUCT *of, UINT style);   /* seg9:005F */

/* seg2:0664: a cpArrow field's steps for SB_LINEUP/LINEDOWN/PAGEUP/PAGEDOWN, its range, the values
 * codes 4 and 5 jump to, and what the last step did (0 in range, 2 stopped at the top, 4 at the
 * bottom) */
typedef struct { int step[4], max, min, v4, v5; int flag; } ARROWSTEP;
int StepField(int code, int value, ARROWSTEP *f);  /* seg2:0664 */
int AdjustArrowWidth(HWND h);                      /* seg2:0000 */

/* applet dialog procedures */
BOOL KeyboardDlgProc(HWND, UINT, WPARAM, LPARAM);  /* seg15:0000, dialog 5 */
void MouseRun(HWND hwnd);                          /* seg3:097F, dialog 6 (seg16) */
BOOL DateTimeDlgProc(HWND, UINT, WPARAM, LPARAM);  /* seg8:077C, dialog 7 */
BOOL DesktopDlgProc(HWND, UINT, WPARAM, LPARAM);   /* seg18:1419, dialog 8 */
BOOL RegisterArrowClass(HINSTANCE hInst);          /* seg2:060E, "cpArrow" */
void NetworkDialog(HWND owner);                    /* arch311: replaces WNetDeviceMode */
BOOL PortsDlgProc(HWND, UINT, WPARAM, LPARAM);     /* seg19:062E, dialog 4 */
int DoPortSettings(HWND hwndOwner, int iPort);     /* seg19:04C6, dialog 19 (also Printers' Connect) */
BOOL RestartDlgProc(HWND, UINT, WPARAM, LPARAM);   /* seg9:05A9, dialog 37 (Ports, Fonts) */

#endif
