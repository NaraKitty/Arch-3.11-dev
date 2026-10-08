/* Keyboard applet: port of MAIN.CPL seg15:0000, the dialog procedure of dialog 5. The repeat
 * delay and rate go through SystemParametersInfo; libw16 does the key repeat itself the way
 * KEYBOARD.DRV programmed the keyboard controller. */
#include "maincpl.h"

#define IDC_KBDTEST 600
#define IDC_KBDRATE 601
#define IDC_KBDDELAY 603

static int nSpeedWas, nDelayWas;    /* [0xe42], [0xe40]: values when the dialog opened */
static int nSpeed;                  /* [0x1842] Repeat Rate position, 0 (slow) .. 31 (fast) */
static int nDelayPos;               /* [0x11ec] Delay position, 0 (long) .. 3 (short) = 3 - delay */
static int nPosWas;                 /* [0xe3e] position before the last scroll action */

BOOL KeyboardDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        HourGlass(TRUE);
        /* seg15:030D: KEYBOARD.DRV must export SetSpeed, else string 0x8C is shown and the dialog
         * ends; libw16 always handles the typematic rate */
        SystemParametersInfo(SPI_GETKEYBOARDSPEED, 0, &nSpeedWas, 0);
        SystemParametersInfo(SPI_GETKEYBOARDDELAY, 0, &nDelayWas, 0);
        nSpeed = nSpeedWas;
        nDelayPos = 3 - nDelayWas;
        SetScrollRange(GetDlgItem(hDlg, IDC_KBDRATE), SB_CTL, 0, 31, FALSE);
        SetScrollPos(GetDlgItem(hDlg, IDC_KBDRATE), SB_CTL, nSpeed, FALSE);
        SetScrollRange(GetDlgItem(hDlg, IDC_KBDDELAY), SB_CTL, 0, 3, FALSE);
        SetScrollPos(GetDlgItem(hDlg, IDC_KBDDELAY), SB_CTL, nDelayPos, FALSE);
        HourGlass(FALSE);
        return TRUE;

    case WM_HSCROLL: {
        int nMin = 0, nMax, nRange, *pPos;
        HWND hScroll;
        if (GetDlgItem(hDlg, IDC_KBDRATE) == W16_CMD_HWND(HIWORD(lParam))) {
            if (wParam != SB_ENDSCROLL) nPosWas = nSpeed;
            nMax = 31; nRange = 32; pPos = &nSpeed;
            hScroll = GetDlgItem(hDlg, IDC_KBDRATE);
        } else {
            if (wParam != SB_ENDSCROLL) nPosWas = nDelayPos;
            nMax = 3; nRange = 4; pPos = &nDelayPos;
            hScroll = GetDlgItem(hDlg, IDC_KBDDELAY);
        }
        switch (wParam) {
        case SB_LINEUP:
            if (--*pPos < nMin) *pPos = nMin;
            break;
        case SB_LINEDOWN:
            if (++*pPos > nMax) *pPos = nMax;
            break;
        case SB_PAGEUP:
            *pPos -= nRange / 4;
            if (*pPos < nMin) *pPos = nMin;
            break;
        case SB_PAGEDOWN:
            *pPos += nRange / 4;
            if (*pPos > nMax) *pPos = nMax;
            break;
        case SB_THUMBPOSITION:
            *pPos = LOWORD(lParam);
            break;
        case SB_TOP:
            *pPos = nMin;
            break;
        case SB_BOTTOM:
            *pPos = nMax;
            break;
        case SB_ENDSCROLL:
            /* try the new rate right away in the Test box */
            if (*pPos != nPosWas) {
                SystemParametersInfo(SPI_SETKEYBOARDSPEED, nSpeed, NULL, 0);
                SystemParametersInfo(SPI_SETKEYBOARDDELAY, 3 - nDelayPos, NULL, 0);
            }
            SetDlgItemText(hDlg, IDC_KBDTEST, "");
            break;
        }
        if (nPosWas != *pPos && wParam != SB_ENDSCROLL)
            SetScrollPos(hScroll, SB_CTL, *pPos, TRUE);
        return TRUE;
    }

    case WM_COMMAND:
        switch (wParam) {
        case IDD_HELP:
            CPHelp(hDlg);
            break;
        case IDOK:
            HourGlass(TRUE);
            SystemParametersInfo(SPI_SETKEYBOARDSPEED, nSpeed, NULL, SPIF_UPDATEINIFILE);
            SystemParametersInfo(SPI_SETKEYBOARDDELAY, 3 - nDelayPos, NULL, SPIF_UPDATEINIFILE);
            BroadcastWinIniChange(0);
            EndDialog(hDlg, 0);
            HourGlass(FALSE);
            break;
        case IDCANCEL:
            SystemParametersInfo(SPI_SETKEYBOARDSPEED, nSpeedWas, NULL, 0);
            SystemParametersInfo(SPI_SETKEYBOARDDELAY, nDelayWas, NULL, 0);
            EndDialog(hDlg, 0);
            break;
        }
        return TRUE;

    default:
        if (msg == wHelpMessage) {
            CPHelp(hDlg);
            return TRUE;
        }
        return FALSE;
    }
}
