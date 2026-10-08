/* DRIVERS.CPL: the Drivers applet, ported from the 3.11 DRIVERS.CPL - seg1 (LibMain, CPlApplet) and
 * seg2: the dialogs (1001 Drivers, 1003 Add, 1002 Add Unlisted or Updated Driver, 1006 Install
 * Driver, 1004 System Setting Change, 1005 Driver Exists, 38 Browse), the installed-driver list,
 * Add, Remove and Setup, and the setup library the module carries (the INF reader, the file copier,
 * the path helpers). Dialogs, strings and the icon come from the user's ripped DRIVERS.CPL.
 *
 * Installable drivers are 16-bit DLLs that arch311 never runs: the applet talks to them through
 * libw16's installable-driver layer (OpenDriver / SendDriverMessage / CloseDriver), whose stand-ins
 * answer DRV_QUERYCONFIGURE, DRV_INSTALL and DRV_REMOVE as the 3.11 drivers do; a driver's own setup
 * dialog (DRV_CONFIGURE) does not appear. Added driver files are copied with VerInstallFile and stay
 * inert files. SYSTEM.INI and CONTROL.INI get exactly what 3.1 writes.
 *
 * Globals carry their 3.1 DS offsets. 3.1 bugs that change what the user sees are kept (comments
 * say where); bugs that read or write past buffers are fixed and marked as deviations. */
#include "cpl.h"
#include "commdlg.h"
#include <stdlib.h>
#include <string.h>

#define IDD_DRIVERS 1001
#define IDD_UNLISTED 1002
#define IDD_ADD 1003
#define IDD_RESTART 1004
#define IDD_EXISTS 1005
#define IDD_INSTALL 1006
#define IDD_BROWSE 38
#define IDC_INSTALLED 305       /* 1001 */
#define IDC_ADD 102
#define IDC_SETUP 103
#define IDC_REMOVE 104
#define IDC_HELP_DRIVERS 5120
#define IDC_AVAILABLE 301       /* 1003 */
#define IDC_HELP_ADD 8403
#define IDC_OEMLIST 306         /* 1002 */
#define IDC_HELP_UNLISTED 8803
#define IDC_PATH 105            /* 1006 */
#define IDC_PROMPT 106
#define IDC_BROWSE 2029
#define IDC_HELP_INSTALL 8703
#define IDC_RESTARTTEXT 2015    /* 1004 */
#define IDC_EXISTSTEXT 4002     /* 1005 */
#define IDC_CURRENT 4
#define IDC_NEW 6
#define IDC_BROWSEPROMPT 1280   /* 38 */
#define IDC_HELP_BROWSE 8038
#define IDC_BROWSEPICK 0x501    /* the hook's own command */
#define lst1 1120
#define lst2 1121
#define cmb2 1137
#define edt1 1152

/* copy-engine messages (FileCopy -> CopyCallback) */
#define COPY_ERROR 1
#define COPY_STATUS 2
#define COPY_INSERTDISK 3
#define COPY_QUERYCOPY 4
#define COPY_START 5
#define COPY_END 6

/* ------------------------------------------------------------------ DRVITEM (LocalAlloc 0x58A) */
typedef struct {
    HDRVR hDriver;              /* +000 open only during an operation */
    char szSection[0x100];      /* +002 "MCI" or "Drivers" */
    char szAlias[0x100];        /* +102 the SYSTEM.INI key */
    char szFile[0x80];          /* +202 the driver file ("file params" in the install template) */
    char szDesc[0x100];         /* +282 the list text */
    WORD w382;
    BOOL fRelated;              /* +384 INF field 6 named related drivers */
    char szRelatedKeys[0x100];  /* +386 INF field 6 */
    char szRelatedFiles[0x100]; /* +486 their files, "msadlib.drv," */
    int fConfigurable;          /* +586 DRV_QUERYCONFIGURE's answer, -1 = not asked yet */
} DRVITEM;

/* ------------------------------------------------------------------ globals (DGROUP) */
static BOOL g_fRestartNeeded;          /* [0x10] DRV_INSTALL/DRV_CONFIGURE asked for a restart */
static int g_idRestartMsg;             /* [0x12] string for dialog 1004, 0 = the template's */
static BOOL g_fBootDriver;             /* [0x14] INF field 7 is "Boot" */
static BOOL g_fBrowsePickFirst;        /* [0x16] a 1006 dialog is up */
static BOOL g_fRelatedPending;         /* [0x18] related drivers are being installed */
static int g_nExistsAnswer = 1;        /* [0x148] Driver Exists: 1 New, -1 Current; never reset */
static BOOL g_fDefDriveDone;           /* [0x15C] */
static BOOL g_fCopyingVxds;            /* [0x17E] */
static char *g_lpSysIni;               /* [0x188] SYSTEM.INI as an INF image while VxDs are queued */
static char *g_lpInfDefault;           /* [0x1F8] the INF the inf functions use for NULL */
static BOOL g_fDiskRetried;            /* [0x2BA] never read */
static HWND g_hwndMain;                /* [0x2DC] */
static char g_chDisk;                  /* [0x2E0] */
static char g_szExistsMsg[0x200];      /* [0x2E2] */
static LPSTR g_lpszDiskPath;           /* [0x4E2] FileCopy's source directory buffer */
static char g_szInfField[0x100];       /* [0x4E6] 0x96 in 3.1; GetExeInfo writes up to 255 (sized to fit) */
static char g_szErrSpec[0x100];        /* [0x57C] */
static char g_szVxdList[0x100];        /* [0x67C] "vsbd.386," */
static char g_szField7[0x0A];          /* [0x77C] */
static char g_szField[0x100];          /* [0x786] 0x96 in 3.1 */
static char g_szSpec[0x100];           /* [0x81C] 0x4C in 3.1 */
static char g_szType[0x20];            /* [0x868] 0x14 in 3.1 */
static int g_n386EnhLines;             /* [0x87C] */
static char *g_lp386Enh;               /* [0x87E] */
static char g_szRelFile[0x100];        /* [0x9E6] */
static char g_szTypes[0x100];          /* [0x10E6] 0x4C in 3.1 */
static DRVITEM g_tmpl;                 /* [0x1232] the install template */
static char g_szAlias[0x100];          /* [0x17BC] 0x4C in 3.1 */
static char g_szRelKey[0x100];         /* [0x1926] */
static HINSTANCE g_hInst;              /* [0x1E3C] */
static BOOL g_fRunning;                /* [0x1E62] */
static char g_szWinDir[0x41];          /* [0x1E64] */
static char g_szLastPath[0x42];        /* [0x1EC2] where "." disks are */
static char g_szOemPath[0x100];        /* [0x1F84] 0x44 in 3.1 */
static BOOL g_fOemRetry;               /* [0x2080] */
static HWND g_hwndInstList;            /* [0x2082] */
static char g_szCurFile[0x78];         /* [0x2378] the file being copied (prompts, Browse) */
static HWND g_hwndInstallParent;       /* [0x23F0] owner of the prompts during an install */
static BOOL g_f386;                    /* [0x2416] */
static UINT g_msgShellHelp;            /* [0x2542] */
static BOOL g_fQuietCopyErrors;        /* [0x263E] */
static char g_szDriverDesc[0x100];     /* [0x2664] 0xD0 in 3.1, filled from 0x100-byte descriptions */
static BOOL g_fAskExists;              /* [0x2766] */
static char *g_lpSavedInf;             /* [0x2768] SETUP.INF while an OEMSETUP.INF is the default */
static int g_idIcon, g_idName, g_idInfo; /* [0x276C] [0x276E] [0x2770] */
static DWORD g_dwHelpContext;          /* [0x2774] */

/* strings loaded by LibMain, with 3.1's buffer sizes (LoadString truncates to them) */
static char szClose[0x10];             /* [0x2406] 2032 */
static char szDescSect[0x26];          /* [0x1FD4] 2008 "drivers.desc" */
static char szFileInstErr[0x32];       /* [0x21C2] 2046 */
static char szInstallable[0x26];       /* [0x205A] 2007 "Installable.drivers" */
static char szRelatedSect[0x1E];       /* [0x2024] 2057 "related.desc" */
static char szUserInst[0x26];          /* [0x2512] 2038 "Userinstallable.drivers" */
static char szOemPrompt[0x96];         /* [0x277A] 2034 */
static char szDiskPrompt[0xFA];        /* [0x2418] 2035 */
static char szOemInf[0x80];            /* [0x1DBC] 2044 "oemsetup.inf" */
static char szDestDisk[0x80];          /* [0x21F4] 2045 "0:system" */
static char szNoUndo[0x36];            /* [0x1D50] 2009 */
static char szNoDesc[0x24];            /* [0x2640] 2013 */
static char szDriverErr[0x0C];         /* [0x2084] 2014, cut to "Driver Erro" as in 3.1 */
static char szSureRemove[0xFA];        /* [0x227E] 2033 */
static char szRequired[0xFA];          /* [0x2544] 2037 */
static char szSetupInf[0x12];          /* [0x2048] 2021 */
static char szDrivers[0x0C];           /* [0x1FFA] 2020 "Drivers": caption and SYSTEM.INI section */
static char szRemove[0x0C];            /* [0x1FC8] 2036 */
static char szControlIni[0x14];        /* [0x23F2] 2022 */
static char szSystemIni[0x14];         /* [0x2010] 2023 */
static char szMCI[6];                  /* [0x2042] 2024 */
static char szMIDI[0x0A];              /* [0x2274] 2047 */
static char szWAVE[0x0A];              /* [0x2538] 2048 */
static char szDiskPath[0x80];          /* [0x1F04] 2004 "A:\" */
static char szHelpFile[0x18];          /* [0x274E] 2026 "control.hlp" */
static char szBoot[6];                 /* [0x1EBA] 2050 "Boot" */

/* DS literals */
static const char szBlowaway[] = "blowaway";   /* [0x13E] */
static const char szOemDisks[] = "oemdisks";   /* [0x14A], [0x2C6] */
static const char szDisks[] = "disks";         /* [0x154], [0x2C0] */
static const char szDevice[] = "\r\ndevice=";  /* [0x164] */
static const char szNextSect[] = "\r\n[";      /* [0x16E] */
static const char sz386EnhHdr[] = "\r\n[386enh]"; /* [0x172] */
static const char sz386Enh[] = "386enh";       /* [0x180] */
static const char szAUX[] = "AUX";             /* [0x1AC] */
static const char szFilter[] = "Inf Files(*.inf)\0*.inf\0Drv Files(*.drv)\0*.drv\0"; /* [0x1B2] */
static const char szDefInf[] = "mmsetup.inf";  /* [0x1EC] */
static const char szSystem[] = "system";       /* [0x1FE] */

static BOOL DriversDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL AddDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL UnlistedListDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL RestartDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL OemPathDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL InsertDiskDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL DriverExistsDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL RemoveDriver(HWND hList, DRVITEM *p, BOOL fTopLevel, int index);
static BOOL InstallDriver(HWND hMain, HWND hDlg, LPSTR pszKey);
static void BrowseForDir(HWND hDlg, int nFilterIndex);

/* LB_GETITEMDATA of an installed-list entry; LB_ERR reads as -1 */
static DRVITEM *ItemOf(HWND hList, int i) { return (DRVITEM *)SendMessage(hList, LB_GETITEMDATA, i, 0); }
#define NO_ITEM ((DRVITEM *)(intptr_t)LB_ERR)

/* ================================================================== the setup library (seg2:3066-584F) */

/* seg2:1CB2 (= seg2:3431, seg2:5668): the first n characters equal, ASCII case-insensitive */
#define UP(c) (((c) >= 'a' && (c) <= 'z') ? ((c) & 0xDF) : (int)(signed char)(c))
static int StrNCmpI(LPCSTR a, LPCSTR b, int n)
{
    if (*a) {
        while (--n > 0) {
            if (UP(*a) != UP(*b)) break;
            a++;
            b++;
            if (*a == 0) break;
        }
    }
    return UP(*a) == UP(*b) ? 0 : 1;
}

/* seg2:306A: C runtime _stricmp, ASCII */
static int StrICmp(LPCSTR a, LPCSTR b)
{
    for (;; a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y || !x) return x - y;
    }
}

/* seg2:30AC: _fstrncpy, with the terminator 3.1 leaves out when src fills n */
static void StrNCpy(LPSTR dst, LPCSTR src, size_t n)
{
    strncpy(dst, src, n);
    dst[n - 1] = 0;
}

/* seg2:51BF CatPath: szName (a drive and ".\" dropped, "." adds nothing) after szPath and a "\" */
static void CatPath(LPSTR szPath, LPCSTR szName)
{
    if (szName[0] && szName[1] == ':') szName += 2;
    while (szName[0] == '.' && (szName[1] == '/' || szName[1] == '\\')) szName += 2;
    if (!*szName || (szName[0] == '.' && !szName[1])) return;
    int len = lstrlen(szPath);
    /* 3.1 reads szPath[-1] when it is empty */
    if (len && szPath[len - 1] != '/' && szPath[len - 1] != '\\' && szPath[len - 1] != ':' && szName[0] != '/' &&
        szName[0] != '\\')
        lstrcat(szPath, "\\");
    lstrcat(szPath, szName);
}

/* seg2:52C6 FileName: after the last '/', '\' or ':' */
static LPSTR FileName(LPCSTR sz)
{
    LPCSTR p = sz + lstrlen(sz);
    while (p >= sz && *p != '/' && *p != '\\' && *p != ':') p--;
    return (LPSTR)p + 1;
}

/* seg2:5348 StripFileName: "A:\X.DRV" -> "A:\", "A:\D\X" -> "A:\D", "X" -> "" */
static LPSTR StripFileName(LPSTR sz)
{
    LPSTR p = FileName(sz);
    if (sz + 1 < p && (p[-1] == '/' || p[-1] == '\\') && p[-2] != ':') p--;
    *p = 0;
    return sz;
}

/* ------------------------------------------------------------------ the INF image (seg2:3192-3E15) */
/* An INF (or SYSTEM.INI) is read into one buffer and normalised: CRs dropped, blanks and tabs dropped
 * outside quotes, ';' comments dropped (inside quotes too), every line NUL-terminated, an empty line
 * before each section header, "\0\0\x1A" at the end. "Handles" are pointers into the image. */

/* seg2:320E InfLoadFile (with InfGetc 3192 / InfPutc 31DE) */
static char *InfLoadFile(HFILE fh)
{
    if (fh == HFILE_ERROR) return NULL;
    WORD cbFile = (WORD)_llseek(fh, 0, 2);
    _llseek(fh, 0, 0);
    uint8_t *in = malloc(cbFile ? cbFile : 1);
    /* 3.1's image is exactly cbFile bytes: a file with nothing to drop is overrun by the three bytes
     * appended at the end (deviation: 4 bytes more) */
    char *out = calloc(1, (size_t)cbFile + 4);
    if (!in || !out) { free(in); free(out); return NULL; }
    WORD got = (WORD)_lread(fh, in, cbFile);
    if (got == (WORD)-1) got = 0;
    WORD nIn = 0, nOut = 0;
    BOOL fQuote = FALSE;
#define GETC() (nIn < got ? (char)in[nIn++] : (nIn++, (char)0x1A))
#define PUTC(c) (out[nOut++] = (c))
    while (cbFile > nIn) {
        char c = GETC();
    dispatch:
        if (cbFile <= nIn) break;           /* 3.1 drops the file's last byte */
        switch (c) {
        case '\r':
            break;
        case ' ':
        case '\t':
            if (fQuote) PUTC(c);
            break;
        case '"':
            fQuote = !fQuote;
            PUTC('"');
            break;
        case ';':                           /* a comment, also inside quotes */
            while (c != '\n' && c != '\r' && c != 0 && c != 0x1A) c = GETC();
            goto dispatch;
        case '\n':
            do c = GETC(); while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (c != ';') PUTC(0);
            if (c == '[') PUTC(0);
            fQuote = FALSE;
            goto dispatch;
        default:
            PUTC(c);
            break;
        }
    }
    PUTC(0);
    PUTC(0);
    PUTC(0x1A);
#undef GETC
#undef PUTC
    free(in);
    return out;
}

/* seg2:3549 infOpen: the file as given, else <windir>\system\file, else <windir>\file; the first INF
 * opened becomes the default */
static char *infOpen(LPCSTR szInf)
{
    char szPath[300];                   /* 0x40 in 3.1 (long Windows directories overflow it) */
    if (!szInf) szInf = szDefInf;
    HFILE fh = _lopen(szInf, OF_READ);
    if (fh == HFILE_ERROR) {
        lstrcpy(szPath, g_szWinDir);
        CatPath(szPath, szSystem);
        CatPath(szPath, szInf);
        fh = _lopen(szPath, OF_READ);
    }
    if (fh == HFILE_ERROR) {
        lstrcpy(szPath, g_szWinDir);
        CatPath(szPath, szInf);
        fh = _lopen(szPath, OF_READ);
    }
    if (fh == HFILE_ERROR) return NULL;
    char *lp = InfLoadFile(fh);
    _lclose(fh);
    if (lp && !g_lpInfDefault) g_lpInfDefault = lp;
    return lp;
}

/* seg2:3671 infClose */
static void infClose(char *lpInf)
{
    if (!lpInf) lpInf = g_lpInfDefault;
    if (lpInf) {
        if (lpInf == g_lpInfDefault) g_lpInfDefault = NULL;
        free(lpInf);
    }
}

/* seg2:397F infSetDefault: returns the old default */
static char *infSetDefault(char *lpInf)
{
    char *old = g_lpInfDefault;
    g_lpInfDefault = lpInf;
    return old;
}

/* seg2:36D9 InfSectionOffset + seg2:39BF infFindSection: the first line of [szSection], NULL when
 * the section is missing or empty */
static char *infFindSection(char *lpInf, LPCSTR szSection)
{
    if (!lpInf) lpInf = g_lpInfDefault;
    if (!lpInf) return NULL;
    int len = lstrlen(szSection);
    BOOL fFound = FALSE;
    char *p = lpInf;
    while (!fFound && *p != 0x1A) {
        if (*p++ == '[') fFound = StrNCmpI(szSection, p, len) == 0 && p[len] == ']';
        while (*p != 0x1A && *p != 0) p++;
        while (*p == 0) p++;
    }
    return fFound && *p != '[' && *p != 0x1A ? p : NULL;
}

/* seg2:3D52 infNextLine: NULL at the end of the section */
static char *infNextLine(char *lp)
{
    if (!lp) return NULL;
    while (*lp != 0 || lp[1] == ' ') lp++;
    lp++;
    return *lp ? lp : NULL;
}

/* seg2:3DC3 infLineCount */
static int infLineCount(char *lp)
{
    int n = 0;
    for (; lp; lp = infNextLine(lp)) n++;
    return n;
}

/* seg2:3CE8: an unquoted ',' or '=' */
static BOOL InfHasSeparator(LPCSTR p)
{
    BOOL fQuote = FALSE;
    for (; *p; p++) {
        if (!fQuote && (*p == '=' || *p == ',')) return TRUE;
        if (*p == '"') fQuote = !fQuote;
    }
    return FALSE;
}

/* seg2:37D5: the value of szKey (a single value without its blanks and quotes, else as written) */
static BOOL InfFindKey(char *lp, LPCSTR szKey, LPSTR szBuf)
{
    int len = lstrlen(szKey);
    LPSTR start = szBuf;
    for (; lp; lp = infNextLine(lp)) {
        if (StrNCmpI(lp, szKey, len) != 0) continue;
        char *v = lp + len;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v != '=') continue;
        v++;
        if (!InfHasSeparator(v)) {
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r' || *v == '"') v++;
            while (*v) *szBuf++ = *v++;
            /* 3.1 has no lower bound here (key="" backs up before the buffer): deviation */
            while (szBuf > start && (szBuf[-1] == ' ' || szBuf[-1] == '\t' || szBuf[-1] == '\n' ||
                                     szBuf[-1] == '\r' || szBuf[-1] == '"'))
                szBuf--;
        } else
            while (*v) *szBuf++ = *v++;
        *szBuf = 0;
        return TRUE;
    }
    *szBuf = 0;
    return FALSE;
}

/* seg2:3A27 infGetProfileString */
static BOOL infGetProfileString(char *lpInf, LPCSTR szSection, LPCSTR szItem, LPSTR szBuf)
{
    char *lp = infFindSection(lpInf, szSection);
    if (!lp) { *szBuf = 0; return FALSE; }
    return InfFindKey(lp, szItem, szBuf);
}

/* seg2:3A8D infParseField: field 0 is the key (when an '=' comes before the first ','), fields 1..
 * the '='/','-separated values, quotes removed; FALSE when the field does not exist. 3.1 copies
 * without a limit; here at most cb-1 characters (deviation for over-long INF lines) */
static BOOL infParseFieldN(LPCSTR szData, int n, LPSTR szBuf, size_t cb)
{
    BOOL fQuote = FALSE;
    LPCSTR p;
    if (!szData || !szBuf) return FALSE;
    for (p = szData; *p && *p != '=' && *p != ','; p++)
        if (*p == '"') fQuote = !fQuote;
    if (n == 0 && *p != '=') return FALSE;
    if (n > 0 && *p == '=' && !fQuote) szData = p + 1;
    fQuote = FALSE;
    for (; n > 1; n--) {
        while (*szData && (fQuote || (*szData != '=' && *szData != ','))) {
            if (*szData == '"') fQuote = !fQuote;
            szData++;
        }
        if (!*szData) { *szBuf = 0; return FALSE; }
        szData++;
    }
    while (*szData == ' ' || *szData == '\t' || *szData == '\n' || *szData == '\r') szData++;
    fQuote = FALSE;
    LPSTR q = szBuf;
    for (; *szData; szData++) {
        if (*szData == '"') { fQuote = !fQuote; continue; }
        if (!fQuote && (*szData == '=' || *szData == ',')) break;
        if ((size_t)(q - szBuf) < cb - 1) *q++ = *szData;
    }
    while (q > szBuf && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r' || q[-1] == '"')) q--;
    *q = 0;
    return TRUE;
}
#define infParseField(d, n, buf) infParseFieldN((d), (n), (buf), sizeof(buf))

/* ------------------------------------------------------------------ disks and paths */
/* seg2:5011 GetDiskPath: the directory of disk chDisk ('0' = Windows directory) from [disks] or
 * [oemdisks]; "." means where the user pointed (g_szLastPath) */
static BOOL GetDiskPath(char chDisk, LPSTR szOut, size_t cb)
{
    char szKey[2], szTmp[300];
    if (chDisk == '0') { lstrcpy(szOut, g_szWinDir); return TRUE; }
    szKey[0] = chDisk;
    szKey[1] = 0;
    if (!infGetProfileString(NULL, szDisks, szKey, szOut) && !infGetProfileString(NULL, szOemDisks, szKey, szOut))
        return FALSE;
    infParseFieldN(szOut, 1, szOut, cb);
    if (*szOut == '.' || *szOut == 0) {
        lstrcpy(szTmp, g_szLastPath);
        CatPath(szTmp, szOut);
        lstrcpy(szOut, szTmp);
    }
    return TRUE;
}

/* seg2:50F7 ExpandFileName: "5:x.drv" -> "<disk 5's directory>\x.drv", "0:system" -> <windir>\system */
static void ExpandFileName(LPCSTR szIn, LPSTR szOut)
{
    char szDisk[0x200];
    if (szIn[0] && szIn[1] == ':' && GetDiskPath(szIn[0], szDisk, sizeof szDisk)) {
        lstrcpy(szOut, szDisk);
        if (szIn[2]) CatPath(szOut, szIn + 2);
    } else
        lstrcpy(szOut, szIn);
}

/* seg2:53EA VifToError: VerInstallFile's result as a DOS-style error code */
static WORD VifToError(DWORD vif)
{
    if (!vif) return 0;
    if (HIWORD(vif) & 0x0001) return 2;      /* VIF_CANNOTREADSRC */
    if (LOWORD(vif) & 0x8000) return 8;      /* VIF_OUTOFMEMORY */
    if (LOWORD(vif) & 0x0200) return 5;      /* VIF_ACCESSVIOLATION */
    if (LOWORD(vif) & 0x0400) return 0x20;   /* VIF_SHARINGVIOLATION */
    return HIWORD(vif) | LOWORD(vif);        /* 0x80 VIF_FILEINUSE, 0x100 VIF_OUTOFSPACE, ... */
}

/* seg2:3FE8 DosDelete */
static void DosDelete(LPCSTR path)
{
    OFSTRUCT of;
    OpenFile(path, &of, OF_DELETE);
}

/* seg2:3EDA IsCDRomDrive (MSCDEX INT 2Fh AX=150Bh). arch311 has no MSCDEX: no drive is a CD-ROM, so
 * the default source stays "A:\" as on the reference machine (TODO: a CD-ROM mounted as a drive
 * letter) */
static int IsCDRomDrive(int nDrive)
{
    (void)nDrive;
    return 0;
}

/* seg2:451D: the callback FileCopy uses when given none */
static int DefCopyCallback(int msg, int wParam, LPSTR lpsz)
{
    (void)msg; (void)wParam; (void)lpsz;
    return 1;
}

typedef int (*COPYPROC)(int msg, int wParam, LPSTR lpsz);

/* ------------------------------------------------------------------ seg2:4540 FileCopy */
/* Copies szSource (fCopy 0: one "n:file" entry; 2: the lines of an INF section; "#name" = 2) to
 * szDir, disk by disk ('1'..'9', 'A'..'Z'), with VerInstallFile, asking fpfn about each file
 * (COPY_QUERYCOPY), for a missing disk (COPY_INSERTDISK) and on errors (COPY_ERROR). Returns 0, 0x12
 * (aborted) or 0x50 (cancelled). (The flags 8, 0x10, 0x20 and 0x40 of the setup library are not used
 * by DRIVERS.CPL and not ported.) */
static WORD FileCopy(LPSTR szSource, LPCSTR szDir, COPYPROC fpfn, WORD fCopy)
{
    char szDestDir[300], szSect[0x100], szFileSpec[0x100], szSrcPath[300], szFileName[0x40], szTmpFile[0x10],
        szTmpPath[300];
    WORD wResult = 0;
    BOOL fWinDirTried = FALSE;
    int nIndex, nFiles = 0, nDisk, nRetry, r;
    char *lpList, *lpCur;
    if (!fpfn) fpfn = DefCopyCallback;
    if (!szSource || !*szSource || !szDir || !*szDir) return 0;
    ExpandFileName(szDir, szDestDir);
    if (*szSource == '#' && fCopy == 0) {
        fCopy = 2;
        szSource++;
    }
    switch (fCopy) {
    case 2:
        StrNCpy(szSect, szSource, sizeof szSect);    /* 0x28 bytes, unchecked, in 3.1 */
        if (!(szSource = infFindSection(NULL, szSect))) goto done;
        fCopy = 1;
        /* fall through */
    case 1:
        lpList = szSource;
        nFiles = infLineCount(szSource);
        break;
    default:
        lpList = szSource;
        nFiles = 1;
        break;
    }
    fpfn(COPY_START, 0, NULL);
    for (nDisk = 1; nFiles > 0 && nDisk <= 35; nDisk++) {
        char chDisk = nDisk >= 10 ? (char)(nDisk + 0x37) : (char)(nDisk + '0');
        lpCur = lpList;
        nIndex = 0;
        while (lpCur) {
            BOOL fThisDisk;
            if (lpCur[1] == ':' && UP(chDisk) == UP(lpCur[0] & 0x7F)) fThisDisk = TRUE;
            else fThisDisk = lpCur[1] != ':' && nDisk == 1 && lpCur[0];
            if (!fThisDisk) goto next;
            nFiles--;
            lstrcpy(g_szCurFile, lpCur[1] == ':' ? lpCur + 2 : lpCur);
            r = fpfn(COPY_QUERYCOPY, nIndex, lpCur);
            if (r == -1) goto next;                  /* keep the file that is there */
            if (r == 0) { wResult = 0x50; goto done; }
            lpCur[0] &= 0x7F;
            infParseField(lpCur, 1, szFileSpec);
            ExpandFileName(szFileSpec, szSrcPath);
            lstrcpy(szFileName, FileName(szSrcPath));
            StripFileName(szSrcPath);
            WORD vifFlags = 0;
            for (nRetry = 0; nRetry <= 15; nRetry++) {
            retry:;
                UINT cbTmp = 0x0E;
                szTmpFile[0] = 0;
                DWORD vif = VerInstallFile(vifFlags, szFileName, szFileName, szSrcPath, szDestDir, szDestDir, szTmpFile,
                                           &cbTmp);
                if (!vif) goto file_done;
                if (vif & VIF_MISMATCH) {
                    if (vif & 0x00080004L) {         /* VIF_SRCOLD: the newer file there stays */
                        lstrcpy(szTmpPath, szDestDir);
                        CatPath(szTmpPath, szTmpFile);
                        DosDelete(szTmpPath);
                        goto file_done;
                    }
                    vifFlags |= VIFF_FORCEINSTALL;
                    continue;
                }
                if (HIWORD(vif) & 0x0001) {          /* VIF_CANNOTREADSRC: ask for the disk */
                    r = fpfn(COPY_INSERTDISK, szFileSpec[1] == ':' ? (int)(signed char)szFileSpec[0] : '1', szSrcPath);
                    if (r == 0) { wResult = 0x12; goto done; }
                    if (r == 2) { g_fDiskRetried = TRUE; goto retry; }
                }
                wResult = VifToError(vif);
                ExpandFileName(szFileSpec, szTmpPath);
                if (!fWinDirTried && wResult != 0x80 && wResult != 0x100) {
                    /* a second try into the Windows directory, for the rest of the call */
                    GetWindowsDirectory(szDestDir, 0x50);
                    fWinDirTried = TRUE;
                    goto retry;
                }
                r = fpfn(COPY_ERROR, wResult, szTmpPath);
                if (r == 1) goto file_done;
                if (r == 2) goto retry;
                if (r == 0) { wResult = 0x12; goto done; }
            }
        file_done:
            if (fpfn(COPY_STATUS, 100, lpCur) == 0) goto done;
        next:
            wResult = 0;
            nIndex++;
            lpCur = fCopy == 1 ? infNextLine(lpCur) : NULL;
        }
    }
    wResult = 0;
done:
    fpfn(COPY_END, wResult, NULL);
    return wResult;
}

/* ------------------------------------------------------------------ seg2:54B4 GetExeInfo */
/* nInfo 1: the module name, 2: the module description (resident / non-resident name #0) of an NE
 * file, into pBuf (cb - 1 limits nothing below 256). The other kinds are not used here */
static BOOL GetExeInfo(LPCSTR szFile, LPSTR pBuf, int cb, int nInfo)
{
    BYTE mz[0x40], ne[0x40], len;
    HFILE fh = _lopen(szFile, OF_READ);
    BOOL ok = FALSE;
    if (fh == HFILE_ERROR) return FALSE;
    if (_lread(fh, mz, 0x40) != 0x40 || mz[0] != 'M' || mz[1] != 'Z') goto out;
    DWORD neOff = mz[0x3C] | mz[0x3D] << 8 | (DWORD)mz[0x3E] << 16 | (DWORD)mz[0x3F] << 24, pos;
    if (!neOff) goto out;
    _llseek(fh, (LONG)neOff, 0);
    if (_lread(fh, ne, 0x40) != 0x40 || ne[0] != 'N' || ne[1] != 'E') goto out;
    if (nInfo == 1) pos = neOff + (ne[0x26] | ne[0x27] << 8);
    else if (nInfo == 2) pos = ne[0x2C] | ne[0x2D] << 8 | (DWORD)ne[0x2E] << 16 | (DWORD)ne[0x2F] << 24;
    else goto out;
    _llseek(fh, (LONG)pos, 0);
    if (_lread(fh, &len, 1) != 1) goto out;
    cb--;
    if ((BYTE)cb < len) len = (BYTE)cb;
    UINT got = _lread(fh, pBuf, len);
    pBuf[got == (UINT)-1 ? 0 : got] = 0;
    ok = TRUE;
out:
    _lclose(fh);
    return ok;
}

/* ================================================================== the applet (seg2:0000-305F) */

/* seg2:1036 / seg2:104A */
static void HourGlassOn(void) { SetCursor(LoadCursor(NULL, IDC_WAIT)); }
static void HourGlassOff(void) { SetCursor(LoadCursor(NULL, IDC_ARROW)); }

/* seg2:105E */
static int DoDialogBox(int id, HWND hwndOwner, DLGPROC proc) { return DialogBox(g_hInst, MAKEINTRESOURCE(id), hwndOwner, proc); }

/* seg2:0DA6 */
static void DriverLoadError(HWND hwnd, LPCSTR lpszDesc, LPCSTR lpszFile)
{
    char caption[0x32], fmt[0x100], text[0x100];
    (void)lpszFile;
    LoadString(g_hInst, 2012, fmt, sizeof fmt);
    LoadString(g_hInst, 2014, caption, sizeof caption);
    wsprintf(text, fmt, lpszDesc);              /* 3.1 passes the file too; the text has one %s */
    MessageBox(hwnd, text, caption, MB_TASKMODAL | MB_ICONEXCLAMATION);
}

/* seg2:0B0A: whether a user (Add) installed the alias */
static BOOL IsUserInstallable(LPCSTR pszAlias)
{
    char buf[0x100];
    return GetPrivateProfileString(szUserInst, pszAlias, NULL, buf, sizeof buf, szControlIni) != 0;
}

/* seg2:0000 */
static BOOL ConfirmRemove(HWND hDlg, LPCSTR pszAlias, LPCSTR pszDesc)
{
    char buf[0x200];
    wsprintf(buf, IsUserInstallable(pszAlias) ? szSureRemove : szRequired, pszDesc);
    return MessageBox(hDlg, buf, szRemove, MB_TASKMODAL | MB_ICONEXCLAMATION | MB_YESNO) == IDYES;
}

/* seg2:0056: GetPrivateProfileString with "" as default; returns the buffer */
static LPSTR GetProfStr(LPCSTR pszSection, LPCSTR pszKey, LPCSTR pszFile, LPSTR pszBuf, int cb)
{
    GetPrivateProfileString(pszSection, pszKey, "", pszBuf, cb, pszFile);
    return pszBuf;
}

/* seg2:24AC: b starts with a's first blank-delimited token (case-insensitive; a prefix test: "snd.drv"
 * matches "snd.drv2", kept as in 3.1) */
static int CmpFirstToken(LPCSTR a, LPCSTR b)
{
    LPCSTR e;
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    for (e = a; *e && *e != ' '; e++) ;
    return StrNCmpI(a, b, (int)(e - a));
}

/* seg2:00A0: adds the driver unless its file is listed already (then 0, and p is never freed, as in
 * 3.1); returns the list index */
static int AddDriverItem(HWND hList, DRVITEM *p)
{
    int i = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    while (i-- > 0) {
        DRVITEM *q = ItemOf(hList, i);
        if (q == NO_ITEM) continue;
        if (CmpFirstToken(q->szFile, p->szFile) == 0) return 0;
    }
    p->hDriver = NULL;
    p->fConfigurable = -1;
    i = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)p->szDesc);
    if (i != LB_ERR) SendMessage(hList, LB_SETITEMDATA, i, (LPARAM)p);
    return i;
}

/* seg2:0E0C: the description of a driver that CONTROL.INI does not describe: SETUP.INF
 * [Installable.drivers] field 3 for the same file name (2), else the module description after its
 * ':' (1); 0 when the driver file cannot be found (it is then not listed) */
static int GetDescFromInfOrExe(DRVITEM *p, LPCSTR pszKey, LPSTR pszOut, size_t cb)
{
    OFSTRUCT of;
    (void)pszKey;
    for (char *lp = infFindSection(NULL, szInstallable); lp; lp = infNextLine(lp)) {
        infParseField(lp, 1, g_szInfField);
        if (lstrcmpi(FileName(p->szFile), FileName(g_szInfField)) == 0) {
            infParseFieldN(lp, 3, pszOut, cb);
            return OpenFile(p->szFile, &of, OF_EXIST) == HFILE_ERROR ? 0 : 2;
        }
    }
    if (OpenFile(p->szFile, &of, OF_EXIST) == HFILE_ERROR) return 0;
    if (!GetExeInfo(of.szPathName, g_szInfField, 0x100, 2))
        *pszOut = 0;
    else {
        LPSTR s = g_szInfField;
        while (*s)
            if (*s++ == ':') break;
        lstrcpy(pszOut, s);
    }
    return 1;
}

/* seg2:0130: one list entry per driver file of SYSTEM.INI [pszSection] (pszKeys: its keys) */
static void LoadDriverSection(HWND hDlg, LPCSTR pszKeys, LPCSTR pszSection)
{
    HWND hList = GetDlgItem(hDlg, IDC_INSTALLED);
    LPCSTR pKey = pszKeys;
    char pDesc[0x100];                          /* LocalAlloc(0x64) in 3.1 */
    while (*pKey) {
        DRVITEM *p = calloc(1, sizeof *p);
        if (!p) break;
        if (*GetProfStr(pszSection, pKey, szSystemIni, p->szFile, sizeof p->szFile) == 0) goto drop;
        p->szFile[strcspn(p->szFile, ", ")] = 0;   /* the file ends at a ',' or ' ' ([0x12F], [0x12E]) */
        GetProfStr(szDescSect, p->szFile, szControlIni, p->szDesc, sizeof p->szDesc);
        if (!p->szDesc[0]) {
            pDesc[0] = 0;
            if (GetDescFromInfOrExe(p, pKey, pDesc, sizeof pDesc) == 0) goto drop;  /* no such file */
            if (!*pDesc) {
                lstrcpy(p->szDesc, p->szFile);
                lstrcat(p->szDesc, szNoDesc);        /* "x.drv[No Driver Description]", no blank */
            } else
                lstrcpy(p->szDesc, pDesc);
            /* cached, the fallback too */
            WritePrivateProfileString(szDescSect, p->szFile, p->szDesc, szControlIni);
        }
        StrNCpy(p->szAlias, pKey, sizeof p->szAlias);
        StrNCpy(p->szSection, pszSection, sizeof p->szSection);
        if (AddDriverItem(hList, p) >= 0) goto next;
    drop:
        free(p);
    next:
        pKey += lstrlen(pKey) + 1;
    }
}

/* seg2:02A0: TRUE when SYSTEM.INI [pszSection] has keys */
static BOOL LoadInstalledSection(HWND hDlg, LPCSTR pszSection)
{
    char buf[0x200];
    BOOL f = FALSE;
    memset(buf, 0, sizeof buf);
    if (*GetProfStr(pszSection, NULL, szSystemIni, buf, sizeof buf)) {
        LoadDriverSection(hDlg, buf, pszSection);
        f = TRUE;
    }
    return f;
}

/* seg2:02E8: Cancel becomes Close */
static void SetCancelToClose(HWND hDlg)
{
    char buf[0x10];
    GetDlgItemText(hDlg, IDCANCEL, buf, sizeof buf);
    if (lstrcmp(buf, szClose)) SetDlgItemText(hDlg, IDCANCEL, szClose);
}

/* seg2:2992 */
static void FillDrvConfigInfo(LPDRVCONFIGINFO lpdci, DRVITEM *p)
{
    lpdci->dwDCISize = sizeof *lpdci;           /* 0x0C, the 16-bit structure, in 3.1 */
    lpdci->lpszDCISectionName = p->szSection;
    lpdci->lpszDCIAliasName = p->szAlias;
}

/* seg2:2B28: DRV_QUERYCONFIGURE, asked once per entry; a driver that cannot be opened reports its
 * error once and stays without Setup */
static int QueryConfigurable(DRVITEM *p, HWND hDlg)
{
    HourGlassOn();
    if (p->fConfigurable == -1) {
        if (!p->hDriver) p->hDriver = OpenDriver(p->szAlias, p->szSection, 0);
        if (!p->hDriver) {
            p->fConfigurable = 0;
            DriverLoadError(hDlg, p->szDesc, p->szFile);
            HourGlassOff();
            return 0;
        }
        p->fConfigurable = (int)SendDriverMessage(p->hDriver, DRV_QUERYCONFIGURE, 0, 0);
        CloseDriver(p->hDriver, 0, 0);
        p->hDriver = NULL;
    }
    HourGlassOff();
    return p->fConfigurable;
}

/* seg2:0D02 */
static void FreeInstalledList(HWND hList)
{
    int i = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    while (i-- > 0) {
        DRVITEM *p = ItemOf(hList, i);
        if (p == NO_ITEM) continue;
        if (p->hDriver) CloseDriver(p->hDriver, 0, 0);
        free(p);
    }
}

/* ------------------------------------------------------------------ removing */
/* seg2:250A: hay contains needle (case-insensitive). 3.1 loops on the pointer instead of the
 * character, scanning DGROUP after the string: an alias found elsewhere (e.g. "Sequencer" inside the
 * description buffer of "[MCI] MIDI Sequencer") is then cut out there - real 3.11 shows "The [MCI]
 * MIDI  driver has been removed." (drivers-remyes/05). Deviation: the search stops at the string's
 * end */
static LPSTR StrStrI(LPSTR hay, LPCSTR needle)
{
    if (!hay) return NULL;
    for (; *hay; hay++)
        if (StrNCmpI(hay, needle, lstrlen(needle)) == 0) return hay;
    return NULL;
}

/* seg2:2404: removes the word pszKey from SYSTEM.INI [boot] drivers= */
static BOOL RemoveBootDriver(LPCSTR pszKey)
{
    static char val[0x100];                     /* [0xFE6]; read with itself as the default */
    int len = lstrlen(pszKey);
    GetPrivateProfileString("boot", "drivers", val, val, 0x80, szSystemIni);
    LPSTR s = StrStrI(val, pszKey);
    if (!s) return FALSE;
    if (s != val && s[-1] != ' ') return FALSE;
    LPSTR e = s + len;
    if (*e != ' ' && *e) return FALSE;
    if (*e) {
        LPSTR d = s;
        do *d++ = *e++; while (*e);
        s = d;
    }
    *s = 0;
    WritePrivateProfileString("boot", "drivers", val, szSystemIni);
    return TRUE;
}

/* seg2:3066 -> 313E: atoi */
static int Atoi(LPCSTR s)
{
    long n = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return (int)(neg ? -n : n);
}

/* seg2:2110: after removing pszKey, the highest-numbered alias of its family (wave2) takes its name
 * (wave) in SYSTEM.INI, CONTROL.INI and [boot] drivers= */
static BOOL RenumberAliases(LPCSTR pszKey, LPCSTR pszSection)
{
    static char keys[0x100], maxKey[0x100], ctlVal[0x100], sysVal[0x100], bootVal[0x100]; /* [0xDE6] [0xAE6] [0xBE6] [0xCE6] [0xEE6] */
    int len = lstrlen(pszKey), base = len, maxN = 0, k;
    BOOL fFamily = FALSE, fRename = FALSE;
    if (len && pszKey[len - 1] > '0' && pszKey[len - 1] <= '9') base--;
    memset(keys, 0, sizeof keys);
    GetPrivateProfileString(pszSection, NULL, NULL, keys, sizeof keys, szSystemIni);
    for (LPCSTR pK = keys; *pK; pK += lstrlen(pK) + 1) {
        if (StrNCmpI(pK, pszKey, base) == 0) {
            LPCSTR c = pK + base;
            if ((*c <= '9' && *c > '0') || *c == 0) {
                fFamily = TRUE;
                if ((k = Atoi(c)) > maxN) {
                    maxN = k;
                    lstrcpy(maxKey, pK);
                }
            }
        }
    }
    if (!fFamily) return FALSE;
    if (len == base || Atoi(pszKey + base) < maxN) fRename = TRUE;
    if (!fRename) return FALSE;
    GetPrivateProfileString(pszSection, maxKey, NULL, sysVal, sizeof sysVal, szSystemIni);
    WritePrivateProfileString(pszSection, maxKey, NULL, szSystemIni);
    WritePrivateProfileString(pszSection, pszKey, sysVal, szSystemIni);
    GetPrivateProfileString(szRelatedSect, maxKey, NULL, ctlVal, sizeof ctlVal, szControlIni);
    if (lstrlen(ctlVal)) {
        WritePrivateProfileString(szRelatedSect, maxKey, NULL, szControlIni);
        WritePrivateProfileString(szRelatedSect, pszKey, ctlVal, szControlIni);
    }
    GetPrivateProfileString(szUserInst, maxKey, NULL, ctlVal, sizeof ctlVal, szControlIni);
    if (lstrlen(ctlVal)) {
        WritePrivateProfileString(szUserInst, maxKey, NULL, szControlIni);
        WritePrivateProfileString(szUserInst, pszKey, ctlVal, szControlIni);
    }
    if (RemoveBootDriver(maxKey)) {
        GetPrivateProfileString(szBoot, szDrivers, bootVal, bootVal, sizeof bootVal, szSystemIni);
        lstrcat(bootVal, " ");
        lstrcat(bootVal, pszKey);
        WritePrivateProfileString(szBoot, szDrivers, bootVal, szSystemIni);
    }
    return TRUE;
}

/* seg2:20A4: the key in SYSTEM.INI and its CONTROL.INI entries ([drivers.desc] by pszFile, which
 * still carries the parameters of the SYSTEM.INI value: such entries stay, as in 3.1) */
static void RemoveDriverKey(LPCSTR pszKey, LPCSTR pszFile, LPCSTR pszSection, BOOL fRenumber)
{
    WritePrivateProfileString(pszSection, pszKey, NULL, szSystemIni);
    WritePrivateProfileString(szUserInst, pszKey, NULL, szControlIni);
    WritePrivateProfileString(szDescSect, pszFile, NULL, szControlIni);
    WritePrivateProfileString(szRelatedSect, pszKey, NULL, szControlIni);
    RemoveBootDriver(pszKey);
    if (fRenumber) RenumberAliases(pszKey, pszSection);
}

/* seg2:1E62: DRV_REMOVE, the list entry, the related drivers ([related.desc]) and every key of the
 * section that names the file. The DRVITEM is not freed (3.1); files and [386Enh] lines stay */
static BOOL RemoveDriver(HWND hList, DRVITEM *p, BOOL fTopLevel, int index)
{
    static char keys[0x100];                    /* [0x8E6] */
    char rel[0x100], val[0x80];
    if (!p || p == NO_ITEM) return FALSE;
    /* the whole value, parameters included; the old file name is the default */
    GetPrivateProfileString(p->szSection, p->szAlias, p->szFile, p->szFile, sizeof p->szFile, szSystemIni);
    if (fTopLevel && (!g_fRelatedPending || p->fRelated)) lstrcpy(g_szDriverDesc, p->szDesc);
    if (!p->hDriver) p->hDriver = OpenDriver(p->szAlias, p->szSection, 0);
    if (p->hDriver) {
        SendDriverMessage(p->hDriver, DRV_REMOVE, 0, 0);
        CloseDriver(p->hDriver, 0, 0);
        p->hDriver = NULL;
    }
    SendMessage(hList, LB_DELETESTRING, index, 0);
    /* 3.1 reads [related.desc] with its uninitialised buffer as the default (stack garbage parsed
     * as file names when the key is missing): deviation, an empty default */
    rel[0] = 0;
    if (fTopLevel && GetPrivateProfileString(szRelatedSect, p->szAlias, rel, rel, sizeof rel, szControlIni) &&
        infParseField(rel, 1, g_szRelFile)) {
        int n = 1;
        do {
            BOOL fFound = FALSE;
            int i = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
            while (i-- > 0 && !fFound) {
                DRVITEM *q = ItemOf(hList, i);
                if (q == NO_ITEM) continue;
                if (CmpFirstToken(q->szFile, g_szRelFile) == 0) {
                    RemoveDriver(hList, q, FALSE, i);
                    fFound = TRUE;
                }
            }
        } while (infParseField(rel, ++n, g_szRelFile));
    }
    memset(keys, 0, sizeof keys);
    GetPrivateProfileString(p->szSection, NULL, NULL, keys, sizeof keys, szSystemIni);
    for (LPCSTR pKey = keys; *pKey; pKey += lstrlen(pKey) + 1) {
        GetPrivateProfileString(p->szSection, pKey, NULL, val, sizeof val, szSystemIni);
        if (CmpFirstToken(p->szFile, val) == 0) RemoveDriverKey(pKey, p->szFile, p->szSection, fTopLevel);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ adding */
/* seg2:2C1E: removes every key of the section whose value starts with the file (values read into
 * 0x14 bytes, as in 3.1) */
static void RemoveKeysForFile(LPCSTR pszFile, LPCSTR pszSection)
{
    static char keys[0x200], val[0x14];         /* [0x1B3A] [0x1B26] */
    memset(keys, 0, sizeof keys);
    GetPrivateProfileString(pszSection, NULL, NULL, keys, sizeof keys, szSystemIni);
    for (LPCSTR pK = keys; *pK; pK += lstrlen(pK) + 1) {
        GetPrivateProfileString(pszSection, pK, NULL, val, sizeof val, szSystemIni);
        if (CmpFirstToken(pszFile, val) == 0) RemoveDriverKey(pK, pszFile, pszSection, FALSE);
    }
}

/* seg2:2BB4: the listed driver with the same file goes (DRV_REMOVE); with none listed, its keys */
static void RemoveOldCopy(LPCSTR pszFile, LPCSTR pszSection)
{
    int i = (int)SendMessage(g_hwndInstList, LB_GETCOUNT, 0, 0);
    while (i-- > 0) {
        DRVITEM *q = ItemOf(g_hwndInstList, i);
        if (q == NO_ITEM) continue;
        if (CmpFirstToken(q->szFile, pszFile) == 0) {
            RemoveDriver(g_hwndInstList, q, FALSE, i);
            return;
        }
    }
    RemoveKeysForFile(pszFile, pszSection);
}

/* seg2:29C4: the alias made unique in the section: "Wave", "Wave1" .. "Wave9" (then "Wave:", a
 * highest number of 10 is refused - as in 3.1); in [Drivers] only wave*, midi* and aux* may repeat */
static BOOL MakeUniqueAlias(HWND hList, LPSTR pszAlias, LPCSTR pszSection)
{
    static char keys[0x100];                    /* [0x1A26] */
    int len = lstrlen(pszAlias), maxN = 0, k;
    BOOL fFound = FALSE;
    (void)hList;
    memset(keys, 0, sizeof keys);
    GetPrivateProfileString(pszSection, NULL, NULL, keys, sizeof keys, szSystemIni);
    for (LPCSTR pK = keys; *pK; pK += lstrlen(pK) + 1)
        if (StrNCmpI(pK, pszAlias, len) == 0) {
            LPCSTR c = pK + len;
            if ((*c > '0' && *c <= '9') || *c == 0) {
                fFound = TRUE;
                if ((k = Atoi(c)) > maxN) maxN = k;
            }
        }
    if (!fFound) return TRUE;
    if (StrNCmpI(pszSection, szMCI, lstrlen(pszSection)) != 0 && StrNCmpI(pszAlias, szMIDI, lstrlen(szMIDI)) != 0 &&
        StrNCmpI(pszAlias, szAUX, lstrlen(szAUX)) != 0 && StrNCmpI(pszAlias, szWAVE, lstrlen(szWAVE)) != 0)
        return FALSE;
    if (maxN == 10) return FALSE;
    pszAlias[len] = (char)('1' + maxN);
    pszAlias[len + 1] = 0;
    return TRUE;
}

/* seg2:2CB2: cut at the first blank after the first word */
static void CutAtSpace(LPSTR s)
{
    while (*s == ' ') s++;
    while (*s != ' ' && *s) s++;
    if (*s == ' ') *s = 0;
}

/* seg2:1D76: the file names (without "n:") of the related keys, "a.drv,b.drv," */
static void BuildRelatedFileList(char *lpSect, LPCSTR pszRelated, LPSTR pszOut)
{
    char rel[0x32], key[0x32], field1[0x32];    /* [0x8B4] for field 1 */
    int n = 1;
    if (!infParseField(pszRelated, 1, rel)) return;
    do {
        for (char *lp = lpSect; lp; lp = infNextLine(lp)) {
            key[0] = 0;
            infParseField(lp, 0, key);
            if (lstrcmpi(key, rel) == 0) {
                if (infParseField(lp, 1, field1)) {
                    lstrcat(pszOut, field1[1] == ':' ? field1 + 2 : field1);
                    lstrcat(pszOut, ",");
                }
                break;
            }
        }
    } while (infParseField(pszRelated, ++n, rel));
}

/* seg2:1956: lpszVxd given: queue it for SYSTEM.INI [386Enh] unless a value there or the queue has it
 * (returns the SYSTEM.INI image, NULL when SYSTEM.INI cannot be read); NULL: write the queue as
 * "device=" lines at the end of [386Enh], rewriting the file */
static char *QueueOrCommitVxd(LPSTR lpszList, LPCSTR lpszVxd)
{
    OFSTRUCT of;
    char line[0x100];                           /* 0x96 in 3.1 */
    if (lpszVxd) {
        if (!g_lpSysIni) WritePrivateProfileString(NULL, NULL, NULL, szSystemIni);
        if (!g_lpSysIni && OpenFile(szSystemIni, &of, OF_EXIST) != HFILE_ERROR) {
            g_lpSysIni = infOpen(of.szPathName);
            if (g_lpSysIni) {
                *lpszList = 0;
                g_lp386Enh = infFindSection(g_lpSysIni, sz386Enh);
                g_n386EnhLines = infLineCount(g_lp386Enh);
            }
        }
        for (int n = 1; infParseField(lpszList, n, line); n++)
            if (lstrcmpi(line, lpszVxd) == 0) return g_lpSysIni;
        char *lp = g_lp386Enh;
        for (int k = g_n386EnhLines; k; k--, lp = infNextLine(lp))
            for (int n = 1; infParseField(lp, n, line); n++)
                if (lstrcmpi(line, lpszVxd) == 0) return g_lpSysIni;
        lstrcat(lpszList, lpszVxd);
        lstrcat(lpszList, ",");
        return g_lpSysIni;
    }
    if (!g_lpSysIni) return NULL;
    infClose(g_lpSysIni);
    g_lpSysIni = NULL;
    WritePrivateProfileString(NULL, NULL, NULL, szSystemIni);
    if (!*lpszList) return NULL;
    HFILE h = OpenFile(szSystemIni, &of, OF_READWRITE);
    if (h == HFILE_ERROR) return NULL;
    LONG size = _llseek(h, 0, 2);
    if (size > 0xFFFE) {
        MessageBeep(0);
        _lclose(h);                             /* 3.1 leaves the file open */
        return NULL;
    }
    UINT cb = (UINT)size;
    char *lpText = malloc(cb + 1);
    if (!lpText) goto close;
    _llseek(h, 0, 0);
    if (_lread(h, lpText, cb) != cb) goto free;
    lpText[cb] = 0;
    /* seg2:190C StrIndexI: the offset of the header, the length when it is missing */
    size_t hdr = strlen(sz386EnhHdr), pos;
    char *found = NULL;
    for (char *p = lpText; *p && !found; p++)
        if (!StrNCmpI(p, sz386EnhHdr, (int)hdr)) found = p;
    if (found)
        pos = (size_t)(found - lpText) + hdr;
    else if (!StrNCmpI(lpText, sz386EnhHdr + 2, (int)hdr - 2))
        pos = hdr - 2;
    else {
        /* 3.1 reads and writes past the text when "\r\n[386enh]" is missing (also when the header is
         * the file's first line, accepted above): deviation, nothing is written */
        goto free;
    }
    {
        char *next = NULL;
        for (char *p = lpText + pos; *p && !next; p++)
            if (!StrNCmpI(p, szNextSect, lstrlen(szNextSect))) next = p;
        pos = next ? (size_t)(next - lpText) : cb;   /* before the CR/LF ahead of the next header */
    }
    _llseek(h, 0, 0);
    _lwrite(h, lpText, (UINT)pos);
    lstrcpy(line, szDevice);
    LPSTR lpItem = line + lstrlen(line);
    for (int n = 1; infParseFieldN(lpszList, n, lpItem, sizeof line - (lpItem - line)) && *lpItem; n++)
        _lwrite(h, line, lstrlen(line));
    _lwrite(h, lpText + pos, lstrlen(lpText + pos));
free:
    free(lpText);
close:
    _lclose(h);
    return NULL;
}

/* ------------------------------------------------------------------ the copy callbacks */
/* seg2:107E: COPY_ERROR, a message (none while related drivers install); always 0 = abort */
static int CopyErrorProc(int wErr, LPSTR lpszSpec)
{
    char fmt[0x100];
    LPSTR s = g_szErrSpec, pName = s;
    if (g_fQuietCopyErrors) return 0;
    lstrcpy(s, lpszSpec);
    if (*s) {
        /* after the last ':' or '\' - but the character right after a separator is never tested, so
         * "A:\X.DRV" gives "\X.DRV" (kept: only the file-in-use comparison below sees it). 3.1 also
         * reads past the terminator when a separator ends the spec (stopped here) */
        int i = 0;
        do {
            if (s[i] == ':' || s[i] == '\\') {
                i++;
                pName = s + i;
                if (!s[i]) break;
            }
            i++;
        } while (s[i] != 0);
    }
    char *pMsg = calloc(1, 0x200);
    if (!pMsg) return 0;
    if (wErr == 0x100)                          /* VIF_OUTOFSPACE */
        LoadString(g_hInst, 2005, pMsg, 0x100);
    else if (wErr != 0x80)
        LoadString(g_hInst, 2049, pMsg, 0x100);
    else {                                      /* VIF_FILEINUSE */
        BOOL fFound = FALSE;
        int i = (int)SendMessage(g_hwndInstList, LB_GETCOUNT, 0, 0);
        while (i-- > 0 && !fFound) {
            DRVITEM *q = ItemOf(g_hwndInstList, i);
            if (q == NO_ITEM) continue;
            if (lstrcmpi(q->szFile, pName) == 0) {
                LoadString(g_hInst, 2055, fmt, sizeof fmt);
                wsprintf(pMsg, fmt, q->szDesc);
                fFound = TRUE;
            }
        }
        if (!fFound) {
            g_idRestartMsg = 2053;
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_RESTART), g_hwndInstallParent, RestartDlgProc);
            free(pMsg);
            return 0;
        }
    }
    MessageBox(g_hwndInstallParent, pMsg, szFileInstErr, MB_TASKMODAL | MB_ICONEXCLAMATION);
    free(pMsg);
    return 0;
}

/* seg2:11E0: dialog 1006 asking for disk chDisk; 0 cancel, 2 retry */
static int InsertDiskPrompt(char chDisk, LPSTR lpszPath)
{
    g_chDisk = chDisk;
    g_lpszDiskPath = lpszPath;
    g_fBrowsePickFirst = TRUE;
    int r = DoDialogBox(IDD_INSTALL, GetActiveWindow(), InsertDiskDlgProc);
    g_fBrowsePickFirst = FALSE;
    return r;
}

/* seg2:136A */
static int CopyCallback(int msg, int wParam, LPSTR lpsz)
{
    char szPath[0x100], fmt[0x100];
    OFSTRUCT of;
    LPSTR lpName;
    switch (msg) {
    case COPY_ERROR:
        return CopyErrorProc(wParam, lpsz);
    case COPY_INSERTDISK:
        return InsertDiskPrompt((char)wParam, lpsz);
    case COPY_QUERYCOPY:
        /* a file already in the SYSTEM directory: the first one of an Add asks (Driver Exists),
         * the rest - and all VxDs - take that answer */
        GetSystemDirectory(szPath, 0x80);
        lpName = lpsz[1] == ':' ? lpsz + 2 : lpsz;
        lstrcat(szPath, "\\");
        lstrcat(szPath, lpName);
        if (OpenFile(szPath, &of, OF_EXIST | OF_SHARE_DENY_NONE) == HFILE_ERROR) return 1;
        if (g_fAskExists && !g_fCopyingVxds) {
            g_fAskExists = FALSE;
            LoadString(g_hInst, 2058, fmt, sizeof fmt);
            wsprintf(g_szExistsMsg, fmt, lpName);
            return DialogBox(g_hInst, MAKEINTRESOURCE(IDD_EXISTS), g_hwndInstallParent, DriverExistsDlgProc);
        }
        return g_nExistsAnswer;
    case COPY_START:
        SetErrorMode(SEM_FAILCRITICALERRORS);
        return 1;
    case COPY_END:
        SetErrorMode(0);                        /* not the previous mode, as in 3.1 */
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ installing */
/* seg2:1526: the first blank-delimited token ("  C:\MY DIR " -> "C:\MY") */
static void CopyFirstToken(LPSTR dst, LPCSTR src)
{
    int i = 0, o = 0;
    while (src[i] == ' ') i++;
    while (src[i]) {
        dst[o++] = src[i++];
        if (src[i] == ' ') break;
    }
    dst[o] = 0;
}

/* seg2:15E0: copies the files of the [Installable.drivers] line lpszKey and fills the template:
 * file (with field 5's parameters), description, section, types, related drivers, Boot */
static BOOL InstallFromInfLine(char *lp, LPCSTR lpszKey, LPSTR lpszVxdList, LPSTR lpszTypes, DRVITEM *pT)
{
    char *lpSect = lp, *lpMine;
    int n;
    for (;;) {
        g_szField[0] = 0;
        infParseField(lp, 0, g_szField);
        if (lstrcmpi(lpszKey, g_szField) == 0) break;
        if (!(lp = infNextLine(lp))) return FALSE;
    }
    if (!infParseField(lp, 1, g_szSpec)) return FALSE;        /* "5:msadlib.drv" */
    lpMine = lp;
    lstrcpy(g_szCurFile, g_szSpec[1] == ':' ? g_szSpec + 2 : g_szSpec);
    if (FileCopy(g_szSpec, szDestDisk, CopyCallback, 0)) return FALSE;
    /* field 5: default parameters, appended even when present but empty ("msadlib.drv ") */
    if (infParseFieldN(lpMine, 5, g_szField + 1, sizeof g_szField - 1)) {
        g_szField[0] = ' ';
        lstrcat(g_szSpec, g_szField);
    }
    StrNCpy(pT->szFile, FileName(g_szSpec), sizeof pT->szFile);
    infParseField(lpMine, 3, pT->szDesc);
    StrNCpy(pT->szSection, strstr(pT->szDesc, szMCI) ? szMCI : szDrivers, sizeof pT->szSection);
    /* field 2: the types, collected as "Wave,MIDI," */
    infParseField(lpMine, 2, g_szField);
    for (n = 1; infParseField(g_szField, n, g_szType); n++) {
        lstrcat(g_szType, ",");
        if (lstrlen(lpszTypes) + lstrlen(g_szType) < 0x100) lstrcat(lpszTypes, g_szType);
    }
    if (!*lpszTypes) return FALSE;
    /* the files of an INF section named after the key (usually there is none) */
    if (FileCopy((LPSTR)lpszKey, szDestDisk, CopyCallback, 2)) return FALSE;
    /* field 4: VxDs, on a 386 or better */
    g_fCopyingVxds = TRUE;
    if (g_f386 && infParseField(lpMine, 4, g_szField) && g_szField[0] && infParseField(g_szField, n = 1, g_szSpec)) {
        do {
            lstrcpy(g_szCurFile, g_szSpec[1] == ':' ? g_szSpec + 2 : g_szSpec);
            if (FileCopy(g_szSpec, szDestDisk, CopyCallback, 0)) goto fail;
            if (!QueueOrCommitVxd(lpszVxdList, FileName(g_szSpec))) goto fail;
        } while (infParseField(g_szField, ++n, g_szSpec));
    }
    g_fCopyingVxds = FALSE;
    /* field 7: "Boot" */
    g_szField7[0] = 0;
    infParseField(lpMine, 7, g_szField7);
    if (StrICmp(g_szField7, szBoot) == 0) g_fBootDriver = TRUE;
    /* field 6: related drivers (outermost install only) */
    if (!g_fRelatedPending) {
        infParseField(lpMine, 6, pT->szRelatedKeys);
        if (strlen(pT->szRelatedKeys)) {
            BuildRelatedFileList(lpSect, pT->szRelatedKeys, pT->szRelatedFiles);
            pT->fRelated = TRUE;
            g_fRelatedPending = TRUE;
        }
    }
    return TRUE;
fail:
    g_fCopyingVxds = FALSE;
    return FALSE;
}

/* seg2:157E */
static BOOL InstallFiles(LPCSTR lpszKey, LPSTR lpszTypes, DRVITEM *pT)
{
    char *lp = infFindSection(NULL, szInstallable);
    if (!lp) return FALSE;
    g_szVxdList[0] = 0;
    if (!InstallFromInfLine(lp, lpszKey, g_szVxdList, lpszTypes, pT)) return FALSE;
    if (g_f386) QueueOrCommitVxd(g_szVxdList, NULL);
    return TRUE;
}

/* seg2:28C4: DRV_INSTALL, then the driver's setup when it has one (opened by its file name) */
static BOOL InstallAndConfigure(HWND hDlg, DRVITEM *p)
{
    DRVCONFIGINFO dci;
    HourGlassOn();
    if (!p->hDriver) p->hDriver = OpenDriver(p->szFile, NULL, 0);
    if (!p->hDriver) {
        DriverLoadError(hDlg, p->szDesc, p->szFile);
        HourGlassOff();
        return FALSE;
    }
    FillDrvConfigInfo(&dci, p);
    if (SendDriverMessage(p->hDriver, DRV_INSTALL, 0, (LPARAM)&dci) == DRVCNF_RESTART) g_fRestartNeeded = TRUE;
    p->fConfigurable = (int)SendDriverMessage(p->hDriver, DRV_QUERYCONFIGURE, 0, 0);
    if (p->fConfigurable && SendDriverMessage(p->hDriver, DRV_CONFIGURE, (LPARAM)hDlg, (LPARAM)&dci) == DRVCNF_RESTART)
        g_fRestartNeeded = TRUE;
    CloseDriver(p->hDriver, 0, 0);
    p->hDriver = NULL;
    HourGlassOff();
    return TRUE;
}

/* seg2:2544: installs the [Installable.drivers] entry pszKey and then its related drivers */
static BOOL InstallDriver(HWND hMain, HWND hDlg, LPSTR pszKey)
{
    DRVITEM *pNew = NULL;
    HWND hList;
    int n, i;
    g_hwndInstallParent = hDlg;
    g_tmpl.fRelated = FALSE;
    g_szTypes[0] = 0;
    g_tmpl.szRelatedFiles[0] = 0;
    if (!InstallFiles(pszKey, g_szTypes, &g_tmpl)) return FALSE;
    g_szTypes[lstrlen(g_szTypes) - 1] = 0;      /* the trailing ',' */
    RemoveOldCopy(g_tmpl.szFile, g_tmpl.szSection);
    hList = GetDlgItem(hMain, IDC_INSTALLED);
    for (n = 1; infParseField(g_szTypes, n, g_szAlias); n++) {
        if (!MakeUniqueAlias(hList, g_szAlias, g_tmpl.szSection)) {
            char cap[0x1E], fmt[0x100];         /* [0x1908] [0x1808] */
            LoadString(g_hInst, 2016, cap, sizeof cap);
            LoadString(g_hInst, 2017, fmt, sizeof fmt);
            char *pMsg = calloc(1, lstrlen(g_szAlias) + 0x100);
            if (pMsg) {
                wsprintf(pMsg, fmt, g_szAlias);
                MessageBox(hDlg, pMsg, cap, MB_TASKMODAL | MB_ICONEXCLAMATION);
                free(pMsg);
            }
            continue;
        }
        if (!(pNew = calloc(1, sizeof *pNew))) return FALSE;
        *pNew = g_tmpl;
        StrNCpy(pNew->szAlias, g_szAlias, sizeof pNew->szAlias);
        if (n > 1) {
            /* a further type: one more SYSTEM.INI key, with the parameters (pNew leaks, as in 3.1) */
            WritePrivateProfileString(pNew->szSection, pNew->szAlias, pNew->szFile, szSystemIni);
            continue;
        }
        i = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)pNew->szDesc);
        if (i < 0) {
            free(pNew);
            return FALSE;
        }
        SendMessage(hList, LB_SETITEMDATA, i, (LPARAM)pNew);
        CutAtSpace(pNew->szFile);               /* the first alias is written without parameters */
        WritePrivateProfileString(pNew->szSection, pNew->szAlias, pNew->szFile, szSystemIni);
        if (!InstallAndConfigure(hDlg, pNew)) {
            /* the key goes again; copied files, [386Enh] lines and the removed old copy stay */
            WritePrivateProfileString(pNew->szSection, pNew->szAlias, NULL, szSystemIni);
            SendMessage(hList, LB_DELETESTRING, i, 0);
            return FALSE;
        }
        if (!g_fRelatedPending || pNew->fRelated) lstrcpy(g_szDriverDesc, pNew->szDesc);
        WritePrivateProfileString(szUserInst, pNew->szAlias, pNew->szFile, szControlIni);
        WritePrivateProfileString(szRelatedSect, pNew->szAlias, pNew->szRelatedFiles, szControlIni);
        WritePrivateProfileString(szDescSect, pNew->szFile, pNew->szDesc, szControlIni);
        if (g_fBootDriver) {
            static char boot[0x100];            /* [0x1132], read with itself as the default */
            GetPrivateProfileString(szBoot, szDrivers, boot, boot, sizeof boot, szSystemIni);
            lstrcat(boot, " ");
            lstrcat(boot, pNew->szAlias);
            WritePrivateProfileString(szBoot, szDrivers, boot, szSystemIni);
            g_fBootDriver = FALSE;
        }
    }
    /* 3.1 reads DS:0x384 (0) when no type was installed */
    if (pNew && pNew->fRelated == 1) {
        g_fQuietCopyErrors = TRUE;
        char keys[0x100];
        lstrcpy(keys, pNew->szRelatedKeys);
        for (n = 1; infParseField(keys, n, g_szRelKey); n++) InstallDriver(hMain, hDlg, g_szRelKey);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ the Install Driver path (1006) */
/* seg2:2DB6: the OEMSETUP.INF in the typed directory becomes the default INF (SETUP.INF saved) */
static BOOL OpenOemInf(HWND hDlg)
{
    OFSTRUCT of;
    HourGlassOn();
    GetDlgItemText(hDlg, IDC_PATH, szDiskPath, 0x41);
    CopyFirstToken(g_szOemPath, szDiskPath);
    lstrcpy(szDiskPath, g_szOemPath);
    LPSTR s = g_szOemPath + lstrlen(g_szOemPath);
    if (s == g_szOemPath || s[-1] != '\\') *s++ = '\\';     /* empty input -> "\" */
    *s = 0;
    lstrcpy(g_szLastPath, g_szOemPath);         /* "." disks now resolve here */
    lstrcpy(s, szOemInf);
    if (OpenFile(g_szOemPath, &of, OF_EXIST) == HFILE_ERROR) {
        SendDlgItemMessage(hDlg, IDC_PATH, EM_SETSEL, 0, MAKELPARAM(0, 0xFFFF));
        HourGlassOff();
        return FALSE;
    }
    if (g_fOemRetry)
        infSetDefault(infOpen(of.szPathName));          /* the old default is dropped (3.1 leaks it) */
    else
        g_lpSavedInf = infSetDefault(infOpen(of.szPathName));
    EndDialog(hDlg, 1);
    HourGlassOff();
    return TRUE;
}

/* seg2:2CE6: dialog 1006 for "Unlisted or Updated Driver" */
static BOOL OemPathDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hDlg, IDC_PROMPT, szOemPrompt);
        SetDlgItemText(hDlg, IDC_PATH, szDiskPath);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            if (OpenOemInf(hDlg))               /* it has ended this dialog with 1 */
                DialogBox(g_hInst, MAKEINTRESOURCE(IDD_UNLISTED), GetParent(hDlg), UnlistedListDlgProc);
            else
                EndDialog(hDlg, 2);             /* the caller shows it again */
            return FALSE;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDC_BROWSE:
            lstrcpy(g_szCurFile, szOemInf);
            BrowseForDir(hDlg, 1);              /* "Inf Files(*.inf)" */
            return FALSE;
        case IDC_HELP_INSTALL:
            goto help;
        }
        return FALSE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_INSTALL);
        return TRUE;
    }
    return FALSE;
}

/* seg2:1218: dialog 1006 during a copy: "Insert <disk label> or the disk with the updated <file>
 * driver in:" with the directory that failed */
static BOOL InsertDiskDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char szDisk[2], diskLine[0x100], label[0x100], text[0x300];
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        szDisk[0] = g_chDisk;
        szDisk[1] = 0;
        infGetProfileString(NULL, szDisks, szDisk, diskLine);
        if (!diskLine[0]) infGetProfileString(NULL, szOemDisks, szDisk, diskLine);
        label[0] = 0;
        infParseField(diskLine, 2, label);
        wsprintf(text, szDiskPrompt, label, g_szCurFile);
        SetDlgItemText(hDlg, IDC_PROMPT, text);
        SetDlgItemText(hDlg, IDC_PATH, g_lpszDiskPath);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            GetDlgItemText(hDlg, IDC_PATH, g_lpszDiskPath, 0x41);
            CopyFirstToken(g_szLastPath, g_lpszDiskPath);
            lstrcpy(g_lpszDiskPath, g_szLastPath);
            EndDialog(hDlg, 2);                 /* FileCopy tries again */
            UpdateWindow(g_hwndInstallParent);
            break;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        case IDC_BROWSE:
            BrowseForDir(hDlg, 3);              /* filter 3: there are two */
            break;
        case IDC_HELP_INSTALL:
            goto help;
        }
        return TRUE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_INSTALL);
        return TRUE;
    }
    return FALSE;
}

/* seg2:2E92: the Browse box as a directory picker: the first file of the current directory that
 * matches the filter (else g_szCurFile) goes into the hidden file edit, so OK takes the directory */
static UINT BrowseHookProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[0xC8];
    switch (msg) {
    case WM_INITDIALOG:
        GetDlgItemText(((LPOPENFILENAME)lParam)->hwndOwner, IDC_PROMPT, buf, sizeof buf);
        SetDlgItemText(hDlg, IDC_BROWSEPROMPT, buf);
        PostMessage(hDlg, WM_COMMAND, IDC_BROWSEPICK, 0);
        return FALSE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
        case lst2:
        case cmb2:
            PostMessage(hDlg, WM_COMMAND, IDC_BROWSEPICK, 0);
            return FALSE;
        case IDC_BROWSEPICK:
            if (g_fBrowsePickFirst) {
                HWND hFiles = GetDlgItem(hDlg, lst1);
                if (SendMessage(hFiles, LB_GETCOUNT, 0, 0)) {
                    SendMessage(hFiles, LB_SETCURSEL, 0, 0);
                    SendMessage(hDlg, WM_COMMAND, lst1, W16_CMD_LPARAM(hFiles, LBN_SELCHANGE));
                    return FALSE;
                }
            }
            SetDlgItemText(hDlg, edt1, g_szCurFile);
            return FALSE;
        case IDC_HELP_BROWSE:
            goto help;
        }
        return FALSE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_BROWSE);
        return TRUE;
    }
    return FALSE;
}

/* seg2:2F90: COMMDLG's Open box on template 38; the chosen directory goes into the path edit */
static void BrowseForDir(HWND hDlg, int nFilterIndex)
{
    char szFile[0x80] = "", szTitle[0x80] = "";
    OPENFILENAME ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hDlg;
    ofn.hInstance = g_hInst;
    ofn.lpstrFilter = szFilter;
    ofn.nFilterIndex = (DWORD)nFilterIndex;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof szFile;
    ofn.lpstrFileTitle = szTitle;
    ofn.nMaxFileTitle = sizeof szTitle;
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_SHOWHELP | OFN_ENABLEHOOK | OFN_ENABLETEMPLATE |
                OFN_FILEMUSTEXIST;
    ofn.lCustData = (LPARAM)hDlg;
    ofn.lpfnHook = BrowseHookProc;
    ofn.lpTemplateName = MAKEINTRESOURCE(IDD_BROWSE);
    if (GetOpenFileName(&ofn)) {
        UpdateWindow(hDlg);
        szFile[ofn.nFileOffset] = 0;            /* "A:\DRIVERS\X.INF" -> "A:\DRIVERS\" */
        SetDlgItemText(hDlg, IDC_PATH, szFile);
    }
}

/* ------------------------------------------------------------------ the small dialogs */
/* seg2:0A52: System Setting Change; Restart Now ends the session as ExitWindows does here */
static BOOL RestartDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char fmt[0x12C], text[0x12C + 0x100];
    (void)lParam;
    switch (msg) {
    case WM_KEYUP:                              /* goes to the focused control, so unused (3.1) */
        if (wParam == VK_F3) EndDialog(hDlg, 0);
        return FALSE;
    case WM_INITDIALOG:
        if (g_idRestartMsg) {
            LoadString(g_hInst, g_idRestartMsg, fmt, sizeof fmt);
            wsprintf(text, fmt, g_szDriverDesc);
            SetDlgItemText(hDlg, IDC_RESTARTTEXT, text);
        }
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDOK) {                   /* Restart Now */
            ExitWindows(EW_RESTARTWINDOWS, 0);
            SetActiveWindow(hDlg);              /* a program refused */
            return TRUE;
        }
        if (wParam == IDCANCEL) {               /* Don't Restart Now */
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* seg2:14BE: Driver Exists: New 1 (copy), Current -1 (keep), Cancel 0 */
static BOOL DriverExistsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    if (msg == WM_INITDIALOG) {
        SetDlgItemText(hDlg, IDC_EXISTSTEXT, g_szExistsMsg);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        if (wParam == IDCANCEL)
            EndDialog(hDlg, 0);
        else if (wParam == IDC_CURRENT) {
            g_nExistsAnswer = -1;
            EndDialog(hDlg, -1);
        } else if (wParam == IDC_NEW) {
            g_nExistsAnswer = 1;
            EndDialog(hDlg, 1);
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ the Add lists */
/* seg2:0674: [Installable.drivers] of the default INF (field 3 shown, the key as item data), with
 * "Unlisted or Updated Driver" first when fUnlisted */
static BOOL FillAvailableList(HWND hList, BOOL fUnlisted)
{
    char buf[0x100], key[0x100];
    BOOL fAny = FALSE;
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    char *lp = infFindSection(NULL, szInstallable);
    if (lp) {
        fAny = TRUE;                            /* even when nothing is added */
        do {
            key[0] = 0;
            buf[0] = 0;
            infParseField(lp, 0, key);          /* into 16 bytes in 3.1: longer keys overflow */
            infParseField(lp, 3, buf);
            char *pKey = strdup(key);
            if (!pKey) break;
            int i = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)buf);
            if (i != LB_ERR) SendMessage(hList, LB_SETITEMDATA, i, (LPARAM)pKey);
            else free(pKey);
            lp = infNextLine(lp);
        } while (lp);
    }
    if (fUnlisted == TRUE) {
        LoadString(g_hInst, 2031, buf, 0x96);
        int i = (int)SendMessage(hList, LB_INSERTSTRING, 0, (LPARAM)buf);
        if (i != LB_ERR) SendMessage(hList, LB_SETITEMDATA, i, 0);
    }
    if (fAny) SendMessage(hList, LB_SETCURSEL, 0, 0);
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    return fAny;
}

/* seg2:078E */
static void FreeAvailableList(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_AVAILABLE);
    int i = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    while (i-- > 0) {
        char *p = (char *)SendMessage(hList, LB_GETITEMDATA, i, 0);
        if (p != (char *)(intptr_t)LB_ERR && p) free(p);
    }
}

/* seg2:0B3E: Add Unlisted or Updated Driver (dialog 1002): the OEMSETUP.INF's drivers. Its key
 * blocks are never freed (no WM_DESTROY handler, as in 3.1) */
static BOOL UnlistedListDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hList;
    int r, iSel;
    switch (msg) {
    case WM_INITDIALOG:
        HourGlassOn();
        hList = GetDlgItem(hDlg, IDC_OEMLIST);
        if (!FillAvailableList(hList, FALSE)) {
            /* the OEMSETUP.INF has no [Installable.drivers]: ask for another directory */
            g_fBrowsePickFirst = TRUE;
            g_fOemRetry = TRUE;
            do r = DialogBox(g_hInst, MAKEINTRESOURCE(IDD_INSTALL), g_hwndInstallParent, OemPathDlgProc);
            while (r == 2);
            g_fBrowsePickFirst = FALSE;
            if (r == 1) {
                /* 3.1 resets control 301 (this dialog has 306) and posts WM_INITDIALOG, which is
                 * lost: the dialog ends below */
                SendDlgItemMessage(hDlg, IDC_AVAILABLE, LB_RESETCONTENT, 0, 0);
            }
            EndDialog(hDlg, 0);
        }
        SendMessage(hList, LB_SETCURSEL, 0, 0);
        HourGlassOff();
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDOK || (wParam == IDC_OEMLIST && HIWORD(lParam) == LBN_DBLCLK)) {
            hList = GetDlgItem(hDlg, IDC_OEMLIST);
            iSel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (iSel != LB_ERR) {
                HourGlassOn();
                LPSTR pKey = (LPSTR)SendMessage(hList, LB_GETITEMDATA, iSel, 0);
                g_fQuietCopyErrors = FALSE;
                g_fAskExists = TRUE;
                if (InstallDriver(g_hwndMain, hDlg, pKey)) {
                    HWND h = GetDlgItem(g_hwndMain, IDC_INSTALLED);
                    PostMessage(h, LB_SETCURSEL, 0, 0);
                    PostMessage(g_hwndMain, WM_COMMAND, IDC_INSTALLED, W16_CMD_LPARAM(h, LBN_SELCHANGE));
                    HourGlassOff();
                    if (g_fRestartNeeded) {
                        g_idRestartMsg = 2051;
                        DialogBox(g_hInst, MAKEINTRESOURCE(IDD_RESTART), hDlg, RestartDlgProc);
                    }
                } else
                    HourGlassOff();
                g_fRelatedPending = FALSE;
                g_fRestartNeeded = FALSE;
            }
            EndDialog(hDlg, 0);
            return FALSE;
        }
        if (wParam == IDCANCEL) {
            EndDialog(hDlg, 2);
            return FALSE;
        }
        if (wParam == IDC_HELP_UNLISTED) goto help;
        return FALSE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_UNLISTED);
        return TRUE;
    }
    return FALSE;
}

/* seg2:07E4: Add (dialog 1003) */
static BOOL AddDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hList;
    int iSel, r;
    switch (msg) {
    case WM_DESTROY:
        FreeAvailableList(hDlg);
        return FALSE;
    case WM_INITDIALOG:
        ShowWindow(hDlg, SW_SHOWNORMAL);
        HourGlassOn();
        if (g_lpSavedInf) {
            /* the last Add used an OEMSETUP.INF: back to SETUP.INF. The OEM image is not freed, as
             * in 3.1 (without SETUP.INF both can be the same image) */
            infSetDefault(g_lpSavedInf);
            g_lpSavedInf = NULL;
        }
        if (!FillAvailableList(GetDlgItem(hDlg, IDC_AVAILABLE), TRUE)) {
            /* no SETUP.INF: straight to the unlisted-driver path. 3.1 posts WM_INITDIALOG when that
             * succeeds, which is lost: this dialog ends */
            EndDialog(hDlg, 0);
            g_fBrowsePickFirst = TRUE;
            lstrcpy(g_szCurFile, szOemInf);
            if (DialogBox(g_hInst, MAKEINTRESOURCE(IDD_INSTALL), hDlg, OemPathDlgProc) != 1)
                g_lpSavedInf = infSetDefault(g_lpSavedInf);
            g_fBrowsePickFirst = FALSE;
        }
        HourGlassOff();
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
            hList = GetDlgItem(hDlg, IDC_AVAILABLE);
            iSel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (iSel == LB_ERR) goto end;
            if (iSel == 0) {                    /* Unlisted or Updated Driver */
                g_fOemRetry = FALSE;
                g_fBrowsePickFirst = TRUE;
                g_hwndInstallParent = hDlg;
                do r = DialogBox(g_hInst, MAKEINTRESOURCE(IDD_INSTALL), hDlg, OemPathDlgProc);
                while (r == 2);
                if (r == 1) {
                    /* the whole OEM install has happened inside; 3.1 then posts WM_INITDIALOG,
                     * which is lost */
                    FreeAvailableList(hDlg);
                    SendDlgItemMessage(hDlg, IDC_AVAILABLE, LB_RESETCONTENT, 0, 0);
                }
                g_fBrowsePickFirst = FALSE;
                goto end;
            }
            HourGlassOn();
            {
                LPSTR pKey = (LPSTR)SendMessage(hList, LB_GETITEMDATA, iSel, 0);
                g_fQuietCopyErrors = FALSE;
                g_fAskExists = TRUE;
                if (InstallDriver(g_hwndMain, hDlg, pKey)) {
                    HWND h = GetDlgItem(g_hwndMain, IDC_INSTALLED);
                    PostMessage(h, LB_SETCURSEL, 0, 0);
                    PostMessage(g_hwndMain, WM_COMMAND, IDC_INSTALLED, W16_CMD_LPARAM(h, LBN_SELCHANGE));
                    HourGlassOff();
                    if (g_fRestartNeeded) {
                        g_idRestartMsg = 2051;
                        DialogBox(g_hInst, MAKEINTRESOURCE(IDD_RESTART), hDlg, RestartDlgProc);
                    }
                } else
                    HourGlassOff();
            }
            g_fRestartNeeded = FALSE;
            g_fRelatedPending = FALSE;
        end:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDC_AVAILABLE:
            if (HIWORD(lParam) == LBN_DBLCLK) SendMessage(hDlg, WM_COMMAND, IDOK, 0);
            return TRUE;
        case IDC_HELP_ADD:
            goto help;
        }
        return FALSE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_ADD);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg2:0322: Drivers (1001) */
static BOOL DriversDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DRVCONFIGINFO dci;
    HWND hList, h;
    if (msg == WM_DESTROY) return FALSE;
    if (msg == WM_INITDIALOG) {
        HourGlassOn();
        g_fDiskRetried = FALSE;
        g_f386 = (GetWinFlags() & (WF_CPU286 | WF_CPU086 | WF_CPU186)) == 0;
        hList = GetDlgItem(hDlg, IDC_INSTALLED);
        SendMessage(hList, WM_SETREDRAW, FALSE, 0);
        BOOL f1 = LoadInstalledSection(hDlg, szMCI);
        if ((LoadInstalledSection(hDlg, szDrivers) | f1) == 0) {
            EnableWindow(GetDlgItem(hDlg, IDC_SETUP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
        }
        SendMessage(hList, LB_SETCURSEL, 0, 0);
        PostMessage(hDlg, WM_COMMAND, IDC_INSTALLED, W16_CMD_LPARAM(hList, LBN_SELCHANGE));
        SendMessage(hList, WM_SETREDRAW, TRUE, 0);
        HourGlassOff();
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        hList = GetDlgItem(hDlg, IDC_INSTALLED);
        g_hwndInstList = hList;
        g_hwndMain = hDlg;
        int iSel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        /* 3.1 takes LB_ERR's 0xFFFF as the item then; Setup and Remove are disabled without a
         * selection, so it is not reached */
        DRVITEM *pSel = iSel == LB_ERR ? NULL : ItemOf(hList, iSel);
        switch (wParam) {
        case IDCANCEL:                          /* Cancel / Close */
            HourGlassOn();
            FreeInstalledList(hList);
            HourGlassOff();
            EndDialog(hDlg, 0);
            return TRUE;
        case IDC_ADD:
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ADD), hDlg, AddDlgProc);
            break;
        case IDC_SETUP:
            if (!pSel || pSel == NO_ITEM) return TRUE;
            if (!pSel->hDriver) pSel->hDriver = OpenDriver(pSel->szAlias, pSel->szSection, 0);
            if (!pSel->hDriver) {
                DriverLoadError(hDlg, pSel->szDesc, pSel->szFile);
                return TRUE;                    /* no relabel */
            }
            FillDrvConfigInfo(&dci, pSel);
            if (SendDriverMessage(pSel->hDriver, DRV_CONFIGURE, (LPARAM)hDlg, (LPARAM)&dci) == DRVCNF_RESTART) {
                g_idRestartMsg = 0;             /* the template's text */
                DialogBox(g_hInst, MAKEINTRESOURCE(IDD_RESTART), hDlg, RestartDlgProc);
            }
            CloseDriver(pSel->hDriver, 0, 0);
            pSel->hDriver = NULL;
            break;
        case IDC_REMOVE:
            if (!pSel || pSel == NO_ITEM) return TRUE;
            if (!ConfirmRemove(hDlg, pSel->szAlias, pSel->szDesc)) break;   /* relabels even after No */
            if (!RemoveDriver(hList, pSel, TRUE, iSel)) {
                MessageBox(hDlg, szNoUndo, szDrivers, MB_TASKMODAL | MB_ICONEXCLAMATION);
                break;
            }
            PostMessage(hList, LB_SETCURSEL, 0, 0);
            if (SendMessage(hList, LB_GETCOUNT, 0, 0) == 0) {
                EnableWindow(GetDlgItem(hDlg, IDC_SETUP), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), FALSE);
            } else
                PostMessage(g_hwndMain, WM_COMMAND, IDC_INSTALLED, W16_CMD_LPARAM(hList, LBN_SELCHANGE));
            g_idRestartMsg = 2052;              /* always shown */
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_RESTART), hDlg, RestartDlgProc);
            break;
        case IDC_INSTALLED:
            switch (HIWORD(lParam)) {
            case LBN_SELCHANGE:
            case LBN_SETFOCUS:
                EnableWindow(GetDlgItem(hDlg, IDC_SETUP), iSel >= 0 && pSel && QueryConfigurable(pSel, hDlg) != 0);
                EnableWindow(GetDlgItem(hDlg, IDC_REMOVE), iSel != -1);
                break;
            case LBN_DBLCLK:
                h = GetDlgItem(hDlg, IDC_SETUP);
                if (IsWindowEnabled(h)) PostMessage(hDlg, WM_COMMAND, IDC_SETUP, W16_CMD_LPARAM(h, 0));
                break;
            }
            return TRUE;
        case IDC_HELP_DRIVERS:
            goto help;
        default:
            return FALSE;
        }
        SetCancelToClose(hDlg);
        return TRUE;
    }
    if (msg == g_msgShellHelp) {
    help:
        WinHelp(hDlg, szHelpFile, HELP_CONTEXT, IDC_HELP_DRIVERS);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ seg2:0F24 InitInf */
/* SETUP.INF (found as OpenFile finds it: the SYSTEM directory first) becomes the default INF, cut at
 * [blowaway]; the default source "A:\" (the first CD-ROM drive once per load) */
static BOOL InitInf(void)
{
    OFSTRUCT of;
    char buf[0x100];
    HourGlassOn();
    if (OpenFile(szSetupInf, &of, OF_EXIST) == HFILE_ERROR) {
        HourGlassOff();
        LoadString(g_hInst, 2003, buf, sizeof buf);
        /* 3.1's owner is 0 the first time, a destroyed window later (USER then uses none) */
        MessageBox(IsWindow(g_hwndInstallParent) ? g_hwndInstallParent : NULL, buf, szDrivers, MB_ICONEXCLAMATION);
        return FALSE;                           /* g_szWinDir and g_szLastPath stay unset (3.1) */
    }
    char *lpInf = infOpen(of.szPathName), *lp;
    if (lpInf && (lp = infFindSection(NULL, szBlowaway)) != NULL) {
        lp[0] = 0;                              /* the image now ends at [blowaway] */
        lp[1] = 0;
        lp[2] = 0x1A;
    }
    HourGlassOff();
    GetWindowsDirectory(g_szWinDir, sizeof g_szWinDir);
    if (!g_fDefDriveDone) {
        for (int i = 0; i < 26; i++)
            if (IsCDRomDrive(i)) {
                szDiskPath[0] = (char)(szDiskPath[0] + i);
                break;
            }
        g_fDefDriveDone = TRUE;
    }
    lstrcpy(g_szLastPath, szDiskPath);
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:01EA LibMain */
static BOOL LoadModule(void)
{
    static const struct { int id; char *buf; int cb; } s[] = {
        {2032, szClose, sizeof szClose}, {2008, szDescSect, sizeof szDescSect},
        {2046, szFileInstErr, sizeof szFileInstErr}, {2007, szInstallable, sizeof szInstallable},
        {2057, szRelatedSect, sizeof szRelatedSect}, {2038, szUserInst, sizeof szUserInst},
        {2034, szOemPrompt, sizeof szOemPrompt}, {2035, szDiskPrompt, sizeof szDiskPrompt},
        {2044, szOemInf, sizeof szOemInf}, {2045, szDestDisk, sizeof szDestDisk},
        {2009, szNoUndo, sizeof szNoUndo}, {2013, szNoDesc, sizeof szNoDesc},
        {2014, szDriverErr, sizeof szDriverErr}, {2033, szSureRemove, sizeof szSureRemove},
        {2037, szRequired, sizeof szRequired}, {2021, szSetupInf, sizeof szSetupInf},
        {2020, szDrivers, sizeof szDrivers}, {2036, szRemove, sizeof szRemove},
        {2022, szControlIni, sizeof szControlIni}, {2023, szSystemIni, sizeof szSystemIni},
        {2024, szMCI, sizeof szMCI}, {2047, szMIDI, sizeof szMIDI}, {2048, szWAVE, sizeof szWAVE},
        {2004, szDiskPath, sizeof szDiskPath}, {2026, szHelpFile, sizeof szHelpFile},
        {2050, szBoot, sizeof szBoot},
    };
    if (g_hInst) return TRUE;
    g_hInst = w16_load_module("DRIVERS.CPL");
    if (!g_hInst) return FALSE;
    /* 3.1 also loads 5008 (cut to 49 characters), 2016, 2019 and 2056 into buffers it never uses */
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++) LoadString(g_hInst, s[i].id, s[i].buf, s[i].cb);
    g_idIcon = 6000;
    g_idName = 2001;
    g_idInfo = 2002;
    g_dwHelpContext = 5120;
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:001E CPlApplet */
LRESULT Drivers_CPlApplet(HWND hwndCPl, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    char buf[0x100], cwd[0x100];
    (void)lParam1;
    switch (msg) {
    case CPL_INIT:
        if (!LoadModule()) return 0;
        g_msgShellHelp = RegisterWindowMessage("ShellHelp");
        return 1;
    case CPL_GETCOUNT:
        return 1;
    case CPL_DBLCLK:
        if (g_fRunning) {
            MessageBeep(0);
            return 0;
        }
        g_fRunning = TRUE;
        g_lpSavedInf = NULL;                    /* dropped, not freed (3.1) */
        /* the SYSTEM directory becomes current so OpenFile finds SETUP.INF there first; 3.1's
         * LocalAlloc for these 0x100 bytes cannot fail here (its out-of-memory path is not ported) */
        GetSystemDirectory(buf, 0x80);
        w16_getcwd(cwd, sizeof cwd);            /* seg2:4098 DosGetCwd */
        w16_chdir(buf);                         /* seg2:4267 DosChDir */
        InitInf();
        /* the dialog always ends with 0; only a DialogBox failure (-1) restarts Windows (3.1) */
        if (DialogBox(g_hInst, MAKEINTRESOURCE(IDD_DRIVERS), hwndCPl, DriversDlgProc) != 0)
            ExitWindows(EW_RESTARTWINDOWS, 0);
        infClose(NULL);
        w16_chdir(cwd);
        g_fRunning = FALSE;
        return 1;
    case CPL_NEWINQUIRE: {
        NEWCPLINFO *ni = (NEWCPLINFO *)lParam2;
        ni->hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(g_idIcon));
        if (!LoadString(g_hInst, g_idName, ni->szName, sizeof ni->szName)) ni->szName[0] = 0;
        if (!LoadString(g_hInst, g_idInfo, ni->szInfo, sizeof ni->szInfo)) ni->szInfo[0] = 0;
        ni->dwSize = sizeof *ni;                /* 0xF2, the 16-bit structure, in 3.1 */
        ni->lData = 0;
        ni->dwHelpContext = g_dwHelpContext;
        lstrcpy(ni->szHelpFile, szHelpFile);
        return 0;                               /* dwFlags is not written (3.1) */
    }
    }
    return 0;                                   /* CPL_INQUIRE is not implemented (3.1) */
}
