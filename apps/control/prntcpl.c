/* MAIN.CPL Printers applet: dialog 1 (seg20/seg21), Connect (12, seg20), the install path (seg22,
 * dialogs 23/29 and seg1's Install Driver / Browse dialogs 30/38) and the network printer connections
 * (seg17 + seg3:0010, dialogs 21/32), ported from 3.11 with the data layer moved to CUPS (cups.c):
 *
 *   3.11                                         arch311
 *   WIN.INI [PrinterPorts]/[devices] printers     CUPS queues (lpstat -v), "NAME on PORT" with PORT the
 *                                                 device URI's port name (w16_printer_port, as Print Setup)
 *   the driver of an entry (atom)                 the WIN.INI entry's driver if the queue has one, else CUPS
 *   [windows] device= (default printer)           CUPS's default (lpstat -d; lpoptions -d on Close), and
 *                                                 device= still written as 3.1 writes it
 *   CONTROL.INF [io.device] (List of Printers)    CUPS's drivers (lpinfo -m)
 *   file copying + the driver's DevInstall        lpadmin -p NAME -m MODEL -v URI -E / -v URI / -x NAME
 *   the driver's ExtDeviceMode dialog (Setup...)  Print Setup (COMMDLG) for the queue, kept with lpoptions
 *   [ports] in Connect                            [ports]' LPTn:/COMn: as the PC's parallel and serial
 *                                                 devices, then every other CUPS device (lpinfo -v)
 *   WNet printer connections (LPTn: -> \\srv\sh)  a raw CUPS queue named after the port (LPT2) printing to
 *                                                 smb://srv/sh or the URI typed
 * Dialogs, layout, strings and message boxes are 3.1's, loaded from the user's MAIN.CPL. The timeouts
 * and Fast Printing Direct to Port stay in WIN.INI exactly as 3.1 writes them (CUPS has no such
 * settings). A failing CUPS command shows its own message in a 3.1-style box. The CPlApplet
 * messages 100/101 (Windows Setup and Print Manager running this dialog) are not ported, so the
 * "Continue" button, the instructions line 323 and the PM_NOTIFY messages of that mode are left out.
 * UNTESTED: everything against a real CUPS server (none on the WSL build host); printer connections
 * against a real share. */
#include "maincpl.h"
#include "cups.h"
#include "commdlg.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#ifndef MAKEWORD
#define MAKEWORD(lo, hi) ((WORD)(((BYTE)(lo)) | ((WORD)((BYTE)(hi))) << 8))
#endif

/* dialogs */
#define IDD_PRINTERS 1
#define IDD_CONNECT 12
#define IDD_NETCONN 21
#define IDD_CURNEW 23
#define IDD_UNLISTED 29
#define IDD_INSTALLDRV 30
#define IDD_PREVCONN 32
#define IDD_BROWSE 38
/* dialog 1 */
#define IDC_INSTALLED 300
#define IDC_PRINTERLIST 301
#define IDC_DEFAULT 302
#define IDC_SETDEFAULT 303
#define IDC_ADD 304
#define IDC_CONNECT 305
#define IDC_SETUP 307
#define IDC_REMOVE 308
#define IDC_PRINTMAN 311
#define IDC_INSTALL 312
#define IDC_LISTLABEL 313
/* dialog 12 */
#define IDC_PORTS 306
#define IDC_SETTINGS 307
#define IDC_PRINTERNAME 314
#define IDC_DNS 315
#define IDC_RETRY 316
#define IDC_NETWORK 317
#define IDC_DNSLABEL 319
#define IDC_RETRYLABEL 320
#define IDC_FASTPRINT 321
#define IDC_REFRESH 322 /* private: the network connections changed */
/* dialog 21 / 32 */
#define IDC_CONNLIST 630
#define IDC_NETPORT 634
#define IDC_NETPATH 635
#define IDC_NETPWD 636
#define IDC_NETCONNECT 637
#define IDC_DISCONNECT 638
#define IDC_NETBROWSE 640
#define IDC_PREVIOUS 642
#define IDC_PREVLIST 642
#define IDC_PREVDELETE 644
/* dialog 29 / 30 / 38 */
#define IDC_UNLISTEDLIST 318
#define IDC_DRVPROMPT 4
#define IDC_DRVPATH 711
#define IDC_DRVBROWSE 120
#define IDC_BROWSEPROMPT 1280
#define IDC_BROWSEHELP 1038

/* help contexts (DoDialogBoxParam) */
#define HC_CONNECT 0x1F4C
#define HC_NETCONN 0x1F55
#define HC_CURNEW 0x1F57
#define HC_UNLISTED 0x1F5D
#define HC_PREVCONN 0x1F60
#define HC_BROWSE 0x1F66
#define HC_INSTALLDRV 0x21FF

#define DLGC_NOTBUTTON_MASK 0xDF
#define CONTROL_INI "CONTROL.INI"

/* +0 dns, +2 retry, +4 driver[16], +0x14 port, +0x16 name, +0x18 "Name\0Port\0" in 3.1; the
 * offsets are pointers here and the port has room for any port name */
#define PI_PORTMAX 64
typedef struct {
    WORD dns;           /* "Device Not Selected" timeout, seconds */
    WORD retry;         /* "Transmission Retry" timeout, seconds */
    char driver[16];
    char *port, *name;
    char text[];
} PRINTERINFO;

/* a List of Printers entry (3.1: an INF line in a global block, MAKELONG(offset, handle)) */
typedef struct {
    char ppd[512];      /* lpadmin -m driver, or the PPD file's host path */
    char model[128];    /* the printer's name as listed */
    BOOL isFile;        /* a PPD file from Install Unlisted (lpadmin -P) */
    BOOL isUnlisted;    /* the "Install Unlisted or Updated Printer" entry */
} PRINTERLINE;

/* a port of the Connect list or of an install (3.1: a [ports] key) */
typedef struct {
    char port[PI_PORTMAX];
    char uri[512];
    char status[128];
} PORTENT;

static HWND hNetDlg;                 /* [0x14] dialog 21 while it runs */
static HWND hDlgPrinters;            /* [0x16] */
static HWND hDlgConnect;             /* [0xC74] */
static BOOL fOemBrowse;              /* [0xE1C] Browse looks for driver files (*.ppd) */
static char szSrcPath[0x9E];         /* [0xE98] string 161, "A:\" */
static char szInstallPath[0xC8];     /* [0xF40] the path of the Install Driver dialog */
static int iDefault;                 /* [0xFF8] list index of the default printer; 0 at first, as in 3.1 */
static WORD wRetryDefault;           /* [0x1014] */
static HWND hListInstalled;          /* [0x1056] */
static char szFile[0x81];            /* [0x11F0] the file Browse looks for */
static RECT rcDlg;                   /* [0x12DE] the whole dialog, screen */
static int iSel;                     /* [0x12E6] */
static WORD wDnsDefault;             /* [0x1750] */
static BOOL fNetChanged;             /* [0x1898] */
static PRINTERLINE *lpUnlistedLine;  /* [0x18A6] */
static char szPrevPath[0x80];        /* [0x1960] */
static BOOL fSpooler;                /* [0x1A22] */
static int idCurNewText;             /* [0x1E2C] */
static char szOn[10];                /* [0x1E32] string 8, " on " */
static RECT rcList;                  /* [0x1FD4] "List of Printers:" and below */
static char szCupsDefault[128];      /* arch311: CUPS's default printer when the dialog opened */
static PORTENT connPorts[96];        /* arch311: the Connect list's ports (item data = index) */
static int nConnPorts;

static BOOL ConnectDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL CurNewDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL UnlistedDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL InstallDriverDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL NetConnDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL PrevConnDlgProc(HWND, UINT, WPARAM, LPARAM);
static void SetDefaultFromSelection(HWND hDlg);
static void PrinterNetworkDialog(HWND hwnd, LPSTR lpPort);

/* ------------------------------------------------------------------ seg1 / seg4 string helpers */
/* seg1:1028 */
static LPSTR StrChr(LPCSTR s, char c)
{
    for (; *s; s++)
        if (*s == c) return (LPSTR)s;
    return NULL;
}

/* seg1:12C8: at most cb - 1 characters of src, and a NUL */
static void StrCpyN(LPSTR dst, LPCSTR src, int cb)
{
    size_t n = strlen(src);
    if (cb <= 0) return;
    if (n > (size_t)cb - 1) n = (size_t)cb - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* seg1:1523: the last occurrence of pat */
static LPSTR StrRStr(LPCSTR s, LPCSTR pat)
{
    size_t n = strlen(pat);
    for (const char *p = s + strlen(s); p-- > s;)
        if (!strncmp(p, pat, n)) return (LPSTR)p;
    return NULL;
}

/* seg4:019E: leading and trailing blanks (spaces only) removed in place */
static void TrimSpaces(LPSTR s)
{
    LPSTR p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    p = s + strlen(s);
    if (p != s) {
        p--;
        while (p >= s && *p == ' ') p--;
        p[1] = 0;
    }
}

/* seg1:184F: unsigned decimal, digits only, 16-bit */
static WORD StrToUInt(LPCSTR s)
{
    WORD n = 0;
    for (; (BYTE)(*s - '0') <= 9; s++) n = (WORD)(n * 10 + (*s - '0'));
    return n;
}

/* seg4:02B4: signed compare of up to n chars, stopping at a NUL in either string */
static int StrNCmpSigned(LPCSTR a, LPCSTR b, int n)
{
    for (int i = 0; i < n && a[i] && b[i]; i++) {
        if ((signed char)b[i] > (signed char)a[i]) return -1;
        if ((signed char)b[i] < (signed char)a[i]) return 1;
    }
    return 0;
}

/* seg1:061C: a backslash at the end ("" becomes "\"); returns the end */
static LPSTR AddBackslash(LPSTR s)
{
    LPSTR e = s + strlen(s);
    if (!*s || e[-1] != '\\') { *e++ = '\\'; *e = 0; }
    return e;
}

/* seg12:178F: the text between the first two '"' */
static int GetQuotedField(LPSTR dst, LPCSTR src)
{
    *dst = 0;
    LPCSTR p = StrChr(src, '"');
    if (!p) return 0;
    p++;
    LPCSTR q = StrChr(p, '"');
    if (!q) return 0;
    int n = (int)(q - p);
    memcpy(dst, p, n);
    dst[n] = 0;
    return n;
}

/* seg6:0000 (Color applet's code): "Are you sure ...?" boxes */
static BOOL ConfirmBox(HWND hwnd, LPCSTR arg, int idFmt)
{
    char fmt[0x9E], buf[0x186];
    LoadString(hInstMain, idFmt, fmt, sizeof fmt);
    wsprintf(buf, fmt, arg);
    return MessageBox(hwnd, buf, szCaption, MB_YESNO | MB_ICONEXCLAMATION) == IDYES;
}

/* arch311: a CUPS tool failed; its message in the box MAIN.CPL shows its errors in */
static void CupsError(HWND hwnd)
{
    MessageBox(hwnd, cups_last_error(), szCaption, MB_ICONASTERISK);
}

/* the part of a URI before its query ("serial:/dev/ttyS0?baud=115200") */
static BOOL SameDevice(LPCSTR a, LPCSTR b)
{
    size_t la = strcspn(a, "?"), lb = strcspn(b, "?");
    return la == lb && !strncmp(a, b, la);
}

/* ------------------------------------------------------------------ seg20:04A0 / 04D3 */
static BOOL IsComPort(LPCSTR s) { return lstrlen(s) == 5 && !StrNCmpSigned(s, "COM", 3); }
static BOOL IsLptPort(LPCSTR s) { return lstrlen(s) == 5 && !StrNCmpSigned(s, "LPT", 3); }

/* ------------------------------------------------------------------ seg20:0000 */
/* (Setup mode says "Continue": not ported) */
static void SetCancelButtonText(HWND hDlg)
{
    SetDlgItemText(hDlg, IDOK, szClose);
}

/* ------------------------------------------------------------------ seg20:0B0B */
/* the entry's name, port, driver and timeouts from "Name on Port" and its item data */
static PRINTERINFO *GetPrinterInfo(HWND hList, int idx)
{
    if (idx < 0) idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    int len = (int)SendMessage(hList, LB_GETTEXTLEN, idx, 0);
    if (len < 0) return NULL;
    PRINTERINFO *p = malloc(sizeof *p + len + 1 + PI_PORTMAX);
    if (!p) return NULL;
    if ((int)SendMessage(hList, LB_GETTEXT, idx, (LPARAM)p->text) < 0) goto fail;
    LPSTR q = StrRStr(p->text, szOn);    /* the last " on " */
    if (!q) goto fail;
    *q = 0;
    p->name = p->text;
    p->port = q + lstrlen(szOn);
    DWORD dw = (DWORD)SendMessage(hList, LB_GETITEMDATA, idx, 0);
    GetAtomName(HIWORD(dw), p->driver, sizeof p->driver);
    p->dns = (WORD)(HIBYTE(LOWORD(dw)) * 5);
    p->retry = (WORD)(LOBYTE(LOWORD(dw)) * 5);
    return p;
fail:
    free(p);
    return NULL;
}

/* ------------------------------------------------------------------ seg20:090C */
static void SetNoDefaultPrinter(HWND hDlg)
{
    char buf[0x9E];
    iDefault = -1;
    LoadString(hInstMain, 160, buf, sizeof buf); /* "No Default Printer" */
    SetDlgItemText(hDlg, IDC_DEFAULT, buf);
}

/* ------------------------------------------------------------------ seg20:0940 */
static void DeletePrinterEntry(HWND hList, int idx)
{
    if (idx < 0) idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    DeleteAtom(HIWORD((DWORD)SendMessage(hList, LB_GETITEMDATA, idx, 0)));
    SendMessage(hList, LB_DELETESTRING, idx, 0);
    if (idx == iDefault) SetNoDefaultPrinter(GetParent(hList));
    else if (idx < iDefault) iDefault--;
}

/* ------------------------------------------------------------------ seg20:09AB */
/* the index of "Name on Port" (a whole string, not a prefix), or -1 */
static int FindPrinterEntry(HWND hList, const PRINTERINFO *p)
{
    char buf[0x13E];
    wsprintf(buf, "%s%s%s", p->name, szOn, p->port);
    int len = lstrlen(buf), i = -1;
    for (;;) {
        int r = (int)SendMessage(hList, LB_FINDSTRING, (WPARAM)i, (LPARAM)buf);
        if (r <= i) return -1; /* none, or wrapped round */
        i = r;
        /* 3.1 also takes the first prefix match when (signed char)LOBYTE(dns) + 0x18 == 0 (a compiler
         * artefact: dns 232, 488 ...) */
        if ((signed char)LOBYTE(p->dns) + 0x18 == 0) return i;
        if ((int)SendMessage(hList, LB_GETTEXTLEN, i, 0) == len) return i;
    }
}

/* ------------------------------------------------------------------ seg20:0A3E */
/* adds and selects "Name on Port"; the driver as an atom and the timeouts in fives in the item data */
static int AddPrinterEntry(HWND hList, const PRINTERINFO *p)
{
    char buf[0x13E];
    ATOM a = AddAtom(p->driver);
    if (!a) return -1;
    wsprintf(buf, "%s%s%s", p->name, szOn, p->port);
    int idx = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)buf);
    if (idx == -1) { DeleteAtom(a); return -1; }
    BYTE d5 = (BYTE)((int)(p->dns + 4) / 5), r5 = (BYTE)((int)(p->retry + 4) / 5);
    SendMessage(hList, LB_SETITEMDATA, idx, (LPARAM)(DWORD)MAKELONG(MAKEWORD(r5, d5), a));
    SendMessage(hList, LB_SETCURSEL, idx, 0);
    if (idx <= iDefault) iDefault++;
    return idx;
}

/* ------------------------------------------------------------------ seg20:0C22 */
static void SetDefaultFromSelection(HWND hDlg)
{
    char buf[0x92 + 0x40];
    HWND hList = GetDlgItem(hDlg, IDC_INSTALLED);
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    PRINTERINFO *p = GetPrinterInfo(hList, sel);
    if (!p) return;
    wsprintf(buf, "%s%s%s", p->name, szOn, p->port);
    SetDlgItemText(hDlg, IDC_DEFAULT, buf);
    iDefault = sel;
    free(p);
}

/* ------------------------------------------------------------------ seg20:08B0 */
/* the default button becomes id unless the focus is on a push button */
static void SetDefIdIfNotButton(HWND hDlg, int id)
{
    char cls[9];
    HWND hFocus = GetFocus();
    GetClassName(hFocus, cls, sizeof cls);
    if (!lstrcmpi(cls, "button")) {
        WORD st = LOWORD(GetWindowLong(hFocus, GWL_STYLE)) & DLGC_NOTBUTTON_MASK;
        if (st == BS_PUSHBUTTON || st == BS_DEFPUSHBUTTON) return;
    }
    SendMessage(hDlg, DM_SETDEFID, id, 0);
}

/* ------------------------------------------------------------------ the ports (seg20:1679) */
/* seg17:0000 / WNetGetConnection: arch311's printer connections are raw CUPS queues named after
 * their port ("LPT2") with a network URI; the remote name as 3.1 shows it (\\server\share for SMB).
 * q/nq: the queues (lpstat -v), NULL to ask CUPS. */
static BOOL NetGetConnection(LPCSTR port, LPSTR remote, int cb, LPSTR uri, int cbUri, const CupsQueue *q, int nq)
{
    char name[16];
    CupsQueue *own = NULL;
    BOOL found = FALSE;
    if (!q) {
        own = calloc(256, sizeof *own);
        nq = own ? cups_list_queues(own, 256) : 0;
        q = own;
    }
    snprintf(name, sizeof name, "%.*s", (int)strcspn(port, ":"), port);
    for (int i = 0; i < nq && !found; i++) {
        if (lstrcmpi(q[i].name, name)) continue;
        if (!strncmp(q[i].uri, "parallel:", 9) || !strncmp(q[i].uri, "serial:", 7) || !strncmp(q[i].uri, "usb:", 4) ||
            !strncmp(q[i].uri, "file:", 5))
            break; /* a local printer that happens to have the name */
        if (!strncmp(q[i].uri, "smb://", 6) && cb > 2) {
            lstrcpy(remote, "\\\\");
            StrCpyN(remote + 2, q[i].uri + 6, cb - 2);
            for (char *c = remote; *c; c++)
                if (*c == '/') *c = '\\';
        } else
            StrCpyN(remote, q[i].uri, cb);
        if (uri) StrCpyN(uri, q[i].uri, cbUri);
        found = TRUE;
    }
    free(own);
    return found;
}

/* The ports of 3.1's FillPortsList(hwnd, flags): the WIN.INI [ports] keys; flags 2 COMn:, 4 LPTn:,
 * 8 every port, 0x20 the status column ("Local Port", "Local Port Not Present" or the network path).
 * On arch311 LPTn: and COMn: are the PC's parallel and serial devices (CUPS parallel:/dev/lp(n-1),
 * serial:/dev/ttyS(n-1)); the other [ports] keys (EPT:, FILE:, LPT1.DOS ...) have no CUPS device and
 * are left out. With flag 8 CUPS's other devices follow (lpinfo -v), and the printer's own device
 * (curUri) when it is not among them. Returns the count. */
static int BuildPorts(PORTENT *out, int max, WORD flags, LPCSTR curUri)
{
    char keys[0x201], remote[0x40];
    CupsDevice *dev = calloc(64, sizeof *dev);
    CupsQueue *q = calloc(256, sizeof *q);
    int count = 0, ndev = 0, nq = 0;
    if (!dev || !q) goto done;
    if (flags & 0x28) {
        ndev = cups_list_devices(dev, 64);
        if (ndev < 0) ndev = 0; /* lpinfo -v needs CUPS admin rights; without them no device is known */
    }
    if (flags & 0x20) {
        nq = cups_list_queues(q, 256);
        if (nq < 0) nq = 0;
    }
    GetProfileString("ports", NULL, "", keys, 0x200);
    for (char *p = keys; *p && count < max; p += lstrlen(p) + 1) {
        BOOL inc = (IsComPort(p) && (flags & 2)) || (IsLptPort(p) && (flags & 4)) || (flags & 8);
        if (!inc || !(IsComPort(p) || IsLptPort(p))) continue;
        PORTENT *e = &out[count++];
        memset(e, 0, sizeof *e);
        lstrcpy(e->port, p);
        if (IsLptPort(p)) wsprintf(e->uri, "parallel:/dev/lp%d", p[3] - '1');
        else wsprintf(e->uri, "serial:/dev/ttyS%d", p[3] - '1');
        BOOL present = FALSE;
        for (int i = 0; i < ndev; i++)
            if (SameDevice(dev[i].uri, e->uri)) { present = TRUE; lstrcpy(e->uri, dev[i].uri); }
        if (!(flags & 0x20)) continue;
        if (NetGetConnection(p, remote, sizeof remote, e->uri, sizeof e->uri, q, nq)) {
            lstrcpy(e->status, remote);
            continue;
        }
        int cb;
        if (IsComPort(p)) {
            DWORD dw = (DWORD)EscapeCommFunction(p[3] - '1', GETBASEIRQ);
            cb = HIWORD(dw) == 0xFFFF ? 0 : -2;
        } else
            cb = present ? -2 : 0; /* 3.1 opens the port (OpenComm); here: CUPS has the parallel device */
        LoadString(hInstMain, cb == -2 ? 149 : 150, e->status, sizeof e->status); /* "Local Port" (Not Present) */
    }
    if (flags & 8)
        for (int i = 0; i <= ndev && count < max; i++) {
            PORTENT *e = &out[count];
            memset(e, 0, sizeof *e);
            if (i < ndev) {
                lstrcpy(e->uri, dev[i].uri);
                lstrcpy(e->status, dev[i].info[0] ? dev[i].info : dev[i].uri);
            } else if (curUri && *curUri) {
                lstrcpy(e->uri, curUri);
                lstrcpy(e->status, curUri);
            } else
                break;
            int k;
            for (k = 0; k < count && !SameDevice(out[k].uri, e->uri); k++) ;
            if (k < count) continue;
            w16_printer_port(e->uri, e->port, sizeof e->port);
            count++;
        }
done:
    free(dev);
    free(q);
    return count;
}

/* seg20:1679: the ports into a list box (with the status column, flag 0x20; item data = index of
 * connPorts) or a combo box (flag 1, port names only) */
static int FillPortsList(HWND hwnd, WORD flags, LPCSTR curUri)
{
    char line[PI_PORTMAX + 130];
    PORTENT combo[16], *e = (flags & 1) ? combo : connPorts;
    int max = (flags & 1) ? 16 : (int)(sizeof connPorts / sizeof connPorts[0]);
    int n = BuildPorts(e, max, flags, curUri);
    if (!(flags & 1)) nConnPorts = n;
    for (int i = 0; i < n; i++) {
        if (flags & 0x20) wsprintf(line, "%s\t%s", e[i].port, e[i].status);
        else lstrcpy(line, e[i].port);
        int idx = (int)SendMessage(hwnd, (flags & 1) ? CB_ADDSTRING : LB_ADDSTRING, 0, (LPARAM)line);
        if (!(flags & 1) && idx >= 0) SendMessage(hwnd, LB_SETITEMDATA, idx, i);
    }
    return n;
}

/* the device URI of a printer (lpstat -v), "" if CUPS does not know it */
static void GetPrinterUri(LPCSTR name, LPSTR uri, int cb)
{
    CupsQueue *q = calloc(256, sizeof *q);
    uri[0] = 0;
    int n = q ? cups_list_queues(q, 256) : 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(q[i].name, name)) StrCpyN(uri, q[i].uri, cb);
    free(q);
}

/* ------------------------------------------------------------------ seg20:0183 */
/* the selected port's text without its "\t<status>" column */
static int GetListSelText(HWND hList, int idx, LPSTR buf)
{
    *buf = 0;
    if (idx < 0) idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (idx >= 0) SendMessage(hList, LB_GETTEXT, idx, (LPARAM)buf);
    LPSTR t = StrChr(buf, '\t');
    if (t) *t = 0;
    return idx;
}

/* ------------------------------------------------------------------ seg20:004F */
static BOOL InitConnectDialog(HWND hDlg)
{
    char uri[512];
    PRINTERINFO *p = GetPrinterInfo(GetDlgItem(GetParent(hDlg), IDC_INSTALLED), -1);
    if (!p) return FALSE;
    SetDlgItemText(hDlg, IDC_PRINTERNAME, p->name);
    HWND hList = GetDlgItem(hDlg, IDC_PORTS);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    GetPrinterUri(p->name, uri, sizeof uri);
    FillPortsList(hList, 0x2E, uri);
    /* 3.1 selects the first entry starting with the port (LB_SELECTSTRING); CUPS knows the device:
     * its entry, else the first with the port name */
    int i, n = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++) {
        int k = (int)SendMessage(hList, LB_GETITEMDATA, i, 0);
        if (uri[0] && k >= 0 && k < nConnPorts && SameDevice(connPorts[k].uri, uri)) break;
    }
    if (i < n) SendMessage(hList, LB_SETCURSEL, i, 0);
    else SendMessage(hList, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)p->port);
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    SendDlgItemMessage(hDlg, IDC_DNS, EM_LIMITTEXT, 3, 0);
    SendDlgItemMessage(hDlg, IDC_RETRY, EM_LIMITTEXT, 3, 0);
    SetDlgItemInt(hDlg, IDC_DNS, p->dns, TRUE);
    SetDlgItemInt(hDlg, IDC_RETRY, p->retry, TRUE);
    if (!cups_network_available()) EnableWindow(GetDlgItem(hDlg, IDC_NETWORK), FALSE); /* WNetGetCaps(WNNC_NET_TYPE) == 0 */
    free(p);
    return TRUE;
}

/* ------------------------------------------------------------------ seg20:01E7 */
/* -1 when done, a string id to show (0: out of memory), -2 when a CUPS error was shown */
static int ConnectOK(HWND hDlg)
{
    BOOL ok;
    int result = 155; /* "Specify a positive number ..." */
    WORD retry = (WORD)GetDlgItemInt(hDlg, IDC_RETRY, &ok, FALSE);
    if (!ok) { SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_RETRY), 1); return result; }
    WORD dns = (WORD)GetDlgItemInt(hDlg, IDC_DNS, &ok, FALSE);
    if (!ok) { SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_DNS), 1); return result; }

    HWND h300 = GetDlgItem(GetParent(hDlg), IDC_INSTALLED);
    result = 0;
    int sel = (int)SendMessage(h300, LB_GETCURSEL, 0, 0);
    PRINTERINFO *p = GetPrinterInfo(h300, sel);
    if (!p) return 0;
    HWND hPorts = GetDlgItem(hDlg, IDC_PORTS);
    char oldPort[0x9E], newPort[0x9E], uri[512] = "";
    int k = GetListSelText(hPorts, -1, newPort);
    k = k >= 0 ? (int)SendMessage(hPorts, LB_GETITEMDATA, k, 0) : -1;
    if (k >= 0 && k < nConnPorts) {
        lstrcpy(uri, connPorts[k].uri);
        w16_printer_port(uri, newPort, PI_PORTMAX); /* the name the queue will be listed with */
    }
    lstrcpy(oldPort, p->port);
    lstrcpy(p->port, newPort);

    int idx = FindPrinterEntry(h300, p);
    if (idx >= 0 && idx != sel) {
        SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)hPorts, 1);
        result = 34; /* "already installed on this port" */
        goto out;
    }
    /* 3.1 tells the driver at the end (CallDevInstall seg21:037F, result ignored); CUPS is told
     * first, so that a refused change leaves the list as it was */
    char cur[512];
    GetPrinterUri(p->name, cur, sizeof cur);
    if (uri[0] && strcmp(uri, cur) && !cups_set_device(p->name, uri)) {
        CupsError(hDlg);
        result = -2;
        goto out;
    }
    if (p->dns != dns || p->retry != retry) {
        /* GetSpoolJob(0x1F): the spooler re-reads WIN.INI - there is no 3.1 spooler */
        p->dns = dns;
        p->retry = retry;
    }
    SendMessage(h300, WM_SETREDRAW, FALSE, 0);
    idx = AddPrinterEntry(h300, p);
    if (idx < 0) goto out; /* (redraw stays off, as in 3.1) */
    if (idx > sel) idx--;
    else sel++;
    if (iDefault == sel) SetDefaultFromSelection(GetParent(hDlg));
    DeletePrinterEntry(h300, sel);
    SendMessage(h300, LB_SETCURSEL, idx, 0);
    iSel = idx;
    /* posted to this dialog, which ends before it runs: the focus goes back to Connect... (as on 3.11) */
    PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)h300, 1);
    SendMessage(h300, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h300, NULL, TRUE);
    result = -1;
out:
    free(p);
    return result;
}

/* ------------------------------------------------------------------ seg20:0CFE */
static BOOL ConnectDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[0xA0];
    switch (msg) {
    case WM_DESTROY:
        hDlgConnect = NULL;
        return FALSE;
    case WM_INITDIALOG:
        hDlgConnect = hDlg;
        GetProfileString("windows", "DosPrint", "no", buf, 0x9E);
        CheckDlgButton(hDlg, IDC_FASTPRINT, lstrcmpi(buf, "yes") != 0);
        if (!InitConnectDialog(hDlg)) { EndDialog(hDlg, -1); return FALSE; }
        PostMessage(hDlg, WM_COMMAND, IDC_PORTS, W16_CMD_LPARAM(GetDlgItem(hDlg, IDC_PORTS), LBN_SELCHANGE));
        return FALSE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
        ok: {
            int r = ConnectOK(hDlg);
            if (r == -2) return TRUE;
            if (r != -1) { MyMessageBox(hDlg, r, 1, MB_ICONASTERISK); return TRUE; }
            WriteProfileString("windows", "DosPrint", IsDlgButtonChecked(hDlg, IDC_FASTPRINT) ? "no" : "yes");
            /* GetSpoolJob(0x1F, 0): no 3.1 spooler to tell */
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, (int)wParam);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_PORTS:
            if (HIWORD(lParam) == LBN_DBLCLK) goto ok;
            if (HIWORD(lParam) == LBN_SELCHANGE) {
                GetListSelText(GetDlgItem(hDlg, IDC_PORTS), -1, buf);
                BOOL com = IsComPort(buf), lpt = IsLptPort(buf), net = cups_network_available();
                EnableWindow(GetDlgItem(hDlg, IDC_SETTINGS), com);
                EnableWindow(GetDlgItem(hDlg, IDC_DNS), lpt);
                EnableWindow(GetDlgItem(hDlg, IDC_DNSLABEL), lpt);
                EnableWindow(GetDlgItem(hDlg, IDC_RETRY), com || lpt);
                EnableWindow(GetDlgItem(hDlg, IDC_RETRYLABEL), com || lpt);
                EnableWindow(GetDlgItem(hDlg, IDC_NETWORK), lpt && net);
            }
            return TRUE;
        case IDC_SETTINGS: {               /* the Ports applet's settings of a COM port */
            GetListSelText(GetDlgItem(hDlg, IDC_PORTS), -1, buf);
            int n = buf[3] - '0';
            if (n < 1 || n > 4) return TRUE;
            if (DoPortSettings(hDlg, n) == 1) goto refresh;
            return TRUE;
        }
        case IDC_NETWORK:
            GetListSelText(GetDlgItem(hDlg, IDC_PORTS), -1, buf);
            buf[5] = 0;
            PrinterNetworkDialog(hDlg, buf);
            return TRUE;
        case IDC_REFRESH:
        refresh: {
            int sel = (int)SendDlgItemMessage(hDlg, IDC_PORTS, LB_GETCURSEL, 0, 0);
            InitConnectDialog(hDlg);
            SendDlgItemMessage(hDlg, IDC_PORTS, LB_SETCURSEL, sel, 0);
            PostMessage(hDlg, WM_COMMAND, IDC_PORTS, W16_CMD_LPARAM(GetDlgItem(hDlg, IDC_PORTS), LBN_SELCHANGE));
            return TRUE;
        }
        default:
            return FALSE;
        }
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg21:00B2 */
/* -1 when done; 3.1 refuses (string 159) while Print Manager runs - there is none on arch311 yet */
static int TogglePrintManager(HWND hDlg)
{
    (void)hDlg;
    fSpooler = !fSpooler;
    WriteProfileString("windows", "spooler", fSpooler ? "yes" : "no");
    /* GetSpoolJob(0x1F, 0) */
    return -1;
}

/* ------------------------------------------------------------------ seg21:01A0 */
/* Setup...: 3.1 loads the printer driver and runs its own ExtDeviceMode (or DeviceMode) dialog.
 * arch311 shows COMMDLG's Print Setup for the queue and keeps paper and orientation with lpoptions. */
static const struct { short dm; const char *media; } Papers[] = {
    {DMPAPER_LETTER, "Letter"}, {DMPAPER_LEGAL, "Legal"}, {DMPAPER_EXECUTIVE, "Executive"}, {DMPAPER_A4, "A4"},
    {DMPAPER_A5, "A5"}, {DMPAPER_B5, "B5"}, {DMPAPER_ENV_10, "Env10"}, {DMPAPER_ENV_DL, "EnvDL"},
};

static HGLOBAL MakeDevNames(const PRINTERINFO *p, BOOL fDefault)
{
    size_t n = sizeof(DEVNAMES) + strlen(p->driver) + strlen(p->name) + strlen(p->port) + 3;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n);
    DEVNAMES *dn = h ? GlobalLock(h) : NULL;
    if (!dn) return h;
    char *s = (char *)(dn + 1);
    dn->wDriverOffset = (WORD)(s - (char *)dn);
    s = stpcpy(s, p->driver) + 1;
    dn->wDeviceOffset = (WORD)(s - (char *)dn);
    s = stpcpy(s, p->name) + 1;
    dn->wOutputOffset = (WORD)(s - (char *)dn);
    strcpy(s, p->port);
    dn->wDefault = fDefault ? DN_DEFAULTPRN : 0;
    GlobalUnlock(h);
    return h;
}

static BOOL PrinterSetup(HWND hDlg)
{
    BOOL result = FALSE;
    char val[64];
    HWND hList = GetDlgItem(hDlg, IDC_INSTALLED);
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    PRINTERINFO *p = GetPrinterInfo(hList, sel);
    if (!p) { MyMessageBox(hDlg, 0, 1, MB_ICONASTERISK); return FALSE; }
    HourGlass(TRUE);
    PRINTDLG pd;
    memset(&pd, 0, sizeof pd);
    pd.lStructSize = sizeof pd;
    pd.hwndOwner = hDlg;
    pd.Flags = PD_PRINTSETUP;
    pd.hDevNames = MakeDevNames(p, sel == iDefault);
    pd.hDevMode = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DEVMODE));
    DEVMODE *dm = pd.hDevMode ? GlobalLock(pd.hDevMode) : NULL;
    if (dm) {
        StrCpyN(dm->dmDeviceName, p->name, sizeof dm->dmDeviceName);
        dm->dmSpecVersion = 0x30A;
        dm->dmSize = sizeof *dm;
        dm->dmFields = DM_ORIENTATION | DM_PAPERSIZE;
        dm->dmOrientation = cups_get_option(p->name, "orientation-requested", val, sizeof val) && !strcmp(val, "4")
                                ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
        dm->dmPaperSize = DMPAPER_LETTER;
        if (cups_get_option(p->name, "media", val, sizeof val))
            for (size_t i = 0; i < sizeof Papers / sizeof Papers[0]; i++)
                if (!lstrcmpi(val, Papers[i].media)) dm->dmPaperSize = Papers[i].dm;
        GlobalUnlock(pd.hDevMode);
    }
    HourGlass(FALSE);
    if (PrintDlg(&pd)) {
        DEVNAMES *dn = GlobalLock(pd.hDevNames);
        dm = GlobalLock(pd.hDevMode);
        if (dn && dm) {
            const char *opts[2];
            char media[40], orient[40];
            int n = 0;
            for (size_t i = 0; i < sizeof Papers / sizeof Papers[0]; i++)
                if (dm->dmPaperSize == Papers[i].dm) {
                    wsprintf(media, "media=%s", Papers[i].media);
                    opts[n++] = media;
                }
            wsprintf(orient, "orientation-requested=%d", dm->dmOrientation == DMORIENT_LANDSCAPE ? 4 : 3);
            opts[n++] = orient;
            HourGlass(TRUE);
            result = cups_set_options((const char *)dn + dn->wDeviceOffset, opts, n);
            HourGlass(FALSE);
            if (!result) CupsError(hDlg);
        }
        if (dn) GlobalUnlock(pd.hDevNames);
        if (dm) GlobalUnlock(pd.hDevMode);
    }
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    free(p);
    return result;
}

/* the queues as COMMDLG lists them: the same (simulated) CUPS the applet sees */
static int CupsPrinterEnum(char (*name)[64], char (*port)[64], int max, int *def)
{
    CupsQueue *q = calloc((size_t)max, sizeof *q);
    char d[128] = "";
    int n = q ? cups_list_queues(q, max) : 0;
    *def = -1;
    if (n < 0) n = 0;
    cups_get_default(d, sizeof d);
    for (int i = 0; i < n; i++) {
        StrCpyN(name[i], q[i].name, 64);
        w16_printer_port(q[i].uri, port[i], 64);
        if (!strcmp(q[i].name, d)) *def = i;
    }
    free(q);
    return n;
}

/* ------------------------------------------------------------------ seg21:0426 */
/* the installed printers: CUPS's queues, with the driver and timeouts their WIN.INI entries have
 * ([PrinterPorts] "driver,port,dns,retry,...", or [devices] "driver,port,...") */
static BOOL InitPrintersDialog(HWND hDlg)
{
    char probe[0x200], val[0x200], dev[0x13E], spool[0x9E];
    PRINTERINFO *p;
    /* 3.1 notes the disk of pscript.drv in CONTROL.INF and reads [ports] here (asking about a
     * missing section, string 165); CUPS installs need neither */
    LoadString(hInstMain, 161, szSrcPath, sizeof szSrcPath); /* "A:\" */
    lstrcpy(szInstallPath, szSrcPath);

    hListInstalled = GetDlgItem(hDlg, IDC_INSTALLED);
    wDnsDefault = (WORD)GetProfileInt("windows", "DeviceNotSelectedTimeout", 15);
    wRetryDefault = (WORD)GetProfileInt("windows", "TransmissionRetryTimeout", 45);

    LPCSTR sect = "PrinterPorts";
    GetProfileString(sect, NULL, "CRC", probe, sizeof probe);
    BOOL isDevices = !lstrcmp(probe, "CRC"); /* no [PrinterPorts] */
    if (isDevices) sect = "devices";

    CupsQueue *q = calloc(256, sizeof *q);
    int nq = q ? cups_list_queues(q, 256) : -1;
    if (nq < 0) {
        CupsError(hDlg); /* e.g. "lpstat is not installed." */
        nq = 0;
    }
    BOOL any = FALSE;
    for (int i = 0; i < nq; i++) {
        char port[PI_PORTMAX];
        w16_printer_port(q[i].uri, port, sizeof port);
        p = malloc(sizeof *p + strlen(q[i].name) + 1 + PI_PORTMAX);
        if (!p) break;
        lstrcpy(p->text, q[i].name);
        p->name = p->text;
        p->port = p->text + lstrlen(p->text) + 1;
        lstrcpy(p->port, port);
        lstrcpy(p->driver, "CUPS");
        p->dns = wDnsDefault;
        p->retry = wRetryDefault;
        GetProfileString(sect, q[i].name, "", val, sizeof val);
        LPSTR s = val, c = StrChr(s, ',');
        if (c) *c++ = 0;
        TrimSpaces(s);
        if (*s) StrCpyN(p->driver, s, sizeof p->driver);
        while (c) { /* the timeouts of the port the queue is on */
            s = c;
            c = StrChr(s, ',');
            if (c) *c++ = 0;
            TrimSpaces(s);
            BOOL mine = !lstrcmpi(s, port);
            if (!isDevices && c) {
                s = c;
                c = StrChr(s, ',');
                if (c) *c++ = 0;
                TrimSpaces(s);
                WORD d = StrToUInt(s);
                WORD r = p->retry;
                if (c) {
                    s = c;
                    c = StrChr(s, ',');
                    if (c) *c++ = 0;
                    TrimSpaces(s);
                    r = StrToUInt(s);
                }
                if (mine) { p->dns = d; p->retry = r; }
            }
        }
        any = TRUE;
        if (FindPrinterEntry(hListInstalled, p) < 0) AddPrinterEntry(hListInstalled, p);
        free(p);
    }
    free(q);

    if (!any) {
        EnableWindow(GetDlgItem(hDlg, IDC_CONNECT), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_SETUP), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_SETDEFAULT), FALSE);
    nodef:
        SetNoDefaultPrinter(hDlg);
        SendMessage(hListInstalled, LB_SETCURSEL, 0, 0);
        goto spooler;
    }
    {
        /* the default printer: CUPS's, else (CUPS has none) WIN.INI's device=Name,Driver,Port */
        int cnt = (int)SendMessage(hListInstalled, LB_GETCOUNT, 0, 0);
        char *c1 = NULL, *c2 = NULL;
        if (cups_get_default(szCupsDefault, sizeof szCupsDefault) <= 0) {
            szCupsDefault[0] = 0;
            GetProfileString("windows", "device", "", dev, sizeof dev);
            c1 = StrChr(dev, ',');
            if (!c1) goto nodef;
            c2 = StrChr(c1 + 1, ',');
            if (!c2) goto nodef;
            *c1++ = 0;
            *c2++ = 0;
            TrimSpaces(dev);
            TrimSpaces(c1);
            TrimSpaces(c2);
        }
        for (int i = 0; i < cnt; i++) {
            PRINTERINFO *pp = GetPrinterInfo(hListInstalled, i);
            if (!pp) goto nodef;
            BOOL found = szCupsDefault[0] ? !strcmp(szCupsDefault, pp->name)
                                          : !lstrcmpi(dev, pp->name) && !lstrcmpi(c1, pp->driver) && !lstrcmpi(c2, pp->port);
            free(pp);
            if (found) {
                SendMessage(hListInstalled, LB_SETCURSEL, i, 0);
                SetDefaultFromSelection(hDlg);
                break;
            }
        } /* not found: iDefault and the static stay as they are (3.1) */
    }
spooler:
    GetProfileString("windows", "spooler", "no", spool, sizeof spool);
    fSpooler = !lstrcmpi(spool, "yes");
    CheckDlgButton(hDlg, IDC_PRINTMAN, fSpooler);
    GetWindowRect(GetDlgItem(hDlg, IDC_LISTLABEL), &rcList);
    GetWindowRect(hDlg, &rcDlg);
    /* without the List of Printers until Add >> */
    MoveWindow(hDlg, rcDlg.left, rcDlg.top, rcDlg.right - rcDlg.left, rcList.top - rcDlg.top, FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_PRINTERLIST), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL), FALSE);
    return TRUE;
}

/* ------------------------------------------------------------------ seg21:0A98 */
/* Cancel/Close: [PrinterPorts], [devices] and [windows] device= rewritten from the list, as 3.1
 * does, and CUPS's default printer set when it changed */
static BOOL SavePrinterSettings(HWND hDlg)
{
    int result = 0, count, i;
    char curName[0x182], devBuf[0x13E];
    char *hPP = malloc(0x13C), *hDev = malloc(0x13C);
    size_t cbPP = 0x13C, cbDev = 0x13C;
    PRINTERINFO *p = NULL;
    if (!hPP || !hDev) { OutOfMemory(hDlg); goto free_both; }

    if (IsDlgButtonChecked(hDlg, IDC_PRINTMAN) != (UINT)fSpooler) {
        result = TogglePrintManager(hDlg);
        if (result != -1) {
            PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_PRINTMAN), 1);
            goto free_both;
        }
    }
    WriteProfileString("PrinterPorts", NULL, NULL);
    WriteProfileString("devices", NULL, NULL);
    WriteProfileString("windows", "device", NULL);
    result = 0;
    count = (int)SendDlgItemMessage(hDlg, IDC_INSTALLED, LB_GETCOUNT, 0, 0);
    if (count > 0 && !(p = GetPrinterInfo(hListInstalled, 0))) goto free_both;
    for (i = 0; i < count;) {
        LPSTR dot = StrChr(p->driver, '.');
        if (dot) *dot = 0; /* "HPPCL.DRV" -> "HPPCL" */
        lstrcpy(curName, p->name);
        lstrcpy(hPP, p->driver);
        lstrcpy(hDev, hPP);
        char *pEndPP = hPP + lstrlen(hPP), *pEndDev = hDev + lstrlen(hDev);
        for (;;) { /* one line for each printer name */
            if ((size_t)(pEndPP - hPP) > cbPP - 0x9E) {
                size_t o = (size_t)(pEndPP - hPP);
                char *n = realloc(hPP, cbPP += 0x9E);
                if (!n) { OutOfMemory(hDlg); goto free_both; }
                hPP = n;
                pEndPP = n + o;
            }
            if ((size_t)(pEndDev - hDev) > cbDev - 0x9E) {
                size_t o = (size_t)(pEndDev - hDev);
                char *n = realloc(hDev, cbDev += 0x9E);
                if (!n) { OutOfMemory(hDlg); goto free_both; }
                hDev = n;
                pEndDev = n + o;
            }
            wsprintf(pEndPP, ",%s,%d,%d", p->port, p->dns, p->retry);
            pEndPP += lstrlen(pEndPP);
            wsprintf(pEndDev, ",%s", p->port);
            pEndDev += lstrlen(pEndDev);
            if (i == iDefault) {
                dot = StrChr(p->driver, '.');
                if (dot) *dot = 0;
                wsprintf(devBuf, "%s,%s,%s", curName, p->driver, p->port);
                WriteProfileString("windows", "device", devBuf);
                /* arch311: the CUPS default too (the user's, lpoptions: no rights needed) */
                if (strcmp(szCupsDefault, curName)) {
                    if (cups_set_default(curName)) lstrcpy(szCupsDefault, curName);
                    else CupsError(hDlg);
                }
            }
            free(p);
            p = NULL;
            if (++i >= count) break;
            if (!(p = GetPrinterInfo(hListInstalled, i))) goto free_both;
            if (lstrcmp(curName, p->name)) break; /* the next name: write this line */
        }
        WriteProfileString("PrinterPorts", curName, hPP);
        WriteProfileString("devices", curName, hDev);
    }
    BroadcastWinIniChange(2); /* devices */
    BroadcastWinIniChange(0); /* windows */
    /* GetSpoolJob(0x1F, 0) */
    result = -1;
free_both:
    free(p);
    free(hDev);
    free(hPP);
    if (result == -1) return TRUE;
    MyMessageBox(hDlg, result, 1, MB_ICONASTERISK); /* result 0: out of memory */
    return FALSE;
}

/* ------------------------------------------------------------------ seg22:0000 */
/* a List of Printers entry; FALSE (and an empty one) for none */
static BOOL GetPrinterListEntry(HWND hList, int idx, PRINTERLINE *line)
{
    LRESULT dw = SendMessage(hList, LB_GETITEMDATA, idx, 0);
    if (dw != LB_ERR && dw) {
        *line = *(PRINTERLINE *)dw;
        return TRUE;
    }
    memset(line, 0, sizeof *line);
    return FALSE;
}

/* ------------------------------------------------------------------ seg21:0E01 */
/* 3.1 AddInfPrinterCB adds an [io.device] line: its description as the item, the line as the data;
 * here one CUPS driver. A duplicate of the next item (only possible in a sorted list) is dropped. */
static int AddInfPrinterCB(HWND hList, const PRINTERLINE *line)
{
    char nb[0xA0];
    int idx = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)line->model);
    if (idx == -1) return 151; /* "Not enough memory available to add printers." */
    if (SendMessage(hList, LB_GETTEXT, idx + 1, (LPARAM)nb) != LB_ERR && !lstrcmpi(line->model, nb)) {
        SendMessage(hList, LB_DELETESTRING, idx, 0);
        return 0;
    }
    PRINTERLINE *d = malloc(sizeof *d);
    if (!d) { SendMessage(hList, LB_DELETESTRING, idx, 0); return 151; }
    *d = *line;
    SendMessage(hList, LB_SETITEMDATA, idx, (LPARAM)d);
    return 0;
}

/* ------------------------------------------------------------------ seg21:0FBF */
/* the List of Printers: CUPS's drivers (3.1: CONTROL.INF [io.device]); the count, or -string id */
static int FillPrinterList(HWND hList)
{
    CupsModel *m;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    int n = cups_list_models(&m);
    if (n < 0) {
        free(m);
        return -1;
    }
    SendMessage(hList, WM_SETREDRAW, FALSE, 0); /* (not in 3.1: CUPS can list thousands) */
    int r = 0;
    for (int i = 0; i < n && !r; i++) {
        PRINTERLINE line;
        memset(&line, 0, sizeof line);
        snprintf(line.ppd, sizeof line.ppd, "%s", m[i].ppd);
        snprintf(line.model, sizeof line.model, "%.159s", m[i].model);
        r = AddInfPrinterCB(hList, &line);
    }
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    free(m);
    if (r) return -r;
    return (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
}

/* ------------------------------------------------------------------ seg21:103E */
static void FreePrinterListData(HWND hList)
{
    int n = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        LRESULT dw = SendMessage(hList, LB_GETITEMDATA, i, 0);
        if (dw != LB_ERR && dw) free((void *)dw);
        SendMessage(hList, LB_SETITEMDATA, i, 0);
    }
}

/* ------------------------------------------------------------------ seg20:0506 */
static void ExpandAddPrinters(HWND hDlg)
{
    char text[0xA8];
    HourGlass(TRUE);
    HWND hList = GetDlgItem(hDlg, IDC_PRINTERLIST);
    int n = FillPrinterList(hList);
    if (n < 0) {
        HourGlass(FALSE);
        if (n == -1) CupsError(hDlg); /* lpinfo -m failed (3.1: 354, the INF is damaged) */
        else MyMessageBox(hDlg, -n, 1, MB_ICONASTERISK);
        return;
    }
    /* "Install Unlisted or Updated Printer" first: string 153's quoted field */
    PRINTERLINE *u = calloc(1, sizeof *u);
    if (u) {
        char s153[0x65];
        LoadString(hInstMain, 153, s153, sizeof s153);
        GetQuotedField(text, s153);
        u->isUnlisted = TRUE;
        lstrcpy(u->model, text);
        if (SendMessage(hList, LB_INSERTSTRING, 0, (LPARAM)text) != LB_ERR) SendMessage(hList, LB_SETITEMDATA, 0, (LPARAM)u);
        else free(u);
    }
    EnableWindow(hList, TRUE);
    SendMessage(hList, LB_SETCURSEL, 0, 0);
    /* (3.1 reads SETUP.INF [fonts] here, for the printer fonts it installs) */
    UpdateWindow(hDlg);
    SetWindowPos(hDlg, NULL, 0, 0, rcDlg.right - rcDlg.left, rcDlg.bottom - rcDlg.top, SWP_NOMOVE | SWP_NOZORDER);
    rcList.left = rcDlg.left;
    rcList.right = rcDlg.right;
    rcList.bottom = rcDlg.bottom;
    ScreenToClient(hDlg, (POINT *)&rcList.left);
    ScreenToClient(hDlg, (POINT *)&rcList.right);
    InvalidateRect(hDlg, &rcList, FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_ADD), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL), TRUE);
    PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)hList, 1);
    UpdateWindow(hDlg);
    HourGlass(FALSE);
}

/* ------------------------------------------------------------------ seg20:0730 */
static void RemovePrinter(HWND hDlg)
{
    char text[0xA2];
    iSel = (int)SendMessage(hListInstalled, LB_GETCURSEL, 0, 0);
    SendMessage(hListInstalled, LB_GETTEXT, iSel, (LPARAM)text);
    PRINTERINFO *p = GetPrinterInfo(hListInstalled, iSel);
    if (!p) { MyMessageBox(hDlg, 0, 1, MB_ICONASTERISK); return; }
    if (ConfirmBox(hDlg, p->name, 237)) { /* "Are you sure you want to remove the %s printer?" */
        /* 3.1 deletes the entry and then tells the driver (CallDevInstall with no new port) when no
         * other entry has the text; the queue goes first here, so a refusal keeps the entry */
        if (!cups_delete_queue(p->name)) {
            CupsError(hDlg);
            free(p);
            return;
        }
        DeletePrinterEntry(hListInstalled, iSel);
        if (SendMessage(hListInstalled, LB_GETCOUNT, 0, 0) != 0) {
            if (iSel > 0) iSel--;
            SendMessage(hListInstalled, LB_SETCURSEL, iSel, 0);
        } else {
            PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDOK), 1);
            EnableWindow(GetDlgItem(hDlg, IDC_CONNECT), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_SETUP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_SETDEFAULT), FALSE);
            SetNoDefaultPrinter(hDlg);
        }
        SetCancelButtonText(hDlg);
    }
    free(p);
}

/* ------------------------------------------------------------------ seg22:080C */
/* 1 not installed, 2 a printer of this name is installed, 0 an error was shown (3.1 allows 16
 * printer names) */
static int CheckPrinterName(HWND hDlg, LPCSTR name)
{
    int result = 0;
    HWND hList = GetDlgItem(hDlg, IDC_INSTALLED);
    int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    if (count == 0) return 1;
    int i = count - 1;
    count++;
    PRINTERINFO *pCur = GetPrinterInfo(hList, i), *pPrev;
    if (!pCur) goto show;
    for (;;) {
        if (!lstrcmp(pCur->name, name)) { result = -2; goto done; }
        for (;;) {
            if (--i < 0) { result = count > 16 ? 158 : -1; goto done; } /* 158: "maximum number of printers" */
            if (!(pPrev = GetPrinterInfo(hList, i))) goto done;
            if (!lstrcmp(pPrev->name, pCur->name)) { free(pPrev); count--; continue; } /* same name, another port */
            free(pCur);
            pCur = pPrev;
            break;
        }
    }
done:
    free(pCur);
show:
    if (result >= 0) { MyMessageBox(hDlg, result, 1, MB_ICONASTERISK); return 0; }
    return -result;
}

/* ------------------------------------------------------------------ seg22:047E */
/* the printer's name: "Description", or the part in brackets of "Model [Name]" */
static BOOL ParseDriverLine(const PRINTERLINE *line, LPSTR desc)
{
    char tmp[0x81];
    snprintf(tmp, sizeof tmp, "%s", line->model);
    if (!tmp[0]) return FALSE;
    LPSTR br = StrChr(tmp, '[');
    if (br) {
        LPSTR e = StrChr(br + 1, ']');
        if (e) *e = 0;
        lstrcpy(desc, br + 1);
    } else
        lstrcpy(desc, tmp);
    lstrcpy(szFile, line->ppd);
    return TRUE;
}

/* ------------------------------------------------------------------ seg22:058B */
static BOOL CurNewDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char a[0xC8 * 2], b[0xC8];
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        LoadString(hInstMain, idCurNewText, a, 0xC8);     /* "This printer is already connected to a port. " */
        LoadString(hInstMain, idCurNewText + 1, b, 0xC8); /* "Do you want to use the current port ..." */
        lstrcat(a, b);
        SetDlgItemText(hDlg, 1, a);
        LoadString(hInstMain, idCurNewText + 2, a, 0xC8); /* "Install Printer" */
        SetWindowText(hDlg, a);
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDD_HELP) { CPHelp(hDlg); return TRUE; }
        if (wParam > IDD_HELP) return TRUE;
        if (wParam == IDCANCEL || wParam == 6 || wParam == 7) EndDialog(hDlg, (int)wParam); /* Cancel, New, Current */
        return TRUE;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* a queue name not taken yet: the name, else name_1, name_2 ... (CUPS names a queue once) */
static void UniqueQueueName(HWND hDlg, LPSTR name, int cb)
{
    CupsQueue *q = calloc(256, sizeof *q);
    int nq = q ? cups_list_queues(q, 256) : 0;
    HWND hList = GetDlgItem(hDlg, IDC_INSTALLED);
    char base[128], cand[140];
    snprintf(base, sizeof base, "%s", name);
    for (int k = 0;; k++) {
        if (k) snprintf(cand, sizeof cand, "%.120s_%d", base, k);
        else snprintf(cand, sizeof cand, "%s", base);
        BOOL used = FALSE;
        for (int i = 0; i < nq && !used; i++) used = !lstrcmpi(q[i].name, cand);
        int n = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n && !used; i++) {
            PRINTERINFO *p = GetPrinterInfo(hList, i);
            if (p) used = !lstrcmpi(p->name, cand);
            free(p);
        }
        if (!used) break;
    }
    StrCpyN(name, cand, cb);
    free(q);
}

/* ------------------------------------------------------------------ seg22:092A */
/* Install...: the PRINTERINFO of the new entry, or NULL. 3.1 picks the first [ports] port where the
 * printer is not installed yet, copies the driver files (prompting for disks) and its fonts; here
 * lpadmin makes the queue on the first such port of the Connect list. */
static PRINTERINFO *InstallSelectedPrinter(HWND hDlg)
{
    int ret = -1;
    BOOL keep = TRUE;
    PRINTERINFO *pi = NULL;
    PORTENT *ports = NULL;
    PRINTERLINE line;
    char desc[0x81], name[128];
    HWND h301 = GetDlgItem(hDlg, IDC_PRINTERLIST);
    int sel = (int)SendMessage(h301, LB_GETCURSEL, 0, 0);
    GetPrinterListEntry(h301, sel, &line);
    BOOL isUnlisted = line.isUnlisted;
    if (isUnlisted) {
        lpUnlistedLine = &line;
        if (DoDialogBoxParam(IDD_UNLISTED, hDlg, UnlistedDlgProc, HC_UNLISTED, 0) != 1) goto finish;
        /* line is now the PPD file chosen */
    }
    if (!ParseDriverLine(&line, desc)) {
        MyMessageBox(hDlg, 162, 1, MB_ICONASTERISK, (LPSTR)line.ppd); /* "... a problem with the %s file" */
        goto finish;
    }
    cups_queue_name(desc, name, sizeof name);

    /* 3.1 checks the name after choosing the port: a listed printer that is installed already goes
     * on its next port under the same name. A CUPS queue name exists once, so the printer gets a
     * name of its own first (name_1 ...), and an unlisted one may replace the driver of the queue
     * that has the name (Current) */
    int c = CheckPrinterName(hDlg, name);
    if (c == 0) goto finish;
    if (c == 2) {
        if (isUnlisted) {
            idCurNewText = 166;
            int rr = DoDialogBoxParam(IDD_CURNEW, hDlg, CurNewDlgProc, HC_CURNEW, 0);
            if (rr == 7) { /* Current: the queue keeps its port and gets the new driver; no new entry */
                keep = FALSE;
                if (!cups_set_ppd(name, line.ppd)) { CupsError(hDlg); goto finish; }
                ret = 1;
                goto finish;
            }
            if (rr != 6) goto finish; /* Cancel */
        }
        UniqueQueueName(hDlg, name, sizeof name);
        if (!CheckPrinterName(hDlg, name)) goto finish;
    }

    /* the first port the printer is not installed on */
    ports = calloc(96, sizeof *ports);
    if (!ports) { MyMessageBox(hDlg, 151, 1, MB_ICONASTERISK); goto finish; }
    int nports = BuildPorts(ports, 96, 0x0E, NULL);
    for (int portIdx = 0;; portIdx++) {
        if (portIdx >= nports) { MyMessageBox(hDlg, 33, 1, MB_ICONASTERISK); goto finish; } /* no more ports */
        PORTENT *e = &ports[portIdx];
        pi = malloc(sizeof *pi + strlen(name) + 1 + PI_PORTMAX);
        if (!pi) { MyMessageBox(hDlg, 151, 1, MB_ICONASTERISK); goto finish; }
        lstrcpy(pi->text, name);
        pi->name = pi->text;
        pi->port = pi->text + lstrlen(name) + 1;
        w16_printer_port(e->uri, pi->port, PI_PORTMAX);
        if (FindPrinterEntry(GetDlgItem(hDlg, IDC_INSTALLED), pi) >= 0) {
            free(pi);
            pi = NULL;
            continue;
        }
        /* 3.1 doubles the retry timeout for PSCRIPT.DRV printers; a CUPS driver is not a file */
        pi->retry = wRetryDefault;
        pi->dns = wDnsDefault;
        lstrcpy(pi->driver, "CUPS");
        HourGlass(TRUE);
        BOOL ok = cups_add_queue(name, line.ppd, line.isFile, e->uri);
        HourGlass(FALSE);
        if (!ok) { CupsError(hDlg); goto finish; }
        break;
    }
    /* (3.1: InstallFiles copies the driver and [io.dependent] files, InstallPrinterFonts the
     * printer's screen fonts from SETUP.INF [fonts]; CUPS needs neither) */
    ret = 1;
finish:
    free(ports);
    if (isUnlisted) lstrcpy(szInstallPath, szSrcPath);
    if (pi && (!keep || ret < 0)) {
        free(pi);
        pi = NULL;
    }
    return pi;
}

/* ------------------------------------------------------------------ seg20:10B4 */
/* (the automatic installs of CPlApplet message 100 are not ported) */
static void PostInit(HWND hDlg)
{
    if (SendMessage(hListInstalled, LB_GETCOUNT, 0, 0) == 0) PostMessage(hDlg, WM_COMMAND, IDC_ADD, 0);
    ShowWindow(hDlg, SW_SHOWNORMAL);
}

/* ------------------------------------------------------------------ seg20:1302 */
static BOOL PrintersDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HourGlass(TRUE);
        ShowWindow(GetDlgItem(hDlg, IDC_PRINTMAN), SW_SHOW);
        EnableWindow(GetDlgItem(hDlg, IDC_PRINTMAN), TRUE);
        w16_set_printer_enum(CupsPrinterEnum);
        BOOL ok = InitPrintersDialog(hDlg);
        HourGlass(FALSE);
        if (!ok) { EndDialog(hDlg, 0); return TRUE; }
        hDlgPrinters = hDlg;
        PostInit(hDlg);
        return TRUE;
    }
    case WM_DESTROY:
        FreePrinterListData(GetDlgItem(hDlg, IDC_PRINTERLIST));
        for (int i = (int)SendMessage(hListInstalled, LB_GETCOUNT, 0, 0) - 1; i >= 0; i--)
            DeleteAtom(HIWORD((DWORD)SendMessage(hListInstalled, LB_GETITEMDATA, i, 0)));
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case 0:
            return TRUE;
        case IDOK:
        case IDCANCEL: { /* Cancel / Close: there is nothing to cancel */
            HourGlass(TRUE);
            BOOL r = SavePrinterSettings(hDlg);
            HourGlass(FALSE);
            if (!r) return TRUE;
            EndDialog(hDlg, 1);
            hDlgPrinters = NULL;
            return TRUE;
        }
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_INSTALLED:
            if (HIWORD(lParam) != LBN_DBLCLK) return TRUE;
            /* fall through */
        case IDC_SETDEFAULT:
            SetDefaultFromSelection(hDlg);
            SetCancelButtonText(hDlg);
            return TRUE;
        case IDC_ADD:
            ExpandAddPrinters(hDlg);
            return TRUE;
        case IDC_CONNECT:
            DoDialogBoxParam(IDD_CONNECT, hDlg, ConnectDlgProc, HC_CONNECT, 0);
            SetCancelButtonText(hDlg);
            return TRUE;
        case IDC_SETUP:
            PrinterSetup(hDlg);
            SetCancelButtonText(hDlg);
            return TRUE;
        case IDC_REMOVE:
            RemovePrinter(hDlg);
            return TRUE;
        case IDC_PRINTMAN:
            SetCancelButtonText(hDlg);
            return TRUE;
        case IDC_PRINTERLIST:
            switch (HIWORD(lParam)) {
            case LBN_DBLCLK:
                goto install;
            case LBN_SETFOCUS:
                SendMessage(hDlg, DM_SETDEFID, IDC_INSTALL, 0);
                return TRUE;
            case LBN_KILLFOCUS:
                SetDefIdIfNotButton(hDlg, IDOK);
                return TRUE;
            }
            return FALSE;
        case IDC_INSTALL:
        install: {
            HourGlass(TRUE);
            PRINTERINFO *p = InstallSelectedPrinter(hDlg);
            if (p) {
                if (AddPrinterEntry(hListInstalled, p) >= 0) {
                    /* (3.1 tells the driver its port here: CallDevInstall; lpadmin did) */
                    EnableWindow(GetDlgItem(hDlg, IDC_CONNECT), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_SETUP), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_SETDEFAULT), TRUE);
                    if (SendMessage(hListInstalled, LB_GETCOUNT, 0, 0) == 1) SetDefaultFromSelection(hDlg);
                }
                free(p);
            }
            HourGlass(FALSE);
            SetCancelButtonText(hDlg);
            return TRUE;
        }
        default:
            return TRUE;
        }
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg3:0782 (dispatcher entry 1) */
void PrintersRun(HWND hwnd)
{
    if (hDlgPrinters) { /* already open: say so and switch to it (string 36) */
        HWND top = hDlgPrinters, t;
        while ((t = GetWindow(top, GW_OWNER)) != NULL) top = t;
        int len = GetWindowTextLength(top) + 1;
        char *s = malloc((size_t)len);
        if (!s) { OutOfMemory(hwnd); return; }
        GetWindowText(top, s, len);
        if (MyMessageBox(hwnd, 36, 1, MB_OKCANCEL | MB_ICONEXCLAMATION, (LPSTR)s) == IDOK) {
            SetWindowPos(top, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            SetActiveWindow(top); /* 3.1: its last active popup */
        }
        free(s);
        return;
    }
    LoadString(hInstMain, 8, szOn, sizeof szOn); /* " on " (seg3:013A loads it at CPL_INIT) */
    DialogBox(hInstMain, MAKEINTRESOURCE(IDD_PRINTERS), hwnd, PrintersDlgProc);
}

/* ------------------------------------------------------------------ seg1:014C */
/* the path's drive exists (3.1: OpenFile(OF_PARSE) on "X:a"), after painting what is waiting */
static BOOL IsValidDrivePath(LPCSTR path)
{
    MSG m;
    BOOL ok = path[1] != ':' || !w16_drive_root(path[0], NULL, 0);
    while (PeekMessage(&m, NULL, WM_PAINT, WM_PAINT, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return ok;
}

/* ------------------------------------------------------------------ seg1:01BC */
/* the Browse dialog (MAIN.CPL template 38 on COMMDLG's GetOpenFileName): directories and drives
 * only; the hidden file list's first match (else szFile) goes to the hidden name edit, so that OK
 * returns the directory */
static UINT BrowseHookProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[0xC8];
    if (msg == WM_INITDIALOG) {
        OPENFILENAME *o = (OPENFILENAME *)lParam;
        GetDlgItemText(o->hwndOwner, IDC_DRVPROMPT, buf, sizeof buf);
        SetDlgItemText(hDlg, IDC_BROWSEPROMPT, buf);
        goto post;
    }
    if (msg == WM_COMMAND) {
        switch (wParam) {
        case IDOK: case 0x461: case 0x471:
        post:
            PostMessage(hDlg, WM_COMMAND, 0x501, 0);
            return 0;
        case IDC_BROWSEHELP:
            CPHelp(hDlg);
            return 1;
        case 0x501: {
            HWND hl = GetDlgItem(hDlg, 0x460);
            if (SendMessage(hl, LB_GETCOUNT, 0, 0) != 0) {
                SendMessage(hl, LB_SETCURSEL, 0, 0);
                SendMessage(hDlg, WM_COMMAND, 0x460, W16_CMD_LPARAM(hl, LBN_SELCHANGE));
            } else
                SetDlgItemText(hDlg, 0x480, szFile);
            return 0;
        }
        }
        return 0;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return 1; }
    /* (3.1 also checks the file on commdlg_FileNameOK: libw16's COMMDLG does not send it) */
    return 0;
}

/* ------------------------------------------------------------------ seg1:03E5 */
static BOOL InstallDriverDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hDlg, IDC_DRVPROMPT, (LPCSTR)lParam);
        SendDlgItemMessage(hDlg, IDC_DRVPATH, EM_LIMITTEXT, 0x8A, 0);
        SetDlgItemText(hDlg, IDC_DRVPATH, szInstallPath);
        SendDlgItemMessage(hDlg, IDC_DRVPATH, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDC_DRVBROWSE) {
            char filter[0x14], file[0x9E];
            static const char drvFilter[] = "a\0";
            /* the files Browse looks for: PPD files (3.1: "*.dr?;*.wp?" for unlisted drivers, else
             * the file asked for, any extension) */
            if (fOemBrowse) lstrcpy(filter, "*.ppd");
            else {
                StrCpyN(filter, szFile, sizeof filter - 2);
                LPSTR dot = StrChr(filter, '.');
                if (!dot) dot = filter + lstrlen(filter);
                lstrcpy(dot, ".*");
            }
            char filters[0x20];
            memset(filters, 0, sizeof filters);
            memcpy(filters, drvFilter, 2);
            lstrcpy(filters + 2, filter);
            file[0] = 0;
            GetDlgItemText(hDlg, IDC_DRVPATH, szInstallPath, 0x9E);
            DWORD save = dwContext;
            dwContext = HC_BROWSE;
            OPENFILENAME ofn;
            memset(&ofn, 0, sizeof ofn);
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = hDlg;
            ofn.hInstance = hInstMain;
            ofn.lpstrFilter = filters;
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof file;
            ofn.lpstrInitialDir = szInstallPath;
            ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_SHOWHELP | OFN_ENABLEHOOK | OFN_ENABLETEMPLATE;
            ofn.lCustData = (LPARAM)hDlg;
            ofn.lpfnHook = BrowseHookProc;
            ofn.lpTemplateName = MAKEINTRESOURCE(IDD_BROWSE);
            BOOL ok = GetOpenFileName(&ofn);
            dwContext = save;
            UpdateWindow(hDlg);
            if (ok) {
                file[ofn.nFileOffset] = 0; /* keep the directory */
                SetDlgItemText(hDlg, IDC_DRVPATH, file);
            }
            return FALSE;
        }
        if (wParam > IDC_DRVBROWSE) return FALSE;
        if (wParam == IDOK) {
            GetDlgItemText(hDlg, IDC_DRVPATH, szInstallPath, 0x9E);
            TrimSpaces(szInstallPath);
            AddBackslash(szInstallPath);
            EndDialog(hDlg, 1);
            return TRUE;
        }
        if (wParam == IDCANCEL) { EndDialog(hDlg, 0); return TRUE; }
        if (wParam == IDD_HELP) { CPHelp(hDlg); return TRUE; }
        return FALSE;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg21:108A / 1198 */
/* 3.1 reads the printer name of a driver (*.DR?: its module description "DDRV ...") or of a
 * PostScript description (*.WP?); a CUPS driver file is a PPD (plain or gzipped): its *NickName,
 * else its *ModelName. FALSE when the file is no printer driver. */
static BOOL HandlePpdFile(const char *host, HWND hList)
{
    char buf[512], nick[0xA0] = "", model[0xA0] = "";
    PRINTERLINE line;
    int n = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) /* listed already */
        if (GetPrinterListEntry(hList, i, &line) && !strcmp(line.ppd, host)) return TRUE;
    gzFile f = gzopen(host, "rb");
    if (!f) return FALSE;
    for (int k = 0; k < 4000 && gzgets(f, buf, sizeof buf); k++) {
        char *d = !strncmp(buf, "*NickName:", 10) ? nick : !strncmp(buf, "*ModelName:", 11) ? model : NULL;
        if (d && !*d) GetQuotedField(d, buf);
        if (*nick) break;
    }
    gzclose(f);
    memset(&line, 0, sizeof line);
    StrCpyN(line.model, *nick ? nick : model, sizeof line.model);
    if (!line.model[0]) return FALSE;
    StrCpyN(line.ppd, host, sizeof line.ppd);
    line.isFile = TRUE;
    return AddInfPrinterCB(hList, &line) == 0;
}

/* ------------------------------------------------------------------ seg21:1261 */
/* the driver files in szInstallPath (3.1: *.DR? and *.WP?): here *.ppd and *.ppd.gz */
static BOOL ScanDriverFiles(HWND hList)
{
    char host[1024];
    BOOL any = FALSE;
    if (w16_dos_to_host(szInstallPath, host, sizeof host)) return FALSE;
    DIR *d = opendir(host);
    if (!d) return FALSE;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        BOOL ppd = (n > 4 && !strcasecmp(e->d_name + n - 4, ".ppd")) || (n > 7 && !strcasecmp(e->d_name + n - 7, ".ppd.gz"));
        if (!ppd) continue;
        char path[1400];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", host, e->d_name);
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        if (HandlePpdFile(path, hList)) any = TRUE;
    }
    closedir(d);
    return any;
}

/* ------------------------------------------------------------------ seg21:136F */
/* asks for the disk (Install Driver) until it has printer drivers; FALSE when cancelled. 3.1 reads
 * the disk's OEMSETUP.INF [io.device] first and scans for driver files only without it; CUPS
 * driver disks have PPD files. */
static BOOL InstallUnlistedInit(HWND hDlg, HWND hList)
{
    char prompt[0x9E];
    for (;;) {
        LoadString(hInstMain, 161, szInstallPath, 0xC8); /* "A:\" */
        LoadString(hInstMain, 152, prompt, sizeof prompt); /* "Insert unlisted, updated, or vendor-provided ..." */
        lstrcpy(szFile, "oemsetup.inf");
        fOemBrowse = TRUE;
        int r = DoDialogBoxParam(IDD_INSTALLDRV, hDlg, InstallDriverDlgProc, HC_INSTALLDRV, (LPARAM)prompt);
        fOemBrowse = FALSE;
        if (r == 0 || r == -1) return FALSE;
        if (!IsValidDrivePath(szInstallPath)) continue;
        HourGlass(TRUE);
        if (!ScanDriverFiles(hList)) continue; /* nothing there: ask again (the hourglass stays, as in 3.1) */
        HourGlass(FALSE);
        return TRUE;
    }
}

/* ------------------------------------------------------------------ seg22:0666 */
static BOOL UnlistedDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hList;
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        hList = GetDlgItem(hDlg, IDC_UNLISTEDLIST);
        if (!InstallUnlistedInit(hDlg, hList)) EndDialog(hDlg, IDCANCEL);
        SendMessage(hList, LB_SETCURSEL, 0, 0);
        HourGlass(FALSE);
        return TRUE;
    case WM_DESTROY:
        FreePrinterListData(GetDlgItem(hDlg, IDC_UNLISTEDLIST));
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_UNLISTEDLIST:
            if (HIWORD(lParam) != LBN_DBLCLK) return FALSE;
            wParam = IDOK;
            /* fall through */
        case IDOK: {
            hList = GetDlgItem(hDlg, IDC_UNLISTEDLIST);
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel == -1) wParam = IDCANCEL;
            else GetPrinterListEntry(hList, sel, lpUnlistedLine);
        }
            /* fall through */
        case IDCANCEL:
            EndDialog(hDlg, (int)wParam);
            return FALSE;
        }
        return FALSE;
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ================================================================== network printer connections */
/* seg17:0044 / 0096: the network error text (WN_NET_ERROR: the network's own message, here the CUPS
 * tool's) in a stop box */
static void ShowNetError(HWND hwnd, int err)
{
    char buf[0xA0];
    if (err == 2) StrCpyN(buf, cups_last_error(), sizeof buf);
    else LoadString(hInstMain, err + 357, buf, 0x9F);
    MessageBox(hwnd, buf, NULL, MB_ICONHAND);
}

/* seg17:0000: 1 connected, 2 not (WN_NOT_CONNECTED etc.), 0 another error */
static int GetConnStatus(LPCSTR port)
{
    char remote[0x80];
    return NetGetConnection(port, remote, sizeof remote, NULL, 0, NULL, 0) ? 1 : 2;
}

/* seg17:00C1 */
static BOOL FillConnectionsList(HWND hDlg, HWND hList)
{
    char buf[0x8C], remote[0x84];
    LoadString(hInstMain, 37, buf, sizeof buf); /* "ERROR\t" */
    int n = lstrlen(buf), tab = n * 6;
    SendMessage(hList, LB_SETTABSTOPS, 1, (LPARAM)&tab);
    EnableWindow(GetDlgItem(hDlg, IDC_DISCONNECT), FALSE);
    BOOL disc = FALSE;
    HWND hCombo = GetDlgItem(hDlg, IDC_NETPORT);
    int cnt = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        LPSTR port = buf + n;
        SendMessage(hCombo, CB_GETLBTEXT, i, (LPARAM)port);
        if (!NetGetConnection(port, remote, sizeof remote, NULL, 0, NULL, 0)) {
            /* a connection WIN.INI [Network] remembers but that is not there */
            LPSTR c = StrChr(port, ':');
            if (!c) continue;
            *c = 0;
            int got = GetProfileString("Network", port, "", remote, sizeof remote);
            *c = ':';
            if (!got) continue;
            port = buf;
        }
        lstrcat(port, "   ");
        lstrcat(port, remote);
        int r2 = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)port);
        if (r2 == -1 || r2 == -2) continue;
        SendMessage(hCombo, CB_DELETESTRING, i, 0); /* the port is taken */
        i--;
        cnt--;
        disc = TRUE;
    }
    if (disc) EnableWindow(GetDlgItem(hDlg, IDC_DISCONNECT), TRUE);
    SendMessage(hList, LB_SETCURSEL, 0, 0);
    return TRUE;
}

/* seg17:0297 */
static BOOL RefreshConnections(HWND hDlg)
{
    BOOL ok = FALSE;
    HWND hCombo = GetDlgItem(hDlg, IDC_NETPORT), hList = GetDlgItem(hDlg, IDC_CONNLIST);
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    if (FillPortsList(hCombo, 5, NULL) == 0 || !FillConnectionsList(hDlg, hList))
        OutOfMemory(hDlg);
    else {
        PostMessage(hDlg, WM_COMMAND, IDC_NETPATH, W16_CMD_LPARAM(GetDlgItem(hDlg, IDC_NETPATH), EN_UPDATE));
        EnableWindow(GetDlgItem(hDlg, IDC_PREVIOUS),
                     GetPrivateProfileString("Previous", NULL, "", szPrevPath, 0x7F, CONTROL_INI) != 0);
        ok = TRUE;
    }
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    return ok;
}

/* seg17:0399: "LPT2" from "LPT2:   \\server\share" (or "ERROR\tLPT2: ...") */
static void GetConnListPort(HWND hList, int idx, LPSTR buf)
{
    SendMessage(hList, LB_GETTEXT, idx, (LPARAM)buf);
    LPSTR c = StrChr(buf, ':');
    if (c) *c = 0;
    LPSTR t = StrChr(buf, '\t');
    if (t) memmove(buf, t + 1, strlen(t + 1) + 1);
}

/* WNetAddConnection: the network path as a CUPS device URI (\\server\share -> smb://server/share,
 * a URI as typed) printed to by a raw queue named after the port. The password is not kept: in a
 * device URI any user could read it (UNTESTED against a real share). 0 or a WN_ error. */
static int NetAddConnection(LPCSTR path, LPCSTR pwd, LPCSTR port)
{
    char uri[0x90];
    (void)pwd;
    if (path[0] == '\\' && path[1] == '\\' && path[2]) {
        snprintf(uri, sizeof uri, "smb://%s", path + 2);
        for (char *c = uri; *c; c++) if (*c == '\\') *c = '/';
    } else if (strstr(path, "://"))
        snprintf(uri, sizeof uri, "%s", path);
    else
        return 0x32; /* WN_BAD_NETNAME: "Unable to connect to the specified server ..." */
    return cups_add_queue(port, NULL, 0, uri) ? 0 : 2 /* WN_NET_ERROR: the tool's message */;
}

/* seg17:03F8 */
static BOOL NetConnDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char port[0x20], path[0x80], pwd[0x20];
    int r;
    switch (msg) {
    case WM_INITDIALOG:
        hNetDlg = hDlg;
        fNetChanged = FALSE;
        if (!cups_network_available()) { MyMessageBox(hDlg, 370, 1, MB_ICONASTERISK); goto end12; }
        if (!RefreshConnections(hDlg)) goto end12;
        r = (int)SendDlgItemMessage(hDlg, IDC_NETPORT, CB_FINDSTRING, (WPARAM)-1, lParam);
        if (r >= 0) SendDlgItemMessage(hDlg, IDC_NETPORT, CB_SETCURSEL, r, 0);
        EnableWindow(GetDlgItem(hDlg, IDC_NETBROWSE), FALSE); /* no network browser (WNNC_CON_BrowseDialog) */
        SendDlgItemMessage(hDlg, IDC_NETPATH, EM_LIMITTEXT, 0x7F, 0);
        SendDlgItemMessage(hDlg, IDC_NETPWD, EM_LIMITTEXT, 0x1F, 0);
        return TRUE;
    end12:
        EndDialog(hDlg, 12); /* WN_CANCELLED */
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case 0:
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(hDlg, fNetChanged ? 0 : 12);
            hNetDlg = NULL;
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_CONNLIST:
            if (HIWORD(lParam) == LBN_SETFOCUS) SendMessage(hDlg, DM_SETDEFID, IDOK, 0);
            return TRUE;
        case IDC_NETPORT:
            if (HIWORD(lParam) == CBN_SETFOCUS) goto defConnect;
            return TRUE;
        case IDC_NETPATH:
            if (HIWORD(lParam) == EN_UPDATE) {
                if (SendDlgItemMessage(hDlg, IDC_NETPATH, WM_GETTEXTLENGTH, 0, 0) != 0 &&
                    (long)SendDlgItemMessage(hDlg, IDC_NETPORT, CB_GETCOUNT, 0, 0) > 0) {
                    EnableWindow(GetDlgItem(hDlg, IDC_NETCONNECT), TRUE);
                    SetDefIdIfNotButton(hDlg, IDC_NETCONNECT);
                } else {
                    SetDefIdIfNotButton(hDlg, IDOK);
                    EnableWindow(GetDlgItem(hDlg, IDC_NETCONNECT), FALSE);
                }
                return TRUE;
            }
            if (HIWORD(lParam) == EN_SETFOCUS) goto defConnect;
            return TRUE;
        case IDC_NETPWD:
            if (HIWORD(lParam) == EN_SETFOCUS) goto defConnect;
            return TRUE;
        defConnect:
            SendMessage(hDlg, DM_SETDEFID, IsWindowEnabled(GetDlgItem(hDlg, IDC_NETCONNECT)) ? IDC_NETCONNECT : IDOK, 0);
            return TRUE;
        case IDC_NETCONNECT: {
            HWND hCombo = GetDlgItem(hDlg, IDC_NETPORT);
            int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            port[0] = 0;
            SendMessage(hCombo, CB_GETLBTEXT, sel, (LPARAM)port);
            if (port[0] && port[lstrlen(port) - 1] == ':') port[lstrlen(port) - 1] = 0;
            GetDlgItemText(hDlg, IDC_NETPATH, path, sizeof path);
            GetDlgItemText(hDlg, IDC_NETPWD, pwd, sizeof pwd);
            HourGlass(TRUE);
            r = NetAddConnection(path, pwd, port);
            HourGlass(FALSE);
            memset(pwd, 0, sizeof pwd);
            if (r) goto showerr;
            fNetChanged = TRUE;
            SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDOK), 1);
            /* 3.1 writes [Network] PORT=path unless the network restores its connections itself or
             * Shift is down; CUPS keeps its queues */
            WritePrivateProfileString("Previous", path, "", CONTROL_INI);
            if (!RefreshConnections(hDlg)) EndDialog(hDlg, 0);
            return TRUE;
        }
        showerr:
            if (r == 12) return TRUE; /* WN_CANCELLED */
            ShowNetError(hDlg, r);
            return TRUE;
        case IDC_DISCONNECT: {
            HWND hList = GetDlgItem(hDlg, IDC_CONNLIST);
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            char text[0x100];
            GetConnListPort(hList, sel, text);
            StrCpyN(port, text, sizeof port);
            int st = GetConnStatus(port);
            r = st == 1 ? (cups_delete_queue(port) ? 0 : 2) : 0x30; /* WN_NOT_CONNECTED */
            WriteProfileString("Network", port, NULL);
            /* (3.1 asks before cutting a connection with files still printing, string 371) */
            if (!(r == 0 || r == 0x30) && st != 2) {
                ShowNetError(hDlg, r);
                return TRUE;
            }
            fNetChanged = TRUE;
            if (!RefreshConnections(hDlg)) { EndDialog(hDlg, 0); return TRUE; }
            SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDC_NETPATH), 1);
            if ((int)SendMessage(hList, LB_GETCOUNT, 0, 0) <= sel) sel--;
            SendMessage(hList, LB_SETCURSEL, sel, 0);
            return TRUE;
        }
        case IDC_PREVIOUS:
            if (DoDialogBoxParam(IDD_PREVCONN, hDlg, PrevConnDlgProc, HC_PREVCONN, 0) > 0)
                SetDlgItemText(hDlg, IDC_NETPATH, szPrevPath);
            EnableWindow(GetDlgItem(hDlg, IDC_PREVIOUS),
                         GetPrivateProfileString("Previous", NULL, "", szPrevPath, 0x7F, CONTROL_INI) != 0);
            return TRUE;
        default:
            return TRUE;
        }
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* seg17:0932: the paths connected before (CONTROL.INI [Previous]) */
static BOOL PrevConnDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hList = GetDlgItem(hDlg, IDC_PREVLIST);
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        int size = 0x200;
        char *h;
        for (;;) {
            h = calloc(1, (size_t)size);
            if (!h) { EndDialog(hDlg, 0); return TRUE; }
            int n = GetPrivateProfileString("Previous", NULL, "", h, size, CONTROL_INI);
            if (n - size != -2) break;
            free(h);
            size += 0x200;
        }
        for (char *p = h; *p; p += lstrlen(p) + 1) SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)p);
        free(h);
        SendMessage(hList, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            goto select;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDD_HELP:
            CPHelp(hDlg);
            return TRUE;
        case IDC_PREVLIST:
            if (HIWORD(lParam) == LBN_DBLCLK) goto select;
            return FALSE;
        case IDC_PREVDELETE: {
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel == -1) return TRUE;
            SendMessage(hList, LB_GETTEXT, sel, (LPARAM)szPrevPath);
            SendMessage(hList, LB_DELETESTRING, sel, 0);
            SendMessage(hList, LB_SETCURSEL, 0, 0);
            WritePrivateProfileString("Previous", szPrevPath, NULL, CONTROL_INI);
            return TRUE;
        }
        }
        return FALSE;
    select: {
        szPrevPath[0] = 0;
        int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (sel != -1) SendMessage(hList, LB_GETTEXT, sel, (LPARAM)szPrevPath);
        EndDialog(hDlg, 1);
        return TRUE;
    }
    }
    if (msg == wHelpMessage) { CPHelp(hDlg); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ seg3:0010 */
/* Network... in Connect: the printer connections dialog (3.1 uses the network's own dialog when it
 * has one; CPlApplet message 101 also runs this, not ported); afterwards the Connect list is
 * refreshed */
static void PrinterNetworkDialog(HWND hwnd, LPSTR lpPort)
{
    int r = 1;
    if (hNetDlg) { /* open already: say so and switch to it (string 35) */
        HWND top = hNetDlg, t;
        while ((t = GetWindow(top, GW_OWNER)) != NULL) top = t;
        int len = GetWindowTextLength(top) + 1;
        char *s = malloc((size_t)len);
        if (!s) { OutOfMemory(hwnd); goto end; }
        GetWindowText(top, s, len);
        if (MyMessageBox(hwnd, 35, 1, MB_OKCANCEL | MB_ICONEXCLAMATION, (LPSTR)s) == IDOK) {
            SetWindowPos(top, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            SetActiveWindow(top);
        }
        free(s);
        goto end;
    }
    r = DoDialogBoxParam(IDD_NETCONN, hwnd, NetConnDlgProc, HC_NETCONN, (LPARAM)lpPort);
    hNetDlg = NULL;
end:
    if (r == 0 && hDlgConnect) PostMessage(hDlgConnect, WM_COMMAND, IDC_REFRESH, 0);
}
