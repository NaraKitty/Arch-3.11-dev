/* commdlg.h / shellapi.h equivalents for libw16 (Windows 3.1 COMMDLG.DLL and SHELL.DLL API) */
#ifndef W16_COMMDLG_H
#define W16_COMMDLG_H
#include "w16.h"

typedef UINT (*LPOFNHOOKPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCSTR lpstrFilter;
    LPSTR lpstrCustomFilter;
    DWORD nMaxCustFilter;
    DWORD nFilterIndex;
    LPSTR lpstrFile;
    DWORD nMaxFile;
    LPSTR lpstrFileTitle;
    DWORD nMaxFileTitle;
    LPCSTR lpstrInitialDir;
    LPCSTR lpstrTitle;
    DWORD Flags;
    UINT nFileOffset;
    UINT nFileExtension;
    LPCSTR lpstrDefExt;
    LPARAM lCustData;
    LPOFNHOOKPROC lpfnHook;
    LPCSTR lpTemplateName;
} OPENFILENAME, *LPOPENFILENAME;

#define OFN_READONLY 0x00000001
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_HIDEREADONLY 0x00000004
#define OFN_NOCHANGEDIR 0x00000008
#define OFN_SHOWHELP 0x00000010
#define OFN_ENABLEHOOK 0x00000020
#define OFN_ENABLETEMPLATE 0x00000040
#define OFN_ENABLETEMPLATEHANDLE 0x00000080
#define OFN_NOVALIDATE 0x00000100
#define OFN_ALLOWMULTISELECT 0x00000200
#define OFN_EXTENSIONDIFFERENT 0x00000400
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_CREATEPROMPT 0x00002000
#define OFN_SHAREAWARE 0x00004000
#define OFN_NOREADONLYRETURN 0x00008000
#define OFN_NOTESTFILECREATE 0x00010000

typedef struct {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    DWORD Flags;
    LPSTR lpstrFindWhat;
    LPSTR lpstrReplaceWith;
    UINT wFindWhatLen;
    UINT wReplaceWithLen;
    LPARAM lCustData;
    void *lpfnHook;
    LPCSTR lpTemplateName;
} FINDREPLACE, *LPFINDREPLACE;

#define FR_DOWN 0x00000001
#define FR_WHOLEWORD 0x00000002
#define FR_MATCHCASE 0x00000004
#define FR_FINDNEXT 0x00000008
#define FR_REPLACE 0x00000010
#define FR_REPLACEALL 0x00000020
#define FR_DIALOGTERM 0x00000040
#define FR_SHOWHELP 0x00000080
#define FR_ENABLEHOOK 0x00000100
#define FR_ENABLETEMPLATE 0x00000200
#define FR_NOUPDOWN 0x00000400
#define FR_NOMATCHCASE 0x00000800
#define FR_NOWHOLEWORD 0x00001000
#define FR_ENABLETEMPLATEHANDLE 0x00002000
#define FR_HIDEUPDOWN 0x00004000
#define FR_HIDEMATCHCASE 0x00008000
#define FR_HIDEWHOLEWORD 0x00010000
#define FINDMSGSTRING "commdlg_FindReplace"
#define HELPMSGSTRING "commdlg_help"

typedef struct {
    DWORD lStructSize;
    HWND hwndOwner;
    HGLOBAL hDevMode;
    HGLOBAL hDevNames;
    HDC hDC;
    DWORD Flags;
    UINT nFromPage, nToPage, nMinPage, nMaxPage, nCopies;
    HINSTANCE hInstance;
    LPARAM lCustData;
    void *lpfnPrintHook, *lpfnSetupHook;
    LPCSTR lpPrintTemplateName, lpSetupTemplateName;
    HGLOBAL hPrintTemplate, hSetupTemplate;
} PRINTDLG, *LPPRINTDLG;

#define PD_ALLPAGES 0x00000000
#define PD_SELECTION 0x00000001
#define PD_PAGENUMS 0x00000002
#define PD_NOSELECTION 0x00000004
#define PD_NOPAGENUMS 0x00000008
#define PD_COLLATE 0x00000010
#define PD_PRINTTOFILE 0x00000020
#define PD_PRINTSETUP 0x00000040
#define PD_NOWARNING 0x00000080
#define PD_RETURNDC 0x00000100
#define PD_RETURNIC 0x00000200
#define PD_RETURNDEFAULT 0x00000400
#define PD_SHOWHELP 0x00000800
#define PD_USEDEVMODECOPIES 0x00040000
#define PD_DISABLEPRINTTOFILE 0x00080000
#define PD_HIDEPRINTTOFILE 0x00100000

#define CDERR_DIALOGFAILURE 0xFFFF
#define CDERR_GENERALCODES 0x0000
#define CDERR_STRUCTSIZE 0x0001
#define CDERR_INITIALIZATION 0x0002
#define CDERR_NOTEMPLATE 0x0003
#define CDERR_NOHINSTANCE 0x0004
#define CDERR_LOADSTRFAILURE 0x0005
#define CDERR_FINDRESFAILURE 0x0006
#define CDERR_LOADRESFAILURE 0x0007
#define CDERR_LOCKRESFAILURE 0x0008
#define CDERR_MEMALLOCFAILURE 0x0009
#define CDERR_MEMLOCKFAILURE 0x000A
#define CDERR_NOHOOK 0x000B
#define PDERR_SETUPFAILURE 0x1001
#define PDERR_PARSEFAILURE 0x1002
#define PDERR_RETDEFFAILURE 0x1003
#define PDERR_LOADDRVFAILURE 0x1004
#define PDERR_GETDEVMODEFAIL 0x1005
#define PDERR_INITFAILURE 0x1006
#define PDERR_NODEVICES 0x1007
#define PDERR_NODEFAULTPRN 0x1008
#define PDERR_DNDMMISMATCH 0x1009
#define PDERR_CREATEICFAILURE 0x100A
#define PDERR_PRINTERNOTFOUND 0x100B

#define FNERR_SUBCLASSFAILURE 0x3001
#define FNERR_INVALIDFILENAME 0x3002
#define FNERR_BUFFERTOOSMALL 0x3003
#define FRERR_BUFFERLENGTHZERO 0x4001

/* print.h subset: what the 3.x apps read from PRINTDLG.hDevNames / hDevMode */
typedef struct {
    WORD wDriverOffset, wDeviceOffset, wOutputOffset, wDefault;
} DEVNAMES, *LPDEVNAMES;
#define DN_DEFAULTPRN 0x0001
typedef struct {
    char dmDeviceName[32];
    WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    short dmOrientation, dmPaperSize, dmPaperLength, dmPaperWidth, dmScale, dmCopies, dmDefaultSource,
        dmPrintQuality, dmColor, dmDuplex;
} DEVMODE, *LPDEVMODE;
#define DM_ORIENTATION 0x0001L
#define DM_PAPERSIZE 0x0002L
#define DM_COPIES 0x0100L
#define DMORIENT_PORTRAIT 1
#define DMORIENT_LANDSCAPE 2
#define DMPAPER_LETTER 1
#define DMPAPER_EXECUTIVE 7
#define DMPAPER_A4 9
#define DMPAPER_A5 11
#define DMPAPER_B5 13
#define DMPAPER_LEGAL 5
#define DMPAPER_ENV_10 20
#define DMPAPER_ENV_DL 27

BOOL GetOpenFileName(OPENFILENAME *ofn);
BOOL GetSaveFileName(OPENFILENAME *ofn);
HWND FindText(FINDREPLACE *fr);
HWND ReplaceText(FINDREPLACE *fr);
BOOL PrintDlg(PRINTDLG *pd);
/* arch311: the port a CUPS device URI is shown on wherever 3.1 shows ports (Print Setup, the
 * Printers applet): parallel:/dev/lpN is LPT(N+1):, serial:/dev/ttySN COM(N+1): as on a PC, any other
 * URI its scheme upper-cased with a colon (USB:, IPP:, SOCKET:, SMB:, FILE:); one without a scheme
 * is WIN.INI's NullPort ("None") */
void w16_printer_port(LPCSTR uri, LPSTR port, int cb);
/* arch311: where Print and Print Setup take the printers from. By default libw16 asks CUPS itself
 * (lpstat); a program with its own CUPS layer (the Control Panel, whose ARCH311_SIMULATE sample
 * queues the dialog must show as well) sets fn, which fills up to max name/port pairs, returns how
 * many and sets *def to the default printer's index (-1: none). NULL restores the default. */
typedef int (*W16PRINTERENUMPROC)(char (*name)[64], char (*port)[64], int max, int *def);
void w16_set_printer_enum(W16PRINTERENUMPROC fn);
DWORD CommDlgExtendedError(void);
int GetFileTitle(LPCSTR file, LPSTR title, UINT cb);

/* SHELL.DLL */
int ShellAbout(HWND h, LPCSTR app, LPCSTR other, HICON icon);

/* KERNEL / printing (GDI Escape interface used by 3.x apps) */
#define STARTDOC 10
#define ENDDOC 11
#define NEWFRAME 1
#define ABORTDOC 2
#define SETABORTPROC 9
#define SP_ERROR (-1)
#define SP_APPABORT (-2)
#define SP_USERABORT (-3)
#define SP_OUTOFDISK (-4)
#define SP_OUTOFMEMORY (-5)
BOOL DeleteFileDos(LPCSTR name);
#endif
