/* MAIN.CPL: the Control Panel's built-in applets, ported from the 3.11 MAIN.CPL (seg3:03A2
 * CPlApplet and its applet table at ds:00CE). Dialog templates, icons and strings come from the
 * user's ripped MAIN.CPL at run time.
 *
 * Applets that are not ported yet are left out of GETCOUNT/INQUIRE, the way MAIN.CPL drops
 * Network when WNetGetCaps reports no network. */
#include "maincpl.h"
#include <stdarg.h>
#include <string.h>

HINSTANCE hInstMain;            /* [0x10] */
DWORD dwContext;                /* [0xa2] */
UINT wHelpMessage;              /* [0x13d8] */
static int cInit;               /* [0x1b4] CPL_INIT count */
static BOOL fHourGlass;         /* [0x44] */
static char szOutOfMem[256];    /* [0x18b0] string 0 */
char szCaption[64];             /* [0x1f6a] string 1, "Control Panel" */
char szClose[0x9E];             /* [0x1730] string 9, "Close" */

static const char szHelpFile[] = "control.hlp";
static const char *const szSections[] = {
    "windows", "colors", "devices", "fonts", "ports", "intl", "Desktop", "TrueType",
};

/* ds:00CE, 16 bytes an entry: icon, name, info, id (lData), present, help context, help file */
typedef struct {
    int idIcon, idName, idInfo, id;
    BOOL present;
    DWORD dwHelpContext;
    BOOL ported;                /* arch311: applet ported yet */
} Applet;

static Applet applets[] = {
    {24, 48, 600, 0, TRUE, 5000, TRUE},    /* Color */
    {26, 50, 602, 2, TRUE, 5002, FALSE},   /* Fonts */
    {28, 52, 604, 4, TRUE, 5004, TRUE},    /* Ports */
    {30, 54, 606, 6, TRUE, 5006, TRUE},    /* Mouse */
    {32, 56, 608, 8, TRUE, 5008, FALSE},   /* Desktop */
    {29, 53, 605, 5, TRUE, 5005, TRUE},    /* Keyboard */
    {25, 49, 601, 1, TRUE, 5001, FALSE},   /* Printers */
    {27, 51, 603, 3, TRUE, 5003, FALSE},   /* International */
    {31, 55, 607, 7, TRUE, 5007, TRUE},    /* Date/Time */
    {34, 58, 610, 10, TRUE, 5010, TRUE},   /* Network */
};
#define NAPPLETS ((int)(sizeof applets / sizeof applets[0]))

static BOOL Shown(const Applet *a) { return a->present && a->ported; }

/* the n-th applet that is shown */
static Applet *Nth(int n)
{
    for (int i = 0; i < NAPPLETS; i++)
        if (Shown(&applets[i]) && n-- == 0) return &applets[i];
    return NULL;
}

/* ------------------------------------------------------------------ seg1:19D7 */
void HourGlass(BOOL fOn)
{
    if ((fHourGlass && !fOn) || (!fHourGlass && fOn)) {
        fHourGlass = fOn;
        ShowCursor(fOn);
    }
    SetCursor(LoadCursor(NULL, fOn ? IDC_WAIT : IDC_ARROW));
}

/* ------------------------------------------------------------------ seg4:0283 */
void BroadcastWinIniChange(int section)
{
    SendMessage(HWND_BROADCAST, WM_WININICHANGE, 0, (LPARAM)szSections[section]);
    if (section == 3) SendMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
}

/* ------------------------------------------------------------------ seg3:09DD
 * (MAIN.CPL maps the contexts to printman.hlp when Print Manager runs Printers through
 * CPlApplet message 100; that entry point is not ported) */
void CPHelp(HWND hwnd)
{
    WinHelp(hwnd, szHelpFile, HELP_CONTEXT, dwContext);
}

/* ------------------------------------------------------------------ seg1:1881 */
void OutOfMemory(HWND hwnd)
{
    MessageBox(hwnd, szOutOfMem, szCaption, MB_SYSTEMMODAL | MB_ICONHAND);
}

/* ------------------------------------------------------------------ seg4:0000
 * a message box from MAIN.CPL strings; the text is a wsprintf format for the arguments */
int MyMessageBox(HWND hwnd, int idText, int idCaption, UINT flags, ...)
{
    char fmt[256], text[256], cap[256];
    int r = -1;
    if (idText && LoadString(hInstMain, idText, fmt, sizeof fmt)) {
        va_list ap;
        va_start(ap, flags);
        wvsprintf(text, fmt, ap);
        va_end(ap);
        if (LoadString(hInstMain, idCaption, cap, sizeof cap)) r = MessageBox(hwnd, text, cap, flags);
    }
    if (r == -1) OutOfMemory(hwnd);
    return r;
}

/* ------------------------------------------------------------------ seg4:007E
 * DialogBoxParam on a MAIN.CPL template with the help context switched for its lifetime */
int DoDialogBoxParam(int id, HWND hwnd, DLGPROC proc, DWORD dwHelp, LPARAM lParam)
{
    DWORD dwWas = dwContext;
    dwContext = dwHelp;
    int r = DialogBoxParam(hInstMain, MAKEINTRESOURCE(id), hwnd, proc, lParam);
    dwContext = dwWas;
    if (r == -1) OutOfMemory(hwnd);
    return r;
}

/* ------------------------------------------------------------------ seg3:0733: run applet <id>
 * Not ported yet: 1 Printers = dialog 1, seg20:1302 (activates the open one, [0x16], instead when
 * there is one); 2 Fonts = dialog 2, seg9:0CBC; 3 International = dialog 3, seg12:194D; 8 Desktop =
 * dialog 8, seg18:1419. */
static void RunApplet(HWND hwnd, int id)
{
    switch (id) {
    case 0:
        ColorRun(hwnd);     /* CreateDialog 100 with seg6:0DC8, then the loop seg3:06AE */
        break;
    case 4:
        DialogBox(hInstMain, MAKEINTRESOURCE(4), hwnd, PortsDlgProc);
        break;
    case 5:
        DialogBox(hInstMain, MAKEINTRESOURCE(5), hwnd, KeyboardDlgProc);
        break;
    case 6:
        MouseRun(hwnd);
        break;
    case 7:
        DialogBox(hInstMain, MAKEINTRESOURCE(7), hwnd, DateTimeDlgProc);
        break;
    case 10:
        NetworkDialog(hwnd);
        break;
    }
}

/* ------------------------------------------------------------------ seg3:013A (the part arch311 needs) */
static BOOL InitApplet(void)
{
    hInstMain = w16_load_module("MAIN.CPL");
    if (!hInstMain) return FALSE;
    wHelpMessage = RegisterWindowMessage("ShellHelp");
    RegisterArrowClass(hInstMain);
    LoadString(hInstMain, 0, szOutOfMem, sizeof szOutOfMem);
    LoadString(hInstMain, 1, szCaption, sizeof szCaption);
    LoadString(hInstMain, 9, szClose, sizeof szClose); /* seg3:01D4 keeps a LocalAlloc copy */
    return TRUE;
}

/* ------------------------------------------------------------------ seg3:03A2 */
LRESULT Main_CPlApplet(HWND hwndCPl, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    switch (msg) {
    case CPL_INIT:
        if (!cInit) {
            if (!InitApplet()) return 0;
            /* MAIN.CPL drops Network when WNetGetCaps(WNNC_NET_TYPE) is 0; arch311's Network
             * applet manages NetworkManager connections and is always present */
        }
        cInit++;
        return 1;
    case CPL_GETCOUNT: {
        int n = 0;
        for (int i = 0; i < NAPPLETS; i++) if (Shown(&applets[i])) n++;
        return n;
    }
    case CPL_INQUIRE: {
        Applet *a = Nth((int)lParam1);
        CPLINFO *ci = (CPLINFO *)lParam2;
        if (!a) return 0;
        ci->idIcon = a->idIcon;
        ci->idName = a->idName;
        ci->idInfo = a->idInfo;
        ci->lData = a->id;
        return 1;
    }
    case CPL_NEWINQUIRE: {
        Applet *a = Nth((int)lParam1);
        NEWCPLINFO *ni = (NEWCPLINFO *)lParam2;
        if (!a) return 0;
        ni->hIcon = LoadIcon(hInstMain, MAKEINTRESOURCE(a->idIcon));
        LoadString(hInstMain, a->idName, ni->szName, sizeof ni->szName);
        if (!LoadString(hInstMain, a->idInfo, ni->szInfo, sizeof ni->szInfo)) ni->szInfo[0] = 0;
        ni->dwSize = sizeof *ni;
        ni->dwFlags = 0;
        ni->lData = a->id;
        ni->dwHelpContext = a->dwHelpContext;
        lstrcpy(ni->szHelpFile, szHelpFile);
        /* MAIN.CPL lets a loaded "MOUSE" driver that exports CplApplet replace the Mouse entry
         * (seg3:08C6); there are no 16-bit mouse drivers here */
        return 1;
    }
    case CPL_DBLCLK: {
        /* MAIN.CPL indexes its table with lParam1 here; with applets left out, look the entry up
         * by its id (lParam2) instead, which is the same entry */
        for (int i = 0; i < NAPPLETS; i++)
            if (applets[i].id == (int)lParam2) dwContext = applets[i].dwHelpContext;
        RunApplet(hwndCPl, (int)lParam2);
        return 0;
    }
    case CPL_EXIT:
        if (--cInit == 0) ColorExit();     /* seg3:0371 frees the GDI objects applets keep */
        return 0;
    }
    /* messages 100 (Printers for Print Manager) and 101 (seg3:0010 for Setup) are private
     * entry points of other 3.11 programs; not ported */
    return 0;
}
