/* lbtest: libw16's list box test program (apps/lbtest/tests). Three lists in one window:
 *   A  multicolumn, extended selection, horizontal scroll bar (as File Manager's file lists)
 *   B  single column, single selection, horizontal extent 520 px, both scroll bars; the parent
 *      answers WM_LBTRACKPOINT with 1 for item 5 and 2 for item 4
 *   C  owner-draw fixed without strings, sorted through WM_COMPAREITEM, extended selection,
 *      LBS_WANTKEYBOARDINPUT (F2 -> -2, F3 -> item 2; WM_CHARTOITEM finds the first item with the
 *      letter)
 * After every notification (and every 100 ms) the lists' state goes to ini/LBTEST.INI in the
 * current directory, where tools/regress.sh's "# ini:" lines check it. No Windows text is used. */
#include <w16.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *w16_app_module = "USER.EXE";

enum { ID_A = 101, ID_B, ID_C };
static int mode2; /* command line "2": B has 3 items and LBS_DISABLENOSCROLL, no extent; C is empty */
static HWND hA, hB, hC;
static int selchange[3], dblclk[3], setfocus[3], killfocus[3], selcancel[3];
static int ntrack, track_id, track_item, track_x, track_y;
static int nvkey, vkey_last, vkey_caret, nchar, char_last;
static char drawlog[256];
static int ndraw;

static const char *cnames[] = {"kiwi", "apple", "mango", "cherry", "banana", "lemon", "grape", "date", "fig", "lime",
                               "melon", "olive", "peach", "pear", "plum", "quince"};

static int idx(int id) { return id - ID_A; }

static void dump_list(FILE *f, const char *sect, HWND h)
{
    int n = (int)SendMessage(h, LB_GETCOUNT, 0, 0), items[64], ns = (int)SendMessage(h, LB_GETSELITEMS, 64, (LPARAM)items);
    char sel[400] = "";
    if (ns == LB_ERR) {
        int c = (int)SendMessage(h, LB_GETCURSEL, 0, 0);
        if (c >= 0) sprintf(sel, "%d", c);
    } else
        for (int i = 0; i < ns; i++) sprintf(sel + strlen(sel), "%s%d", i ? "," : "", items[i]);
    POINT p = {0, 0};
    ClientToScreen(h, &p);
    RECT r;
    GetClientRect(h, &r);
    LONG st = GetWindowLong(h, GWL_STYLE);
    fprintf(f, "[%s]\r\ncount=%d\r\nsel=%s\r\nselcount=%d\r\ncaret=%d\r\nanchor=%d\r\ntop=%d\r\n", sect, n, sel,
            (int)SendMessage(h, LB_GETSELCOUNT, 0, 0), (int)SendMessage(h, LB_GETCARETINDEX, 0, 0),
            (int)SendMessage(h, LB_GETANCHORINDEX, 0, 0), (int)SendMessage(h, LB_GETTOPINDEX, 0, 0));
    fprintf(f, "client=%d,%d,%d,%d\r\nhbar=%d\r\nvbar=%d\r\nhpos=%d\r\nvpos=%d\r\n", (int)p.x, (int)p.y, (int)r.right, (int)r.bottom,
            (st & WS_HSCROLL) != 0, (st & WS_VSCROLL) != 0, GetScrollPos(h, SB_HORZ), GetScrollPos(h, SB_VERT));
    RECT ir;
    int vis = (int)SendMessage(h, LB_GETITEMRECT, 1, (LPARAM)&ir);
    fprintf(f, "rect1=%d,%d,%d,%d,%d\r\n", vis, (int)ir.left, (int)ir.top, (int)ir.right, (int)ir.bottom);
}

static void dump(void)
{
    mkdir("ini", 0777);
    FILE *f = fopen("ini/LBTEST.INI", "wb");
    if (!f) return;
    dump_list(f, "A", hA);
    dump_list(f, "B", hB);
    dump_list(f, "C", hC);
    fprintf(f, "[events]\r\n");
    for (int i = 0; i < 3; i++)
        fprintf(f, "selchange%c=%d\r\ndblclk%c=%d\r\nsetfocus%c=%d\r\nkillfocus%c=%d\r\nselcancel%c=%d\r\n", 'A' + i,
                selchange[i], 'A' + i, dblclk[i], 'A' + i, setfocus[i], 'A' + i, killfocus[i], 'A' + i, selcancel[i]);
    fprintf(f, "track=%d,%d,%d,%d,%d\r\nvkey=%d,%d,%d\r\nchartoitem=%d,%d\r\ndraws=%d\r\ndrawlog=%s\r\n", ntrack, track_id,
            track_item, track_x, track_y, nvkey, vkey_last, vkey_caret, nchar, char_last, ndraw, drawlog);
    char t[64] = "";
    int i2 = (int)SendMessage(hC, LB_FINDSTRING, (WPARAM)-1, (LPARAM)cnames[4]);
    ULONG_PTR d = 0;
    int tl = (int)SendMessage(hC, LB_GETTEXT, 0, (LPARAM)&d);
    SendMessage(hB, LB_GETTEXT, 2, (LPARAM)t);
    fprintf(f, "cfind=%d\r\nctext=%d,%s\r\nbtext=%s\r\n", i2, tl, d ? (const char *)d : "", t);
    fclose(f);
}

static void logdraw(const DRAWITEMSTRUCT *di)
{
    char e[32];
    sprintf(e, "%d:%d:%x;", (int)di->itemID, di->itemAction, di->itemState);
    ndraw++;
    if (strlen(drawlog) + strlen(e) >= sizeof drawlog) memmove(drawlog, drawlog + 64, strlen(drawlog + 64) + 1);
    strcat(drawlog, e);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        char s[64];
        hA = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_HSCROLL | LBS_MULTICOLUMN | LBS_EXTENDEDSEL |
                          LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 20, 20, 300, 113, h, (HMENU)ID_A, NULL, NULL);
        SendMessage(hA, LB_SETCOLUMNWIDTH, 80, 0);
        for (int i = 0; i < 30; i++) {
            sprintf(s, "file%02d.txt", i);
            SendMessage(hA, LB_ADDSTRING, 0, (LPARAM)s);
        }
        hB = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY |
                          (mode2 ? LBS_DISABLENOSCROLL : 0), 340, 20, 260, 120, h, (HMENU)ID_B, NULL, NULL);
        for (int i = 0; i < (mode2 ? 3 : 20); i++) {
            sprintf(s, "line %02d of a list wider than its window, %s", i, i % 3 ? "abc" : "xyz");
            SendMessage(hB, LB_ADDSTRING, 0, (LPARAM)s);
        }
        if (!mode2) SendMessage(hB, LB_SETHORIZONTALEXTENT, 520, 0);
        hC = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_SORT |
                          LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_WANTKEYBOARDINPUT, 20, 200, 200, 130, h, (HMENU)ID_C, NULL, NULL);
        for (int i = 0; i < (mode2 ? 0 : 16); i++) SendMessage(hC, LB_ADDSTRING, 0, (LPARAM)cnames[i]);
        SetTimer(h, 1, 100, NULL);
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        mi->itemHeight = 18;
        return TRUE;
    }
    case WM_COMPAREITEM: {
        COMPAREITEMSTRUCT *ci = (COMPAREITEMSTRUCT *)lp;
        int c = strcmp((const char *)ci->itemData1, (const char *)ci->itemData2);
        return c < 0 ? -1 : c > 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        logdraw(di);
        if (di->itemAction & (ODA_DRAWENTIRE | ODA_SELECT)) {
            int sel = (di->itemState & ODS_SELECTED) != 0;
            FillRect(di->hDC, &di->rcItem, (HBRUSH)GetStockObject(sel ? BLACK_BRUSH : WHITE_BRUSH));
            if (di->itemID != (UINT)-1) {
                SetBkMode(di->hDC, TRANSPARENT);
                SetTextColor(di->hDC, sel ? RGB(255, 255, 255) : RGB(0, 0, 0));
                const char *s = (const char *)di->itemData;
                TextOut(di->hDC, di->rcItem.left + 4, di->rcItem.top + 1, s, strlen(s));
            }
            if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
        } else if (di->itemAction & ODA_FOCUS)
            DrawFocusRect(di->hDC, &di->rcItem);
        return TRUE;
    }
    case WM_LBTRACKPOINT:
        ntrack++;
        track_item = (int)wp;
        track_x = (SHORT)LOWORD(lp);
        track_y = (SHORT)HIWORD(lp);
        /* the list clicked has just taken the focus */
        track_id = GetFocus() == hB ? ID_B : GetFocus() == hA ? ID_A : GetFocus() == hC ? ID_C : 0;
        dump();
        if (track_id == ID_B && wp == 5) return 1;
        if (track_id == ID_B && wp == 4) return 2;
        return 0;
    case WM_VKEYTOITEM:
        nvkey++;
        vkey_last = (int)wp;
        vkey_caret = HIWORD(lp);
        dump();
        if (wp == VK_F2) return -2;
        if (wp == VK_F3) return 2;
        return -1;
    case WM_CHARTOITEM: {
        nchar++;
        char_last = (int)wp;
        int n = (int)SendMessage(hC, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            const char *s = (const char *)SendMessage(hC, LB_GETITEMDATA, i, 0);
            if (s[0] == (char)wp) { dump(); return i; }
        }
        dump();
        return -1;
    }
    case WM_COMMAND:
        if (wp >= ID_A && wp <= ID_C) {
            int k = idx((int)wp);
            switch (HIWORD(lp)) {
            case LBN_SELCHANGE: selchange[k]++; break;
            case LBN_DBLCLK: dblclk[k]++; break;
            case LBN_SETFOCUS: setfocus[k]++; break;
            case LBN_KILLFOCUS: killfocus[k]++; break;
            case LBN_SELCANCEL: selcancel[k]++; break;
            }
            dump();
        }
        return 0;
    case WM_TIMER: dump(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    mode2 = lpCmdLine && lpCmdLine[0] == '2';
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "LbTest";
    RegisterClass(&wc);
    HWND h = CreateWindow("LbTest", "List boxes", WS_OVERLAPPEDWINDOW, 0, 0, 640, 400, NULL, NULL, hInstance, NULL);
    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
