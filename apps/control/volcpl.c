/* Volume - an arch311 Control Panel applet (new; 3.11 had none). Speaker and microphone level with
 * mute, and the output device, through PipeWire (audio.c). Built like the 3.11 applets: a modal
 * dialog in Helv 8 with group boxes and scroll bar "sliders" as in MAIN.CPL's Mouse dialog.
 * Changes are heard immediately; Cancel puts everything back. */
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "cpl.h"
#include "audio.h"

enum { IDC_OUTVOL = 601, IDC_OUTMUTE = 602, IDC_INVOL = 611, IDC_INMUTE = 612, IDC_DEVICE = 621, IDC_TEST = 622 };

/* the applet icon (original art in the 3.1 icon idiom): 16-colour indexes, '.' transparent */
static const char *const vol_icon[32] = {
    "................................",
    "................................",
    "..................00............",
    ".................008............",
    "................0F08....4.......",
    "...............0F708.....4......",
    "..............0F7708......4.....",
    ".............0F77708.......4....",
    "............0F777708...4....4...",
    "...........0F7777708....4...4...",
    "...00000000F77777708.....4...4..",
    "...0FFFFFF0777777708......4..4..",
    "...0F777780777777708.4....4..4..",
    "...0F777780777777708..4....4..4.",
    "...0F777780777777708...4...4..4.",
    "...0F777780777777708...4...4..4.",
    "...0F777780888888808...4...4..4.",
    "...0F777780888888808...4...4..4.",
    "...0F777780888888808..4....4..4.",
    "...0F777780888888808.4....4..4..",
    "...0F888880888888808......4..4..",
    "...00000000888888808.....4...4..",
    "...........088888808....4...4...",
    "............08888808...4....4...",
    ".............0888808.......4....",
    "..............088808......4.....",
    "...............08808.....4......",
    "................0808....4.......",
    ".................008............",
    "..................00............",
    "................................",
    "................................",
};

static HICON hIconVolume;

static int hexv(char c) { return c >= 'A' ? c - 'A' + 10 : c - '0'; }

static HICON MakeIcon(const char *const rows[32])
{
    uint8_t andb[32 * 4], xorb[32 * 16];
    memset(andb, 0, sizeof andb);
    memset(xorb, 0, sizeof xorb);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            char c = rows[y][x];
            if (c == '.') andb[y * 4 + x / 8] |= 0x80 >> (x & 7);
            else xorb[y * 16 + x / 2] |= hexv(c) << ((x & 1) ? 0 : 4);
        }
    return CreateIcon(NULL, 32, 32, 1, 4, andb, xorb);
}

/* the dialog, in dialog units, laid out like MAIN.CPL's Mouse dialog */
static W16DlgTemplate *VolumeTemplate(void)
{
    W16DlgTemplate *t = w16_dlgt_new(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 22, 18, 208, 128, "Volume", 8, "Helv");
    w16_dlgt_add(t, "BUTTON", "&Speaker Volume", -1, BS_GROUPBOX, 4, 4, 150, 50);
    w16_dlgt_add(t, "STATIC", "Low", -1, SS_LEFT | WS_GROUP, 10, 16, 44, 9);
    w16_dlgt_add(t, "STATIC", "High", -1, SS_RIGHT | WS_GROUP, 104, 16, 44, 9);
    w16_dlgt_add(t, "SCROLLBAR", "", IDC_OUTVOL, SBS_HORZ | WS_GROUP | WS_TABSTOP, 10, 26, 138, 12);
    w16_dlgt_add(t, "BUTTON", "&Mute", IDC_OUTMUTE, BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 10, 40, 60, 12);
    w16_dlgt_add(t, "BUTTON", "M&icrophone Level", -1, BS_GROUPBOX, 4, 58, 150, 50);
    w16_dlgt_add(t, "STATIC", "Low", -1, SS_LEFT | WS_GROUP, 10, 70, 44, 9);
    w16_dlgt_add(t, "STATIC", "High", -1, SS_RIGHT | WS_GROUP, 104, 70, 44, 9);
    w16_dlgt_add(t, "SCROLLBAR", "", IDC_INVOL, SBS_HORZ | WS_GROUP | WS_TABSTOP, 10, 80, 138, 12);
    w16_dlgt_add(t, "BUTTON", "Mu&te", IDC_INMUTE, BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 10, 94, 60, 12);
    w16_dlgt_add(t, "STATIC", "&Play through:", -1, SS_LEFT | WS_GROUP, 4, 114, 50, 9);
    w16_dlgt_add(t, "COMBOBOX", "", IDC_DEVICE, CBS_DROPDOWNLIST | WS_VSCROLL | WS_GROUP | WS_TABSTOP, 54, 112, 150, 60);
    w16_dlgt_add(t, "BUTTON", "OK", IDOK, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, 160, 4, 44, 14);
    w16_dlgt_add(t, "BUTTON", "Cancel", IDCANCEL, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 160, 22, 44, 14);
    w16_dlgt_add(t, "BUTTON", "&Test", IDC_TEST, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 160, 42, 44, 14);
    return t;
}

typedef struct {
    int vol[2], mute[2], device; /* as found, for Cancel */
    AudioDev dev[16];
    int ndev;
} VolState;
static VolState vs;

static int which_of(int id) { return id == IDC_INVOL || id == IDC_INMUTE ? AUDIO_IN : AUDIO_OUT; }

static void LoadLevels(HWND dlg)
{
    for (int w = 0; w < 2; w++) {
        int pct = 0, mute = 0;
        HWND sb = GetDlgItem(dlg, w == AUDIO_OUT ? IDC_OUTVOL : IDC_INVOL);
        int ok = audio_get(w, &pct, &mute);
        SetScrollRange(sb, SB_CTL, 0, 100, FALSE);
        SetScrollPos(sb, SB_CTL, pct, TRUE);
        CheckDlgButton(dlg, w == AUDIO_OUT ? IDC_OUTMUTE : IDC_INMUTE, mute);
        EnableWindow(sb, ok);
        EnableWindow(GetDlgItem(dlg, w == AUDIO_OUT ? IDC_OUTMUTE : IDC_INMUTE), ok);
    }
}

static BOOL VolumeDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (!audio_available())
            MessageBox(dlg, "Cannot find the sound system.\n\nThe Volume settings need PipeWire with WirePlumber (wpctl).",
                       "Volume", MB_OK | MB_ICONEXCLAMATION);
        for (int w = 0; w < 2; w++) audio_get(w, &vs.vol[w], &vs.mute[w]);
        LoadLevels(dlg);
        vs.ndev = audio_list_outputs(vs.dev, 16);
        vs.device = -1;
        HWND cb = GetDlgItem(dlg, IDC_DEVICE);
        for (int i = 0; i < vs.ndev; i++) {
            SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)vs.dev[i].name);
            if (vs.dev[i].is_default) {
                SendMessage(cb, CB_SETCURSEL, i, 0);
                vs.device = vs.dev[i].id;
            }
        }
        EnableWindow(cb, vs.ndev > 0);
        return TRUE;
    }
    case WM_HSCROLL: {
        HWND sb = W16_CMD_HWND(HIWORD(lp));
        int id = sb ? GetDlgCtrlID(sb) : 0;
        if (id != IDC_OUTVOL && id != IDC_INVOL) break;
        int pos = GetScrollPos(sb, SB_CTL);
        switch (wp) {
        case SB_LINELEFT: pos -= 2; break;
        case SB_LINERIGHT: pos += 2; break;
        case SB_PAGELEFT: pos -= 10; break;
        case SB_PAGERIGHT: pos += 10; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = LOWORD(lp); break;
        case SB_LEFT: pos = 0; break;
        case SB_RIGHT: pos = 100; break;
        default: return TRUE;
        }
        if (pos < 0) pos = 0;
        if (pos > 100) pos = 100;
        SetScrollPos(sb, SB_CTL, pos, TRUE);
        audio_set_volume(which_of(id), pos);
        return TRUE;
    }
    case WM_COMMAND:
        switch (wp) {
        case IDC_OUTMUTE:
        case IDC_INMUTE:
            if (HIWORD(lp) == BN_CLICKED) audio_set_mute(which_of((int)wp), IsDlgButtonChecked(dlg, (int)wp));
            return TRUE;
        case IDC_DEVICE:
            if (HIWORD(lp) == CBN_SELCHANGE) {
                int i = (int)SendDlgItemMessage(dlg, IDC_DEVICE, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < vs.ndev && audio_set_default_output(vs.dev[i].id)) LoadLevels(dlg);
            }
            return TRUE;
        case IDC_TEST: {
            HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
            if (!audio_play_test()) MessageBeep(0);
            SetCursor(old);
            return TRUE;
        }
        case IDOK:
            EndDialog(dlg, TRUE);
            return TRUE;
        case IDCANCEL:
            if (vs.device >= 0) audio_set_default_output(vs.device);
            for (int w = 0; w < 2; w++) {
                audio_set_volume(w, vs.vol[w]);
                audio_set_mute(w, vs.mute[w]);
            }
            EndDialog(dlg, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

LRESULT Volume_CPlApplet(HWND hwndCPl, UINT msg, LPARAM l1, LPARAM l2)
{
    switch (msg) {
    case CPL_INIT:
        if (!hIconVolume) hIconVolume = MakeIcon(vol_icon);
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_NEWINQUIRE: {
        NEWCPLINFO *ni = (NEWCPLINFO *)l2;
        memset(ni, 0, sizeof *ni);
        ni->dwSize = sizeof *ni;
        ni->hIcon = hIconVolume;
        lstrcpy(ni->szName, "&Volume");
        lstrcpy(ni->szInfo, "Adjusts the volume of your speakers and microphone");
        return 0;
    }
    case CPL_DBLCLK: {
        W16DlgTemplate *t = VolumeTemplate();
        DialogBoxIndirectParam(NULL, w16_dlgt_data(t), hwndCPl, VolumeDlgProc, 0);
        w16_dlgt_free(t);
        return 0;
    }
    case CPL_EXIT:
        if (hIconVolume) { DestroyIcon(hIconVolume); hIconVolume = NULL; }
        return 0;
    }
    (void)l1;
    return 0;
}
