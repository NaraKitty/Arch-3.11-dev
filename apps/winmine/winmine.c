/*
 * Minesweeper - native 64-bit port of the Windows 3.11 Minesweeper (WINMINE.EXE 3.10,
 * "Minesweeper v3.0a", 27,776 bytes).
 *
 * Ported function by function from the disassembly of the user's own WINMINE.EXE; every function
 * notes the seg:offset it comes from (all of the program is in seg1; DGROUP offsets are given for
 * the variables). The menu, the accelerators, the three dialogs, the strings, the icon and the six
 * bitmaps (blocks, LED digits and faces, each in a colour and a monochrome version) are loaded at
 * run time from the user's ripped WINMINE.EXE; nothing Microsoft-made is compiled into this file.
 *
 * Settings live in the [Minesweeper] section of WINMINE.INI in the Windows directory - 3.1's
 * Minesweeper does not use WIN.INI. They are read at start-up (seg1:22FD) and written when the
 * window is destroyed or the session ends, only after a change (seg1:24A5).
 *
 * Mine layout: the Microsoft C run time's rand() (seg1:25E2) after srand(LOWORD(GetCurrentTime()))
 * at start-up (seg1:17B9). For repeatable tests the environment variable WINMINE_SEED replaces the
 * tick count as the seed; it is read in InitConst only. Real 3.11 has no such override.
 *
 * Differences from the original, all deliberate:
 *  - the second-instance branch of WinMain is kept but never runs: every arch311 program is its
 *    own process, so hPrevInstance is always NULL;
 *  - the bitmap resources are copied before the EGA colour-table patch (3.1 patches the loaded
 *    resource in memory); the copies are freed where 3.1 unlocks the resources.
 */
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "commdlg.h" /* ShellAbout */

const char *w16_app_module = "WINMINE.EXE";

/* ------------------------------------------------------------------ resources */
#define ID_ICON_MAIN 100
#define ID_MENU 500
#define ID_MENU_ACCEL 501
#define ID_BMP_BLOCKS 410 /* +1: the monochrome version (seg1:19C1) */
#define ID_BMP_LED 420
#define ID_BMP_BUTTON 430

/* menu commands */
#define IDM_NEW 510
#define IDM_EXIT 512
#define IDM_BEGIN 521
#define IDM_INTER 522
#define IDM_EXPERT 523
#define IDM_CUSTOM 524
#define IDM_MARK 527
#define IDM_BEST 528
#define IDM_COLOR 529
#define IDM_HELP 591
#define IDM_HOW2PLAY 593
#define IDM_HELP_HELP 594
#define IDM_ABOUT 596

/* dialogs: Custom Field (80), the name prompt (600), Best Times (700) */
#define ID_DLG_PREF 80
#define ID_EDIT_HEIGHT 141
#define ID_EDIT_WIDTH 142
#define ID_EDIT_MINES 143
#define ID_DLG_ENTER 600
#define ID_TEXT_BEST 601
#define ID_EDIT_NAME 602
#define ID_DLG_BEST 700
#define ID_TIME_BEGIN 701 /* time; the name is the next id */
#define ID_TIME_INTER 703
#define ID_TIME_EXPERT 705
#define ID_BTN_RESET 777
/* the dialog procedures also accept 100 like OK and 109 like Cancel (no control has these ids) */
#define ID_BTN_OK_OLD 100
#define ID_BTN_CANCEL_OLD 109

/* strings */
#define ID_GAMENAME 1   /* window class and INI section */
#define ID_HELPFILE 2
#define ID_ERR_TITLE 3
#define ID_ERR_TIMER 4
#define ID_ERR_MEM 5
#define ID_ERR_UNKNOWN 6 /* "Error: %d" */
#define ID_MSG_SEC 7     /* "%d seconds" */
#define ID_NAME_DEFAULT 8
#define ID_MSG_BEGIN 9   /* 9, 10, 11: the record messages of the three levels */
#define ID_MSG_ABOUT 12
#define ID_MSG_CREDIT 13
#define ID_MSG_VERSION 14 /* the window title */
#define ID_ERR_MAX 999    /* ReportErr ids from here up are numbers */

#define ID_TIMER 1

/* ------------------------------------------------------------------ the board */
/* one byte per box, 32 per row; rows and columns 0 and max + 1 are a border (seg1:0D08) */
#define cBlkRow 32
#define cBlkMax 0x360
#define MaskBomb 0x80
#define MaskVisit 0x40
#define MaskFlags 0xE0
#define MaskData 0x1F
/* box images, from the bottom of the blocks bitmap up (the bitmap has 16 of 16 x 16) */
#define iBlkBlank 0      /* 0 .. 8: open, with that many mines around */
#define iBlkGuessDown 9  /* "?" pressed */
#define iBlkBombDown 10  /* a mine shown at the end */
#define iBlkWrong 11     /* a flag on a box without a mine */
#define iBlkExplode 12   /* the mine that went off */
#define iBlkGuessUp 13   /* "?" */
#define iBlkBombUp 14    /* flag */
#define iBlkBlankUp 15   /* covered */
#define iBlkMax 16       /* border */

#define BLK(x, y) rgBlk[((y) << 5) + (x)]
#define IsBomb(x, y) (BLK(x, y) & MaskBomb)
#define IsVisit(x, y) (BLK(x, y) & MaskVisit)
#define SetBomb(x, y) (BLK(x, y) |= MaskBomb)
#define iBlk(x, y) (BLK(x, y) & MaskData)
#define fInRange(x, y) ((x) > 0 && (y) > 0 && (x) <= xBoxMac && (y) <= yBoxMac)

/* faces, from the bottom of the button bitmap up */
#define iButtonHappy 0
#define iButtonCaution 1
#define iButtonLose 2
#define iButtonWin 3
#define iButtonDown 4

#define iLedBlank 10
#define iLedNegative 11

/* fStatus */
#define fPlay 0x01
#define fPause 0x02
#define fEsc 0x04 /* set by the Esc key, read nowhere */
#define fIcon 0x08
#define fDemo 0x10 /* game over */

/* game types */
#define wGameBegin 0
#define wGameInter 1
#define wGameExpert 2
#define wGameOther 3

/* sound (WINMINE.INI Sound=3 only): fSound 2 = off, 3 = on */
#define fsoundOff 2
#define fsoundOn 3
#define TUNE_TICK 1
#define TUNE_WINGAME 2
#define TUNE_LOSEGAME 3

/* WINMINE.INI keys, in the order of the pointer table at ds:00C2 */
enum { iszPrefGame, iszPrefMines, iszPrefHeight, iszPrefWidth, iszPrefxWindow, iszPrefyWindow,
       iszPrefSound, iszPrefMark, iszPrefMenu, iszPrefTick, iszPrefColor, iszPrefBeginTime,
       iszPrefBeginName, iszPrefInterTime, iszPrefInterName, iszPrefExpertTime, iszPrefExpertName };
static const char *const rgszPref[] = {"Difficulty", "Mines", "Height", "Width", "Xpos", "Ypos",
                                       "Sound", "Mark", "Menu", "Tick", "Color", "Time1", "Name1",
                                       "Time2", "Name2", "Time3", "Name3"};
static const char szIniFile[] = "winmine.ini";

/* ------------------------------------------------------------------ globals (DGROUP) */
static BOOL fButton1Down;       /* [0x10] a box press is being tracked */
static BOOL fBlock;             /* [0x12] ... with both buttons (open around a number) */
static BOOL fIgnoreClick;       /* [0x14] the click that activated the window */
static int fStatus = fDemo | fIcon; /* [0x16] */
static BOOL fInMenu;            /* [0x18] between WM_ENTERMENULOOP and WM_EXITMENULOOP */
static const int rgLevelData[3][3] = {{10, 8, 8}, {40, 16, 16}, {99, 16, 30}}; /* [0x22] mines, height, width */
static int iXyzzy;              /* [0x34] */
static const char szXyzzy[] = "XYZZY"; /* [0x36] */
static int iButtonCur;          /* [0x3c] */
static BOOL fTimer;             /* [0x3e] */
static int xCur = -1, yCur = -1; /* [0x40] [0x42] the box under the pressed mouse */
static HDC hdcBlk;              /* [0x44] memory DC holding the blocks bitmap */
static HBITMAP hbmOld;          /* [0x46] its original bitmap */
static HBITMAP hbmPrev;         /* [0x48] the blocks bitmap being replaced */
static HPEN hGrayPen;           /* [0x4a] */
static BOOL fUpdateIni;         /* [0xb4] */
static DWORD holdrand = 1;      /* [0xfc] the C run time's random seed */
static int dypCaption;          /* [0x250] SM_CYCAPTION + 1 */
static HINSTANCE hInst;         /* [0x252] */
static BYTE *lpDibBlks;         /* [0x254] */
static int wGameType;           /* [0x258] Difficulty */
static int Mines, Height, Width; /* [0x25a] [0x25c] [0x25e] the preferences */
static int xWindow, yWindow;    /* [0x260] [0x262] client origin + 1 */
static int fSound;              /* [0x264] */
static BOOL fMark;              /* [0x266] */
static BOOL fTick;              /* [0x268] */
static int fMenu;               /* [0x26a] 0 always shown, 1 hidden (F5), 2 shown (F6) */
static BOOL fColor;             /* [0x26c] */
static int rgTime[3];           /* [0x26e] */
static char szBegin[64], szInter[64], szExpert[64]; /* [0x274] [0x2b4] [0x2f4] record holders */
static BYTE *lpDibButton;       /* [0x334] */
static char szTitle[128];       /* [0x338] */
static int dypMenu;             /* [0x358] SM_CYMENU + 1 */
static HMENU hMenu;             /* [0x35a] */
static char szHelpFile[128];    /* [0x35c] */
static int cBoxVisit;           /* [0x37c] boxes opened */
static BOOL fColorCapable;      /* [0x380] the display has more than 2 colours */
static int cSec;                /* [0x384] */
static char szClass[128];       /* [0x386] */
static HBITMAP hbmBlocks;       /* [0x3a6] */
static int xBoxMac, yBoxMac;    /* [0x3c6] [0x3c8] the board in play */
static BYTE *lpDibLed;          /* [0x3ca] */
static HWND hwndMain;           /* [0x3ce] */
static int dypAdjust;           /* [0x3d0] caption (+ menu bar) height */
static int rgDibLedOff[12];     /* [0x3d2] byte offset of each digit's bits */
static int cBombStart;          /* [0x3ea] */
static BOOL fInitMinimized;     /* [0x3ec] */
static char szTime[128];        /* [0x3f2] "%d seconds" */
static int cBoxVisitMac;        /* [0x412] boxes without a mine */
static int dxpBorder;           /* [0x414] SM_CXBORDER + 1 */
static int iStepMac;            /* [0x416] end of the open-area queue */
static char szDefaultName[128]; /* [0x418] */
static int dxWindow;            /* [0x438] client width */
static int cBombLeft;           /* [0x43a] */
static int dypBorder;           /* [0x43c] SM_CYBORDER + 1 (set, never used) */
static int dyWindow;            /* [0x43e] client height */
static BYTE rgBlk[cBlkMax];     /* [0x440] */
static int rgDibButOff[5];      /* [0x7a0] */
static BOOL fEGA;               /* [0x7aa] screen under 351 lines */
static int rgStepX[100], rgStepY[100]; /* [0x7ac] [0x874] the open-area queue */

static void StartGame(void);
static void DoTimer(void);
static void GameOver(BOOL fWinLose);
static void DisplayButton(int iButton);
static void DisplayBlk(int x, int y);
static void DisplayGrid(void);
static void DisplayBombCount(void);
static void DisplayTime(void);
static void DisplayScreen(void);
static void DrawScreen(HDC hDC);
static void PlayTune(int iTune);
static int InitTunes(void);
static void EndTunes(void);
static void KillTune(void);
static BOOL FLoadBitmaps(void);
static void TrackMouse(int xNew, int yNew);
static void DoEnterName(void);
static void DoDisplayBest(void);

/* ------------------------------------------------------------------ C run time */
/* seg1:25D0 srand: the seed becomes the low word of the 32-bit state */
static void Srand(WORD seed) { holdrand = seed; }

/* seg1:25E2 rand: state * 214013 + 2531011 (32 bits), bits 16..30 of it */
static int Rand(void)
{
    holdrand = holdrand * 214013u + 2531011u;
    return (int)((holdrand >> 16) & 0x7FFF);
}

/* seg1:170A */
static int Rnd(int rndMax) { return Rand() % rndMax; }

/* ------------------------------------------------------------------ errors and strings */
/* seg1:171A: ids under 999 are strings, others are shown as "Error: id" */
static void ReportErr(int idErr)
{
    char szMsg[128], szMsgTitle[128];
    if (idErr < ID_ERR_MAX)
        LoadString(hInst, idErr, szMsg, 128);
    else {
        LoadString(hInst, ID_ERR_UNKNOWN, szMsgTitle, 128);
        wsprintf(szMsg, szMsgTitle, idErr);
    }
    LoadString(hInst, ID_ERR_TITLE, szMsgTitle, 128);
    MessageBox(NULL, szMsg, szMsgTitle, MB_ICONHAND);
}

/* seg1:1794 */
static void LoadSz(int id, char *sz)
{
    if (!LoadString(hInst, id, sz, 128)) ReportErr(1001);
}

/* ------------------------------------------------------------------ seg1:17B9: InitConst */
static void InitConst(void)
{
    HDC hDC;
    /* WINMINE_SEED (tests only): a fixed seed instead of the tick count, for repeatable boards */
    const char *seed = getenv("WINMINE_SEED");
    Srand(seed && *seed ? (WORD)strtoul(seed, NULL, 0) : LOWORD(GetCurrentTime()));
    LoadSz(ID_GAMENAME, szClass);
    LoadSz(ID_MSG_VERSION, szTitle);
    LoadSz(ID_MSG_SEC, szTime);
    LoadSz(ID_NAME_DEFAULT, szDefaultName);
    LoadSz(ID_HELPFILE, szHelpFile);
    fEGA = GetSystemMetrics(SM_CYSCREEN) < 351;
    hDC = GetDC(GetDesktopWindow());
    fColorCapable = GetDeviceCaps(hDC, NUMCOLORS) > 2;
    ReleaseDC(GetDesktopWindow(), hDC);
    dypCaption = GetSystemMetrics(SM_CYCAPTION) + 1;
    dypMenu = GetSystemMetrics(SM_CYMENU) + 1;
    dypBorder = GetSystemMetrics(SM_CYBORDER) + 1;
    dxpBorder = GetSystemMetrics(SM_CXBORDER) + 1;
}

/* ------------------------------------------------------------------ preferences */
/* seg1:2242: the INI value limited to valMin..valMax (16-bit, as KERNEL returns it) */
static int ReadInt(int iszPref, int valDefault, int valMin, int valMax)
{
    int val = (SHORT)GetPrivateProfileInt(szClass, rgszPref[iszPref], valDefault, szIniFile);
    return max(valMin, min(valMax, val));
}

/* seg1:22D3 */
static void ReadSz(int iszPref, char *szRet)
{
    GetPrivateProfileString(szClass, rgszPref[iszPref], szDefaultName, szRet, 32, szIniFile);
}

/* seg1:22FD */
static void ReadPreferences(void)
{
    yBoxMac = Height = ReadInt(iszPrefHeight, 8, 8, fEGA ? 16 : 25);
    xBoxMac = Width = ReadInt(iszPrefWidth, 8, 8, 30);
    wGameType = ReadInt(iszPrefGame, wGameBegin, wGameBegin, wGameExpert + 1);
    Mines = ReadInt(iszPrefMines, 10, 10, 999);
    xWindow = ReadInt(iszPrefxWindow, 80, 0, 1024);
    yWindow = ReadInt(iszPrefyWindow, 80, 0, 1024);
    fSound = ReadInt(iszPrefSound, 0, 0, fsoundOn);
    fMark = ReadInt(iszPrefMark, TRUE, 0, 1);
    fTick = ReadInt(iszPrefTick, FALSE, 0, 1);
    fMenu = ReadInt(iszPrefMenu, 0, 0, 2);
    rgTime[wGameBegin] = ReadInt(iszPrefBeginTime, 999, 0, 999);
    rgTime[wGameInter] = ReadInt(iszPrefInterTime, 999, 0, 999);
    rgTime[wGameExpert] = ReadInt(iszPrefExpertTime, 999, 0, 999);
    ReadSz(iszPrefBeginName, szBegin);
    ReadSz(iszPrefInterName, szInter);
    ReadSz(iszPrefExpertName, szExpert);
    fColor = fColorCapable;
    if (fColor) fColor = ReadInt(iszPrefColor, fColorCapable, 0, 1);
    if (fSound == fsoundOn) fSound = InitTunes();
}

/* seg1:2447 */
static void WriteInt(int iszPref, int val)
{
    char szVal[10];
    wsprintf(szVal, "%d", val);
    WritePrivateProfileString(szClass, rgszPref[iszPref], szVal, szIniFile);
}

/* seg1:2481 */
static void WriteSz(int iszPref, const char *sz)
{
    WritePrivateProfileString(szClass, rgszPref[iszPref], sz, szIniFile);
}

/* seg1:24A5: Menu and Tick are never written */
static void WritePreferences(void)
{
    WriteInt(iszPrefGame, wGameType);
    WriteInt(iszPrefHeight, Height);
    WriteInt(iszPrefWidth, Width);
    WriteInt(iszPrefMines, Mines);
    WriteInt(iszPrefMark, fMark);
    WriteInt(iszPrefColor, fColor);
    WriteInt(iszPrefxWindow, xWindow);
    WriteInt(iszPrefyWindow, yWindow);
    WriteInt(iszPrefBeginTime, rgTime[wGameBegin]);
    WriteInt(iszPrefInterTime, rgTime[wGameInter]);
    WriteInt(iszPrefExpertTime, rgTime[wGameExpert]);
    WriteSz(iszPrefBeginName, szBegin);
    WriteSz(iszPrefInterName, szInter);
    WriteSz(iszPrefExpertName, szExpert);
    if (fSound > 1) WriteInt(iszPrefSound, fSound);
}

/* ------------------------------------------------------------------ sound */
/* seg1:20FE: OpenSound and the accent of voice 1; 2 when there is no sound driver */
static int InitTunes(void)
{
    if (OpenSound() < 1) return fsoundOff;
    SetVoiceAccent(1, 120, 128, S_LEGATO, 0);
    return fsoundOn;
}

/* seg1:2127 */
static void KillTune(void)
{
    if (fSound == fsoundOn) StopSound();
}

/* seg1:213A */
static void EndTunes(void)
{
    if (fSound == fsoundOn) {
        KillTune();
        CloseSound();
    }
}

/* seg1:2150 */
static void PlayTune(int iTune)
{
    if (fSound != fsoundOn) return;
    if (iTune == TUNE_TICK && !fTick) return;
    switch (iTune) {
    case TUNE_TICK:
        SetVoiceNote(1, 64, 32, 1);
        break;
    case TUNE_WINGAME:
        SetVoiceNote(1, 24, 16, 1);
        SetVoiceNote(1, 26, 16, 1);
        SetVoiceNote(1, 28, 16, 1);
        SetVoiceNote(1, 29, 16, 1);
        SetVoiceNote(1, 31, 16, 1);
        SetVoiceNote(1, 33, 16, 1);
        SetVoiceNote(1, 35, 16, 1);
        SetVoiceNote(1, 36, 16, 1);
        break;
    case TUNE_LOSEGAME:
        SetVoiceNote(1, 36, 8, 1);
        SetVoiceNote(1, 24, 8, 1);
        SetVoiceNote(1, 36, 8, 1);
        SetVoiceNote(1, 24, 8, 1);
        SetVoiceNote(1, 36, 8, 1);
        SetVoiceNote(1, 24, 8, 1);
        break;
    }
    StartSound();
}

/* ------------------------------------------------------------------ menus and window size */
/* seg1:1874 */
static void CheckEm(UINT idm, BOOL fCheck)
{
    CheckMenuItem(hMenu, idm, fCheck ? MF_CHECKED : MF_UNCHECKED);
}

/* seg1:031C */
static void FixMenus(void)
{
    CheckEm(IDM_BEGIN, wGameType == wGameBegin);
    CheckEm(IDM_INTER, wGameType == wGameInter);
    CheckEm(IDM_EXPERT, wGameType == wGameExpert);
    CheckEm(IDM_CUSTOM, wGameType == wGameOther);
    if (fColorCapable)
        CheckEm(IDM_COLOR, fColor);
    else
        EnableMenuItem(hMenu, IDM_COLOR, MF_GRAYED | MF_DISABLED);
    CheckEm(IDM_MARK, fMark);
}

/* seg1:0C0A: the client size from the board; fAdjust 2 moves/resizes the window (also when it
 * would leave the screen), 4 repaints the board area */
static void AdjustWindow(int fAdjust)
{
    RECT rect;
    int t;
    dypAdjust = dypCaption;
    if (!(fMenu & 1)) dypAdjust += dypMenu;
    dxWindow = (xBoxMac << 4) + 24;
    dyWindow = (yBoxMac << 4) + 67;
    t = dxWindow - GetSystemMetrics(SM_CXSCREEN) + xWindow;
    if (t > 0) {
        fAdjust |= 2;
        xWindow -= t;
    }
    t = yWindow - GetSystemMetrics(SM_CYSCREEN) + dyWindow;
    if (t > 0) {
        fAdjust |= 2;
        yWindow -= t;
    }
    if (fInitMinimized) return;
    if (fAdjust & 2)
        MoveWindow(hwndMain, xWindow - dxpBorder, yWindow - dypAdjust, dxWindow + dxpBorder,
                   dypAdjust + dyWindow, TRUE);
    if (fAdjust & 4) {
        SetRect(&rect, 0, 0, dxWindow, dyWindow);
        InvalidateRect(hwndMain, &rect, TRUE);
    }
}

/* seg1:1892: menu bar on (0, 2) or off (1) */
static void SetMenuBar(int fActive)
{
    fMenu = fActive;
    FixMenus();
    SetMenu(hwndMain, (fMenu & 1) ? NULL : hMenu);
    AdjustWindow(2);
}

/* ------------------------------------------------------------------ bitmaps */
/* seg1:19C1: the colour bitmap, or the monochrome one after it */
static HANDLE HFindBitmap(int id)
{
    return FindResource(hInst, MAKEINTRESOURCE(id + !fColor), RT_BITMAP);
}

/* LoadResource + LockResource of a bitmap (seg1:1A0E..1A8D). libw16 hands out the module's image,
 * which the EGA colour patch must not change: the port works on a copy. */
static BYTE *LockDib(int id)
{
    HANDLE hRes = HFindBitmap(id);
    HGLOBAL h = hRes ? LoadResource(hInst, hRes) : NULL;
    DWORD cb = hRes ? SizeofResource(hInst, hRes) : 0;
    BYTE *p = h && cb ? malloc(cb) : NULL;
    if (p) memcpy(p, LockResource(h), cb);
    return p;
}

/* seg1:19E4: bytes of a w x h DIB in the current colour depth (rows padded to 32 bits) */
static int CbBitmap(int x, int y)
{
    x *= fColor ? 4 : 1;
    return ((x + 31) & ~31) / 8 * y;
}

/* an RGBQUAD of a DIB's colour table, written as the DWORD 3.1 stores */
static void SetDibColor(BYTE *lpDib, int i, DWORD rgb)
{
    BYTE *q = lpDib + 40 + 4 * i;
    q[0] = (BYTE)rgb; q[1] = (BYTE)(rgb >> 8); q[2] = (BYTE)(rgb >> 16); q[3] = (BYTE)(rgb >> 24);
}

/* seg1:1A06 */
static BOOL FLoadBitmaps(void)
{
    HDC hDC;
    int i, cbDibHeader, cb;
    BYTE *lpBlks = LockDib(ID_BMP_BLOCKS), *lpLed = LockDib(ID_BMP_LED), *lpButton = LockDib(ID_BMP_BUTTON);
    free(lpDibBlks);
    free(lpDibLed);
    free(lpDibButton);
    lpDibBlks = lpBlks;
    lpDibLed = lpLed;
    lpDibButton = lpButton;
    if (!lpDibBlks || !lpDibLed || !lpDibButton) return FALSE;
    if (!fColor)
        hGrayPen = GetStockObject(BLACK_PEN);
    else if (fEGA) {
        /* on EGA the light gray (8) and dark gray (7) of the blocks and the faces darken to match
         * the dark gray window background */
        SetDibColor(lpDibBlks, 8, 0x00808080);
        SetDibColor(lpDibButton, 8, 0x00808080);
        SetDibColor(lpDibBlks, 7, 0x00404040);
        SetDibColor(lpDibButton, 7, 0x00404040);
        hGrayPen = CreatePen(PS_SOLID, 1, RGB(64, 64, 64));
    } else
        hGrayPen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    cbDibHeader = 40 + (fColor ? 16 : 2) * 4;
    CbBitmap(16, 16); /* computed and not used in 3.1 too */
    hDC = GetDC(hwndMain);
    hbmPrev = hbmBlocks;
    hbmBlocks = CreateDIBitmap(hDC, (BITMAPINFOHEADER *)lpDibBlks, CBM_INIT, lpDibBlks + cbDibHeader,
                               (BITMAPINFO *)lpDibBlks, DIB_RGB_COLORS);
    ReleaseDC(hwndMain, hDC);
    cb = CbBitmap(13, 23);
    for (i = 0; i < 12; i++) rgDibLedOff[i] = cb * i + cbDibHeader;
    cb = CbBitmap(24, 24);
    for (i = 0; i < 5; i++) rgDibButOff[i] = cb * i + cbDibHeader;
    if (!hbmOld)
        hbmOld = SelectObject(hdcBlk, hbmBlocks);
    else
        SelectObject(hdcBlk, hbmBlocks);
    if (hbmPrev) DeleteObject(hbmPrev);
    return TRUE;
}

/* seg1:1BE8 */
static void FreeBitmaps(void)
{
    DeleteObject(hbmBlocks);
    if (hGrayPen) DeleteObject(hGrayPen);
    free(lpDibBlks);
    free(lpDibLed);
    free(lpDibButton);
    lpDibBlks = lpDibLed = lpDibButton = NULL;
}

/* seg1:1C23 */
static void CleanUp(void)
{
    SelectObject(hdcBlk, hbmOld);
    DeleteDC(hdcBlk);
    FreeBitmaps();
    EndTunes();
}

/* ------------------------------------------------------------------ drawing */
/* seg1:1C46 */
static void DisplayBlk(int x, int y)
{
    HDC hDC = GetDC(hwndMain);
    BitBlt(hDC, (x << 4) - 4, (y << 4) + 39, 16, 16, hdcBlk, 0, (iBlkBlankUp - iBlk(x, y)) << 4, SRCCOPY);
    ReleaseDC(hwndMain, hDC);
}

/* seg1:1CAC */
static void DrawGrid(HDC hDC)
{
    int x, y, dx, dy = 55;
    for (y = 1; y <= yBoxMac; y++, dy += 16) {
        dx = 12;
        for (x = 1; x <= xBoxMac; x++, dx += 16)
            BitBlt(hDC, dx, dy, 16, 16, hdcBlk, 0, (iBlkBlankUp - iBlk(x, y)) << 4, SRCCOPY);
    }
}

/* seg1:1D18 */
static void DisplayGrid(void)
{
    HDC hDC = GetDC(hwndMain);
    DrawGrid(hDC);
    ReleaseDC(hwndMain, hDC);
}

/* seg1:1D3F: one 13 x 23 digit, a band of the LED bitmap counted from its bottom */
static void DrawLed(HDC hDC, int x, int iLed)
{
    SetDIBitsToDevice(hDC, x, 16, 13, 23, 0, 0, 0, 23, lpDibLed + rgDibLedOff[iLed], (BITMAPINFO *)lpDibLed,
                      DIB_RGB_COLORS);
}

/* seg1:1D79 */
static void DrawBombCount(HDC hDC)
{
    int iLed, cBombs;
    if (cBombLeft < 0) {
        iLed = iLedNegative;
        cBombs = (-cBombLeft) % 100;
    } else {
        iLed = cBombLeft / 100;
        cBombs = cBombLeft % 100;
    }
    DrawLed(hDC, 17, iLed);
    DrawLed(hDC, 30, cBombs / 10);
    DrawLed(hDC, 43, cBombs % 10);
}

/* seg1:1DE3 */
static void DisplayBombCount(void)
{
    HDC hDC = GetDC(hwndMain);
    DrawBombCount(hDC);
    ReleaseDC(hwndMain, hDC);
}

/* seg1:1E0A */
static void DrawTime(HDC hDC)
{
    int iLed = cSec;
    DrawLed(hDC, dxWindow - dxpBorder - 56, iLed / 100);
    iLed %= 100;
    DrawLed(hDC, dxWindow - dxpBorder - 43, iLed / 10);
    DrawLed(hDC, dxWindow - dxpBorder - 30, iLed % 10);
}

/* seg1:1E79 */
static void DisplayTime(void)
{
    HDC hDC = GetDC(hwndMain);
    DrawTime(hDC);
    ReleaseDC(hwndMain, hDC);
}

/* seg1:1EA0: a 24 x 24 face, a band of the button bitmap counted from its bottom */
static void DrawButton(HDC hDC, int iButton)
{
    SetDIBitsToDevice(hDC, (dxWindow - 24) >> 1, 16, 24, 24, 0, 0, 0, 24, lpDibButton + rgDibButOff[iButton],
                      (BITMAPINFO *)lpDibButton, DIB_RGB_COLORS);
}

/* seg1:1EE0 */
static void DisplayButton(int iButton)
{
    HDC hDC = GetDC(hwndMain);
    DrawButton(hDC, iButton);
    ReleaseDC(hwndMain, hDC);
}

/* seg1:1F0B: odd modes draw white (R2_WHITE with the pen in the DC), even ones the gray pen */
static void SetThePen(HDC hDC, int fNormal)
{
    if (fNormal & 1)
        SetROP2(hDC, R2_WHITE);
    else {
        SetROP2(hDC, R2_COPYPEN);
        SelectObject(hDC, hGrayPen);
    }
}

/* seg1:1F3A: a bevel `width` lines thick: top and left in the first pen, then bottom and right in
 * the other one (modes 0 and 1) or the same one (modes 2 and 3). The corners at (x1, y2) and
 * (x2, y1) are left out. */
static void DrawBorder(HDC hDC, int x1, int y1, int x2, int y2, int width, int fNormal)
{
    int i = 0;
    SetThePen(hDC, fNormal);
    while (i++ < width) {
        MoveTo(hDC, x1, --y2);
        LineTo(hDC, x1++, y1);
        LineTo(hDC, x2--, y1++);
    }
    if (fNormal < 2) SetThePen(hDC, fNormal ^ 1);
    while (--i) {
        MoveTo(hDC, x1--, ++y2);
        LineTo(hDC, ++x2, y2);
        LineTo(hDC, x2, --y1);
    }
}

/* seg1:1FEC */
static void DrawBackground(HDC hDC)
{
    int dx, dy;
    dx = dxWindow - 1;
    dy = dyWindow - 1;
    DrawBorder(hDC, 0, 0, dx, dy, 3, 1);
    dx -= 9;
    dy -= 9;
    DrawBorder(hDC, 9, 52, dx, dy, 3, 0);
    DrawBorder(hDC, 9, 9, dx, 45, 2, 0);
    DrawBorder(hDC, 16, 15, 56, 39, 1, 0);
    dx = dxWindow - dxpBorder - 57;
    DrawBorder(hDC, dx, 15, dx + 40, 39, 1, 0);
    dx = ((dxWindow - 24) >> 1) - 1;
    DrawBorder(hDC, dx, 15, dx + 25, 40, 1, 2);
}

/* seg1:20A8 */
static void DrawScreen(HDC hDC)
{
    DrawBackground(hDC);
    DrawBombCount(hDC);
    DrawButton(hDC, iButtonCur);
    DrawTime(hDC);
    DrawGrid(hDC);
}

/* seg1:20D7 */
static void DisplayScreen(void)
{
    HDC hDC = GetDC(hwndMain);
    DrawScreen(hDC);
    ReleaseDC(hwndMain, hDC);
}

/* ------------------------------------------------------------------ the board */
/* seg1:0D08: all covered, and a border of iBlkMax around the board */
static void ClearField(void)
{
    int i;
    for (i = cBlkMax; i-- != 0;) rgBlk[i] = iBlkBlankUp;
    for (i = xBoxMac + 2; i-- != 0;) {
        rgBlk[i] = iBlkMax;
        rgBlk[(yBoxMac << 5) + cBlkRow + i] = iBlkMax;
    }
    for (i = yBoxMac + 2; i-- != 0;) {
        rgBlk[i << 5] = iBlkMax;
        rgBlk[(i << 5) + xBoxMac + 1] = iBlkMax;
    }
}

/* seg1:0CE0 */
static void ChangeBlk(int x, int y, int iBlkNew)
{
    BLK(x, y) = (BYTE)((BLK(x, y) & MaskFlags) | iBlkNew);
    DisplayBlk(x, y);
}

/* seg1:0D6B: mines in the 3 x 3 square */
static int CountBombs(int xCenter, int yCenter)
{
    int x, y, cBombs = 0;
    for (y = yCenter - 1; y <= yCenter + 1; y++)
        for (x = xCenter - 1; x <= xCenter + 1; x++)
            if (IsBomb(x, y)) cBombs++;
    return cBombs;
}

/* seg1:109A: flags in the 3 x 3 square */
static int CountMarks(int xCenter, int yCenter)
{
    int x, y, cMarks = 0;
    for (y = yCenter - 1; y <= yCenter + 1; y++)
        for (x = xCenter - 1; x <= xCenter + 1; x++)
            if (iBlk(x, y) == iBlkBombUp) cMarks++;
    return cMarks;
}

/* seg1:0DB0: the end of a game: the mines (iBlk) and the flags that were wrong */
static void ShowBombs(int iBlkShow)
{
    int x, y;
    for (y = 1; y <= yBoxMac; y++)
        for (x = 1; x <= xBoxMac; x++) {
            BYTE b = BLK(x, y);
            if (b & MaskVisit) continue;
            if (b & MaskBomb) {
                if ((b & MaskData) != iBlkBombUp) BLK(x, y) = (BYTE)((b & MaskFlags) | iBlkShow);
            } else if ((b & MaskData) == iBlkBombUp)
                BLK(x, y) = (BYTE)((b & 0xEB) | iBlkWrong);
        }
    DisplayGrid();
}

/* seg1:1242: a covered box or "?" pressed down */
static void PushBoxDown(int x, int y)
{
    int iBlkT = iBlk(x, y);
    if (iBlkT == iBlkGuessUp) iBlkT = iBlkGuessDown;
    else if (iBlkT == iBlkBlankUp) iBlkT = iBlkBlank;
    BLK(x, y) = (BYTE)((BLK(x, y) & MaskFlags) | iBlkT);
}

/* seg1:128D: and back up */
static void PopBoxUp(int x, int y)
{
    int iBlkT = iBlk(x, y);
    if (iBlkT == iBlkGuessDown) iBlkT = iBlkGuessUp;
    else if (iBlkT == iBlkBlank) iBlkT = iBlkBlankUp;
    BLK(x, y) = (BYTE)((BLK(x, y) & MaskFlags) | iBlkT);
}

/* seg1:16F9 */
static void UpdateBombCount(int cBombAdd)
{
    cBombLeft += cBombAdd;
    DisplayBombCount();
}

/* seg1:0EBC: open one box; empty ones go on the queue (100 entries, round) */
static void StepXY(int x, int y)
{
    int cBombs;
    int iBlkT = (y << 5) + x;
    BYTE blk = rgBlk[iBlkT];
    if (blk & MaskVisit) return;
    blk &= MaskData;
    if (blk == iBlkMax || blk == iBlkBombUp) return;
    cBoxVisit++;
    cBombs = CountBombs(x, y);
    rgBlk[iBlkT] = (BYTE)(cBombs | MaskVisit);
    DisplayBlk(x, y);
    if (cBombs != 0) return;
    rgStepX[iStepMac] = x;
    rgStepY[iStepMac] = y;
    if (++iStepMac == 100) iStepMac = 0;
}

/* seg1:0F39: open a box and, if it is empty, the area around it */
static void StepBox(int x, int y)
{
    int iStepCur = 1;
    iStepMac = 1;
    StepXY(x, y);
    if (iStepMac == 1) return;
    while (iStepCur != iStepMac) {
        x = rgStepX[iStepCur];
        y = rgStepY[iStepCur] - 1;
        StepXY(x - 1, y);
        StepXY(x, y);
        StepXY(x + 1, y);
        y++;
        StepXY(x - 1, y);
        StepXY(x + 1, y);
        y++;
        StepXY(x - 1, y);
        StepXY(x, y);
        StepXY(x + 1, y);
        if (++iStepCur == 100) iStepCur = 0;
    }
}

/* seg1:0E18 */
static void GameOver(BOOL fWinLose)
{
    fTimer = FALSE;
    iButtonCur = fWinLose ? iButtonWin : iButtonLose;
    DisplayButton(iButtonCur);
    PlayTune(fWinLose ? TUNE_WINGAME : TUNE_LOSEGAME);
    ShowBombs(fWinLose ? iBlkBombUp : iBlkBombDown);
    if (fWinLose && cBombLeft != 0) UpdateBombCount(-cBombLeft);
    fStatus = fDemo;
    if (fWinLose && wGameType != wGameOther && cSec < rgTime[wGameType]) {
        rgTime[wGameType] = cSec;
        DoEnterName();
        DoDisplayBest();
    }
}

/* seg1:0FF7: a click on one box. The first click of a game never hits a mine: the mine moves to
 * the first free box scanning rows 1 .. height-1 and columns 1 .. width-1 (never the last row or
 * column, as in 3.1). */
static void StepSquare(int x, int y)
{
    int xT, yT;
    if (IsBomb(x, y)) {
        if (cBoxVisit == 0) {
            for (yT = 1; yT < yBoxMac; yT++)
                for (xT = 1; xT < xBoxMac; xT++)
                    if (!IsBomb(xT, yT)) {
                        BLK(x, y) = iBlkBlankUp;
                        SetBomb(xT, yT);
                        StepBox(x, y);
                        return;
                    }
        } else {
            ChangeBlk(x, y, MaskVisit | iBlkExplode);
            GameOver(FALSE);
        }
    } else {
        StepBox(x, y);
        if (cBoxVisit == cBoxVisitMac) GameOver(TRUE);
    }
}

/* seg1:10E2: both buttons on an open number with as many flags around it: open the others */
static void StepBlock(int xCenter, int yCenter)
{
    int x, y;
    BOOL fGameOver = FALSE;
    if (!IsVisit(xCenter, yCenter) || CountMarks(xCenter, yCenter) != iBlk(xCenter, yCenter)) {
        TrackMouse(-2, -2);
        return;
    }
    for (y = yCenter - 1; y <= yCenter + 1; y++)
        for (x = xCenter - 1; x <= xCenter + 1; x++) {
            BYTE b = BLK(x, y);
            if ((b & MaskData) != iBlkBombUp && (b & MaskBomb)) {
                fGameOver = TRUE;
                ChangeBlk(x, y, MaskVisit | iBlkExplode);
            } else
                StepBox(x, y);
        }
    if (fGameOver)
        GameOver(FALSE);
    else if (cBoxVisit == cBoxVisitMac)
        GameOver(TRUE);
}

/* seg1:1192 */
static void StartGame(void)
{
    int x, y, fAdjust;
    fTimer = FALSE;
    fAdjust = (xBoxMac != Width || yBoxMac != Height) ? 6 : 4;
    xBoxMac = Width;
    yBoxMac = Height;
    ClearField();
    iButtonCur = iButtonHappy;
    cBombStart = Mines;
    do {
        do {
            x = Rnd(xBoxMac) + 1;
            y = Rnd(yBoxMac) + 1;
        } while (IsBomb(x, y));
        SetBomb(x, y);
    } while (--cBombStart);
    cBombStart = cBombLeft = Mines;
    cBoxVisitMac = xBoxMac * yBoxMac - cBombLeft;
    fStatus = fPlay;
    cSec = cBoxVisit = 0;
    UpdateBombCount(0);
    AdjustWindow(fAdjust);
}

/* seg1:1578: right button: flag, "?" (with Marks) and covered again */
static void MakeGuess(int x, int y)
{
    int iBlkNew;
    if (!fInRange(x, y) || IsVisit(x, y)) return;
    if (iBlk(x, y) == iBlkBombUp) {
        iBlkNew = fMark ? iBlkGuessUp : iBlkBlankUp;
        UpdateBombCount(1);
    } else if (iBlk(x, y) == iBlkGuessUp)
        iBlkNew = iBlkBlankUp;
    else {
        iBlkNew = iBlkBombUp;
        UpdateBombCount(-1);
    }
    ChangeBlk(x, y, iBlkNew);
    if (iBlk(x, y) == iBlkBombUp && cBoxVisit == cBoxVisitMac) GameOver(TRUE);
}

/* seg1:12D7: the pressed box (or 3 x 3 square with both buttons) follows the mouse */
static void TrackMouse(int xNew, int yNew)
{
    int xOld, yOld, x, y;
    int xOldMin, xOldMax, yOldMin, yOldMax, xCurMin, xCurMax, yCurMin, yCurMax;
    BOOL fValidNew, fValidOld;
    if (xNew == xCur && yNew == yCur) return;
    xOld = xCur;
    yOld = yCur;
    xCur = xNew;
    yCur = yNew;
    if (fBlock) {
        fValidNew = fInRange(xNew, yNew);
        fValidOld = fInRange(xOld, yOld);
        yOldMin = max(yOld - 1, 1);
        yOldMax = min(yOld + 1, yBoxMac);
        yCurMin = max(yCur - 1, 1);
        yCurMax = min(yCur + 1, yBoxMac);
        xOldMin = max(xOld - 1, 1);
        xOldMax = min(xOld + 1, xBoxMac);
        xCurMin = max(xCur - 1, 1);
        xCurMax = min(xCur + 1, xBoxMac);
        if (fValidOld)
            for (y = yOldMin; y <= yOldMax; y++)
                for (x = xOldMin; x <= xOldMax; x++)
                    if (!IsVisit(x, y)) PopBoxUp(x, y);
        if (fValidNew)
            for (y = yCurMin; y <= yCurMax; y++)
                for (x = xCurMin; x <= xCurMax; x++)
                    if (!IsVisit(x, y)) PushBoxDown(x, y);
        if (fValidOld)
            for (y = yOldMin; y <= yOldMax; y++)
                for (x = xOldMin; x <= xOldMax; x++) DisplayBlk(x, y);
        if (fValidNew)
            for (y = yCurMin; y <= yCurMax; y++)
                for (x = xCurMin; x <= xCurMax; x++) DisplayBlk(x, y);
    } else {
        if (fInRange(xOld, yOld) && !IsVisit(xOld, yOld)) {
            PopBoxUp(xOld, yOld);
            DisplayBlk(xOld, yOld);
        }
        if (fInRange(xNew, yNew) && !IsVisit(xNew, yNew) && iBlk(xNew, yNew) != iBlkBombUp) {
            PushBoxDown(xCur, yCur);
            DisplayBlk(xCur, yCur);
        }
    }
}

/* seg1:1626: the button is released over the board. The first click of a game starts the clock
 * at 1. (The -2 reset below cannot happen: the only caller checks fPlay first.) */
static void DoButton1Up(void)
{
    if (fInRange(xCur, yCur)) {
        if (cBoxVisit == 0 && cSec == 0) {
            PlayTune(TUNE_TICK);
            cSec++;
            DisplayTime();
            fTimer = TRUE;
        }
        if (!(fStatus & fPlay)) xCur = yCur = -2;
        if (fBlock)
            StepBlock(xCur, yCur);
        else if (!IsVisit(xCur, yCur) && iBlk(xCur, yCur) != iBlkBombUp)
            StepSquare(xCur, yCur);
    }
    DisplayButton(iButtonCur);
}

/* seg1:16C5 */
static void PauseGame(void)
{
    KillTune();
    if (fStatus & fPlay) fTimer = FALSE;
    fStatus |= fPause;
}

/* seg1:16E1: a game in play restarts the clock (even before its first click) */
static void ResumeGame(void)
{
    if (fStatus & fPlay) fTimer = TRUE;
    fStatus &= ~fPause;
}

/* seg1:0E99 */
static void DoTimer(void)
{
    if (fTimer && cSec < 999) {
        cSec++;
        DisplayTime();
        PlayTune(TUNE_TICK);
    }
}

/* ------------------------------------------------------------------ the face button */
/* seg1:0212: a press on the face: it stays down while the mouse is over it; releasing it there
 * starts a new game. Returns FALSE when the press is elsewhere. */
static BOOL FLocalButton(LPARAM lParam)
{
    BOOL fDown = TRUE;
    RECT rcCapt;
    MSG msg;
    POINT pt;
    pt.x = (SHORT)LOWORD(lParam);
    pt.y = (SHORT)HIWORD(lParam);
    rcCapt.left = (dxWindow - 24) >> 1;
    rcCapt.right = rcCapt.left + 24;
    rcCapt.top = 16;
    rcCapt.bottom = 40;
    if (!PtInRect(&rcCapt, pt)) return FALSE;
    SetCapture(hwndMain);
    DisplayButton(iButtonDown);
    ClientToScreen(hwndMain, (LPPOINT)&rcCapt.left);
    ClientToScreen(hwndMain, (LPPOINT)&rcCapt.right);
    for (;;) {
        while (!PeekMessage(&msg, hwndMain, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE))
            ;
        switch (msg.message) {
        case WM_MOUSEMOVE:
            if (PtInRect(&rcCapt, msg.pt)) {
                if (!fDown) {
                    fDown = TRUE;
                    DisplayButton(iButtonDown);
                }
            } else if (fDown) {
                fDown = FALSE;
                DisplayButton(iButtonCur);
            }
            break;
        case WM_LBUTTONUP:
            if (fDown && PtInRect(&rcCapt, msg.pt)) {
                iButtonCur = iButtonHappy;
                DisplayButton(iButtonHappy);
                StartGame();
            }
            ReleaseCapture();
            return TRUE;
        }
    }
}

/* ------------------------------------------------------------------ dialogs */
/* seg1:1947: the edit's number limited to numLo..numHi (16-bit, as 3.1 compares it) */
static int GetDlgInt(HWND hDlg, int dlgID, int numLo, int numHi)
{
    BOOL fFlag;
    int num = (SHORT)GetDlgItemInt(hDlg, dlgID, &fFlag, FALSE);
    if (num < numLo)
        num = numLo;
    else if (num > numHi)
        num = numHi;
    return num;
}

/* seg1:0955: Custom Field. Cancel closes it the same way; the game becomes "Custom" either way. */
static BOOL PrefDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        SetDlgItemInt(hDlg, ID_EDIT_HEIGHT, Height, FALSE);
        SetDlgItemInt(hDlg, ID_EDIT_WIDTH, Width, FALSE);
        SetDlgItemInt(hDlg, ID_EDIT_MINES, Mines, FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
        case ID_BTN_OK_OLD:
            Height = GetDlgInt(hDlg, ID_EDIT_HEIGHT, 8, fEGA ? 16 : 24);
            Width = GetDlgInt(hDlg, ID_EDIT_WIDTH, 8, 30);
            Mines = GetDlgInt(hDlg, ID_EDIT_MINES, 10, min((Height - 1) * (Width - 1), 999));
            /* fall through */
        case IDCANCEL:
        case ID_BTN_CANCEL_OLD:
            EndDialog(hDlg, TRUE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* seg1:0A2D */
static void SetDText(HWND hDlg, int id, int time, LPCSTR lpszName)
{
    char szTimeT[32];
    wsprintf(szTimeT, szTime, time);
    SetDlgItemText(hDlg, id, szTimeT);
    SetDlgItemText(hDlg, id + 1, lpszName);
}

/* seg1:0A72: Best Times */
static BOOL BestDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
    LReset:
        SetDText(hDlg, ID_TIME_BEGIN, rgTime[wGameBegin], szBegin);
        SetDText(hDlg, ID_TIME_INTER, rgTime[wGameInter], szInter);
        SetDText(hDlg, ID_TIME_EXPERT, rgTime[wGameExpert], szExpert);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case ID_BTN_RESET:
            rgTime[wGameBegin] = rgTime[wGameInter] = rgTime[wGameExpert] = 999;
            lstrcpy(szBegin, szDefaultName);
            lstrcpy(szInter, szDefaultName);
            lstrcpy(szExpert, szDefaultName);
            fUpdateIni = TRUE;
            goto LReset;
        case IDOK:
        case IDCANCEL:
        case ID_BTN_OK_OLD:
        case ID_BTN_CANCEL_OLD:
            EndDialog(hDlg, TRUE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* seg1:0B47: the name of a new record holder (Cancel keeps the typed name too) */
static BOOL EnterDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    char sz[128];
    char *szName = wGameType == wGameBegin ? szBegin : wGameType == wGameInter ? szInter : szExpert;
    switch (message) {
    case WM_INITDIALOG:
        LoadSz(wGameType + ID_MSG_BEGIN, sz);
        SetDlgItemText(hDlg, ID_TEXT_BEST, sz);
        SetDlgItemText(hDlg, ID_EDIT_NAME, szName);
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDOK:
        case IDCANCEL:
        case ID_BTN_OK_OLD:
        case ID_BTN_CANCEL_OLD:
            GetDlgItemText(hDlg, ID_EDIT_NAME, szName, 32);
            EndDialog(hDlg, TRUE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* seg1:03A9 */
static void DoPref(void)
{
    DialogBox(hInst, MAKEINTRESOURCE(ID_DLG_PREF), hwndMain, PrefDlgProc);
    wGameType = wGameOther;
    FixMenus();
    fUpdateIni = TRUE;
    StartGame();
}

/* seg1:03F8 */
static void DoEnterName(void)
{
    DialogBox(hInst, MAKEINTRESOURCE(ID_DLG_ENTER), hwndMain, EnterDlgProc);
    fUpdateIni = TRUE;
}

/* seg1:043C */
static void DoDisplayBest(void)
{
    DialogBox(hInst, MAKEINTRESOURCE(ID_DLG_BEST), hwndMain, BestDlgProc);
}

/* seg1:1907: HELP_QUIT, HELP_INDEX and HELP_PARTIALKEY name WINMINE.HLP; HELP_HELPONHELP none */
static void DoHelp(UINT wCommand, DWORD lParam)
{
    switch (wCommand) {
    case HELP_QUIT:
    case HELP_INDEX:
    case HELP_PARTIALKEY:
        WinHelp(hwndMain, szHelpFile, wCommand, lParam);
        break;
    case HELP_HELPONHELP:
        WinHelp(hwndMain, NULL, wCommand, lParam);
        break;
    }
}

/* seg1:18C3 */
static void DoAbout(void)
{
    char szVersion[128], szCredit[128];
    LoadSz(ID_MSG_ABOUT, szVersion);
    LoadSz(ID_MSG_CREDIT, szCredit);
    ShellAbout(hwndMain, szVersion, szCredit, LoadIcon(hInst, MAKEINTRESOURCE(ID_ICON_MAIN)));
}

/* ------------------------------------------------------------------ seg1:047E: MainWndProc */
/* the box under a client point: WINMINE shifts the 16-bit coordinates without sign, so points
 * left of or above the board give large numbers (outside it), not negative ones */
static int BoxX(LPARAM lParam) { return (WORD)(LOWORD(lParam) + 4) >> 4; }
static int BoxY(LPARAM lParam) { return (WORD)(HIWORD(lParam) - 39) >> 4; }

static LRESULT MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_DESTROY: /* 06EB */
        KillTimer(hwndMain, ID_TIMER);
        DoHelp(HELP_QUIT, 0L);
        PostQuitMessage(0);
        /* fall through */
    case WM_ENDSESSION: /* 0709 */
        if (fUpdateIni) WritePreferences();
        break;

    case WM_MOVE: /* 0505: the position is kept unless iconic */
        if (!(fStatus & fIcon)) {
            xWindow = (SHORT)(LOWORD(lParam) + 1);
            yWindow = (SHORT)(HIWORD(lParam) + 1);
        }
        break;

    case WM_SYSCOMMAND: /* 0520 */
        switch (wParam & 0xFFF0) {
        case SC_MINIMIZE:
            PauseGame();
            fStatus |= fPause | fIcon;
            break;
        case SC_RESTORE:
            fStatus &= ~(fPause | fIcon);
            ResumeGame();
            break;
        }
        break;

    case WM_COMMAND: /* 0549 */
        switch (wParam) {
        case IDM_NEW:
            StartGame();
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_BEGIN:
        case IDM_INTER:
        case IDM_EXPERT:
            wGameType = (int)wParam - IDM_BEGIN;
            Mines = rgLevelData[wGameType][0];
            Height = rgLevelData[wGameType][1];
            Width = rgLevelData[wGameType][2];
            StartGame();
            goto LUpdateMenu;
        case IDM_CUSTOM:
            DoPref();
            break;
        case IDM_COLOR:
            fColor = !fColor;
            if (!FLoadBitmaps()) {
                ReportErr(ID_ERR_MEM);
                DestroyWindow(hwnd);
                break;
            }
            DisplayScreen();
            goto LUpdateMenu;
        case IDM_MARK:
            fMark = !fMark;
        LUpdateMenu:
            fUpdateIni = TRUE;
            FixMenus();
            break;
        case IDM_BEST:
            DoDisplayBest();
            break;
        case IDM_HELP:
            DoHelp(HELP_INDEX, 0L);
            break;
        case IDM_HOW2PLAY:
            /* 3.1 passes an empty key string; libw16's WinHelp takes the key as a number */
            DoHelp(HELP_PARTIALKEY, 0L);
            break;
        case IDM_HELP_HELP:
            DoHelp(HELP_HELPONHELP, 0L);
            break;
        case IDM_ABOUT:
            DoAbout();
            return 0;
        }
        break;

    case WM_KEYDOWN: /* 063A */
        switch (wParam) {
        case VK_F4: /* sound on/off (only with WINMINE.INI Sound=2 or 3) */
            if (fSound > 1) {
                if (fSound == fsoundOn) {
                    EndTunes();
                    fSound = fsoundOff;
                } else
                    fSound = InitTunes();
            }
            break;
        case VK_F5: /* menu bar off (only with WINMINE.INI Menu=1 or 2) */
            if (fMenu) SetMenuBar(1);
            break;
        case VK_F6: /* menu bar on */
            if (fMenu) SetMenuBar(2);
            break;
        case VK_SHIFT:
            if (iXyzzy >= 5) iXyzzy ^= 0x14;
            break;
        case VK_ESCAPE: /* the boss key */
            fStatus |= fEsc;
            PostMessage(hwndMain, WM_SYSCOMMAND, SC_MINIMIZE, 0L);
            break;
        default:
            if (iXyzzy < 5) {
                if (szXyzzy[iXyzzy] == (char)wParam) iXyzzy++;
                else iXyzzy = 0;
            }
            break;
        }
        break;

    case WM_RBUTTONDOWN: /* 0878 */
        if (fIgnoreClick) {
            fIgnoreClick = FALSE;
            return 0;
        }
        if (!(fStatus & fPlay)) break;
        if (fButton1Down) {
            TrackMouse(-3, -3);
            fBlock = TRUE;
            PostMessage(hwndMain, WM_MOUSEMOVE, wParam, lParam);
            return 0;
        }
        if (wParam & MK_LBUTTON) goto LBeginTrack;
        if (!fInMenu) MakeGuess(BoxX(lParam), BoxY(lParam));
        return 0;

    case WM_MBUTTONDOWN: /* 0719 */
        if (fIgnoreClick) {
            fIgnoreClick = FALSE;
            return 0;
        }
        if (!(fStatus & fPlay)) break;
        fBlock = TRUE;
        goto LBeginTrack;

    case WM_LBUTTONDOWN: /* 073B */
        if (fIgnoreClick) {
            fIgnoreClick = FALSE;
            return 0;
        }
        if (FLocalButton(lParam)) return 0;
        if (!(fStatus & fPlay)) break;
        fBlock = (wParam & (MK_SHIFT | MK_RBUTTON)) != 0;
    LBeginTrack: /* 076D */
        SetCapture(hwnd);
        xCur = yCur = -1;
        fButton1Down = TRUE;
        DisplayButton(iButtonCaution);
        /* fall through */
    case WM_MOUSEMOVE: /* 0789 */
        if (fButton1Down) {
            if (fStatus & fPlay) {
                TrackMouse(BoxX(lParam), BoxY(lParam));
                break;
            }
            goto LButtonUp;
        }
        /* XYZZY, then Shift (or Ctrl held): the screen's top left pixel shows black over a mine */
        if (iXyzzy == 0) break;
        if ((iXyzzy == 5 && (wParam & MK_CONTROL)) || iXyzzy > 5) {
            xCur = BoxX(lParam);
            yCur = BoxY(lParam);
            if (fInRange(xCur, yCur)) {
                HDC hDC = GetDC(GetDesktopWindow());
                SetPixel(hDC, 0, 0, IsBomb(xCur, yCur) ? 0x00000000L : 0x00FFFFFFL);
                ReleaseDC(GetDesktopWindow(), hDC);
            }
        }
        break;

    case WM_LBUTTONUP: /* 084F */
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (!fButton1Down) break;
    LButtonUp: /* 0859 */
        fButton1Down = FALSE;
        ReleaseCapture();
        if (fStatus & fPlay)
            DoButton1Up();
        else
            TrackMouse(-2, -2);
        break;

    case WM_ACTIVATE: /* 08E9 */
        if (LOWORD(wParam) == WA_CLICKACTIVE) fIgnoreClick = TRUE;
        break;

    case WM_TIMER: /* 08F7 */
        DoTimer();
        return 0;

    case WM_ENTERMENULOOP: /* 08FD */
        fInMenu = TRUE;
        break;

    case WM_EXITMENULOOP: /* 0905 */
        fInMenu = FALSE;
        break;

    case WM_PAINT: { /* 090D */
        PAINTSTRUCT ps;
        HDC hDC = BeginPaint(hwnd, &ps);
        DrawScreen(hDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------ seg1:197E: FInitLocal */
static BOOL FInitLocal(void)
{
    HDC hDC = GetDC(hwndMain);
    hdcBlk = CreateCompatibleDC(hDC);
    ReleaseDC(hwndMain, hDC);
    if (!hdcBlk) return FALSE;
    if (!FLoadBitmaps()) return FALSE;
    ClearField();
    return TRUE;
}

/* ------------------------------------------------------------------ seg1:0010: WinMain */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    MSG msg;
    WNDCLASS wc;
    HACCEL hAccel;
    (void)lpCmdLine;
    hInst = hInstance;
    InitConst();
    fInitMinimized = nCmdShow == SW_SHOWMINNOACTIVE || nCmdShow == SW_SHOWMINIMIZED;
    if (hPrevInstance) {
        /* never in arch311 (one process per program): bring the running Minesweeper up */
        HWND hwnd = GetLastActivePopup(FindWindow(szClass, NULL));
        BringWindowToTop(hwnd);
        if (!fInitMinimized && IsIconic(hwnd)) SendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0L);
        return 0;
    }
    memset(&wc, 0, sizeof wc);
    wc.style = 0;
    wc.lpfnWndProc = MainWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(ID_ICON_MAIN));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = fEGA ? CreateSolidBrush(RGB(128, 128, 128)) : GetStockObject(LTGRAY_BRUSH);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = szClass;
    if (!RegisterClass(&wc)) return 0;
    hMenu = LoadMenu(hInst, MAKEINTRESOURCE(ID_MENU));
    hAccel = LoadAccelerators(hInst, MAKEINTRESOURCE(ID_MENU_ACCEL));
    ReadPreferences();
    AdjustWindow(1);
    hwndMain = CreateWindow(szClass, szTitle, WS_MINIMIZE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            xWindow - dxpBorder, yWindow - dypAdjust, dxWindow + dxpBorder, dypAdjust + dyWindow,
                            NULL, NULL, hInst, NULL);
    if (!hwndMain) {
        ReportErr(1000);
        return 0;
    }
    if (!SetTimer(hwndMain, ID_TIMER, 1000, NULL)) {
        ReportErr(ID_ERR_TIMER);
        return 0;
    }
    if (!FInitLocal()) {
        ReportErr(ID_ERR_MEM);
        return 0;
    }
    SetMenuBar(fMenu);
    StartGame();
    if (nCmdShow == SW_SHOWMAXIMIZED) nCmdShow = SW_SHOWNORMAL;
    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);
    fInitMinimized = FALSE;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateAccelerator(hwndMain, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    CleanUp();
    return (int)msg.wParam;
}
