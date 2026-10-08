/* The Windows 3.1 Control Panel applet interface (cpl.h). On arch311 the applet "libraries" are
 * native modules linked into the Control Panel; the protocol between host and applet is unchanged,
 * so ported 3.11 applets (MAIN.CPL, ...) and new ones (Volume, Network) plug in the same way. */
#ifndef ARCH311_CPL_H
#define ARCH311_CPL_H
#include "w16.h"

#define CPL_INIT 1
#define CPL_GETCOUNT 2
#define CPL_INQUIRE 3
#define CPL_SELECT 4
#define CPL_DBLCLK 5
#define CPL_STOP 6
#define CPL_EXIT 7
#define CPL_NEWINQUIRE 8

typedef struct {
    int idIcon, idName, idInfo;
    LPARAM lData;
} CPLINFO;

typedef struct {
    DWORD dwSize, dwFlags, dwHelpContext;
    LPARAM lData;
    HICON hIcon;
    char szName[32];
    char szInfo[64];
    char szHelpFile[128];
} NEWCPLINFO;

/* lParam1/lParam2 carry pointers (CPLINFO, NEWCPLINFO, lData), so they are pointer-sized here */
typedef LRESULT (*APPLET_PROC)(HWND hwndCPl, UINT msg, LPARAM lParam1, LPARAM lParam2);

/* one applet module ("xxx.CPL") */
typedef struct {
    const char *file;     /* shown as "Loading FILE" and used for [don't load] / command lines */
    APPLET_PROC proc;     /* its CPlApplet */
    const char *resfile;  /* ripped 3.11 module its CPLINFO resources come from, or NULL */
} CplModuleDef;

/* the modules the Control Panel finds, in load order (cplreg.c) */
extern const CplModuleDef cpl_modules[];

/* applets */
LRESULT Main_CPlApplet(HWND, UINT, LPARAM, LPARAM);
LRESULT Volume_CPlApplet(HWND, UINT, LPARAM, LPARAM);

#endif
