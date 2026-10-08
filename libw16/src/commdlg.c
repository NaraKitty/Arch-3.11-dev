/* COMMDLG.DLL: File Open / Save As, Find / Replace, Print / Print Setup, ChooseFont.
 *
 * File Open / Save As (GetOpenFileName, GetSaveFileName, GetFileTitle) and Find / Replace (FindText,
 * ReplaceText) are ported function by function from Windows 3.11's COMMDLG.DLL ("Common Windows
 * Dialogs, Ver. 3.10"): seg2 (the file dialogs), seg5 (Find / Replace), seg6-seg8 (LibMain,
 * CommDlgExtendedError, LoadAlterBitmap) and the seg1 DOS helpers seg2 calls; every function names
 * its seg:offset. The dialog templates (1536, 1537 with OFN_ALLOWMULTISELECT, 1540 Find, 1541
 * Replace), the strings and the folder/drive bitmap 576 come from the user's ripped COMMDLG.DLL.
 *
 * Where arch311 differs from 3.1 (nothing else does):
 * - The DOS calls (drives, current directory, find first/next, attributes, the unique-file test,
 *   OpenFile) go through libw16's DOS path layer (sys.c), which maps drive letters onto Linux folders.
 * - Drive types (GetDriveType, the CD-ROM and RAM-disk tests COMMDLG makes itself) come from the file
 *   system each drive's folder is on, in /proc/mounts (w16_drive_class); volume labels are libw16's
 *   (the drive folder's name).
 * - The network: USER's WNet functions answer "not supported" (sys.c) and the code below does with
 *   that answer what 3.1 does: no "Network..." button, a network drive listed as "x: ".
 * - 64 bits: WM_COMMAND's lParam carries libw16's control slot (W16_CMD_HWND, W16_CMD_LPARAM); window
 *   procedures and the instance data are pointers (w16_SetWindowPtr, w16_GetClassPtr, malloc).
 * 3.1's visible quirks are kept (lower-cased names, grey names in Save As, the private 0x8000 combo
 * notification, the property block, the internal bits in lpOFN->Flags, MatchSpec's own wildcards,
 * 8.3 names); the 3.1 bugs that would crash or corrupt memory are fixed, each marked "3.1 BUG".
 *
 * Print / Print Setup further down are libw16's own (not ported yet); ChooseFont is a stub. */
#include "w16int.h"
#include "commdlg.h"
#include <ctype.h>

/* control ids used by the COMMDLG templates (dlgs.h) */
enum {
    psh1 = 1024, psh2 = 1025, psh14 = 1037, pshHelp = 1038, chx1 = 1040, chx2 = 1041,
    grp1 = 1072, rad1 = 1056, rad2 = 1057, rad3 = 1058, rad4 = 1059,
    stc1 = 1088, stc2 = 1089, stc3 = 1090, stc4 = 1091, stc5 = 1092,
    lst1 = 1120, lst2 = 1121, cmb1 = 1136, cmb2 = 1137, cmb3 = 1138,
    edt1 = 1152, edt2 = 1153, edt3 = 1154, ico1 = 1084,
};
enum { DLG_OPEN = 1536, DLG_MULTIOPEN = 1537, DLG_PRINT = 1538, DLG_SETUP = 1539, DLG_FIND = 1540, DLG_REPLACE = 1541 };

/* COMMDLG string ids used by Print / Print Setup */
enum {
    IDS_PRINTERLABEL = 1089, IDS_PRNONPORT = 1090,
    IDS_FROMLOW = 1104, IDS_FROMHIGH = 1105, IDS_TOLOW = 1106, IDS_TOHIGH = 1107, IDS_FROMBAD = 1108,
    IDS_TOBAD = 1109, IDS_NOPAGES = 1110, IDS_COPIESEMPTY = 1111, IDS_COPIESBAD = 1112, IDS_COPIESZERO = 1113,
    IDS_NODEFPRN = 1114, IDS_QUALITY = 1072,
};

/* ================================================================== shared */
static DWORD cd_err; /* DS:02AC g_dwExtError: CDERR_, FNERR_, FRERR_, PDERR_ codes, 0 = none */

/* seg6:01EA: the last extended error; never reset by this call */
DWORD CommDlgExtendedError(void) { return cd_err; }

static HINSTANCE commdlg(void) { return w16_system_module("COMMDLG.DLL"); }

/* Print / Print Setup's helpers */
static void cd_str(UINT id, char *buf, int cb)
{
    if (!LoadString(commdlg(), id, buf, cb)) buf[0] = 0;
}

static void cd_msg(HWND owner, UINT id, LPCSTR arg, UINT type)
{
    char fmt[300], text[600], cap[80];
    cd_str(id, fmt, sizeof fmt);
    if (strstr(fmt, "%c")) snprintf(text, sizeof text, fmt, arg && arg[0] ? toupper((unsigned char)arg[0]) : '?');
    else snprintf(text, sizeof text, fmt, arg ? arg : "");
    GetWindowText(owner, cap, sizeof cap);
    MessageBox(owner, text, cap, type);
}

static int template_ok(HINSTANCE m, int id)
{
    if (m && w16_find_res(m, MAKEINTRESOURCE(id), RT_DIALOG)) return 1;
    cd_err = m ? CDERR_FINDRESFAILURE : CDERR_LOADRESFAILURE;
    return 0;
}

/* ------------------------------------------------------------------ seg7 / seg8: LibMain */
static HINSTANCE g_hInst;                       /* DS:02AA */
static HDC g_hdcMem;                            /* DS:02B0: holds the folder/drive bitmap while in use */
static HBITMAP g_hbmMemOld;                     /* DS:02B2 */
static UINT g_msgHelp;                          /* DS:0D78 commdlg_help */
static UINT g_msgLBSelChanged;                  /* DS:1002 commdlg_LBSelChangedNotify */
static UINT g_msgShareViolation;                /* DS:0D76 commdlg_ShareViolation */
static UINT g_msgFileNameOK;                    /* DS:0D70 commdlg_FileNameOK */
static WORD g_wWinVer;                          /* DS:0F90 */
static BOOL g_fMousePresent;                    /* DS:1000 */
static BOOL g_fWLO;                             /* DS:0F8A */
static WORD g_wNetCaps;                         /* DS:103E WNetGetCaps(WNNC_NET_TYPE) */
static WORD g_wWFWGDlgStack, g_wNetDlgStack, g_wDuelNetStack; /* DS:0F92, 0D84, 0FF8 */

/* seg8:0000 LibEntry -> seg7:0190 LibMain with seg7:011A InitMessages. 3.1 runs it when the DLL is
 * loaded; here at the first call of a dialog. FALSE when the user's COMMDLG.DLL is not there. */
static BOOL LibMain(void)
{
    static int done;
    if (done) return g_hInst != NULL;
    done = 1;
    if (!(g_hInst = commdlg())) return FALSE;
    /* InitMessages (seg7:011A); the colour dialog's two messages belong to its own port */
    g_fMousePresent = GetSystemMetrics(SM_MOUSEPRESENT);
    g_wWinVer = 0x030A; /* GetVersion() with its bytes swapped: 3.10 on 3.11 */
    g_msgLBSelChanged = RegisterWindowMessage(LBSELCHSTRING);
    g_msgShareViolation = RegisterWindowMessage(SHAREVISTRING);
    g_msgFileNameOK = RegisterWindowMessage(FILEOKSTRING);
    g_fWLO = FALSE; /* GetWinFlags() & WF_WLO: never OS/2 */
    HDC hdc = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm = CreateCompatibleBitmap(g_hdcMem, 1, 1);
    g_hbmMemOld = SelectObject(g_hdcMem, hbm);
    SelectObject(g_hdcMem, g_hbmMemOld);
    DeleteObject(hbm);
    ReleaseDC(NULL, hdc);
    /* MakeObjectShared (seg6:0164, GDI's SetObjectOwner): nothing to share between tasks here */
    g_msgHelp = RegisterWindowMessage(HELPMSGSTRING);
    g_wNetCaps = WNetGetCaps(WNNC_NET_TYPE);
    if (!(g_wWFWGDlgStack = GetPrivateProfileInt("network", "WFWGDlgStack", 0x0C00, "SYSTEM.INI"))) g_wWFWGDlgStack = 0x0C00;
    if (!(g_wNetDlgStack = GetPrivateProfileInt("network", "NetDlgStack", 0x1400, "SYSTEM.INI"))) g_wNetDlgStack = 0x1400;
    if (!(g_wDuelNetStack = GetPrivateProfileInt("network", "DuelNetStack", 0x1C00, "SYSTEM.INI"))) g_wDuelNetStack = 0x1C00;
    return TRUE;
}

/* seg6:0202 StackAvail: the free bytes on the application's stack. A 64-bit Linux stack is never
 * short of the few kilobytes the network dialogs want. */
static WORD StackAvail(void) { return 0xFFFF; }

/* ================================================================== File Open / Save As (seg2) */
#define PROP_INST MAKEINTATOM(0xA000) /* the property holding a dialog's instance data */

/* ParseFile results (LOWORD) */
enum {
    PARSE_DIRECTORYNAME = -1, PARSE_INVALIDDRIVE = -2, PARSE_INVALIDPERIOD = -3, PARSE_INVALIDCHAR = -5,
    PARSE_INVALIDDIRCHAR = -6, PARSE_INVALIDSPACE = -7, PARSE_EXTENTIONTOOLONG = -8, PARSE_FILETOOLONG = -9,
    PARSE_EMPTYSTRING = -10, PARSE_WILDCARDINDIR = -11, PARSE_WILDCARDINFILE = -12, PARSE_INVALIDNETPATH = -13,
};
/* string ids */
enum {
    IDS_FILEEXISTS = 257, IDS_SAVEAS = 385, IDS_SAVETYPE = 386, IDS_NODRIVE = 387, IDS_CANTREAD = 388,
    IDS_WRONGDISK = 389, IDS_FILENOTFOUND = 391, IDS_PATHNOTFOUND = 392, IDS_BADNAME = 393, IDS_INUSE = 394,
    IDS_NETACCESS = 395, IDS_READONLY = 396, IDS_CRITERR = 397, IDS_DEVICENAME = 398, IDS_WRITEPROT = 399,
    IDS_DISKFULL = 400, IDS_TOOMANYOPEN = 401, IDS_CREATEPROMPT = 402, IDS_CREATENOMODIFY = 403,
    IDS_CANTSELDRIVE = 404, IDS_CLOSE = 1037, IDS_NETWORK = 1120,
};
/* the internal bits COMMDLG keeps in lpOFN->Flags while the dialog runs */
#define FLAG_TYPEDROPPED 0x10000000L  /* the file type list is open: a selection waits */
#define FLAG_DRIVEDROPPED 0x20000000L /* the drive list is open */
#define FLAG_DIRPENDING 0x40000000L   /* lst2's selection is not the current directory */

typedef struct {               /* FILEINST: LocalAlloc(LMEM_ZEROINIT, 0x40D) on 3.1 */
    LPOPENFILENAME lpOFN;      /* +000 */
    char szInitDir[0x101];     /* +004 the directory at the start (OFN_NOCHANGEDIR) */
    char szPath[0x100];        /* +105 work buffer: typed text, current directory, UNC path */
    char szSpec[0x100];        /* +205 the spec lst1 is filled with */
    char szDefSpec[0x106];     /* +305 the last spec typed; goes back into lpstrCustomFilter */
    WORD cDirs;                /* +40B items at the top of lst2: root .. current directory */
} FILEINST, *PFILEINST;

static WNDPROC g_lpfnLBClassProc;               /* DS:0024 */
static WNDPROC g_lpfnBtnClassProc;              /* DS:0028 */
static WORD g_cBmpRef;                          /* DS:002C */
static COLORREF g_rgbWindow, g_rgbHighlight, g_rgbWindowText, g_rgbHighlightText, g_rgbGrayText; /* DS:002E-003E */
static COLORREF g_rgbBmpWindow, g_rgbBmpHighlight; /* DS:0042, 0046 */
static LPOFNHOOKPROC g_lpfnHook;                /* DS:004A: the hook while there is no property */
static int g_cxBmp, g_cyBmp;                    /* DS:004E, 0050 */
static int g_cyItem;                            /* DS:0052: lst2 / cmb2 item height, set once */
static BOOL g_fOKFromLB;                        /* DS:0054 */
static WORD g_fNoDraw;                          /* DS:0056 */
static HBITMAP g_hbmDirDrives;                  /* DS:0058 */
static WORD g_wNetDlgCaps;                      /* DS:006A */
static WORD (*g_pfnNetLastConn)(WORD, WORD *);  /* DS:006C network driver ordinal 148 */
static BOOL g_fNetProcQueried;                  /* DS:0070 */
static LPSTR g_lpTokNext;                       /* DS:02D0 */
static char g_szTmp1[0x102], g_szTmp2[0x102], g_szBuf1[0x102], g_szTmp4[0x102]; /* DS:02D4, 03D6, 04D8, 05DA */
static char g_szMsgFmt[0x1C0], g_szMsg[0x1C0];  /* DS:06DC, 089C */
static int g_cyFont;                            /* DS:0A5C */
static WORD g_wCurDirErr;                       /* DS:0D30 */
static BOOL g_fInCreatePrompt;                  /* DS:0D60 */
static BOOL g_fFirstActivate;                   /* DS:0D7E */
static BOOL g_fInitializing;                    /* DS:0F8E */
static BOOL g_fHourGlass;                       /* DS:0FFC */
static W16FINDDATA g_DTA;                       /* DS:1008: the find buffer */

static DWORD ParseFile(LPSTR lpszFile);
static int ChangeDir(LPSTR lpszDir, BOOL fAnsi);
static BOOL ChangeDriveTo(char chDrive);
static BOOL GetCurDirNear(LPSTR psz);
static void FillDriveCombo(HWND hDlg, int id);
static void SelectDriveInCombo(HWND hDlg, int id, char chDrive);
static int SetSpecAndDir(PFILEINST p, HWND hDlg, LPSTR lpSpec);
static void AppendExt(LPSTR lpDst, LPCSTR lpExt, BOOL fStar);
static BOOL OverwritePrompt(HWND hDlg, LPSTR lpszFile);
static BOOL IsCDROM(int drv);
static void LowerAscii(LPSTR lpsz);
static int FillFilterCombo(HWND hDlg, LPCSTR lpFilter);
static int GetDriveBitmapIndex(int drv, WORD wType);

static int pf_file(DWORD dw) { return (SHORT)LOWORD(dw); } /* ParseFile's file offset or PARSE_ code */
static int pf_ext(DWORD dw) { return HIWORD(dw); }        /* its extension offset */

/* ------------------------------------------------------------------ seg1: the DOS helpers seg2 calls */
static int s_dosErr; /* what GetExtendedErrorDOS reports: the last DOS error of these helpers */

/* lstrcpy between overlapping parts of one string, as 3.1's forward copy does it (seg1:2BCA MemMove
 * where COMMDLG uses it) */
static void StrShift(LPSTR dst, LPCSTR src) { memmove(dst, src, strlen(src) + 1); }

/* seg1:2C43 (AH=19h): 0 = A: */
static int GetCurrentDrive(void)
{
    char cwd[300];
    w16_getcwd(cwd, sizeof cwd);
    return toupper((unsigned char)cwd[0]) - 'A';
}

/* seg1:2C5D (AH=0Eh): a drive that does not exist leaves the current one */
static int SetCurrentDrive(int drv)
{
    char d[3] = {(char)('A' + drv), ':', 0};
    if (drv >= 0 && drv < 26) w16_chdir(d);
    return 0;
}

/* seg1:2C82 (AH=3Bh): 0 or the DOS error */
static int ChDirDOS(LPCSTR dir) { return w16_chdir(dir) ? (s_dosErr = 3) : 0; }

/* seg1:2CA9 (AH=47h): "X:\PATH" of the current drive, upper case; 0, or the DOS error when the drive
 * cannot be read (here: the folder behind the current directory is not there). DOS keeps 63
 * characters after "X:\"; libw16's directories can be deeper and are kept whole. */
static int GetCurDir(LPSTR buf, WORD cb)
{
    char cwd[300];
    w16_getcwd(cwd, sizeof cwd);
    int a = w16_dos_attr(cwd);
    if (a < 0 || !(a & 0x10)) return s_dosErr = 0x15; /* drive not ready */
    if (cb) snprintf(buf, cb, "%s", cwd);
    return 0;
}

/* seg1:2D6B (AX=4E00h) into the DTA g_DTA (seg1:2D17 SetDTA / 2D47 RestoreDTA have nothing to do
 * here); 0, 0x12 when nothing matches, or the DOS error */
static int FindFirst(LPCSTR spec, WORD attr)
{
    char dir[300] = "";
    w16_find_close(&g_DTA);
    if (w16_find_first(spec, attr & 0xFF, &g_DTA) == 0) return 0;
    const char *bs = strrchr(spec, '\\');
    if (!bs && spec[0] && spec[1] == ':') bs = spec + 1;
    if (bs) snprintf(dir, sizeof dir, "%.*s", (int)(bs - spec + 1), spec);
    int a = w16_dos_attr(dir[0] ? dir : ".");
    return s_dosErr = (a >= 0 && (a & 0x10)) ? 0x12 : 3;
}

/* seg1:2D95 (AX=4F00h) */
static int FindNext(void) { return w16_find_next(&g_DTA) == 0 ? 0 : (s_dosErr = 0x12); }

/* seg1:2DB7: the label of drive drv (0 = A:), in brackets if asked; "" when there is none */
static void GetVolumeLabel(int drv, LPSTR out, BOOL fBrackets)
{
    char lab[16];
    w16_volume_label((char)('A' + drv), lab, sizeof lab);
    if (fBrackets && lab[0]) sprintf(out, "[%s]", lab);
    else strcpy(out, lab);
}

/* seg1:2E41 (AX=4300h): the attributes, or 0x80xx (AH=80h, AL=the DOS error) */
static WORD GetFileAttr(LPCSTR path)
{
    int a = w16_dos_attr(path);
    return a >= 0 ? (WORD)a : (WORD)(0x8000 | (s_dosErr = -a));
}

/* seg1:2E6A (AH=59h) */
static int GetExtendedErrorDOS(void) { return s_dosErr; }

/* seg1:2E86 (AH=5Ah): a new file with a unique name in directory `dir`, whose name is appended to
 * `dir` as DOS does; the handle, or -1. COMMDLG passes the directory without the trailing backslash
 * DOS wants ("C:\DIR"), with which DOS most likely made the file in the parent directory; libw16
 * makes it in the directory named. */
static int CreateUniqueFile(LPSTR dir, size_t cb)
{
    char out[300];
    HFILE h = w16_dos_create_temp(dir, out, sizeof out);
    if (h < 0) { s_dosErr = -h; return -1; }
    snprintf(dir, cb, "%s", out);
    return h;
}

/* ------------------------------------------------------------------ seg2 */
/* seg2:0000 */
static LPSTR StrChr(LPCSTR lpsz, int ch)
{
    while (*lpsz) {
        if ((int)(signed char)*lpsz == ch) return (LPSTR)lpsz;
        lpsz = AnsiNext(lpsz);
    }
    return NULL;
}

/* seg2:0034: the last ch before lpEnd */
static LPSTR StrRChr(LPSTR lpStart, LPSTR lpEnd, int ch)
{
    LPSTR lpLast = NULL;
    for (;;) {
        lpStart = StrChr(lpStart, ch);
        if (!lpStart || lpStart >= lpEnd) break;
        lpLast = lpStart;
        lpStart = AnsiNext(lpStart);
    }
    return lpLast;
}

/* seg2:0084: leading delimiters are skipped on the first call only ("a;;b" gives "a", "", "b") */
static LPSTR StrTok(LPSTR lpsz, LPCSTR lpDelims)
{
    LPSTR lpTok;
    if (lpsz) {
        g_lpTokNext = lpsz;
        while (*g_lpTokNext && StrChr(lpDelims, (signed char)*g_lpTokNext)) g_lpTokNext = AnsiNext(g_lpTokNext);
    }
    if (!*g_lpTokNext) return NULL;
    lpTok = g_lpTokNext;
    while (*g_lpTokNext && !StrChr(lpDelims, (signed char)*g_lpTokNext)) g_lpTokNext = AnsiNext(g_lpTokNext);
    if (*g_lpTokNext) {
        LPSTR lpNext = AnsiNext(g_lpTokNext);
        *g_lpTokNext = '\0';
        g_lpTokNext = lpNext;
    }
    return lpTok;
}

/* seg2:0142: exact byte compare (the second byte of a DBCS character too, on a DBCS system; libw16
 * has none) */
static BOOL ChrDiffer(LPCSTR a, LPCSTR b) { return *b != *a; }

/* seg2:0186: a UNC path made to fit stc1, as "\\s...\dir\sub". Measured with the static's own DC,
 * i.e. the system font rather than the dialog's (as 3.1 does). */
static LPSTR FitPathToControl(HWND hDlg, int id, LPSTR lpPath)
{
    BOOL fShortened = FALSE;
    LPSTR lpOrig = lpPath;
    HWND hCtl = GetDlgItem(hDlg, id);
    RECT rc;
    GetClientRect(hCtl, &rc);
    int cxAvail = rc.right - rc.left;
    HDC hdc = GetDC(hCtl);
    while ((SHORT)LOWORD(GetTextExtent(hdc, lpPath, lstrlen(lpPath))) > cxAvail) {
        if (!fShortened) {
            cxAvail -= LOWORD(GetTextExtent(hdc, lpPath, 7));
            if (cxAvail <= 0) break;
            lpPath += min(7, lstrlen(lpPath));
        }
        /* skip to just after the next backslash. 3.1 BUG fixed: the count bounding this skip
         * (register DI) is never set; it holds a large value in practice */
        while (*lpPath)
            if (*lpPath++ == '\\') break;
        fShortened = TRUE;
    }
    ReleaseDC(hCtl, hdc);
    /* the first three characters, "..." and the tail, built in place (3.1 BUG fixed: only when there
     * is room in front of the tail, which a path under 8 characters would not leave) */
    if (fShortened && lpPath - lpOrig >= 8) {
        lpPath -= 2; *lpPath = '.';
        lpPath--; *lpPath = '.';
        lpPath--; *lpPath = '.';
        lpPath--; *lpPath = lpOrig[2];
        lpPath--; *lpPath = lpOrig[1];
        lpPath--; *lpPath = lpOrig[0];
    }
    return lpPath;
}

/* seg2:029A: lst2's items for the UNC directory in p->szPath ("\\srv\share\d1\d2\"): "\\srv\share",
 * "d1", "d2", lower case */
static BOOL FillDirListUNC(HWND hLB, PFILEINST p)
{
    LPSTR lpCur, lpItem;
    int nSep = 0;
    p->cDirs = 0;
    AnsiLower(p->szPath);
    lpCur = lpItem = p->szPath;
    while (*lpCur) {
        if (*lpCur == '\\' || *lpCur == '/') {
            *lpCur = '\0';
            if (++nSep >= 4) {
                SendMessage(hLB, LB_INSERTSTRING, p->cDirs++, (LPARAM)lpItem);
                lpItem = lpCur + 1;
            }
            *lpCur = '\\';
        }
        lpCur++;
    }
    return TRUE;
}

/* seg2:032A: lst2's items for the current directory: "C:\", "WINDOWS", "SYSTEM" (as DOS gives them;
 * DrawItem shows them in lower case). LB_INSERTSTRING keeps them in path order above the sorted
 * subdirectories. */
static BOOL FillDirListCurDir(HWND hLB, PFILEINST p)
{
    char szAnsi[0x102];
    LPSTR lpItem = szAnsi, lpCur;
    char chSave;
    p->szPath[0] = '\0';
    GetCurDirNear(p->szPath);
    p->cDirs = 0;
    OemToAnsi(p->szPath, szAnsi);
    AnsiLower(p->szPath); /* the OEM copy, not szAnsi (3.1 does so; invisible) */
    lpCur = StrChr(szAnsi, '\\');
    if (!lpCur) return FALSE;
    lpCur++;
    chSave = *lpCur;
    *lpCur = '\0';
    SendMessage(hLB, LB_INSERTSTRING, p->cDirs++, (LPARAM)lpItem);
    *lpCur = chSave;
    while (lpCur && *lpCur) {
        lpItem = lpCur;
        lpCur = StrChr(lpCur, '\\');
        if (lpCur) { *lpCur = '\0'; lpCur++; }
        SendMessage(hLB, LB_INSERTSTRING, p->cDirs++, (LPARAM)lpItem);
    }
    return TRUE;
}

/* seg2:041A: COMMDLG's own wildcards, on upper-case names: '*' skips the rest of its part on both
 * sides ("A*B.TXT" is "A*.TXT"), '?' needs a character ("A?.TXT" does not match "A.TXT"), "NAME.*"
 * does not match "NAME", "*." only matches names without an extension */
static BOOL MatchSpec(LPCSTR lpName, LPCSTR lpSpec)
{
    if (!lstrcmp(lpSpec, "*") || !lstrcmp(lpSpec, "*.*")) return TRUE;
    while (*lpName && *lpSpec) {
        switch ((signed char)*lpSpec) {
        case '*':
            while (*lpSpec != '.' && *lpSpec) lpSpec = AnsiNext(lpSpec);
            if (*lpSpec == '.') lpSpec++;
            while (*lpName != '.' && *lpName) lpName = AnsiNext(lpName);
            if (*lpName == '.') lpName++;
            break;
        case '?':
            lpName = AnsiNext(lpName);
            lpSpec++;
            break;
        default:
            if (ChrDiffer(lpName, lpSpec)) return FALSE;
            lpSpec = AnsiNext(lpSpec);
            lpName = AnsiNext(lpName);
            break;
        }
    }
    if (*lpName) return FALSE;
    return *lpSpec == '\0' || *lpSpec == '*';
}

static void HourGlass(BOOL fOn);
static int GetUNCDirPath(HWND hDlg, int idLB, PFILEINST p);

/* seg2:0530: fills lst1 with the files of the current directory (or of the UNC spec) matching the
 * spec - lpszSpec, or with NULL the edit's text when it is a pure wildcard pattern, else p->szSpec -
 * and with wFlags & 0x10 lst2 with the directories; stc1 shows the directory. wFlags: 0 files, 0x10
 * files and directories, 0x4010 the same after a drive change (lst2 from the top), 0x20 / 0x30 UNC.
 * The spec is tokenised in place (upper case, OEM, cut at the first ';'). */
static BOOL ListFiles(HWND hDlg, PFILEINST p, LPSTR lpszSpec, WORD wFlags)
{
    HWND hFileLB = GetDlgItem(hDlg, lst1);
    HWND hDirLB = GetDlgItem(hDlg, lst2);
    BOOL fResult = FALSE;
    WORD fDrives = wFlags & 0x4000;
    WORD fUNC = wFlags & 0x0020;
    char szBuf[0x100], szName[0x100];
    LPSTR aTok[37];
    int i, k, err;
    LPSTR lpSlash = NULL;
    RECT rc;

    HourGlass(TRUE);
    if (lpszSpec == NULL) {
        GetDlgItemText(hDlg, edt1, szBuf, 0xFF);
        lpszSpec = szBuf;
        if (StrChr(szBuf, '\\') || StrChr(szBuf, '/') || StrChr(szBuf, ':') ||
            (!StrChr(szBuf, '*') && !StrChr(szBuf, '?')))
            lstrcpy(szBuf, p->szSpec);
    }
    i = 0;
    aTok[0] = StrTok(lpszSpec, "; \t");
    while (aTok[i] != NULL && i < 36) {
        AnsiUpper(aTok[i]);
        AnsiToOem(aTok[i], aTok[i]);
        aTok[++i] = StrTok(NULL, "; \t");
    }
    if (i >= 36) aTok[36] = NULL;

    SendMessage(hFileLB, WM_SETREDRAW, FALSE, 0L);
    SendMessage(hFileLB, LB_RESETCONTENT, 0, 0L);
    if (wFlags & 0x10) {
        g_fNoDraw = 1;
        SendMessage(hDirLB, WM_SETREDRAW, FALSE, 0L);
        SendMessage(hDirLB, LB_RESETCONTENT, 0, 0L);
    }
    if (fUNC) {
        err = FindFirst(lpszSpec, wFlags);
        lstrcpy(szBuf, p->szDefSpec); /* the match patterns come from szDefSpec */
        i = 0;
        aTok[0] = StrTok(szBuf, "; \t");
        while (aTok[i] != NULL && i < 36) {
            AnsiUpper(aTok[i]);
            AnsiToOem(aTok[i], aTok[i]);
            aTok[++i] = StrTok(NULL, "; \t");
        }
        if (i >= 36) aTok[36] = NULL;
    } else
        err = FindFirst("*.*", wFlags);

    if (err != 0 && err != 0x12) {
        wFlags = 0x4000; /* the search failed */
        goto after_scan;
    }
    fResult = TRUE;
    wFlags &= 0x10;
    if (err != 0x12) do {
        lstrcpy(szName, g_DTA.name);
        if (wFlags != 0 && (g_DTA.attrib & 0x10)) {
            if (szName[0] != '.') { /* not "." and ".." */
                OemToAnsi(szName, szName);
                SendMessage(hDirLB, LB_ADDSTRING, 0, (LPARAM)szName);
            }
        } else {
            for (k = 0; aTok[k] != NULL; k++)
                if (MatchSpec(szName, aTok[k])) {
                    OemToAnsi(szName, szName);
                    SendMessage(hFileLB, LB_ADDSTRING, 0, (LPARAM)szName);
                    break;
                }
        }
    } while (FindNext() == 0);

after_scan:
    if (wFlags == 0) goto files_only;
    if (fUNC) {
        /* 3.1 BUG fixed: StrRChr(...) + 1 was tested for NULL after the +1, so a spec without a
         * backslash wrote to address 1; there always is one for UNC specs */
        lpSlash = StrRChr(lpszSpec, lpszSpec + lstrlen(lpszSpec), '\\');
        if (lpSlash) lpSlash[1] = '\0'; /* "...\dir\" */
        AnsiLower(lpszSpec);
        FillDirListUNC(hDirLB, p);
        SendMessage(hDirLB, LB_SETCURSEL, p->cDirs - 1, 0L);
        if (lpSlash) lpSlash[0] = '\0'; /* without the trailing backslash */
        SetDlgItemText(hDlg, stc1, FitPathToControl(hDlg, stc1, lpszSpec));
        SendDlgItemMessage(hDlg, cmb2, CB_SETCURSEL, (WPARAM)-1, 0L);
    } else if (wFlags == 0x10) {
        FillDirListCurDir(hDirLB, p);
        DlgDirList(hDlg, NULL, 0, stc1, 0); /* USER writes the current directory into stc1 */
        SendMessage(hDirLB, LB_SETCURSEL, p->cDirs - 1, 0L);
        if (!fDrives) { k = p->cDirs - 2; if (k < 0) k = 0; }
        else k = 0;
        SendMessage(hDirLB, LB_SETTOPINDEX, k, 0L);
    } else
        SetDlgItemText(hDlg, stc1, "");
    g_fNoDraw = 0;
    SendMessage(hDirLB, WM_SETREDRAW, TRUE, 0L);
    GetWindowRect(hDirLB, &rc);
    rc.left++; rc.top++; rc.right--; rc.bottom--;
    ScreenToClient(hDlg, (LPPOINT)&rc.left);
    ScreenToClient(hDlg, (LPPOINT)&rc.right);
    InvalidateRect(hDlg, &rc, g_wWinVer < 0x30A);

files_only:
    SendMessage(hFileLB, WM_SETREDRAW, TRUE, 0L);
    InvalidateRect(hFileLB, NULL, TRUE);
    HourGlass(FALSE);
    return fResult;
}

/* (seg2:0A92 DlgDirListMulti and seg2:0D8E DlgUnitsX are not referenced by COMMDLG and not ported) */

/* seg2:0C2C: the network driver's connect dialogs. With no network driver (libw16's WNetGetCaps
 * answers 0) there is none, and the Network... button never appears. */
static BOOL IsNetConnectAvailable(void)
{
    if (!g_fNetProcQueried) {
        g_fNetProcQueried = TRUE;
        /* WNetGetCaps(0xFFFF) is the driver's module handle; 3.1 then looks up its ordinal 148 (the
         * drive connected last). No driver here, so the lookup never happens. */
        if (WNetGetCaps(0xFFFF)) g_pfnNetLastConn = NULL;
    }
    g_wNetDlgCaps = WNetGetCaps(WNNC_DIALOG) & 0x44; /* ConnectDialog | ConnectionDialog */
    return g_wNetDlgCaps != 0;
}

/* seg2:0CA6: "Network..." pressed */
static BOOL ConnectNetDrive(HWND hDlg)
{
    WORD wStack = StackAvail(), wRet;
    BOOL fWfW = (g_wNetCaps & 0x8004) == 0x8004; /* multinet + Windows for Workgroups */
    if (wStack >= g_wDuelNetStack || (wStack >= g_wNetDlgStack && !fWfW))
        wRet = WNetConnectDialog(hDlg, WNTYPE_DRIVE);
    else {
        if (wStack < g_wWFWGDlgStack || !fWfW) return FALSE;
        /* the WfW driver's ordinal 535 (a connect dialog): there is no driver here */
        if (!WNetGetCaps(0xFFFF)) return FALSE;
        wRet = 1;
    }
    return wRet == WN_SUCCESS;
}

/* seg2:0DB2 */
static int DlgUnitsY(int n, DWORD dwBase) { return (SHORT)(HIWORD(dwBase) * n) / 8; }

/* seg2:0DE4: a "Network..." button (psh14) in the OK button's column, in the first free slot below it */
static void AddNetworkButton(HWND hDlg, HINSTANCE hInst, int nGapDlu)
{
    DWORD dwBase;
    int cxFrame, cyFrame, cxBtn, cyBtn, xBtn, yBtn, yLimit;
    RECT rcClient, rcBtn;
    HWND hCtl, hPrevCtl, hBtn;
    POINT TL, BL, TR, BR, C;
    WORD wStack;
    if (GetDlgItem(hDlg, psh14)) return;
    g_wNetCaps = WNetGetCaps(WNNC_NET_TYPE);
    wStack = StackAvail();
    if ((g_wNetCaps & 0x8004) == 0x8004) { if (wStack < g_wWFWGDlgStack) return; }
    else if (wStack < g_wNetDlgStack) return;
    dwBase = GetDialogBaseUnits();
    cxFrame = GetSystemMetrics(SM_CXDLGFRAME);
    cyFrame = GetSystemMetrics(SM_CYDLGFRAME);
    GetWindowRect(hDlg, &rcClient);
    rcClient.left += cxFrame;
    rcClient.right -= cxFrame;
    rcClient.top += GetSystemMetrics(SM_CYCAPTION) + cyFrame;
    rcClient.bottom -= cyFrame;
    if (!(hCtl = GetDlgItem(hDlg, IDOK))) return;
    GetWindowRect(hCtl, &rcBtn);
    if (!PtInRect(&rcClient, (POINT){rcBtn.left, rcBtn.top})) {
        if (!(hCtl = GetDlgItem(hDlg, IDCANCEL))) return;
        GetWindowRect(hCtl, &rcBtn);
    }
    cxBtn = rcBtn.right - rcBtn.left;
    cyBtn = rcBtn.bottom - rcBtn.top;
    xBtn = rcBtn.left;
    yLimit = rcClient.bottom - DlgUnitsY(nGapDlu, dwBase);
    for (;;) {
        if (!hCtl) return;
        hPrevCtl = hCtl;
        GetWindowRect(hCtl, &rcBtn);
        yBtn = DlgUnitsY(4, dwBase) + rcBtn.bottom;
        if ((unsigned)(yBtn + cyBtn) > (unsigned)yLimit) return;
        TL = (POINT){xBtn, yBtn};
        BL = (POINT){xBtn, yBtn + cyBtn};
        TR = (POINT){xBtn + cxBtn, yBtn};
        BR = (POINT){xBtn + cxBtn, yBtn + cyBtn};
        C = (POINT){xBtn + (unsigned)cxBtn / 2, yBtn + (unsigned)cyBtn / 2};
        ScreenToClient(hDlg, &TL); ScreenToClient(hDlg, &BL); ScreenToClient(hDlg, &TR);
        ScreenToClient(hDlg, &BR); ScreenToClient(hDlg, &C);
        if ((hCtl = ChildWindowFromPoint(hDlg, TL)) != hDlg) continue;
        if ((hCtl = ChildWindowFromPoint(hDlg, TR)) != hDlg) continue;
        if ((hCtl = ChildWindowFromPoint(hDlg, C)) != hDlg) continue;
        if ((hCtl = ChildWindowFromPoint(hDlg, BL)) != hDlg) continue;
        if ((hCtl = ChildWindowFromPoint(hDlg, BR)) != hDlg) continue;
        break;
    }
    LoadString(g_hInst, IDS_NETWORK, g_szTmp2, 0x102);
    hBtn = CreateWindow("button", g_szTmp2, WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_PUSHBUTTON,
                        TL.x, TL.y, cxBtn, cyBtn, hDlg, NULL, hInst, NULL);
    if (!hBtn) return;
    SetWindowWord(hBtn, GWW_ID, psh14);
    SetWindowPos(hBtn, hPrevCtl, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)SendDlgItemMessage(hDlg, IDOK, WM_GETFONT, 0, 0L), 0L);
}

/* seg2:118C */
static void HourGlass(BOOL fOn)
{
    if (g_fInitializing) return;
    if (!g_fMousePresent) {
        g_fHourGlass = fOn;
        ShowCursor(fOn);
    }
    SetCursor(LoadCursor(NULL, fOn ? IDC_WAIT : IDC_ARROW));
}

/* seg2:11F0: a bare name is opened as ".\name", so OpenFile does not search the path */
static HFILE OpenFileRel(LPCSTR lpszFile, LPOFSTRUCT lpOF, UINT wStyle)
{
    char sz[0x102];
    LPCSTR lp;
    if (StrChr(lpszFile, '\\') || StrChr(lpszFile, '/') || StrChr(lpszFile, ':'))
        lp = lpszFile;
    else {
        sz[0] = '.'; sz[1] = '\\'; sz[2] = 0;
        snprintf(sz + 2, sizeof sz - 2, "%s", lpszFile);
        lp = sz;
    }
    return OpenFile(lp, lpOF, wStyle);
}

static BOOL AddFolderBitmapRef(void);
static void ReleaseFolderBitmapRef(void);

/* seg2:12AC: both dialogs, from the template on */
static BOOL GetFileName(LPOPENFILENAME lpOFN, DLGPROC lpfnDlgProc)
{
    int nRet = 0;
    HGLOBAL hTemplate;
    HINSTANCE hInstRes;
    LPCSTR lpszTemplate;
    HANDLE hRsrc;
    UINT wOld;
    if (lpOFN->lStructSize != sizeof(OPENFILENAME)) {
        cd_err = CDERR_STRUCTSIZE;
        return FALSE;
    }
    if (!LibMain()) { cd_err = CDERR_LOADRESFAILURE; return FALSE; } /* (no COMMDLG.DLL ripped) */
    HourGlass(TRUE);
    cd_err = 0;
    if (!AddFolderBitmapRef()) {
        cd_err = CDERR_LOADRESFAILURE;
        goto cleanup;
    }
    if (lpOFN->Flags & OFN_ENABLETEMPLATEHANDLE)
        hTemplate = (HGLOBAL)lpOFN->hInstance;
    else {
        if (lpOFN->Flags & OFN_ENABLETEMPLATE) {
            if (!lpOFN->lpTemplateName) { cd_err = CDERR_NOTEMPLATE; goto cleanup; }
            if (!lpOFN->hInstance) { cd_err = CDERR_NOHINSTANCE; goto cleanup; }
            lpszTemplate = lpOFN->lpTemplateName;
            hInstRes = lpOFN->hInstance;
        } else {
            hInstRes = g_hInst;
            lpszTemplate = MAKEINTRESOURCE((lpOFN->Flags & OFN_ALLOWMULTISELECT) ? DLG_MULTIOPEN : DLG_OPEN);
        }
        if (!(hRsrc = FindResource(hInstRes, lpszTemplate, RT_DIALOG))) { cd_err = CDERR_FINDRESFAILURE; goto cleanup; }
        if (!(hTemplate = LoadResource(hInstRes, hRsrc))) { cd_err = CDERR_LOADRESFAILURE; goto cleanup; }
    }
    wOld = SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    if (LockResource(hTemplate)) { /* a failure sets no error (as 3.1) */
        if (lpOFN->Flags & OFN_ENABLEHOOK) g_lpfnHook = lpOFN->lpfnHook; /* the hook sees what comes before WM_INITDIALOG */
        nRet = DialogBoxIndirectParam(g_hInst, LockResource(hTemplate), lpOFN->hwndOwner, lpfnDlgProc, (LPARAM)lpOFN);
        g_lpfnHook = NULL;
        if (nRet == -1) { cd_err = CDERR_DIALOGFAILURE; nRet = 0; }
        /* GlobalUnlock(hTemplate): libw16's resources are not locked */
    }
    SetErrorMode(wOld);
    /* FreeResource(hTemplate) unless it is the caller's: libw16's resources stay loaded */
cleanup:
    ReleaseFolderBitmapRef(); /* counted even when AddFolderBitmapRef failed (3.1) */
    HourGlass(FALSE);
    return nRet == 1;
}

static BOOL FileOpenDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static BOOL FileSaveDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

/* seg2:1556 / seg2:1586 (no NULL test: as 3.1) */
BOOL GetOpenFileName(OPENFILENAME *lpOFN) { return GetFileName(lpOFN, FileOpenDlgProc); }
BOOL GetSaveFileName(OPENFILENAME *lpOFN) { return GetFileName(lpOFN, FileSaveDlgProc); }

/* seg2:15B6: the name with its extension, case unchanged; a negative PARSE_ code, or the buffer size
 * needed. ParseFile edits the caller's string (leading blanks, a final '.', ...), as on 3.1. */
int GetFileTitle(LPCSTR lpszFile, LPSTR lpszTitle, UINT cbBuf)
{
    int n = pf_file(ParseFile((LPSTR)lpszFile));
    LPSTR lpName;
    if (n < 0) return n;
    lpName = (LPSTR)lpszFile + n;
    n = lstrlen(lpName) + 1;
    if (n > (int)(WORD)cbBuf) return n;
    if (StrChr(lpName, '*') || StrChr(lpName, '?')) return PARSE_WILDCARDINFILE;
    lstrcpy(lpszTitle, lpName);
    return 0;
}

/* seg2:166C */
static void FreeFolderBitmaps(void)
{
    if (g_hbmMemOld) {
        SelectObject(g_hdcMem, g_hbmMemOld);
        if (g_hbmDirDrives) { DeleteObject(g_hbmDirDrives); g_hbmDirDrives = NULL; }
    }
}

/* seg6:013C: COLORREF 0x00BBGGRR -> the RGBQUAD dword 0x00RRGGBB */
static DWORD RGBtoQuad(COLORREF c) { return ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c); }

/* seg6:0000 LoadAlterBitmap: bitmap id with the first colour-table entry equal to rgbReplace made
 * rgbInstead. 3.1 patches the loaded resource and frees it again; libw16's resources stay loaded, so a
 * copy is patched. */
static HBITMAP LoadAlterBitmap(int id, COLORREF rgbReplace, COLORREF rgbInstead)
{
    HANDLE hrsrc = FindResource(g_hInst, MAKEINTRESOURCE(id), RT_BITMAP);
    HGLOBAL hRes;
    if (!hrsrc || !(hRes = LoadResource(g_hInst, hrsrc))) return NULL;
    DWORD qReplace = RGBtoQuad(rgbReplace), qInstead = RGBtoQuad(rgbInstead);
    DWORD cb = SizeofResource(g_hInst, hrsrc);
    uint8_t *lpbi = malloc(cb);
    if (!lpbi) return NULL;
    memcpy(lpbi, LockResource(hRes), cb);
    DWORD biSize = lpbi[0] | lpbi[1] << 8 | lpbi[2] << 16 | (DWORD)lpbi[3] << 24;
    int bitcount = lpbi[14] | lpbi[15] << 8;
    int ncolors = bitcount <= 8 ? 1 << bitcount : 0;
    /* 3.1 BUG fixed: the search had no end and ran past the colour table when the colour was absent */
    for (int i = 0; i < ncolors && biSize + 4 * i + 4 <= cb; i++) {
        uint8_t *e = lpbi + biSize + 4 * i;
        if ((e[0] | e[1] << 8 | (DWORD)e[2] << 16 | (DWORD)e[3] << 24) == qReplace) {
            e[0] = (BYTE)qInstead; e[1] = (BYTE)(qInstead >> 8); e[2] = (BYTE)(qInstead >> 16); e[3] = 0;
            break;
        }
    }
    HBITMAP hbm = w16_bitmap_from_dib(lpbi, (int)cb, 0); /* CreateDIBitmap(GetDC(NULL), lpbi, CBM_INIT, ...) */
    free(lpbi);
    return hbm;
}

/* seg2:16AC: bitmap 576 twice side by side, its blue background made COLOR_WINDOW (x 0..127) and
 * COLOR_HIGHLIGHT (x 128..255); stays selected in g_hdcMem */
static BOOL LoadFolderBitmaps(void)
{
    BOOL fOK = FALSE;
    HDC hdcTmp;
    HBITMAP hbm, hbmOld;
    BITMAP bm;
    if (g_hbmDirDrives && g_rgbBmpWindow == g_rgbWindow && g_rgbBmpHighlight == g_rgbHighlight)
        if (SelectObject(g_hdcMem, g_hbmDirDrives)) return TRUE;
    FreeFolderBitmaps();
    g_rgbBmpWindow = g_rgbWindow;
    g_rgbBmpHighlight = g_rgbHighlight;
    if (!(hdcTmp = CreateCompatibleDC(g_hdcMem))) return FALSE;
    if (!(hbm = LoadAlterBitmap(576, RGB(0, 0, 255), g_rgbWindow))) goto del_dc;
    GetObject(hbm, sizeof bm, &bm);
    g_cyBmp = bm.bmHeight;
    g_cxBmp = bm.bmWidth;
    hbmOld = SelectObject(hdcTmp, hbm);
    g_hbmDirDrives = CreateDiscardableBitmap(hdcTmp, g_cxBmp * 2, g_cyBmp);
    if (!g_hbmDirDrives) goto del_bmp;
    if (!SelectObject(g_hdcMem, g_hbmDirDrives)) { FreeFolderBitmaps(); goto del_bmp; }
    BitBlt(g_hdcMem, 0, 0, g_cxBmp, g_cyBmp, hdcTmp, 0, 0, SRCCOPY);
    SelectObject(hdcTmp, hbmOld);
    DeleteObject(hbm);
    if (!(hbm = LoadAlterBitmap(576, RGB(0, 0, 255), g_rgbHighlight))) goto del_dc;
    hbmOld = SelectObject(hdcTmp, hbm);
    BitBlt(g_hdcMem, g_cxBmp, 0, g_cxBmp, g_cyBmp, hdcTmp, 0, 0, SRCCOPY);
    SelectObject(hdcTmp, hbmOld);
    fOK = TRUE;
del_bmp:
    DeleteObject(hbm);
del_dc:
    DeleteDC(hdcTmp);
    return fOK;
}

/* seg2:18B8 */
static void LoadSysColors(void)
{
    g_rgbWindow = GetSysColor(COLOR_WINDOW);
    g_rgbHighlight = GetSysColor(COLOR_HIGHLIGHT);
    g_rgbWindowText = GetSysColor(COLOR_WINDOWTEXT);
    g_rgbHighlightText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    g_rgbGrayText = GetSysColor(COLOR_GRAYTEXT);
}

/* seg2:1918 / seg2:1948 */
static BOOL AddFolderBitmapRef(void)
{
    if (g_cBmpRef++ != 0) return TRUE;
    LoadSysColors();
    return LoadFolderBitmaps();
}
static void ReleaseFolderBitmapRef(void)
{
    if (--g_cBmpRef == 0) SelectObject(g_hdcMem, g_hbmMemOld); /* the bitmap is kept for next time */
}

/* seg2:1978: the OK button; focus leaving it after lst2 handed it over puts lst2 back on the
 * current directory */
static LRESULT DwOkSubclass(HWND hBtn, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KILLFOCUS && g_fOKFromLB) {
        HWND hDlg = GetParent(hBtn);
        PFILEINST p = (PFILEINST)GetProp(hDlg, PROP_INST);
        if (p) SendDlgItemMessage(hDlg, lst2, LB_SETCURSEL, p->cDirs - 1, 0L);
        g_fOKFromLB = FALSE;
    }
    return CallWindowProc(g_lpfnBtnClassProc, hBtn, msg, wParam, lParam);
}

/* seg2:1A12: lst2; a selection is dropped when the focus goes anywhere but to OK */
static LRESULT DwLbSubclass(HWND hLB, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KILLFOCUS) {
        HWND hDlg = GetParent(hLB);
        g_fOKFromLB = (HWND)wParam == GetDlgItem(hDlg, IDOK);
        if (!g_fOKFromLB) {
            PFILEINST p = (PFILEINST)GetProp(hDlg, PROP_INST);
            if (p) SendMessage(hLB, LB_SETCURSEL, p->cDirs - 1, 0L);
        }
    }
    return CallWindowProc(g_lpfnLBClassProc, hLB, msg, wParam, lParam);
}

/* seg2:1AC6: WM_INITDIALOG of both dialogs (with g_fInitializing set). Fills, in this order: the
 * file type combo and its pattern into edt1, the drive combo, the lists for the current drive, then
 * edt1 = lpstrFile. */
static BOOL InitFileDlg(HWND hDlg, WPARAM wParam, LPOPENFILENAME lpOFN)
{
    PFILEINST p = calloc(1, sizeof(FILEINST)); /* LocalAlloc(LMEM_ZEROINIT, 0x40D) */
    DWORD dw;
    int n;
    LPSTR lpPat;
    char chDrive;
    RECT rcEd, rcLB;
    BOOL fRet;

    if (!p) { cd_err = CDERR_MEMALLOCFAILURE; EndDialog(hDlg, FALSE); return FALSE; }
    g_lpfnLBClassProc = (WNDPROC)w16_GetClassPtr(GetDlgItem(hDlg, lst2), GCL_WNDPROC);
    g_lpfnBtnClassProc = (WNDPROC)w16_GetClassPtr(GetDlgItem(hDlg, IDOK), GCL_WNDPROC);
    if (!g_lpfnLBClassProc || !g_lpfnBtnClassProc) {
        cd_err = FNERR_SUBCLASSFAILURE;
        free(p);
        EndDialog(hDlg, FALSE);
        return FALSE;
    }
    if (lpOFN->Flags & OFN_NOCHANGEDIR) {
        p->szInitDir[0] = 0;
        GetCurDirNear(p->szInitDir);
    }
    if (lpOFN->lpstrFile && *lpOFN->lpstrFile && !(lpOFN->Flags & OFN_NOVALIDATE)) {
        LowerAscii(lpOFN->lpstrFile); /* the caller's buffer */
        if (lpOFN->lpstrFile[1] == ':' && lpOFN->lpstrFile[2] == '\\' && lpOFN->lpstrFile[3] == '\\')
            StrShift(lpOFN->lpstrFile, lpOFN->lpstrFile + 2); /* "x:\\srv\sh" -> "\\srv\sh" */
        dw = ParseFile(lpOFN->lpstrFile);
        if (pf_file(dw) < 0 && pf_file(dw) != PARSE_EMPTYSTRING && lpOFN->lpstrFile[pf_ext(dw)] != ';') {
            cd_err = FNERR_INVALIDFILENAME;
            free(p);
            EndDialog(hDlg, FALSE);
            return FALSE;
        }
    }
    if (lpOFN->lpstrInitialDir) ChangeDir((LPSTR)lpOFN->lpstrInitialDir, TRUE); /* result ignored */
    if (lpOFN->Flags & OFN_ENABLEHOOK) {
        if (!lpOFN->lpfnHook) {
            cd_err = CDERR_NOHOOK;
            free(p);
            EndDialog(hDlg, FALSE);
            return FALSE;
        }
    } else
        lpOFN->lpfnHook = NULL; /* the caller's structure */

    SetProp(hDlg, PROP_INST, (HANDLE)p);
    lpOFN->Flags &= 0x8FFFFFFFL;
    p->lpOFN = lpOFN;
    p->cDirs = 0;

    if (!(lpOFN->Flags & OFN_SHOWHELP)) {
        HWND h = GetDlgItem(hDlg, pshHelp);
        EnableWindow(h, FALSE);
        MoveWindow(h, -8000, -8000, 20, 20, FALSE);
        ShowWindow(h, SW_HIDE);
    }
    if (lpOFN->Flags & OFN_CREATEPROMPT) lpOFN->Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    else if (lpOFN->Flags & OFN_FILEMUSTEXIST) lpOFN->Flags |= OFN_PATHMUSTEXIST;
    if (lpOFN->Flags & OFN_HIDEREADONLY) {
        HWND h = GetDlgItem(hDlg, chx1);
        EnableWindow(h, FALSE);
        MoveWindow(h, -8000, -8000, 20, 20, FALSE);
        ShowWindow(h, SW_HIDE);
    } else
        CheckDlgButton(hDlg, chx1, (lpOFN->Flags & OFN_READONLY) ? 1 : 0);
    SendDlgItemMessage(hDlg, edt1, EM_LIMITTEXT, 0x100, 0L);
    /* 3.10 and later: the extended combo interface (3.0 disabled the combos under a system-modal window) */
    SendDlgItemMessage(hDlg, cmb1, CB_SETEXTENDEDUI, TRUE, 0L);
    SendDlgItemMessage(hDlg, cmb2, CB_SETEXTENDEDUI, TRUE, 0L);

    if (lpOFN->lpstrFile && (StrChr(lpOFN->lpstrFile, '*') || StrChr(lpOFN->lpstrFile, '?')))
        lstrcpy(p->szDefSpec, lpOFN->lpstrFile);
    else
        p->szDefSpec[0] = 0;
    if (lpOFN->lpstrCustomFilter && *lpOFN->lpstrCustomFilter) {
        SendDlgItemMessage(hDlg, cmb1, CB_INSERTSTRING, 0, (LPARAM)lpOFN->lpstrCustomFilter);
        n = lstrlen(lpOFN->lpstrCustomFilter) + 1;
        SendDlgItemMessage(hDlg, cmb1, CB_SETITEMDATA, 0, (LPARAM)n);
        SendDlgItemMessage(hDlg, cmb1, CB_LIMITTEXT, LOWORD(lpOFN->nMaxCustFilter), 0L); /* no effect */
        if (p->szDefSpec[0] == 0) lstrcpy(p->szDefSpec, lpOFN->lpstrCustomFilter + n);
    } else
        lpOFN->nFilterIndex--; /* 1-based -> 0-based */
    if (lpOFN->lpstrFilter) {
        n = FillFilterCombo(hDlg, lpOFN->lpstrFilter);
        if (lpOFN->nFilterIndex > (DWORD)(WORD)n || (SHORT)LOWORD(lpOFN->nFilterIndex) < 0) lpOFN->nFilterIndex = 0;
    } else
        lpOFN->nFilterIndex = 0;
    if (lpOFN->lpstrFilter || (lpOFN->lpstrCustomFilter && *lpOFN->lpstrCustomFilter)) {
        SendDlgItemMessage(hDlg, cmb1, CB_SETCURSEL, LOWORD(lpOFN->nFilterIndex), 0L);
        SendMessage(hDlg, WM_COMMAND, cmb1, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb1), 0x8000));
        if (!lpOFN->lpstrFile || !*lpOFN->lpstrFile) {
            if (lpOFN->nFilterIndex == 0 && lpOFN->lpstrCustomFilter && *lpOFN->lpstrCustomFilter)
                lpPat = lpOFN->lpstrCustomFilter + lstrlen(lpOFN->lpstrCustomFilter) + 1;
            else
                lpPat = (LPSTR)lpOFN->lpstrFilter +
                        (WORD)SendDlgItemMessage(hDlg, cmb1, CB_GETITEMDATA, LOWORD(lpOFN->nFilterIndex), 0L);
            if (*lpPat) {
                lstrcpy(g_szBuf1, lpPat);
                LowerAscii(g_szBuf1);
                if (p->szDefSpec[0] == 0) lstrcpy(p->szDefSpec, g_szBuf1);
                SetDlgItemText(hDlg, edt1, g_szBuf1);
            }
        }
    }

    chDrive = (char)(GetCurrentDrive() + 'a');
    FillDriveCombo(hDlg, cmb2);
    SelectDriveInCombo(hDlg, cmb2, chDrive);
    g_fFirstActivate = TRUE;
    g_fInCreatePrompt = FALSE;
    SendMessage(hDlg, WM_COMMAND, cmb2, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb2), 0x8000)); /* the lists */

    if (lpOFN->lpstrFile && *lpOFN->lpstrFile) {
        lstrcpy(g_szMsgFmt, lpOFN->lpstrFile);
        SetDlgItemText(hDlg, edt1, g_szMsgFmt);
    }
    w16_SetWindowPtr(GetDlgItem(hDlg, lst2), GWL_WNDPROC, (intptr_t)DwLbSubclass);
    w16_SetWindowPtr(GetDlgItem(hDlg, IDOK), GWL_WNDPROC, (intptr_t)DwOkSubclass);
    if (lpOFN->lpstrTitle && *lpOFN->lpstrTitle) SetWindowText(hDlg, lpOFN->lpstrTitle);

    if (g_cyItem == 0) { /* no WM_MEASUREITEM yet */
        GetClientRect(GetDlgItem(hDlg, lst1), &rcEd);
        g_cyFont = rcEd.bottom / 8;
        if (g_cyFont == 0) g_cyFont = 8;
    }
    if (!(lpOFN->Flags & (OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE))) {
        /* the standard template: edt1 exactly as wide as lst1 */
        GetWindowRect(GetDlgItem(hDlg, lst1), &rcLB);
        GetWindowRect(GetDlgItem(hDlg, edt1), &rcEd);
        rcEd.left = rcLB.left;
        rcEd.right = rcLB.right;
        ScreenToClient(hDlg, (LPPOINT)&rcEd.left);
        ScreenToClient(hDlg, (LPPOINT)&rcEd.right);
        SetWindowPos(GetDlgItem(hDlg, edt1), NULL, rcEd.left, rcEd.top, rcEd.right - rcEd.left,
                     rcEd.bottom - rcEd.top, SWP_NOZORDER);
    }
    if (lpOFN->lpfnHook) fRet = (BOOL)lpOFN->lpfnHook(hDlg, WM_INITDIALOG, wParam, (LPARAM)lpOFN);
    else fRet = TRUE;
    if (!(lpOFN->Flags & OFN_NONETWORKBUTTON) && IsNetConnectAvailable())
        AddNetworkButton(hDlg, (lpOFN->Flags & (OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) ? lpOFN->hInstance : g_hInst, 3);
    return fRet;
}

/* seg2:25F8: the message box for DOS error (or negative PARSE_ code) wErr; the name is lower-cased
 * in the caller's buffer */
static void ShowFileError(HWND hDlg, LPSTR lpszFile, WORD wErr)
{
    UINT ids;
    BOOL fDrive = FALSE;
    if (lstrlen(lpszFile) > 0x100) lpszFile[0x100] = '\0';
    switch (wErr) {
    case 0x02: ids = IDS_FILENOTFOUND; break;
    case 0x03: ids = IDS_PATHNOTFOUND; break;
    case 0x04: ids = IDS_TOOMANYOPEN; break;
    case 0x05: ids = IDS_CANTREAD; fDrive = TRUE; break;
    case 0x13: ids = IDS_WRITEPROT; fDrive = TRUE; break;
    case 0x20: ids = IDS_INUSE; break;
    case 0x41: ids = IDS_NETACCESS; break;
    case 0x52: ids = IDS_DISKFULL; fDrive = TRUE; break;
    case 0x53: ids = IDS_CRITERR; break;
    case 0x60: ids = IDS_CREATENOMODIFY; break;
    case 0x61: ids = IDS_NODRIVE; fDrive = TRUE; break;
    case 0x62: ids = IDS_DEVICENAME; break;
    case 0x63: ids = IDS_READONLY; break;
    default: ids = IDS_BADNAME; break;
    }
    LoadString(g_hInst, ids, g_szMsgFmt, 0xC0);
    LowerAscii(lpszFile);
    if (fDrive) wsprintf(g_szMsg, g_szMsgFmt, (int)(signed char)lpszFile[0]);
    else wsprintf(g_szMsg, g_szMsgFmt, (LPSTR)lpszFile);
    GetWindowText(hDlg, g_szMsgFmt, 0xC0);
    MessageBox(hDlg, g_szMsg, g_szMsgFmt, MB_OK | MB_ICONEXCLAMATION);
    if (ids == IDS_BADNAME) PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, edt1), 1L);
}

static int TestCreate(LPSTR lpszPath);

/* seg2:27E4: several names separated by spaces (OFN_ALLOWMULTISELECT): lpstrFile becomes
 * "C:\DIR name1 name2" */
static BOOL MultiSelectOK(HWND hDlg, PFILEINST p, BOOL fSave)
{
    LPOPENFILENAME lpOFN = p->lpOFN;
    BOOL fLast = FALSE, fNotExist;
    WORD cb;
    int cch, n;
    LPSTR lpName, lpEnd = NULL;
    OFSTRUCT of;
    memset(&of, 0, sizeof of);
    GetCurDirNear(p->szPath);
    cch = (int)SendDlgItemMessage(hDlg, edt1, WM_GETTEXTLENGTH, 0, 0L);
    cb = lstrlen(p->szPath) + 2 + cch;
    if (!lpOFN->lpstrFile) goto done;
    if (cb > LOWORD(lpOFN->nMaxFile)) {
        lpOFN->lpstrFile[0] = LOBYTE(cb);
        lpOFN->lpstrFile[1] = HIBYTE(cb);
        lpOFN->lpstrFile[2] = 0;
        goto done;
    }
    lstrcpy(lpOFN->lpstrFile, p->szPath);
    lstrcat(lpOFN->lpstrFile, " ");
    cb = lstrlen(lpOFN->lpstrFile);
    lpOFN->nFileOffset = cb;
    lpName = lpOFN->lpstrFile + cb;
    GetDlgItemText(hDlg, edt1, lpName, LOWORD(lpOFN->nMaxFile) - cb - 1);
    while (*lpName == ' ') lpName = AnsiNext(lpName);
    if (*lpName == '\0') return FALSE;
    while (!fLast) {
        lpEnd = lpName;
        while (*lpEnd && *lpEnd != ' ') lpEnd = AnsiNext(lpEnd);
        if (*lpEnd == ' ') *lpEnd = '\0';
        else fLast = TRUE;
        fNotExist = OpenFileRel(lpName, &of, OF_EXIST | OF_SHARE_DENY_NONE) == HFILE_ERROR;
        if (fNotExist) {
            if (!(lpOFN->Flags & OFN_FILEMUSTEXIST) && of.nErrCode == 2) goto accept;
            if (!(lpOFN->Flags & OFN_PATHMUSTEXIST) && of.nErrCode == 3) goto accept;
            if ((lpOFN->Flags & OFN_SHAREAWARE) && of.nErrCode == 0x20) goto accept;
            if (of.nErrCode == 0x20 && lpOFN->lpfnHook) {
                n = (int)lpOFN->lpfnHook(hDlg, g_msgShareViolation, 0, (LPARAM)of.szPathName);
                if (n == OFN_SHARENOWARN) return FALSE;
                if (n == OFN_SHAREFALLTHROUGH) goto accept;
            } else if (of.nErrCode == 5) {
                of.szPathName[0] |= 0x60;
                if (GetDriveType(of.szPathName[0] - 'a') != DRIVE_REMOVABLE) of.nErrCode = 0x41;
            }
            if (of.nErrCode == 0x13 || of.nErrCode == 0x52 || of.nErrCode == 5) *lpName = of.szPathName[0];
        show:
            ShowFileError(hDlg, lpName, of.nErrCode);
            return FALSE;
        }
    accept:
        /* GetFileAttr's failure value 0x80xx counts as read-only when the error code is odd (3.1) */
        if ((lpOFN->Flags & OFN_NOREADONLYRETURN) && (GetFileAttr(of.szPathName) & 1)) {
            of.nErrCode = 0x63;
            goto show;
        }
        if (fSave || (lpOFN->Flags & OFN_NOREADONLYRETURN))
            if ((of.nErrCode = TestCreate(of.szPathName)) != 0) goto show;
        if ((lpOFN->Flags & OFN_OVERWRITEPROMPT) && fSave) { /* also for names that do not exist (3.1) */
            if (!OverwritePrompt(hDlg, of.szPathName)) {
                PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, edt1), 1L);
                return FALSE;
            }
        }
        if (!fLast) {
            lpName = lpEnd + 1;
            while (*lpName == ' ') lpName = AnsiNext(lpName);
            if (*lpName == '\0') fLast = TRUE;
            else *lpEnd = ' ';
        }
    }
    *lpEnd = '\0';
done:
    lpOFN->nFileExtension = 0;
    lpOFN->nFilterIndex = (DWORD)(WORD)SendDlgItemMessage(hDlg, cmb1, CB_GETCURSEL, 0, 0L);
    return TRUE; /* also when the buffer is too small: the caller sees lpstrFile[2] == 0 */
}

/* seg2:2CD4: "...This file does not exist. Create the file?" with the name as typed; IDNO when the
 * string cannot be loaded */
static int CreatePrompt(HWND hDlg, LPSTR lpszFile)
{
    if (lpszFile[2] == '\\' && lpszFile[3] == '\\') lpszFile += 2;
    if (!LoadString(g_hInst, IDS_CREATEPROMPT, g_szMsgFmt, 0x100)) return IDNO;
    if (lstrlen(lpszFile) > 0x100) lpszFile[0x100] = '\0';
    wsprintf(g_szMsg, g_szMsgFmt, (LPSTR)lpszFile);
    GetWindowText(hDlg, g_szMsgFmt, 0x100);
    return MessageBox(hDlg, g_szMsg, g_szMsgFmt, MB_YESNO | MB_ICONQUESTION);
}

static BOOL ListFiles(HWND hDlg, PFILEINST p, LPSTR lpszSpec, WORD wFlags);

/* seg2:2D92: every activation but the first: the drives again (connections may have changed) */
static void RefreshDrives(HWND hDlg, PFILEINST p)
{
    HWND hCombo = GetDlgItem(hDlg, cmb2);
    int iSel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0L);
    SendMessage(hCombo, CB_GETLBTEXT, iSel, (LPARAM)g_szTmp2);
    FillDriveCombo(hDlg, cmb2);
    if (iSel == -1)
        SendMessage(hCombo, CB_SETCURSEL, (WPARAM)-1, 0L);
    else if (SendMessage(hCombo, CB_FINDSTRING, (WPARAM)-1, (LPARAM)g_szTmp2) == CB_ERR) {
        SelectDriveInCombo(hDlg, cmb2, (char)(GetCurrentDrive() + 'a'));
        ListFiles(hDlg, p, NULL, 0x10);
    }
}

/* seg2:2E64: DOS's device names, with or without an extension ("CON.TXT"; CLOCK$ only without) */
static BOOL IsDeviceName(LPSTR lpsz)
{
    static const char *const apszDevices[14] = {"LPT1", "LPT2", "LPT3", "LPT4", "COM1", "COM2", "COM3",
                                                "COM4", "EPT", "NUL", "PRN", "CLOCK$", "CON", "AUX"};
    char c4 = lpsz[4], c3 = lpsz[3];
    int i;
    if (c4 == '.') lpsz[4] = '\0';
    if (c3 == '.') lpsz[3] = '\0';
    for (i = 0; i < 14; i++)
        if (!lstrcmpi(apszDevices[i], lpsz)) break;
    lpsz[4] = c4;
    lpsz[3] = c3;
    return i != 14;
}

/* seg2:2F0C: can a file be made in lpszPath's directory? 0 or a DOS error. The root is not tested. */
static int TestCreate(LPSTR lpszPath)
{
    OFSTRUCT of;
    int nFile, hf;
    lstrcpy(g_szTmp4, lpszPath);
    nFile = pf_file(ParseFile(g_szTmp4));
    if (nFile <= 3) return 0;
    g_szTmp4[nFile - 1] = '\0'; /* "C:\DIR" */
    hf = CreateUniqueFile(g_szTmp4, sizeof g_szTmp4);
    if (hf >= 0) {
        _lclose(hf);
        OemToAnsi(g_szTmp4, g_szTmp4);
        OpenFile(g_szTmp4, &of, OF_DELETE);
        return 0;
    }
    return GetExtendedErrorDOS();
}

/* seg2:2FBC: the UNC directory selected in lst2 into p->szPath ("\\srv\share\d1\"); its length, or 0
 * when lst2 shows no UNC path */
static int GetUNCDirPath(HWND hDlg, int idLB, PFILEINST p)
{
    int cch, iSel, i;
    cch = (int)SendDlgItemMessage(hDlg, idLB, LB_GETTEXT, 0, (LPARAM)p->szPath);
    if (cch < 0 || p->szPath[0] != '\\') return 0;
    iSel = (int)SendDlgItemMessage(hDlg, idLB, LB_GETCURSEL, 0, 0L);
    if ((WORD)iSel < (WORD)(p->cDirs - 1)) p->cDirs = iSel;
    p->szPath[cch++] = '\\';
    for (i = 1; (WORD)i < p->cDirs; i++) {
        cch += (int)SendDlgItemMessage(hDlg, idLB, LB_GETTEXT, i, (LPARAM)(p->szPath + cch));
        p->szPath[cch++] = '\\';
    }
    if (iSel != 0 && (WORD)iSel >= p->cDirs) {
        cch += (int)SendDlgItemMessage(hDlg, idLB, LB_GETTEXT, iSel, (LPARAM)(p->szPath + cch));
        p->szPath[cch++] = '\\';
    }
    p->szPath[cch] = '\0';
    return cch;
}

/* seg2:3114: OK pressed - validation, completion and every error box. TRUE when lpOFN is filled and
 * the dialog may end. lpstrFile becomes OpenFile's full upper-case path. */
static BOOL OKButtonPressed(HWND hDlg, PFILEINST p, BOOL fSave)
{
    LPOPENFILENAME lpOFN = p->lpOFN;
    BOOL fDefExtAdded = FALSE, fUNC = FALSE;
    int cchDir, cchBase, nFile, nExt, n, i;
    HFILE hf;
    char chSave, szDotPath[0x104], szExt[4], szDrive[2];
    DWORD dw;
    OFSTRUCT of;
    LPSTR P = p->szPath;
    memset(&of, 0, sizeof of); /* (3.1 leaves nErrCode unset on the "spec;spec" error path) */

    /* 1. the text; in a UNC directory prefixed with it */
    cchDir = GetUNCDirPath(hDlg, lst2, p);
    cchBase = cchDir ? (int)SendDlgItemMessage(hDlg, lst2, LB_GETTEXTLEN, 0, 0L) : 0;
    GetDlgItemText(hDlg, edt1, P + cchDir, 0xFF);
    if (cchDir) {
        if (P[cchDir + 1] == ':' || (P[cchDir] == '\\' && P[cchDir + 1] == '\\'))
            StrShift(P, P + cchDir);
        else if (P[cchDir] == '\\')
            StrShift(P + cchBase, P + cchDir);
    }

    /* 2. parse */
    dw = ParseFile(P);
    nFile = pf_file(dw);
    nExt = pf_ext(dw);
    if (nFile == PARSE_EMPTYSTRING) {
        ListFiles(hDlg, p, NULL, 0);
        return FALSE;
    }
    if (nFile != PARSE_DIRECTORYNAME && (lpOFN->Flags & OFN_NOVALIDATE)) {
        lpOFN->nFileOffset = nFile;
        lpOFN->nFileExtension = nExt;
        if (lpOFN->lpstrFile) {
            n = lstrlen(P);
            if ((WORD)n < LOWORD(lpOFN->nMaxFile)) /* 3.1 BUG fixed: "<=" wrote the NUL past the buffer */
                lstrcpy(lpOFN->lpstrFile, P);
            else {
                lpOFN->lpstrFile[0] = LOBYTE(n);
                lpOFN->lpstrFile[1] = HIBYTE(n);
                lpOFN->lpstrFile[2] = 0;
            }
        }
        return TRUE;
    }
    if (nFile == PARSE_INVALIDSPACE && (lpOFN->Flags & OFN_ALLOWMULTISELECT)) return MultiSelectOK(hDlg, p, fSave);

    if (P[nExt] == ';') { /* "*.c;*.h" */
        P[nExt] = '\0';
        nFile = pf_file(ParseFile(P));
        P[nExt] = ';';
        if (nFile >= 0 && (StrChr(P + nFile, '*') || StrChr(P + nFile, '?'))) {
            lstrcpy(p->szDefSpec, P + nFile);
            if ((cchDir = SetSpecAndDir(p, hDlg, P)) == 0) return FALSE;
            goto map_dir_error;
        }
        nFile = PARSE_INVALIDCHAR;
        goto show_error;
    }
    if (nFile == PARSE_DIRECTORYNAME) {
        char c = P[nExt - 1];
        if (c == '\\' || c == '/') {
            if (nExt != 1 && P[nExt - 2] != ':' && nExt != cchBase + 1) P[nExt - 1] = '\0'; /* not for roots */
        } else if (c == '.') {
            char c2 = nExt >= 2 ? P[nExt - 2] : 0;
            if ((c2 == '.' || c2 == '\\' || c2 == '/') &&
                ((P[0] == '\\' && P[1] == '\\') || (P[1] == ':' && P[2] == '\\' && P[3] == '\\'))) {
                P[nExt] = '\\';
                P[nExt + 1] = '\0'; /* UNC "..": "..\" */
            }
        }
    } else if (nFile < 0) {
        of.nErrCode = nFile; /* -> "This filename is not valid." */
        goto show_error;
    }

    /* 3. a directory? */
    cchBase = nFile;
    if ((P[0] == '\\' && P[1] == '\\') || (P[1] == ':' && P[2] == '\\' && P[3] == '\\')) {
        fUNC = TRUE;
        goto check_path;
    }
    if ((cchDir = ChangeDir(P, TRUE)) == 0) {
    is_directory:
        /* the whole text was a directory, now the current one: its files */
        SendDlgItemMessage(hDlg, edt1, WM_SETREDRAW, FALSE, 0L);
        SetDlgItemText(hDlg, edt1, "*.*");
        SelectDriveInCombo(hDlg, cmb2, (char)(GetCurrentDrive() + 'a'));
        SendMessage(hDlg, WM_COMMAND, cmb1, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb1), CBN_CLOSEUP));
        SendMessage(hDlg, WM_COMMAND, cmb2, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb2), 0x8000));
        SendDlgItemMessage(hDlg, edt1, WM_SETREDRAW, TRUE, 0L);
        InvalidateRect(GetDlgItem(hDlg, edt1), NULL, FALSE);
        return FALSE;
    }
    if (nFile > 0) {
        /* the directory part only */
        if (nFile > 1 && P[nFile - 1] != ':' && P[nFile - 2] != ':') cchBase = nFile - 1;
        GetCurDirNear(g_szMsg);
        chSave = P[cchBase];
        P[cchBase] = '\0';
        cchDir = ChangeDir(P, TRUE); /* 0 ok, 1 bad drive, 2 bad directory */
        P[cchBase] = chSave;
        ChangeDir(g_szMsg, FALSE);
    }

check_path:
    if (!fUNC && nFile != 0 && cchDir != 0 && (lpOFN->Flags & OFN_PATHMUSTEXIST)) {
    map_dir_error:
        if (cchDir == 2)
            of.nErrCode = 3;
        else if (cchDir == 1) {
            szDrive[0] = of.szPathName[0] = P[0];
            szDrive[1] = '\0';
            if ((LONG)SendDlgItemMessage(hDlg, cmb2, CB_FINDSTRING, (WPARAM)-1, (LPARAM)szDrive) < 0)
                of.nErrCode = 0x61; /* not in the drive list */
            else
                switch (GetDriveType(szDrive[0] - 'a')) {
                case DRIVE_REMOVABLE: of.nErrCode = 5; break;
                case 1: RefreshDrives(hDlg, p); of.nErrCode = 0x61; break;
                default: of.nErrCode = 3; break;
                }
        } else
            of.nErrCode = 2;
        goto show_error;
    }

    /* 4. wildcards: a new filter */
    if (StrChr(P + nFile, '*') || StrChr(P + nFile, '?')) {
        if (!fUNC) {
            if (cchBase) {
                chSave = P[cchBase];
                P[cchBase] = '\0';
                ChangeDir(P, TRUE);
                P[cchBase] = chSave;
            }
            szDotPath[0] = '.';
            szDotPath[1] = '\\';
            if (nExt == 0) lstrcat(P + nFile, "."); /* "*." keeps its dot */
            lstrcpy(szDotPath + 2, P + nFile);
            lstrcpy(p->szDefSpec, P + nFile);
            if (SetSpecAndDir(p, hDlg, szDotPath)) MessageBeep(0);
            return FALSE;
        }
        lstrcpy(p->szDefSpec, P + nFile);
        lstrcpy(P + nFile, "*.*");
    list_unc:
        ListFiles(hDlg, p, P, 0x30);
        lstrcpy(P + nFile, p->szDefSpec);
        LowerAscii(p->szDefSpec);
        SetDlgItemText(hDlg, edt1, p->szDefSpec);
        return FALSE;
    }

    /* 5. a file name */
    if (IsDeviceName(P + nFile)) { of.nErrCode = 0x62; goto show_error; }
    if (nFile == 2 && P[1] == ':') { /* "c:name" -> "c:.\name" */
        lstrcpy(g_szMsg, P + 2);
        lstrcpy(P + 4, g_szMsg);
        P[2] = '.';
        P[3] = '\\';
        nExt += 2;
    }
    if (nExt != 0 && P[nExt] == '\0' && lpOFN->lpstrDefExt && *lpOFN->lpstrDefExt &&
        (DWORD)(nExt + 4) < lpOFN->nMaxFile && (WORD)(nExt + 4) < 0x80) {
        /* no extension typed: name.<default> first */
        fDefExtAdded = TRUE;
        AppendExt(P, p->lpOFN->lpstrDefExt, FALSE);
        if (ChangeDir(P, TRUE) == 0) goto is_directory;
        if (fUNC) {
            hf = OpenFile(P, &of, OF_EXIST | OF_SHARE_DENY_NONE);
            if (hf == HFILE_ERROR) {
                n = lstrlen(of.szPathName);
                if (n && of.szPathName[n - 1] == '\\') of.szPathName[--n] = '\0';
                if (GetFileAttr(of.szPathName) & 0x10) { /* a UNC directory */
                    lstrcpy(P, of.szPathName);
                    n = lstrlen(P);
                    if (P[n - 1] != '\\') { P[n++] = '\\'; P[n] = '\0'; } /* 3.1 BUG fixed: not terminated */
                    goto list_unc;
                }
            }
        } else
            hf = OpenFileRel(P, &of, OF_EXIST | OF_SHARE_DENY_NONE);
        of.szPathName[sizeof of.szPathName - 1] = '\0'; /* (3.1: [127], its OFSTRUCT's last byte) */
        if (of.nErrCode == 0x20) hf = -2; /* a sharing violation: it exists */
        if (hf != HFILE_ERROR) goto exists;
        P[nExt] = '\0'; /* then as typed */
    } else
        fDefExtAdded = FALSE;

    if (fUNC) {
        hf = OpenFile(P, &of, OF_EXIST | OF_SHARE_DENY_NONE);
        if (hf == HFILE_ERROR) {
            n = lstrlen(of.szPathName);
            if (n && of.szPathName[n - 1] == '\\') of.szPathName[--n] = '\0';
            if (GetFileAttr(of.szPathName) & 0x10) {
                lstrcpy(P, of.szPathName);
                n = lstrlen(P);
                if (P[n - 1] != '\\') { P[n++] = '\\'; P[n] = '\0'; } /* 3.1 BUG fixed: not terminated */
                goto list_unc;
            }
        }
    } else
        hf = OpenFileRel(P, &of, OF_EXIST | OF_SHARE_DENY_NONE);
    of.szPathName[sizeof of.szPathName - 1] = '\0';
    if (hf != HFILE_ERROR) goto exists;

    /* 6. no such file */
    if (of.nErrCode == 2 || of.nErrCode == 3) {
        if (fDefExtAdded) { /* the name goes back with the default extension */
            AppendExt(P, lpOFN->lpstrDefExt, FALSE);
            AppendExt(of.szPathName, lpOFN->lpstrDefExt, FALSE);
        }
    } else if (of.nErrCode == 0x20)
        goto share_violation;
    if (!fSave) {
        if (of.nErrCode != 2 && of.nErrCode != 3) goto show_error;
        if (lpOFN->Flags & OFN_FILEMUSTEXIST) {
            if (!(lpOFN->Flags & OFN_CREATEPROMPT)) goto show_error;
            g_fInCreatePrompt = TRUE;
            n = CreatePrompt(hDlg, P);
            g_fInCreatePrompt = FALSE;
            if (n != IDYES) return FALSE;
        }
    }
    if ((lpOFN->Flags & OFN_PATHMUSTEXIST) && !(lpOFN->Flags & OFN_NOTESTFILECREATE)) {
        /* prove the file can be made: create it, delete it, make sure it is gone */
        hf = fUNC ? OpenFile(P, &of, OF_CREATE) : OpenFileRel(P, &of, OF_CREATE);
        of.szPathName[sizeof of.szPathName - 1] = '\0';
        if (hf == HFILE_ERROR) {
            if (of.nErrCode != 0x13 && of.nErrCode != 0x52 && of.nErrCode != 0x41 && of.nErrCode != 5) of.nErrCode = 0;
            goto show_error;
        }
        _lclose(hf);
        if (fUNC) OpenFile(P, &of, OF_DELETE);
        else OpenFileRel(P, &of, OF_DELETE);
        hf = fUNC ? OpenFile(P, &of, OF_EXIST | OF_SHARE_DENY_NONE) : OpenFileRel(P, &of, OF_EXIST | OF_SHARE_DENY_NONE);
        of.szPathName[sizeof of.szPathName - 1] = '\0';
        if (hf != HFILE_ERROR) { of.nErrCode = 0x60; goto show_error; }
    }
    goto success;

exists:
    if ((lpOFN->Flags & OFN_NOREADONLYRETURN) && (GetFileAttr(of.szPathName) & 1)) {
        of.nErrCode = 0x63;
        goto show_error;
    }
    if (fSave || (lpOFN->Flags & OFN_NOREADONLYRETURN))
        if ((n = TestCreate(of.szPathName)) != 0) { of.nErrCode = n; goto show_error; }
    if ((lpOFN->Flags & OFN_OVERWRITEPROMPT) && fSave) {
        if (!OverwritePrompt(hDlg, of.szPathName)) {
            PostMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, edt1), 1L);
            return FALSE;
        }
    }
    if (of.nErrCode != 0x20) goto success;

share_violation:
    if (!(lpOFN->Flags & OFN_SHAREAWARE)) {
        if (!lpOFN->lpfnHook) goto show_error;
        n = (int)lpOFN->lpfnHook(hDlg, g_msgShareViolation, 0, (LPARAM)of.szPathName);
        if (n == OFN_SHARENOWARN) return FALSE;
        if (n != OFN_SHAREFALLTHROUGH) goto show_error;
    }

success:
    HourGlass(TRUE); /* off again in GetFileName */
    OemToAnsi(of.szPathName, of.szPathName);
    dw = ParseFile(of.szPathName);
    if (pf_file(dw) < 0) {
        /* arch311: DOS hands out 8.3 names only, so 3.1's ParseFile never refuses OpenFile's path;
         * libw16's drives hold Linux names, and a directory longer than 8.3 on the way makes it
         * refuse (PARSE_FILETOOLONG). The offsets then come from the path itself (a negative one
         * would index outside it). */
        LPSTR lpName = StrRChr(of.szPathName, of.szPathName + lstrlen(of.szPathName), '\\');
        lpName = lpName ? lpName + 1 : of.szPathName;
        LPSTR lpDot = StrRChr(lpName, lpName + lstrlen(lpName), '.');
        dw = MAKELONG(lpName - of.szPathName, lpDot ? lpDot + 1 - of.szPathName : lstrlen(of.szPathName));
    }
    lpOFN->nFileOffset = LOWORD(dw);
    lpOFN->nFileExtension = (nExt != 0 || fDefExtAdded) ? HIWORD(dw) : 0; /* nExt of the typed text */
    lpOFN->Flags &= ~OFN_EXTENSIONDIFFERENT;
    if (lpOFN->lpstrDefExt && lpOFN->nFileExtension) {
        for (i = 0; i < 3 && lpOFN->lpstrDefExt[i]; i++) szExt[i] = lpOFN->lpstrDefExt[i];
        szExt[i] = '\0';
        if (lstrcmpi(szExt, of.szPathName + HIWORD(dw))) lpOFN->Flags |= OFN_EXTENSIONDIFFERENT;
    }
    if (lpOFN->lpstrFile) {
        n = lstrlen(of.szPathName);
        if ((WORD)n < LOWORD(lpOFN->nMaxFile)) /* 3.1 BUG fixed: "<=" wrote the NUL past the buffer */
            lstrcpy(lpOFN->lpstrFile, of.szPathName);
        else {
            lpOFN->lpstrFile[0] = LOBYTE(n);
            lpOFN->lpstrFile[1] = HIBYTE(n);
            lpOFN->lpstrFile[2] = 0;
        }
    }
    if (lpOFN->lpstrFileTitle) {
        n = lstrlen(of.szPathName + LOWORD(dw));
        if ((DWORD)n >= lpOFN->nMaxFileTitle && lpOFN->nMaxFileTitle)
            of.szPathName[LOWORD(dw) + lpOFN->nMaxFileTitle - 1] = '\0';
        lstrcpy(lpOFN->lpstrFileTitle, of.szPathName + LOWORD(dw));
    }
    /* "if (Flags | 1)" in 3.1: always - OFN_READONLY follows the box, cleared when it is hidden */
    if (IsDlgButtonChecked(hDlg, chx1)) lpOFN->Flags |= OFN_READONLY;
    else lpOFN->Flags &= ~OFN_READONLY;
    return TRUE;

show_error:
    if (fUNC) {
        of.nErrCode = 2;
        n = lstrlen(P) - 1;
        if (n >= 2 && P[n] == '\\' && P[n - 1] == '.' && P[n - 2] == '\\') P[n - 2] = '\0';
    } else if (nFile == 2 && P[2] == '.')
        StrShift(P + 2, P + 4); /* undo "c:.\name" */
    if (of.nErrCode == 5) {
        of.szPathName[0] |= 0x60;
        if (GetDriveType(of.szPathName[0] - 'a') != DRIVE_REMOVABLE) of.nErrCode = 0x41;
    }
    if (of.nErrCode == 0x13 || of.nErrCode == 0x52 || of.nErrCode == 5) P[0] = of.szPathName[0];
    ShowFileError(hDlg, P, of.nErrCode);
    return FALSE;
}

/* seg2:45B8: drive changed or lst2 moved: only the name / spec part stays in edt1 */
static void KeepFileSpecInEdit(HWND hDlg)
{
    char sz[0x100];
    DWORD dw;
    int nFile, nExt;
    if (!GetDlgItemText(hDlg, edt1, sz, 0xFF)) return;
    dw = ParseFile(sz);
    nFile = pf_file(dw);
    nExt = pf_ext(dw);
    if (nFile < 0) {
        if (sz[nExt] == ';') {
            sz[nExt] = '\0';
            nFile = pf_file(ParseFile(sz));
            sz[nExt] = ';';
            if (nFile < 0) sz[0] = '\0';
        } else
            sz[0] = '\0';
    }
    if (nFile > 0) StrShift(sz, sz + nFile);
    if (nFile != 0) SetDlgItemText(hDlg, edt1, sz);
}

/* seg2:4698 (seg1:2BCA MemMove inclusive of the NUL) */
static void StripBrackets(LPSTR lpsz)
{
    if (*lpsz == '[') {
        LPSTR p = lpsz + 1;
        while (*p && *p != ']') p = AnsiNext(p);
        *p = '\0';
        memmove(lpsz, lpsz + 1, p - lpsz);
    }
}

/* seg2:4724: WM_COMMAND from cmb2 */
static BOOL DriveComboCommand(HWND hDlg, LPARAM lParam, PFILEINST p)
{
    LPOPENFILENAME lpOFN = p->lpOFN;
    HWND hCombo = W16_CMD_HWND(lParam);
    char szItem[0x100], chOrig;
    LPSTR lpSpec;
    int iSel;
    UINT ids;
    switch (HIWORD(lParam)) {
    case CBN_DROPDOWN:
        if (g_wWinVer >= 0x30A) lpOFN->Flags |= FLAG_DRIVEDROPPED;
        return TRUE;
    case CBN_CLOSEUP:
        PostMessage(hDlg, WM_COMMAND, cmb2, MAKELPARAM(LOWORD(lParam), 0x8000));
        return TRUE;
    case CBN_SELCHANGE:
        KeepFileSpecInEdit(hDlg);
        if (lpOFN->Flags & FLAG_DRIVEDROPPED) return TRUE; /* the list is open: CBN_CLOSEUP decides */
        /* fall through */
    case 0x8000:
        break;
    default:
        return FALSE;
    }
    HourGlass(TRUE);
    lpOFN->Flags &= ~FLAG_DRIVEDROPPED;
    if ((LONG)SendMessage(hCombo, CB_GETCURSEL, 0, 0L) < 0) goto done;
    SendMessage(hCombo, CB_GETLBTEXT, (WPARAM)SendMessage(hCombo, CB_GETCURSEL, 0, 0L), (LPARAM)szItem);
    if (szItem[0] == '\0') goto done;
    chOrig = (char)(GetCurrentDrive() + 'a');
    lpSpec = NULL;
    if (g_fInitializing) { /* the first filling, from InitFileDlg */
        lpSpec = g_szBuf1;
        if (lpOFN->lpstrFile && (StrChr(lpOFN->lpstrFile, '*') || StrChr(lpOFN->lpstrFile, '?')))
            lstrcpy(g_szBuf1, lpOFN->lpstrFile);
        else {
            HWND hCmb1 = GetDlgItem(hDlg, cmb1);
            iSel = (int)SendMessage(hCmb1, CB_GETCURSEL, 0, 0L);
            if (iSel < 0) lpSpec = NULL;
            else if (iSel == 0 && lpOFN->lpstrCustomFilter && *lpOFN->lpstrCustomFilter)
                lpSpec = lpOFN->lpstrCustomFilter + lstrlen(lpOFN->lpstrCustomFilter) + 1;
            else
                lpSpec = (LPSTR)lpOFN->lpstrFilter + (WORD)SendMessage(hCmb1, CB_GETITEMDATA, iSel, 0L);
        }
    }
    if (lpSpec) {
        if (lpSpec != g_szBuf1) lstrcpy(g_szBuf1, lpSpec);
        LowerAscii(g_szBuf1);
    }
    if (!ChangeDriveTo(szItem[0])) goto failed;
    for (;;) {
        if (ListFiles(hDlg, p, lpSpec ? g_szBuf1 : NULL, 0x4010)) break;
    failed:
        if (g_wCurDirErr == 0x22) ids = IDS_WRONGDISK;
        else if (GetDriveType(szItem[0] - 'a') != DRIVE_REMOVABLE && !IsCDROM(szItem[0] - 'a')) ids = IDS_CANTSELDRIVE;
        else ids = IDS_CANTREAD;
        if (!LoadString(g_hInst, ids, g_szBuf1, 0xC0)) {
            MessageBeep(0);
            g_szTmp2[0] = '\0';
        } else
            wsprintf(g_szTmp2, g_szBuf1, (int)szItem[0]);
        GetWindowText(hDlg, g_szBuf1, 0xC0);
        if (!g_fFirstActivate && MessageBox(hDlg, g_szTmp2, g_szBuf1, MB_RETRYCANCEL | MB_ICONEXCLAMATION) == IDRETRY) {
            if (!ChangeDriveTo(szItem[0])) goto failed;
        } else {
            /* Cancel (or quietly while the dialog first comes up): back to the drive before */
            if (chOrig == szItem[0] && ChangeDir("\\", TRUE) != 0) chOrig = 'c';
            szItem[0] = chOrig;
            ChangeDriveTo(szItem[0]);
        }
        SelectDriveInCombo(hDlg, cmb2, szItem[0]);
    }
    if (lpOFN->lpfnHook) {
        iSel = (int)SendDlgItemMessage(hDlg, cmb2, CB_GETCURSEL, 0, 0L);
        lpOFN->lpfnHook(hDlg, g_msgLBSelChanged, cmb2, MAKELPARAM(iSel, CD_LBSELCHANGE));
    }
done:
    HourGlass(FALSE);
    return TRUE;
}

/* seg2:4C40: WM_COMMAND of both dialogs */
static BOOL FileDlgCommand(HWND hDlg, WPARAM wID, LPARAM lParam, PFILEINST p, BOOL fSave)
{
    LPOPENFILENAME lpOFN;
    int nRet;
    if (p == NULL) return FALSE;
    lpOFN = p->lpOFN;

    switch (wID) {
    case IDOK:
        if (g_fOKFromLB || (GetFocus() == GetDlgItem(hDlg, lst2) && (lpOFN->Flags & FLAG_DIRPENDING))) {
            g_fOKFromLB = FALSE;
            goto open_selected_dir;
        }
        if (GetFocus() == GetDlgItem(hDlg, cmb2) && (lpOFN->Flags & FLAG_DRIVEDROPPED)) {
            SendDlgItemMessage(hDlg, cmb2, CB_SHOWDROPDOWN, FALSE, 0L); /* Enter closes the list */
            return FALSE;
        }
        if (GetFocus() == GetDlgItem(hDlg, cmb1) && (lpOFN->Flags & FLAG_TYPEDROPPED)) {
            SendDlgItemMessage(hDlg, cmb1, CB_SHOWDROPDOWN, FALSE, 0L);
            lParam = W16_CMD_LPARAM(GetDlgItem(hDlg, cmb1), HIWORD(lParam));
            goto filter_selected; /* (the dropped bit stays set here) */
        }
        if (!OKButtonPressed(hDlg, p, fSave)) {
            SendDlgItemMessage(hDlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
            return TRUE;
        }
        if (lpOFN->lpstrFile == NULL) nRet = TRUE;
        else if (lpOFN->lpstrFile[2] == '\0' && !(lpOFN->Flags & OFN_NOVALIDATE)) {
            nRet = FALSE; /* the buffer is too small */
            cd_err = FNERR_BUFFERTOOSMALL;
        } else
            nRet = TRUE;
        goto end_dialog;

    case IDCANCEL: nRet = FALSE; goto end_dialog;
    case IDABORT: nRet = LOWORD(lParam); goto end_dialog; /* a hook ends the dialog */

    end_dialog:
        lpOFN->nFilterIndex = (DWORD)(WORD)SendDlgItemMessage(hDlg, cmb1, CB_GETCURSEL, 0, 0L);
        if (lpOFN->lpstrCustomFilter) {
            int cchDesc = lstrlen(lpOFN->lpstrCustomFilter) + 1;
            int cchSpec = lstrlen(p->szDefSpec);
            if (lpOFN->nMaxCustFilter > (DWORD)(WORD)(cchSpec + cchDesc))
                lstrcpy(lpOFN->lpstrCustomFilter + cchDesc, p->szDefSpec); /* the last spec */
        }
        if (!lpOFN->lpstrCustomFilter || !*lpOFN->lpstrCustomFilter) lpOFN->nFilterIndex++;
        if (wID == IDOK && lpOFN->lpfnHook) {
            if (lpOFN->lpfnHook(hDlg, g_msgFileNameOK, 0, (LPARAM)lpOFN)) {
                HourGlass(FALSE);
                return FALSE; /* the hook turned the name down */
            }
        }
        RemoveProp(hDlg, PROP_INST);
        if (lpOFN->Flags & OFN_ENABLEHOOK) g_lpfnHook = lpOFN->lpfnHook; /* it keeps getting messages */
        if (p) {
            if ((lpOFN->Flags & OFN_NOCHANGEDIR) && p->szInitDir[0]) ChangeDir(p->szInitDir, FALSE);
            free(p);
        }
        /* LocalShrink(NULL, 0) after OFN_ALLOWMULTISELECT: no local heap here */
        EndDialog(hDlg, nRet);
        return TRUE;

    case lst1:
        switch (HIWORD(lParam)) {
        case LBN_DBLCLK:
            SendMessage(hDlg, WM_COMMAND, IDOK, 0L);
            return TRUE;
        case LBN_SELCHANGE: {
            HWND hLB = W16_CMD_HWND(lParam);
            int si = 0, wCode = 0, n = 0;
            if (lpOFN->Flags & OFN_ALLOWMULTISELECT) {
                n = (int)SendMessage(hLB, LB_GETSELCOUNT, 0, 0L);
                if (n == 0)
                    SetDlgItemText(hDlg, edt1, "");
                else {
                    int *aIdx = malloc(sizeof(int) * n); /* LocalAlloc(LMEM_FIXED, n * 2) */
                    if (aIdx) {
                        n = (int)SendMessage(hLB, LB_GETSELITEMS, n, (LPARAM)aIdx);
                        char *pszBuf = malloc(0x800), *cur = pszBuf;
                        if (pszBuf) {
                            *pszBuf = '\0';
                            for (si = 0; si < n; si++) {
                                int len = (int)SendMessage(hLB, LB_GETTEXT, aIdx[si], (LPARAM)cur);
                                if (len < 0) len = 0;
                                if (!StrChr(cur, '.')) cur[len++] = '.'; /* "README" -> "README." */
                                cur += len;
                                *cur++ = ' ';
                                if (cur - pszBuf > 0x7F1) break;
                            }
                            if (cur != pszBuf) *--cur = '\0';
                            LowerAscii(pszBuf);
                            SetDlgItemText(hDlg, edt1, pszBuf);
                            free(pszBuf);
                        }
                        free(aIdx);
                    }
                }
                if (lpOFN->lpfnHook) {
                    si = (int)SendMessage(hLB, LB_GETCARETINDEX, 0, 0L);
                    if (n == 0) wCode = CD_LBSELNOITEMS;
                    else if (SendMessage(hLB, LB_GETSEL, si, 0L)) wCode = CD_LBSELADD;
                    else wCode = CD_LBSELSUB;
                }
            } else {
                g_szTmp1[0] = '\0';
                si = (int)SendMessage(hLB, LB_GETTEXT, (WPARAM)SendMessage(hLB, LB_GETCURSEL, 0, 0L), (LPARAM)g_szTmp1);
                if (si < 0) si = 0;
                if (!StrChr(g_szTmp1, '.')) { g_szTmp1[si] = '.'; g_szTmp1[si + 1] = '\0'; }
                LowerAscii(g_szTmp1);
                SetDlgItemText(hDlg, edt1, g_szTmp1);
                if (lpOFN->lpfnHook) {
                    si = (int)SendMessage(hLB, LB_GETCURSEL, 0, 0L);
                    wCode = CD_LBSELCHANGE;
                }
            }
            if (lpOFN->lpfnHook) lpOFN->lpfnHook(hDlg, g_msgLBSelChanged, lst1, MAKELPARAM(si, wCode));
            SendDlgItemMessage(hDlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
            return TRUE;
        }
        default:
            return FALSE;
        }

    case edt1:
        if (HIWORD(lParam) == EN_CHANGE) {
            /* type-ahead: lst1 scrolls to the first name starting with the text, nothing selected */
            HWND hLB = GetDlgItem(hDlg, lst1);
            int iCaret = (int)SendMessage(hLB, LB_GETCARETINDEX, 0, 0L);
            int i, iTop;
            RECT rc;
            SendMessage(GetDlgItem(hDlg, edt1), WM_GETTEXT, 0x100, (LPARAM)g_szTmp1);
            i = (int)SendMessage(hLB, LB_FINDSTRING, iCaret, (LPARAM)g_szTmp1);
            if (i != LB_ERR && g_cyFont) { /* (g_cyFont is set by then; the test keeps a divide by 0 out) */
                iTop = (int)SendMessage(hLB, LB_GETTOPINDEX, 0, 0L);
                GetClientRect(hLB, &rc);
                if ((WORD)i < (WORD)iTop || (WORD)i >= (WORD)(iTop + (WORD)rc.bottom / (WORD)g_cyFont)) {
                    SendMessage(hLB, LB_SETCARETINDEX, i, 0L);
                    SendMessage(hLB, LB_SETTOPINDEX, i, 0L);
                }
            }
            return TRUE;
        }
        return FALSE;

    case lst2:
        switch (HIWORD(lParam)) {
        case LBN_SELCHANGE:
            if (!(lpOFN->Flags & FLAG_DIRPENDING) &&
                (int)SendDlgItemMessage(hDlg, lst2, LB_GETCURSEL, 0, 0L) != p->cDirs - 1) {
                KeepFileSpecInEdit(hDlg);
                lpOFN->Flags |= FLAG_DIRPENDING;
            }
            return TRUE;
        case LBN_SETFOCUS:
            EnableWindow(GetDlgItem(hDlg, IDOK), TRUE);
            SendMessage(GetDlgItem(hDlg, IDCANCEL), BM_SETSTYLE, BS_PUSHBUTTON, 1L);
            return FALSE;
        case LBN_KILLFOCUS:
            if (lpOFN && (lpOFN->Flags & FLAG_DIRPENDING)) lpOFN->Flags &= ~FLAG_DIRPENDING;
            else g_fOKFromLB = FALSE;
            return FALSE;
        case LBN_DBLCLK:
        open_selected_dir: {
            int cch, iSel, i;
            LPSTR psz;
            lpOFN->Flags &= ~FLAG_DIRPENDING;
            if ((cch = GetUNCDirPath(hDlg, lst2, p)) != 0) {
                lstrcpy(p->szPath + cch, "*.*");
                ListFiles(hDlg, p, p->szPath, 0x30);
                lstrcpy(p->szPath + cch, p->szDefSpec);
                LowerAscii(p->szDefSpec);
                SetDlgItemText(hDlg, edt1, p->szDefSpec);
                return TRUE;
            }
            iSel = (int)SendDlgItemMessage(hDlg, lst2, LB_GETCURSEL, 0, 0L);
            p->szPath[0] = '\0';
            if ((WORD)iSel >= p->cDirs) { /* a subdirectory: its name */
                SendDlgItemMessage(hDlg, lst2, LB_GETTEXT, iSel, (LPARAM)p->szPath);
                iSel = p->cDirs;
            } else { /* an ancestor: the path down to it */
                cch = (int)SendDlgItemMessage(hDlg, lst2, LB_GETTEXT, 0, (LPARAM)p->szPath);
                for (i = 1; (WORD)i <= (WORD)iSel; i++) {
                    cch += (int)SendDlgItemMessage(hDlg, lst2, LB_GETTEXT, i, (LPARAM)(p->szPath + cch));
                    p->szPath[cch++] = '\\';
                }
                if (iSel != 0) p->szPath[cch - 1] = '\0';
            }
            psz = p->szPath;
            if (*psz && ChangeDir(psz, TRUE) == 0) {
                ListFiles(hDlg, p, NULL, 0x10);
                if (lpOFN->lpfnHook) lpOFN->lpfnHook(hDlg, g_msgLBSelChanged, lst2, MAKELPARAM(iSel, CD_LBSELCHANGE));
                return TRUE;
            }
            return FALSE;
        }
        default:
            return FALSE;
        }

    case cmb1:
        switch (HIWORD(lParam)) {
        case CBN_DROPDOWN:
            if (g_wWinVer >= 0x30A) lpOFN->Flags |= FLAG_TYPEDROPPED;
            return TRUE;
        case CBN_CLOSEUP:
            PostMessage(hDlg, WM_COMMAND, cmb1, MAKELPARAM(LOWORD(lParam), 0x8000));
            return TRUE;
        case CBN_SELCHANGE:
            if (lpOFN->Flags & FLAG_TYPEDROPPED) return TRUE; /* the list is open: wait */
            /* fall through */
        case 0x8000:
            lpOFN->Flags &= ~FLAG_TYPEDROPPED;
        filter_selected: {
            int iSel = (int)SendDlgItemMessage(hDlg, cmb1, CB_GETCURSEL, 0, 0L);
            LPSTR lpSpec;
            BOOL fReplace;
            if (iSel < 0) return FALSE;
            HourGlass(TRUE);
            if (iSel == 0 && lpOFN->lpstrCustomFilter && *lpOFN->lpstrCustomFilter)
                lpSpec = lpOFN->lpstrCustomFilter + lstrlen(lpOFN->lpstrCustomFilter) + 1;
            else
                lpSpec = (LPSTR)lpOFN->lpstrFilter + (WORD)SendDlgItemMessage(hDlg, cmb1, CB_GETITEMDATA, iSel, 0L);
            if (*lpSpec) {
                GetDlgItemText(hDlg, edt1, g_szTmp1, 0xFF);
                fReplace = g_szTmp1[0] == '\0' || StrChr(g_szTmp1, '*') || StrChr(g_szTmp1, '?');
                lstrcpy(g_szTmp1, lpSpec);
                if (fReplace) { /* a typed file name stays */
                    LowerAscii(g_szTmp1);
                    SetDlgItemText(hDlg, edt1, g_szTmp1);
                    SendDlgItemMessage(hDlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                }
                SetSpecAndDir(p, hDlg, g_szTmp1);
                /* g_szTmp1 has been tokenised by ListFiles: the first pattern only, in upper case */
                if (!g_fInitializing) lstrcpy(p->szDefSpec, g_szTmp1);
            }
            if (lpOFN->lpfnHook) lpOFN->lpfnHook(hDlg, g_msgLBSelChanged, cmb1, MAKELPARAM(iSel, CD_LBSELCHANGE));
            HourGlass(FALSE);
            return TRUE;
        }
        default:
            return FALSE;
        }

    case cmb2:
        return DriveComboCommand(hDlg, lParam, p);

    case pshHelp:
        if (g_msgHelp && lpOFN->hwndOwner) SendMessage(lpOFN->hwndOwner, g_msgHelp, (WPARAM)hDlg, (LPARAM)lpOFN);
        return FALSE;

    case psh14: /* "Network..." */
        if (ConnectNetDrive(hDlg)) {
            char chDrive = (char)(GetCurrentDrive() + 'a');
            if (g_pfnNetLastConn) {
                WORD w = 0xFFFF;
                if (g_pfnNetLastConn(1, &w) == 0 && w != 0xFFFF) chDrive = (char)(w + 'a');
            }
            FillDriveCombo(hDlg, cmb2);
            SelectDriveInCombo(hDlg, cmb2, chDrive);
            SendMessage(hDlg, WM_COMMAND, cmb2, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb2), CBN_SELCHANGE));
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static void MeasureItem(HWND hDlg, LPMEASUREITEMSTRUCT lpMIS);
static void DrawItem(PFILEINST p, HWND hDlg, WPARAM wParam, LPDRAWITEMSTRUCT lpDIS, BOOL fSave);

/* seg2:5CAE FileOpenDlgProc / seg2:5E88 FileSaveDlgProc. The hook sees every message first; a
 * non-zero answer is the dialog's. */
static BOOL FileDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam, BOOL fSave)
{
    PFILEINST p = (PFILEINST)GetProp(hDlg, PROP_INST);
    BOOL r;
    if (p) {
        if (p->lpOFN->lpfnHook && (r = (BOOL)p->lpOFN->lpfnHook(hDlg, msg, wParam, lParam)) != 0) return r;
    } else if (g_lpfnHook && msg != WM_INITDIALOG) {
        if ((r = (BOOL)g_lpfnHook(hDlg, msg, wParam, lParam)) != 0) return r;
    }
    switch (msg) {
    case WM_INITDIALOG:
        g_lpfnHook = NULL;
        if (fSave && !(((LPOPENFILENAME)lParam)->Flags & (OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE))) {
            LoadString(g_hInst, IDS_SAVEAS, g_szBuf1, 0x40);
            SetWindowText(hDlg, g_szBuf1);
            LoadString(g_hInst, IDS_SAVETYPE, g_szBuf1, 0x40);
            SetDlgItemText(hDlg, stc2, g_szBuf1);
        }
        g_fInitializing = TRUE;
        r = InitFileDlg(hDlg, wParam, (LPOPENFILENAME)lParam);
        g_fInitializing = FALSE;
        HourGlass(FALSE);
        return r;
    case WM_ACTIVATE:
        if (!g_fInCreatePrompt) {
            if (g_fFirstActivate == TRUE) g_fFirstActivate = FALSE;
            else if (LOWORD(wParam) != WA_INACTIVE && p) RefreshDrives(hDlg, p);
        }
        return FALSE;
    case WM_MEASUREITEM:
        MeasureItem(hDlg, (LPMEASUREITEMSTRUCT)lParam);
        return TRUE;
    case WM_DRAWITEM:
        if (!g_fNoDraw) DrawItem(p, hDlg, wParam, (LPDRAWITEMSTRUCT)lParam, fSave);
        return TRUE;
    case WM_SYSCOLORCHANGE:
        LoadSysColors();
        LoadFolderBitmaps();
        return TRUE;
    case WM_COMMAND:
        return FileDlgCommand(hDlg, wParam, lParam, p, fSave);
    }
    return FALSE;
}
static BOOL FileOpenDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) { return FileDlgProc(hDlg, msg, wParam, lParam, FALSE); }
static BOOL FileSaveDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) { return FileDlgProc(hDlg, msg, wParam, lParam, TRUE); }

/* seg2:60D0: -1, 0 or 1 */
static int Sign(int x) { return x < 0 ? -1 : x > 0; }

/* seg2:60EA: the descriptions of "desc\0pattern\0..." into cmb1, each item's data the offset of its
 * pattern; the index of the last one added */
static int FillFilterCombo(HWND hDlg, LPCSTR lpFilter)
{
    DWORD dwOff = 0;
    int iItem = 0, len; /* (3.1 returns an unset value when the filter is empty) */
    while (*lpFilter) {
        iItem = (int)SendDlgItemMessage(hDlg, cmb1, CB_ADDSTRING, 0, (LPARAM)lpFilter);
        len = lstrlen(lpFilter) + 1;
        lpFilter += len;
        dwOff += len;
        SendDlgItemMessage(hDlg, cmb1, CB_SETITEMDATA, iItem, (LPARAM)dwOff);
        len = lstrlen(lpFilter) + 1;
        lpFilter += len;
        dwOff += len;
    }
    return iItem;
}

/* seg2:6194: the item starting with the drive letter, or none */
static void SelectDriveInCombo(HWND hDlg, int id, char chDrive)
{
    WORD n, i;
    chDrive |= 0x60;
    n = (WORD)SendDlgItemMessage(hDlg, id, CB_GETCOUNT, 0, 0L);
    g_szTmp2[0] = '\0';
    for (i = 0; i < n; i++) {
        SendDlgItemMessage(hDlg, id, CB_GETLBTEXT, i, (LPARAM)g_szTmp2);
        if (g_szTmp2[0] == chDrive) break;
        g_szTmp2[0] = '\0';
    }
    SendDlgItemMessage(hDlg, id, CB_SETCURSEL, i == n ? (WPARAM)-1 : i, 0L);
}

/* seg2:624E: lpBuf[0] = the current drive, then every drive there is (DOS: every one AH=0Eh can
 * select; here every letter mapped to a folder), in order; the count */
static int EnumDrives(LPSTR lpBuf)
{
    int n = 0;
    *lpBuf++ = (char)GetCurrentDrive();
    for (int d = 0; d < 26; d++)
        if (!w16_drive_root((char)('A' + d), NULL, 0)) { n++; *lpBuf++ = (char)d; }
    return n;
}

/* seg2:629A: cmb2's items: "a:" for floppies and CD-ROMs (no disk access), "c: <label>" for the
 * other local drives, "x: <connection>" for network drives, lower case; item data = bitmap cell */
static void FillDriveCombo(HWND hDlg, int id)
{
    HWND hCombo = GetDlgItem(hDlg, id);
    char ab[0x1E];
    int n, i, iIns = -1, drv;
    WORD wType;
    g_fNoDraw |= 2;
    SendMessage(hCombo, WM_SETREDRAW, FALSE, 0L);
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0L);
    n = EnumDrives(ab);
    for (i = 0; i < n; i++) {
        drv = (signed char)ab[i + 1];
        g_szTmp1[0] = (char)(drv + 'A');
        g_szTmp1[1] = ':';
        g_szTmp1[2] = '\0';
        wType = GetDriveType(drv);
        if (wType < 2) continue;
        iIns++;
        if (wType != DRIVE_REMOVABLE && !(wType == DRIVE_REMOTE && IsCDROM(drv)) && ChangeDriveTo(g_szTmp1[0])) {
            if (wType != DRIVE_REMOTE) { /* fixed or RAM disk: the volume label */
                g_szTmp1[2] = ' ';
                GetVolumeLabel(drv, g_szTmp1 + 3, FALSE);
                StripBrackets(g_szTmp1 + 3);
                OemToAnsi(g_szTmp1, g_szTmp1);
            } else { /* network: the connection's name (libw16's WNetGetConnection has none) */
                char szRemote[0x40];
                WORD cb = 0x40;
                szRemote[0] = '\0';
                WNetGetConnection(g_szTmp1, szRemote, &cb);
                wsprintf(g_szTmp1 + 2, " %s", (LPSTR)szRemote);
            }
        }
        LowerAscii(g_szTmp1);
        SendMessage(hCombo, CB_INSERTSTRING, iIns, (LPARAM)g_szTmp1);
        SendMessage(hCombo, CB_SETITEMDATA, iIns, (LPARAM)GetDriveBitmapIndex(drv, wType));
        if (drv == (signed char)ab[0]) SendMessage(hCombo, CB_SETCURSEL, iIns, 0L);
    }
    g_fNoDraw &= ~2;
    SendMessage(hCombo, WM_SETREDRAW, TRUE, 0L);
    ChangeDriveTo((char)(ab[0] + 'a')); /* the current drive again */
}

/* seg2:64A6: ".ext" (at most 3 characters of lpExt), after a '*' with fStar */
static void AppendExt(LPSTR lpDst, LPCSTR lpExt, BOOL fStar)
{
    char szExt[4];
    int n, i;
    if (!lpExt || !*lpExt) return;
    n = lstrlen(lpDst);
    if (fStar) lpDst[n++] = '*';
    lpDst[n++] = '.';
    for (i = 0; lpExt[i] && i < 3; i++) szExt[i] = lpExt[i];
    szExt[i] = '\0';
    lstrcpy(lpDst + n, szExt);
}

/* seg2:6584: a spec that may name a directory ("c:\dir\*.txt", "*.c;*.h", "..\*.*"): into the
 * directory, the spec stored, the lists refilled. 0 or ChangeDir's error. */
static int SetSpecAndDir(PFILEINST p, HWND hDlg, LPSTR lpSpec)
{
    int nRet = 0, n;
    LPSTR lpSlash;
    char c;
    LowerAscii(lpSpec);
    lpSlash = StrRChr(lpSpec, lpSpec + lstrlen(lpSpec), '\\');
    if (lpSlash == NULL && StrChr(lpSpec, ':') == NULL) { /* just a spec */
        lstrcpy(p->szSpec, lpSpec);
        if (!g_fInitializing) {
            if ((n = GetUNCDirPath(hDlg, lst2, p)) != 0) {
                lstrcpy(p->szDefSpec, p->szSpec);
                lstrcpy(p->szPath + n, "*.*");
                ListFiles(hDlg, p, p->szPath, 0x20);
                nRet = 0;
            } else {
                lstrcpy(lpSpec, p->szSpec);
                ListFiles(hDlg, p, lpSpec, 0);
            }
        }
        return nRet;
    }
    g_szTmp4[0] = '\0';
    if (lpSlash == StrChr(lpSpec, '\\')) { /* no or one backslash */
        if (lpSlash == NULL)
            lpSlash = AnsiNext(AnsiNext(lpSpec)); /* "c:*.txt" */
        else if (lpSlash == lpSpec || (lpSlash - 2 == lpSpec && lpSpec[1] == ':'))
            lpSlash = AnsiNext(lpSlash); /* "\*.txt", "c:\*.txt" */
        else
            goto split_at_slash; /* "dir\*.txt" */
        c = *lpSlash;
        if (c != '.') *lpSlash = '\0';
        lstrcpy(g_szTmp4, lpSpec);
        if (c == '.') { /* "c:\." and the like: the directory only, spec "*.<default extension>" */
            lpSlash = lpSpec + lstrlen(lpSpec);
            AppendExt(lpSlash, p->lpOFN->lpstrDefExt, TRUE);
        } else
            *lpSlash = c;
    } else {
    split_at_slash:
        *lpSlash++ = '\0';
        lstrcpy(g_szTmp4, lpSpec);
    }
    if ((nRet = ChangeDir(g_szTmp4, TRUE)) != 0) return nRet;
    lstrcpy(p->szSpec, lpSlash);
    SetDlgItemText(hDlg, edt1, p->szSpec);
    SelectDriveInCombo(hDlg, cmb2, (char)(GetCurrentDrive() + 'a'));
    if (!g_fInitializing) SendMessage(hDlg, WM_COMMAND, cmb2, W16_CMD_LPARAM(GetDlgItem(hDlg, cmb2), 0x8000));
    return 0;
}

/* seg2:6898: "<path>\nThis file already exists.\n\nReplace existing file?" with the full upper-case
 * path, No the default */
static BOOL OverwritePrompt(HWND hDlg, LPSTR lpszFile)
{
    if (!LoadString(g_hInst, IDS_FILEEXISTS, g_szMsgFmt, 0xBF)) return FALSE;
    if (lpszFile[2] == '\\' && lpszFile[3] == '\\') lpszFile += 2;
    OemToAnsi(lpszFile, g_szTmp4);
    wsprintf(g_szMsg, g_szMsgFmt, (LPSTR)g_szTmp4);
    GetWindowText(hDlg, g_szMsgFmt, 0x40);
    return MessageBox(hDlg, g_szMsg, g_szMsgFmt, MB_YESNO | MB_ICONEXCLAMATION | MB_DEFBUTTON2) == IDYES;
}

/* seg2:6960: lst1 items are as high as the dialog font, lst2 and cmb2 items max(16, that); measured
 * once for the life of the DLL */
static void MeasureItem(HWND hDlg, LPMEASUREITEMSTRUCT lpMIS)
{
    if (g_cyItem == 0) {
        HDC hdc = GetDC(hDlg);
        HFONT hFont = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0L);
        TEXTMETRIC tm;
        if (!hFont) hFont = GetStockObject(SYSTEM_FONT);
        hFont = SelectObject(hdc, hFont);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, hFont);
        ReleaseDC(hDlg, hdc);
        g_cyFont = tm.tmHeight;
        g_cyItem = ((WORD)g_cyBmp > (WORD)g_cyFont) ? g_cyBmp : g_cyFont;
    }
    lpMIS->itemHeight = lpMIS->CtlID == lst1 ? g_cyFont : g_cyItem;
}

/* seg2:6A28: lst1, lst2 and cmb2 items: lower case; lst2 indented 4 px a level behind its folder
 * (open above the current directory, open and shaded for it, closed below), cmb2 behind its drive;
 * the highlight on lst2 only while it has the focus; Save As's file names grey, never highlighted */
static void DrawItem(PFILEINST p, HWND hDlg, WPARAM wParam, LPDRAWITEMSTRUCT lpDIS, BOOL fSave)
{
    int nLevel = 1, cyItem, cxCell, iBmp = 0;
    WORD wState = 0;
    COLORREF rgbBk, rgbText, rgbOldBk, rgbOldText;
    RECT rc;
    HDC hdc;

    g_szTmp1[0] = '\0';
    if (lpDIS->CtlID != lst1 && lpDIS->CtlID != lst2 && lpDIS->CtlID != cmb2) return;
    hdc = lpDIS->hDC;
    SendDlgItemMessage(hDlg, lpDIS->CtlID, (lpDIS->CtlID == lst1 || lpDIS->CtlID == lst2) ? LB_GETTEXT : CB_GETLBTEXT,
                       lpDIS->itemID, (LPARAM)g_szTmp1);
    if (g_szTmp1[0] == '\0') { /* nothing there: DefWindowProc's (focus) drawing */
        DefWindowProc(hDlg, WM_DRAWITEM, wParam, (LPARAM)lpDIS);
        return;
    }
    if (lpDIS->CtlID == lst2 && !p) return; /* (no instance data yet: lst2 items cannot be placed) */
    AnsiLower(g_szTmp1);
    cyItem = lpDIS->CtlID == lst1 ? g_cyFont : g_cyItem;
    CopyRect(&rc, &lpDIS->rcItem);
    rc.bottom = rc.top + cyItem;

    if (fSave && lpDIS->CtlID == lst1) {
        rgbBk = g_rgbWindow;
        rgbText = g_rgbGrayText;
    } else {
        /* (Windows 3.0 also took a combo's selection field for focused here) */
        wState = lpDIS->itemState & (ODS_SELECTED | ODS_FOCUS);
        if ((wState & ODS_SELECTED) && (lpDIS->CtlID != lst2 || (wState & ODS_FOCUS))) {
            rgbBk = g_rgbHighlight;
            rgbText = g_rgbHighlightText;
        } else {
            rgbBk = g_rgbWindow;
            rgbText = g_rgbWindowText;
        }
    }
    rgbOldBk = SetBkColor(hdc, rgbBk);
    rgbOldText = SetTextColor(hdc, rgbText);

    if (lpDIS->CtlID == cmb2) {
        cxCell = (WORD)g_cxBmp / 8;
        iBmp = (int)SendDlgItemMessage(hDlg, cmb2, CB_GETITEMDATA, lpDIS->itemID, 0L);
        if (wState & ODS_SELECTED) iBmp += 8;
    } else if (lpDIS->CtlID == lst2) {
        cxCell = (WORD)g_cxBmp / 8;
        nLevel = ((WORD)lpDIS->itemID > p->cDirs) ? p->cDirs : (int)lpDIS->itemID;
        nLevel++;
        iBmp = Sign((int)lpDIS->itemID + 1 - p->cDirs) + 1;
        if (wState & ODS_FOCUS) iBmp += 8;
    } else
        cxCell = -4;

    if (fSave && lpDIS->CtlID == lst1 && rgbText == 0) { /* no COLOR_GRAYTEXT: GrayString */
        HBRUSH hbr = CreateSolidBrush(rgbBk), hbrOld;
        int oldMode = SetBkMode(hdc, TRANSPARENT);
        hbrOld = SelectObject(hdc, hbr ? hbr : GetStockObject(WHITE_BRUSH));
        FillRect(hdc, &lpDIS->rcItem, hbr);
        SelectObject(hdc, hbrOld);
        if (hbr) DeleteObject(hbr);
        GrayString(hdc, GetStockObject(BLACK_BRUSH), NULL, (LPARAM)g_szTmp1, 0, lpDIS->rcItem.left + 4, lpDIS->rcItem.top, 0, 0);
        SetBkMode(hdc, oldMode);
    } else
        ExtTextOut(hdc, rc.left + 4 + cxCell + nLevel * 4, rc.top + (WORD)(cyItem - g_cyFont) / 2, ETO_OPAQUE | ETO_CLIPPED, &rc,
                   g_szTmp1, lstrlen(g_szTmp1), NULL);
    if (lpDIS->CtlID != lst1)
        BitBlt(hdc, nLevel * 4 + rc.left, rc.top + (WORD)(g_cyItem - g_cyBmp) / 2, cxCell, g_cyBmp, g_hdcMem, iBmp * cxCell, 0, SRCCOPY);
    SetTextColor(hdc, rgbOldText);
    SetBkColor(hdc, rgbOldBk);
    if (lpDIS->itemState & ODS_FOCUS) DrawFocusRect(hdc, &lpDIS->rcItem);
}

/* seg2:6F06: make chDrive current if it can be read (a CD-ROM must list); back to the old drive if not */
static BOOL ChangeDriveTo(char chDrive)
{
    char chOld = (char)(GetCurrentDrive() + 'a');
    BOOL fOK = FALSE;
    chDrive |= 0x60;
    SetCurrentDrive(chDrive - 'a');
    if (IsCDROM(chDrive - 'a')) {
        if (FindFirst("*.*", 0x10) != 0) goto restore; /* no disc */
    }
    if ((int)chDrive != GetCurrentDrive() + 'a') return FALSE; /* no such drive */
    if (GetCurDirNear(g_szTmp4)) return TRUE;
restore:
    SetCurrentDrive(chOld - 'a');
    return fOK;
}

/* seg2:6FB6: 0 done, 1 bad or unreadable drive, 2 bad directory, -1 empty */
static int ChangeDir(LPSTR lpszDir, BOOL fAnsi)
{
    char chOld = (char)(GetCurrentDrive() + 'a');
    int err;
    if (lpszDir == NULL || *lpszDir == '\0') return -1;
    if (lpszDir[1] == ':') {
        if (!ChangeDriveTo(lpszDir[0])) return 1;
        lpszDir += 2;
    }
    err = 0;
    if (*lpszDir) {
        if (fAnsi) AnsiToOem(lpszDir, lpszDir);
        err = ChDirDOS(lpszDir) ? 2 : 0;
        if (fAnsi) OemToAnsi(lpszDir, lpszDir);
    }
    if (err == 2) SetCurrentDrive(chOld - 'a');
    return err;
}

/* seg2:70A4 */
static BOOL GetCurDirNear(LPSTR psz)
{
    g_wCurDirErr = (WORD)GetCurDir(psz, 0x100);
    return g_wCurDirErr == 0;
}

/* (seg2:70DC FileDlgWEP frees the bitmaps when the DLL unloads; libw16 does not unload) */

/* seg2:710C: the bitmap cell: 5 CD-ROM, 3 floppy, 6 network, 7 RAM disk, 4 hard disk */
static int GetDriveBitmapIndex(int drv, WORD wType)
{
    if (wType == 1) return 0;
    if (IsCDROM(drv)) return 5;
    if (wType == DRIVE_REMOVABLE) return 3;
    if (wType == DRIVE_REMOTE) return 6;
    if (w16_drive_class((char)('A' + drv)) == W16_DRV_RAM) return 7; /* IsRAMDrive (seg2:71AE) */
    return 4;
}

/* seg2:7178: an MSCDEX drive (INT 2Fh 1500h/150Bh); here a drive on an iso9660 / udf file system */
static BOOL IsCDROM(int drv)
{
    if (g_fWLO) return FALSE;
    return w16_drive_class((char)('A' + drv)) == W16_DRV_CDROM;
}

/* (seg2:71AE IsRAMDrive: a DPB with one FAT; here tmpfs / ramfs - in GetDriveBitmapIndex) */

/* seg2:71F6 / seg2:7214: 'A'..'Z' only, byte by byte (not AnsiLower) */
static char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? c + 0x20 : c; }
static void LowerAscii(LPSTR lpsz)
{
    if (!lpsz) return;
    for (; *lpsz; lpsz++) *lpsz = ToLowerAscii(*lpsz);
}

/* seg2:725C: checks and normalises a file name in place (leading and trailing blanks, a final '.',
 * "\\srv\share" -> "\\srv\share\."); MAKELONG(offset of the name or a PARSE_ code, offset of the
 * extension: its first character, the end when there is none, 0 when the name ended in '.') */
static DWORD ParseFile(LPSTR lpszFile)
{
    LPSTR lpstr = lpszFile, lpSpace;
    int nNetwork = 0, nFileOffset = 0, nExtOffset = 0, nFile = 0, nExt = 0;
    BOOL bUNC = FALSE, bExt = FALSE, bWildcard = FALSE;
    BYTE c;

    while (*lpstr == ' ') lpstr++;
    if (*lpstr == '\0') { nFileOffset = PARSE_EMPTYSTRING; goto FAILURE; }
    if (lpstr != lpszFile) {
        StrShift(lpszFile, lpstr);
        lpstr = lpszFile;
    }
    if (*AnsiNext(lpstr) == ':') {
        c = *lpstr | 0x20;
        if ((signed char)c < 'a' || (signed char)c > 'z') { nFileOffset = PARSE_INVALIDDRIVE; goto FAILURE; }
        lpstr = AnsiNext(AnsiNext(lpstr));
    }
    if (*lpstr == '\\' || *lpstr == '/') {
        lpstr++;
        if (*lpstr == '.') {
            lpstr++;
            if (*lpstr == '\\' || *lpstr == '/') { lpstr++; goto BODY; }
            if (*lpstr == '\0') goto MUSTBEDIR;
            nFileOffset = PARSE_INVALIDPERIOD;
            goto FAILURE;
        }
        if (*lpstr == '\\' && lpstr[-1] == '\\') {
            lpstr++;
            nNetwork = -1; /* a server and a share must follow */
            bUNC = TRUE;
        } else if (*lpstr == '/') {
            nFileOffset = PARSE_INVALIDDIRCHAR;
            goto FAILURE;
        }
    } else if (*lpstr == '.') {
        lpstr++;
        if (*lpstr == '.') lpstr++;
        if (*lpstr == '\0') goto MUSTBEDIR;
        if (*lpstr != '\\' && *lpstr != '/') { nFileOffset = PARSE_INVALIDPERIOD; goto FAILURE; }
        lpstr++;
    }
BODY:
    if (*lpstr == '\0') goto MUSTBEDIR;
    nFile = nExt = nFileOffset = nExtOffset = 0;
    bExt = bWildcard = FALSE;
    while ((c = (BYTE)*lpstr) != 0) {
        if (c < ' ') { nFileOffset = PARSE_INVALIDCHAR; goto FAILURE; }
        switch (c) {
        case '|': case '"': case '+': case ',': case ':': case ';':
        case '<': case '=': case '>': case '[': case ']':
            nFileOffset = PARSE_INVALIDCHAR;
            goto FAILURE;
        case '/': case '\\':
            nNetwork++;
            if (bWildcard) { nFileOffset = PARSE_WILDCARDINDIR; goto FAILURE; }
            if (nFile == 0) { nFileOffset = PARSE_INVALIDDIRCHAR; goto FAILURE; }
            lpstr++;
            if (nNetwork == 0 && *lpstr == '\0') { nFileOffset = PARSE_INVALIDNETPATH; goto FAILURE; }
            nFile = nExt = 0;
            bExt = FALSE;
            break;
        case ' ': /* trailing blanks are cut; others are an error */
            lpSpace = lpstr;
            *lpSpace = '\0';
            while (*++lpSpace)
                if (*lpSpace != ' ') {
                    *lpstr = ' ';
                    nFileOffset = PARSE_INVALIDSPACE;
                    goto FAILURE;
                }
            break;
        case '.':
            if (nFile == 0) { /* a "." or ".." part */
                lpstr++;
                if (*lpstr == '.') lpstr++;
                if (*lpstr == '\0') goto MUSTBEDIR;
                if (*lpstr != '\\' && *lpstr != '/') { nFileOffset = PARSE_INVALIDPERIOD; goto FAILURE; }
                lpstr++;
                break;
            }
            if (bExt) { nFileOffset = PARSE_INVALIDPERIOD; goto FAILURE; }
            nExtOffset = 0;
            lpstr++;
            bExt = TRUE;
            break;
        case '*': case '?':
            bWildcard = TRUE;
            /* fall through */
        default:
            if (bExt) {
                if (++nExt == 1) nExtOffset = (int)(lpstr - lpszFile);
                else if (nExt > 3) { nFileOffset = PARSE_EXTENTIONTOOLONG; goto FAILURE; }
            } else {
                if (++nFile == 1) nFileOffset = (int)(lpstr - lpszFile);
                else if (nFile > 8 && nNetwork > 0) { nFileOffset = PARSE_FILETOOLONG; goto FAILURE; }
            }
            lpstr = AnsiNext(lpstr);
            break;
        }
    }
    if (nNetwork == -1) { nFileOffset = PARSE_INVALIDNETPATH; goto FAILURE; } /* "\\server" only */
    if (bUNC && (nNetwork == 0 || (nNetwork == 1 && nFile == 0))) {
        if (nNetwork == 0) *lpstr++ = '\\';
        *lpstr++ = '.';
        *lpstr = '\0';
        goto MUSTBEDIR;
    }
    if (nFile == 0) goto MUSTBEDIR;
    if (lpstr[-1] == '.' && *AnsiNext(lpstr - 2) == '.') {
        lpstr[-1] = '\0'; /* "name.": no extension (nExtOffset stays 0) */
        goto RETURN;
    }
    if (nExt != 0) goto RETURN;
    goto FAILURE; /* no extension: nExtOffset = the end */
MUSTBEDIR:
    nFileOffset = PARSE_DIRECTORYNAME;
FAILURE:
    nExtOffset = (int)(lpstr - lpszFile);
RETURN:
    return MAKELONG(nFileOffset, nExtOffset);
}

/* ================================================================== Find / Replace (seg5, modeless) */
typedef UINT (*FRHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
#define FRHOOK(lpfr) ((FRHOOKPROC)(lpfr)->lpfnHook)
static FRHOOKPROC g_lpfnFRHookInit; /* DS:00E2: the hook before WM_INITDIALOG sets the property */
static UINT g_msgFindReplace;       /* DS:0A5E commdlg_FindReplace */
static char g_szClose[6];           /* DS:0A62 "Close" */

static BOOL FindTextDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static BOOL ReplaceTextDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

/* seg5:0000 FindText / seg5:019C ReplaceText: the dialog, hidden until its WM_INITDIALOG shows it;
 * the caller routes its messages through IsDialogMessage and keeps lpfr */
static HWND FRDialog(LPFINDREPLACE lpfr, BOOL fFind)
{
    HANDLE hrsrc;
    HGLOBAL hRes;
    void *lpTemplate;
    HWND hDlg;
    if (lpfr == NULL) return NULL;
    if (lpfr->lStructSize != sizeof(FINDREPLACE)) { cd_err = CDERR_STRUCTSIZE; return NULL; }
    if (!IsWindow(lpfr->hwndOwner) || !lpfr->lpstrFindWhat || !lpfr->wFindWhatLen ||
        (!fFind && (!lpfr->lpstrReplaceWith || !lpfr->wReplaceWithLen))) {
        cd_err = FRERR_BUFFERLENGTHZERO; /* also for a bad owner (3.1) */
        return NULL;
    }
    if (!LibMain()) { cd_err = CDERR_FINDRESFAILURE; return NULL; } /* (no COMMDLG.DLL ripped) */
    if (lpfr->Flags & FR_ENABLEHOOK) {
        if (!lpfr->lpfnHook) { cd_err = CDERR_NOHOOK; return NULL; }
    } else
        lpfr->lpfnHook = NULL; /* the caller's structure */
    if (!fFind && !LoadString(g_hInst, IDS_CLOSE, g_szClose, sizeof g_szClose)) {
        cd_err = CDERR_LOADSTRFAILURE;
        return NULL;
    }
    if (!(g_msgFindReplace = RegisterWindowMessage(FINDMSGSTRING))) { cd_err = CDERR_REGISTERMSGFAIL; return NULL; }
    if (lpfr->Flags & FR_ENABLETEMPLATE) {
        if (!(hrsrc = FindResource(lpfr->hInstance, lpfr->lpTemplateName, RT_DIALOG))) { cd_err = CDERR_FINDRESFAILURE; return NULL; }
        if (!(hRes = LoadResource(lpfr->hInstance, hrsrc))) { cd_err = CDERR_LOADRESFAILURE; return NULL; }
    } else if (lpfr->Flags & FR_ENABLETEMPLATEHANDLE)
        hRes = (HGLOBAL)lpfr->hInstance;
    else {
        hrsrc = FindResource(g_hInst, MAKEINTRESOURCE(fFind ? DLG_FIND : DLG_REPLACE), RT_DIALOG);
        if (!hrsrc) { cd_err = CDERR_FINDRESFAILURE; return NULL; }
        /* FindText reports a LoadResource failure as CDERR_FINDRESFAILURE, ReplaceText as LOADRES */
        if (!(hRes = LoadResource(g_hInst, hrsrc))) { cd_err = fFind ? CDERR_FINDRESFAILURE : CDERR_LOADRESFAILURE; return NULL; }
    }
    if (!(lpTemplate = LockResource(hRes))) { cd_err = CDERR_LOCKRESFAILURE; return NULL; }
    cd_err = 0;
    g_lpfnFRHookInit = FRHOOK(lpfr);
    hDlg = CreateDialogIndirectParam(g_hInst, lpTemplate, lpfr->hwndOwner, fFind ? FindTextDlgProc : ReplaceTextDlgProc,
                                     (LPARAM)lpfr);
    /* GlobalUnlock(hRes); the template is never freed (3.1) */
    if (!hDlg) g_lpfnFRHookInit = NULL;
    return hDlg;
}
HWND FindText(FINDREPLACE *lpfr) { return FRDialog(lpfr, TRUE); }
HWND ReplaceText(FINDREPLACE *lpfr) { return FRDialog(lpfr, FALSE); }

/* seg5:0836: FR_DIALOGTERM to the owner while the window still exists, then gone */
static void TerminateFR(HWND hDlg, LPFINDREPLACE lpfr)
{
    lpfr->Flags = (lpfr->Flags & ~(DWORD)(FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL)) | FR_DIALOGTERM;
    SendMessage(lpfr->hwndOwner, g_msgFindReplace, 0, (LPARAM)lpfr);
    RemoveProp(hDlg, PROP_INST); /* the property block is not freed (3.1) */
    DestroyWindow(hDlg);
}

/* seg5:0876 */
static void InitFRControls(HWND hDlg, LPFINDREPLACE lpfr, BOOL fFind)
{
    HWND h;
    SetDlgItemText(hDlg, edt1, lpfr->lpstrFindWhat);
    SendMessage(hDlg, WM_COMMAND, edt1, MAKELPARAM(0, EN_CHANGE)); /* the buttons */
    if (!(lpfr->Flags & FR_SHOWHELP)) {
        h = GetDlgItem(hDlg, pshHelp);
        ShowWindow(h, SW_HIDE);
        EnableWindow(h, FALSE);
    }
    if (lpfr->Flags & FR_HIDEWHOLEWORD) {
        h = GetDlgItem(hDlg, chx1);
        ShowWindow(h, SW_HIDE);
        EnableWindow(h, FALSE);
    } else if (lpfr->Flags & FR_NOWHOLEWORD)
        EnableWindow(GetDlgItem(hDlg, chx1), FALSE);
    CheckDlgButton(hDlg, chx1, (lpfr->Flags & FR_WHOLEWORD) ? 1 : 0);
    if (lpfr->Flags & FR_HIDEMATCHCASE) {
        h = GetDlgItem(hDlg, chx2);
        ShowWindow(h, SW_HIDE);
        EnableWindow(h, FALSE);
    } else if (lpfr->Flags & FR_NOMATCHCASE)
        EnableWindow(GetDlgItem(hDlg, chx2), FALSE);
    CheckDlgButton(hDlg, chx2, (lpfr->Flags & FR_MATCHCASE) ? 1 : 0);
    if (lpfr->Flags & FR_HIDEUPDOWN) {
        ShowWindow(GetDlgItem(hDlg, grp1), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, rad1), SW_HIDE);
        EnableWindow(GetDlgItem(hDlg, rad1), FALSE);
        ShowWindow(GetDlgItem(hDlg, rad2), SW_HIDE);
        EnableWindow(GetDlgItem(hDlg, rad2), FALSE);
    } else if (lpfr->Flags & FR_NOUPDOWN) {
        EnableWindow(GetDlgItem(hDlg, rad1), FALSE);
        EnableWindow(GetDlgItem(hDlg, rad2), FALSE);
    }
    if (fFind)
        CheckRadioButton(hDlg, rad1, rad2, (lpfr->Flags & FR_DOWN) ? rad2 : rad1);
    else {
        SetDlgItemText(hDlg, edt2, lpfr->lpstrReplaceWith);
        SendMessage(hDlg, WM_COMMAND, edt2, MAKELPARAM(0, EN_CHANGE));
    }
}

/* seg5:0A90: the options and texts into lpfr, with dwAction; Replace always searches down */
static void GetFRState(HWND hDlg, LPFINDREPLACE lpfr, DWORD dwAction, BOOL fFind)
{
    lpfr->Flags &= ~(DWORD)0x3F; /* DOWN, WHOLEWORD, MATCHCASE, FINDNEXT, REPLACE, REPLACEALL */
    if (IsDlgButtonChecked(hDlg, chx1)) lpfr->Flags |= FR_WHOLEWORD;
    if (IsDlgButtonChecked(hDlg, chx2)) lpfr->Flags |= FR_MATCHCASE;
    lpfr->Flags |= dwAction;
    GetDlgItemText(hDlg, edt1, lpfr->lpstrFindWhat, lpfr->wFindWhatLen);
    if (fFind) {
        if (!IsDlgButtonChecked(hDlg, rad1)) lpfr->Flags |= FR_DOWN;
    } else {
        GetDlgItemText(hDlg, edt2, lpfr->lpstrReplaceWith, lpfr->wReplaceWithLen);
        lpfr->Flags |= FR_DOWN;
    }
}

/* seg5:036C FindTextDlgProc / seg5:0584 ReplaceTextDlgProc */
static BOOL FRDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam, BOOL fFind)
{
    LPFINDREPLACE *hProp = (LPFINDREPLACE *)GetProp(hDlg, PROP_INST);
    LPFINDREPLACE lpfr = NULL; /* (unset on 3.1 while there is no property) */
    BOOL r;
    if (hProp) {
        lpfr = *hProp;
        if (lpfr && (lpfr->Flags & FR_ENABLEHOOK) && (r = (BOOL)FRHOOK(lpfr)(hDlg, msg, wParam, lParam)) != 0) return r;
    } else if (g_lpfnFRHookInit && msg != WM_INITDIALOG) {
        if ((r = (BOOL)g_lpfnFRHookInit(hDlg, msg, wParam, lParam)) != 0) return r;
    }
    switch (msg) {
    case WM_CLOSE:
        SendMessage(hDlg, WM_COMMAND, IDCANCEL, 0L);
        return TRUE;
    case WM_INITDIALOG:
        hProp = calloc(1, sizeof *hProp); /* LocalAlloc(LMEM_ZEROINIT, 4) */
        lpfr = (LPFINDREPLACE)lParam;
        if (!hProp) { /* 3.1 BUG fixed: 3.1 terminated with an unset lpfr and went on through NULL */
            TerminateFR(hDlg, lpfr);
            return FALSE;
        }
        SetProp(hDlg, PROP_INST, (HANDLE)hProp);
        g_lpfnFRHookInit = NULL;
        *hProp = lpfr;
        InitFRControls(hDlg, lpfr, fFind);
        if (lpfr->Flags & FR_ENABLEHOOK) r = (BOOL)FRHOOK(lpfr)(hDlg, WM_INITDIALOG, wParam, lParam);
        else r = TRUE;
        if (r) { /* a hook that answers FALSE keeps the dialog hidden */
            ShowWindow(hDlg, fFind ? SW_SHOWNORMAL : SW_SHOW);
            UpdateWindow(hDlg);
        }
        return r;
    case WM_COMMAND:
        if (!lpfr) return FALSE;
        switch (wParam) {
        case IDOK: /* Find Next */
            GetFRState(hDlg, lpfr, FR_FINDNEXT, fFind);
            SendMessage(lpfr->hwndOwner, g_msgFindReplace, 0, (LPARAM)lpfr);
            return TRUE;
        case IDCANCEL:
        case IDABORT:
            TerminateFR(hDlg, lpfr);
            return TRUE;
        case psh1: /* Replace */
        case psh2: /* Replace All */
            if (fFind) return FALSE;
            GetFRState(hDlg, lpfr, wParam == psh1 ? FR_REPLACE : FR_REPLACEALL, FALSE);
            if (SendMessage(lpfr->hwndOwner, g_msgFindReplace, 0, (LPARAM)lpfr) == 1L)
                SetWindowText(GetDlgItem(hDlg, IDCANCEL), g_szClose); /* "Cancel" becomes "Close" */
            return TRUE;
        case pshHelp:
            if (g_msgHelp && lpfr->hwndOwner) SendMessage(lpfr->hwndOwner, g_msgHelp, (WPARAM)hDlg, (LPARAM)lpfr);
            return TRUE;
        case edt1:
            if (HIWORD(lParam) == EN_CHANGE) {
                BOOL f = SendDlgItemMessage(hDlg, edt1, WM_GETTEXTLENGTH, 0, 0L) != 0;
                EnableWindow(GetDlgItem(hDlg, IDOK), f);
                if (!fFind) {
                    EnableWindow(GetDlgItem(hDlg, psh1), f);
                    EnableWindow(GetDlgItem(hDlg, psh2), f);
                }
            }
            return TRUE; /* every edt1 notification */
        default:
            return FALSE;
        }
    }
    return FALSE;
}
static BOOL FindTextDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) { return FRDlgProc(hDlg, msg, wParam, lParam, TRUE); }
static BOOL ReplaceTextDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) { return FRDlgProc(hDlg, msg, wParam, lParam, FALSE); }

/* ================================================================== Print / Print Setup */
/* Printers come from CUPS (lpstat). TODO(T-PRN-01): the printer DC itself does not render yet. */
typedef struct { char name[64], port[64]; } Printer;
static Printer printers[32];
static int nprinters, def_printer = -1;

static void enum_printers(void)
{
    nprinters = 0;
    def_printer = -1;
    char def[64] = "", line[300];
    FILE *p = popen("lpstat -d 2>/dev/null", "r");
    if (p) {
        if (fgets(line, sizeof line, p)) {
            char *c = strchr(line, ':');
            if (c) { sscanf(c + 1, " %63s", def); }
        }
        pclose(p);
    }
    p = popen("lpstat -v 2>/dev/null", "r"); /* "device for NAME: URI" */
    while (p && fgets(line, sizeof line, p) && nprinters < 32) {
        char name[64], uri[200];
        if (sscanf(line, "device for %63[^:]: %199s", name, uri) != 2) continue;
        Printer *pr = &printers[nprinters];
        snprintf(pr->name, sizeof pr->name, "%s", name);
        char *s = strstr(uri, "://");
        snprintf(pr->port, sizeof pr->port, "%.*s:", s ? (int)(s - uri) : 3, s ? uri : "LPT");
        AnsiUpper(pr->port);
        if (!strcmp(name, def)) def_printer = nprinters;
        nprinters++;
    }
    if (p) pclose(p);
    if (def_printer < 0 && nprinters) def_printer = 0;
}

static const struct { UINT ids; short dm; } papers[] = {
    {1153, DMPAPER_LETTER}, {1157, DMPAPER_LEGAL}, {1159, DMPAPER_EXECUTIVE}, {1161, DMPAPER_A4},
    {1163, DMPAPER_A5}, {1165, DMPAPER_B5}, {1172, DMPAPER_ENV_10}, {1179, DMPAPER_ENV_DL},
};

typedef struct {
    PRINTDLG *pd;
    int printer;      /* index into printers, -1 = none */
    int use_default;
    short orient, paper;
} PrnDlg;

static void pd_describe(char *out, size_t cb, int i)
{
    char fmt[64];
    cd_str(IDS_PRNONPORT, fmt, sizeof fmt); /* "%s on %s (%s)" -> drop the driver part */
    char *paren = strstr(fmt, " (");
    if (paren) *paren = 0;
    snprintf(out, cb, fmt, printers[i].name, printers[i].port);
}

static BOOL SetupDlgProc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    PrnDlg *st = (PrnDlg *)GetProp(dlg, "W16PD");
    switch (m) {
    case WM_INITDIALOG: {
        st = (PrnDlg *)lp;
        SetProp(dlg, "W16PD", (HANDLE)st);
        char t[200], lab[64];
        if (def_printer >= 0) {
            cd_str(IDS_PRINTERLABEL, lab, sizeof lab); /* "Default Printer (" */
            char d[150];
            pd_describe(d, sizeof d, def_printer);
            snprintf(t, sizeof t, "(currently %s)", d);
            char cur[64];
            if (LoadString(commdlg(), 1094, cur, sizeof cur)) snprintf(t, sizeof t, cur, d);
            SetDlgItemText(dlg, stc1, t);
        } else
            EnableWindow(GetDlgItem(dlg, rad3), FALSE);
        HWND cb = GetDlgItem(dlg, cmb1);
        for (int i = 0; i < nprinters; i++) {
            pd_describe(t, sizeof t, i);
            SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)t);
        }
        SendMessage(cb, CB_SETCURSEL, st->printer >= 0 ? st->printer : 0, 0);
        CheckRadioButton(dlg, rad3, rad4, st->use_default && def_printer >= 0 ? rad3 : rad4);
        EnableWindow(cb, !(st->use_default && def_printer >= 0) && nprinters);
        CheckRadioButton(dlg, rad1, rad2, st->orient == DMORIENT_LANDSCAPE ? rad2 : rad1);
        cb = GetDlgItem(dlg, cmb2);
        for (size_t i = 0; i < sizeof papers / sizeof papers[0]; i++) {
            cd_str(papers[i].ids, t, sizeof t);
            SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)t);
            if (papers[i].dm == st->paper) SendMessage(cb, CB_SETCURSEL, i, 0);
        }
        cb = GetDlgItem(dlg, cmb3);
        SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)"Auto Select"); /* MEASURE: comes from the printer driver in 3.1 */
        SendMessage(cb, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(dlg, psh1), FALSE); /* Options...: no 3.1 printer driver UI */
        if (!(st->pd->Flags & PD_SHOWHELP)) ShowWindow(GetDlgItem(dlg, pshHelp), SW_HIDE);
        /* focus the checked printer radio; "Default Printer" may be disabled */
        SetFocus(GetDlgItem(dlg, IsDlgButtonChecked(dlg, rad3) ? rad3 : rad4));
        return FALSE;
    }
    case WM_COMMAND:
        if (!st) break;
        switch (wp) {
        case rad3:
        case rad4:
            CheckRadioButton(dlg, rad3, rad4, (int)wp);
            EnableWindow(GetDlgItem(dlg, cmb1), wp == rad4 && nprinters);
            return TRUE;
        case rad1:
        case rad2:
            CheckRadioButton(dlg, rad1, rad2, (int)wp);
            return TRUE;
        case IDOK: {
            st->use_default = IsDlgButtonChecked(dlg, rad3);
            st->printer = st->use_default ? def_printer : (int)SendDlgItemMessage(dlg, cmb1, CB_GETCURSEL, 0, 0);
            st->orient = IsDlgButtonChecked(dlg, rad2) ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
            int ps = (int)SendDlgItemMessage(dlg, cmb2, CB_GETCURSEL, 0, 0);
            if (ps >= 0) st->paper = papers[ps].dm;
            RemoveProp(dlg, "W16PD");
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            RemoveProp(dlg, "W16PD");
            EndDialog(dlg, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static int pd_get_num(HWND dlg, int id, int *ok)
{
    char t[16];
    GetDlgItemText(dlg, id, t, sizeof t);
    *ok = 1;
    if (!t[0]) { *ok = 0; return 0; }
    for (char *c = t; *c; c++) if (!isdigit((unsigned char)*c)) { *ok = -1; return 0; }
    long v = atol(t);
    if (v > 65535) { *ok = -1; return 0; }
    return (int)v;
}

static BOOL PrintDlgProc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    PrnDlg *st = (PrnDlg *)GetProp(dlg, "W16PD");
    PRINTDLG *pd = st ? st->pd : NULL;
    switch (m) {
    case WM_INITDIALOG: {
        st = (PrnDlg *)lp;
        pd = st->pd;
        SetProp(dlg, "W16PD", (HANDLE)st);
        char t[200], lab[64], d[150];
        if (st->printer >= 0) {
            pd_describe(d, sizeof d, st->printer);
            if (st->use_default) {
                cd_str(IDS_PRINTERLABEL, lab, sizeof lab);
                snprintf(t, sizeof t, "%s%s)", lab, d);
            } else
                snprintf(t, sizeof t, "%s", d);
            SetDlgItemText(dlg, stc1, t);
        }
        CheckRadioButton(dlg, rad1, rad3, (pd->Flags & PD_SELECTION) ? rad2 : (pd->Flags & PD_PAGENUMS) ? rad3 : rad1);
        if (pd->Flags & PD_NOSELECTION) EnableWindow(GetDlgItem(dlg, rad2), FALSE);
        if (pd->Flags & PD_NOPAGENUMS) {
            int ids[] = {rad3, stc2, edt1, stc3, edt2};
            for (int i = 0; i < 5; i++) EnableWindow(GetDlgItem(dlg, ids[i]), FALSE);
        } else {
            if (pd->nFromPage != 0xFFFF) SetDlgItemInt(dlg, edt1, pd->nFromPage, FALSE);
            if (pd->nToPage != 0xFFFF) SetDlgItemInt(dlg, edt2, pd->nToPage, FALSE);
        }
        SetDlgItemInt(dlg, edt3, pd->nCopies ? pd->nCopies : 1, FALSE);
        HWND cb = GetDlgItem(dlg, cmb1);
        for (int i = 0; i < 4; i++) {
            cd_str(IDS_QUALITY + i, t, sizeof t);
            SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)t);
        }
        SendMessage(cb, CB_SETCURSEL, 0, 0);
        if (pd->Flags & PD_HIDEPRINTTOFILE) ShowWindow(GetDlgItem(dlg, chx1), SW_HIDE);
        else if (pd->Flags & PD_DISABLEPRINTTOFILE) EnableWindow(GetDlgItem(dlg, chx1), FALSE);
        CheckDlgButton(dlg, chx1, (pd->Flags & PD_PRINTTOFILE) != 0);
        CheckDlgButton(dlg, chx2, (pd->Flags & PD_COLLATE) != 0);
        if (!(pd->Flags & PD_SHOWHELP)) ShowWindow(GetDlgItem(dlg, pshHelp), SW_HIDE);
        return TRUE;
    }
    case WM_COMMAND:
        if (!st) break;
        switch (wp) {
        case rad1:
        case rad2:
        case rad3:
            CheckRadioButton(dlg, rad1, rad3, (int)wp);
            return TRUE;
        case edt1:
        case edt2:
            if (HIWORD(lp) == EN_CHANGE && !IsDlgButtonChecked(dlg, rad3) && GetFocus() == W16_CMD_HWND(lp))
                CheckRadioButton(dlg, rad1, rad3, rad3);
            return TRUE;
        case psh1: { /* Setup... */
            HINSTANCE inst = commdlg();
            if (DialogBoxParam(inst, MAKEINTRESOURCE(DLG_SETUP), dlg, SetupDlgProc, (LPARAM)st) > 0) {
                SetProp(dlg, "W16PD", (HANDLE)st);
                SendMessage(dlg, WM_INITDIALOG, 0, (LPARAM)st);
            }
            return TRUE;
        }
        case IDOK: {
            int ok, copies = pd_get_num(dlg, edt3, &ok);
            if (ok == 0) { cd_msg(dlg, IDS_COPIESEMPTY, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
            if (ok < 0) { cd_msg(dlg, IDS_COPIESBAD, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
            if (copies < 1) { cd_msg(dlg, IDS_COPIESZERO, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
            pd->Flags &= ~(PD_SELECTION | PD_PAGENUMS | PD_PRINTTOFILE | PD_COLLATE);
            if (IsDlgButtonChecked(dlg, rad3)) {
                int okf, okt, from = pd_get_num(dlg, edt1, &okf), to = pd_get_num(dlg, edt2, &okt);
                if (okf == 0 && okt == 0) { cd_msg(dlg, IDS_NOPAGES, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (okf < 0) { cd_msg(dlg, IDS_FROMBAD, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (okt < 0) { cd_msg(dlg, IDS_TOBAD, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (!okf) from = pd->nMinPage;
                if (!okt) to = pd->nMaxPage;
                if (from < pd->nMinPage) { cd_msg(dlg, IDS_FROMLOW, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (from > pd->nMaxPage) { cd_msg(dlg, IDS_FROMHIGH, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (to < pd->nMinPage) { cd_msg(dlg, IDS_TOLOW, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                if (to > pd->nMaxPage) { cd_msg(dlg, IDS_TOHIGH, NULL, MB_OK | MB_ICONEXCLAMATION); return TRUE; }
                pd->nFromPage = from;
                pd->nToPage = to;
                pd->Flags |= PD_PAGENUMS;
            } else if (IsDlgButtonChecked(dlg, rad2))
                pd->Flags |= PD_SELECTION;
            if (IsDlgButtonChecked(dlg, chx1)) pd->Flags |= PD_PRINTTOFILE;
            if (IsDlgButtonChecked(dlg, chx2)) pd->Flags |= PD_COLLATE;
            pd->nCopies = copies;
            RemoveProp(dlg, "W16PD");
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            RemoveProp(dlg, "W16PD");
            EndDialog(dlg, FALSE);
            return TRUE;
        case pshHelp:
            SendMessage(pd->hwndOwner, RegisterWindowMessage(HELPMSGSTRING), 0, (LPARAM)pd);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* DEVNAMES: offsets are from the start of the block; strings follow the header */
static HGLOBAL make_devnames(int i, int is_default)
{
    const char *drv = "CUPS", *dev = printers[i].name, *port = printers[i].port;
    size_t n = sizeof(DEVNAMES) + strlen(drv) + strlen(dev) + strlen(port) + 3;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n);
    DEVNAMES *dn = GlobalLock(h);
    char *s = (char *)(dn + 1);
    dn->wDriverOffset = (WORD)(s - (char *)dn);
    s = stpcpy(s, drv) + 1;
    dn->wDeviceOffset = (WORD)(s - (char *)dn);
    s = stpcpy(s, dev) + 1;
    dn->wOutputOffset = (WORD)(s - (char *)dn);
    strcpy(s, port);
    dn->wDefault = is_default ? DN_DEFAULTPRN : 0;
    GlobalUnlock(h);
    return h;
}

static HGLOBAL make_devmode(int i, short orient, short paper, short copies)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DEVMODE));
    DEVMODE *dm = GlobalLock(h);
    snprintf(dm->dmDeviceName, sizeof dm->dmDeviceName, "%s", printers[i].name);
    dm->dmSpecVersion = 0x30A;
    dm->dmSize = sizeof *dm;
    dm->dmFields = DM_ORIENTATION | DM_PAPERSIZE | DM_COPIES;
    dm->dmOrientation = orient;
    dm->dmPaperSize = paper;
    dm->dmCopies = copies;
    GlobalUnlock(h);
    return h;
}

static void pd_from_handles(PRINTDLG *pd, PrnDlg *st)
{
    st->printer = def_printer;
    st->use_default = 1;
    st->orient = DMORIENT_PORTRAIT;
    st->paper = DMPAPER_LETTER;
    DEVNAMES *dn = pd->hDevNames ? GlobalLock(pd->hDevNames) : NULL;
    if (dn) {
        const char *dev = (const char *)dn + dn->wDeviceOffset;
        st->printer = -1;
        for (int i = 0; i < nprinters; i++) if (!strcmp(printers[i].name, dev)) st->printer = i;
        st->use_default = (dn->wDefault & DN_DEFAULTPRN) != 0;
        GlobalUnlock(pd->hDevNames);
        if (st->printer < 0) { cd_err = PDERR_PRINTERNOTFOUND; return; }
    }
    DEVMODE *dm = pd->hDevMode ? GlobalLock(pd->hDevMode) : NULL;
    if (dm) {
        if (dm->dmFields & DM_ORIENTATION) st->orient = dm->dmOrientation;
        if (dm->dmFields & DM_PAPERSIZE) st->paper = dm->dmPaperSize;
        GlobalUnlock(pd->hDevMode);
    }
}

BOOL PrintDlg(PRINTDLG *pd)
{
    cd_err = 0;
    if (!pd || pd->lStructSize != sizeof *pd) { cd_err = CDERR_STRUCTSIZE; return FALSE; }
    enum_printers();
    PrnDlg st = {pd};
    pd_from_handles(pd, &st);
    if (cd_err) return FALSE;
    if (pd->Flags & PD_RETURNDEFAULT) {
        if (pd->hDevMode || pd->hDevNames) { cd_err = PDERR_RETDEFFAILURE; return FALSE; }
        if (def_printer < 0) { cd_err = PDERR_NODEFAULTPRN; return FALSE; }
    } else {
        int id = (pd->Flags & PD_PRINTSETUP) ? DLG_SETUP : DLG_PRINT;
        if (def_printer < 0 && id == DLG_PRINT) {
            cd_msg(pd->hwndOwner, IDS_NODEFPRN, NULL, MB_OK | MB_ICONEXCLAMATION);
            cd_err = PDERR_NODEFAULTPRN;
            return FALSE;
        }
        if (!template_ok(commdlg(), id)) return FALSE;
        int r = DialogBoxParam(commdlg(), MAKEINTRESOURCE(id), pd->hwndOwner, id == DLG_SETUP ? SetupDlgProc : PrintDlgProc, (LPARAM)&st);
        if (r < 0) { cd_err = CDERR_DIALOGFAILURE; return FALSE; }
        if (!r) return FALSE;
    }
    if (st.printer < 0) { cd_err = PDERR_NODEFAULTPRN; return FALSE; }
    if (pd->hDevNames) GlobalFree(pd->hDevNames);
    if (pd->hDevMode) GlobalFree(pd->hDevMode);
    pd->hDevNames = make_devnames(st.printer, st.use_default);
    pd->hDevMode = make_devmode(st.printer, st.orient, st.paper, pd->nCopies ? pd->nCopies : 1);
    if (pd->Flags & (PD_RETURNDC | PD_RETURNIC)) {
        DEVMODE *dm = GlobalLock(pd->hDevMode);
        pd->hDC = (pd->Flags & PD_RETURNDC) ? CreateDC("CUPS", printers[st.printer].name, printers[st.printer].port, dm)
                                            : CreateIC("CUPS", printers[st.printer].name, printers[st.printer].port, dm);
        GlobalUnlock(pd->hDevMode);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ ChooseFont (ordinal 15) */
/* TODO: port COMMDLG.DLL's font dialog - ChooseFont (ordinal 15) with FormatCharDlgProc (16), the
 * FontFamilyEnumProc / FontStyleEnumProc enumerators (19, 18) and dialog template 1543 ("Font"), or an
 * application template such as Clock's dialog 100 under CF_ENABLETEMPLATE. Until then the dialog
 * does not open and the call returns FALSE with no extended error, exactly what COMMDLG returns when
 * the user presses Cancel, so callers keep their font. */
BOOL ChooseFont(CHOOSEFONT *cf)
{
    (void)cf;
    cd_err = 0;
    return FALSE;
}
