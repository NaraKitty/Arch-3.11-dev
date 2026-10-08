/* USER dialog manager: templates, modal/modeless dialogs, keyboard interface, MessageBox.
 * 3.1 rules measured from real 3.11: dialog fonts are bold, base units come from
 * ((width of "A..Za..z") / 26 + 1) / 2 and the font height; dialogs have a white background. */
#include "w16int.h"
#include <ctype.h>

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }

W16Dialog *w16_dlg(HWND h)
{
    if (!w16_valid(h) || !h->is_dialog) return NULL;
    return (W16Dialog *)h->ctl;
}

/* ------------------------------------------------------------------ base units */
/* USER's average character width of the DC's font (seg2:03A4; the edit control uses it too):
 * tmAveCharWidth for a fixed-pitch font, else the extent of "a".."z" and "A".."Z" over 26, plus one,
 * halved */
int w16_ave_char_width(HDC dc, TEXTMETRIC *tm)
{
    GetTextMetrics(dc, tm);
    if (!(tm->tmPitchAndFamily & 1)) return tm->tmAveCharWidth;
    const char *s = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return ((LOWORD(GetTextExtent(dc, s, 52)) / 26) + 1) / 2;
}

static void char_dims(HFONT f, int *cx, int *cy)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f ? f : GetStockObject(SYSTEM_FONT));
    TEXTMETRIC tm;
    *cx = w16_ave_char_width(dc, &tm);
    *cy = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
}

DWORD GetDialogBaseUnits(void)
{
    int cx, cy;
    char_dims(NULL, &cx, &cy);
    return MAKELONG(cx, cy);
}

void MapDialogRect(HWND h, LPRECT r)
{
    W16Dialog *d = w16_dlg(h);
    int cx = d ? d->cxchar : 8, cy = d ? d->cychar : 16;
    r->left = (r->left * cx + 2) / 4;
    r->right = (r->right * cx + 2) / 4;
    r->top = (r->top * cy + 4) / 8;
    r->bottom = (r->bottom * cy + 4) / 8;
}

/* ------------------------------------------------------------------ template parsing */
static const char *sz(const uint8_t **p)
{
    const char *s = (const char *)*p;
    *p += strlen(s) + 1;
    return s;
}
static int name_or_ord(const uint8_t **p, const char **name)
{
    if (**p == 0xFF) { int v = u16(*p + 1); *p += 3; *name = NULL; return v; }
    *name = sz(p);
    return 0;
}

static int tabbable(HWND c);
static void check_def_push(HWND dlg, HWND old, HWND nw);
static void dlg_set_focus(HWND c);
static HWND create_dialog(HINSTANCE inst, const uint8_t *t, HWND owner, DLGPROC proc, LPARAM lp, int modal)
{
    DWORD style = u32(t);
    int n = t[4];
    int x = (SHORT)u16(t + 5), y = (SHORT)u16(t + 7), cx = (SHORT)u16(t + 9), cy = (SHORT)u16(t + 11);
    const uint8_t *p = t + 13;
    const char *menuname, *clsname, *caption;
    int menuord = name_or_ord(&p, &menuname);
    int clsord = name_or_ord(&p, &clsname);
    (void)clsord;
    caption = sz(&p);
    HFONT font = NULL;
    int ownfont = 0;
    if (style & DS_SETFONT) {
        int pt = u16(p);
        p += 2;
        const char *face = sz(&p);
        font = CreateFont(-((pt * 96 + 36) / 72), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, face);
        ownfont = 1;
    }
    int cxc, cyc;
    char_dims(font, &cxc, &cyc);
    /* dialog rectangle: template units -> pixels (client area), relative to the owner's client */
    RECT rc = {0, 0, (cx * cxc + 2) / 4, (cy * cyc + 4) / 8};
    int px = (x * cxc + 2) / 4, py = (y * cyc + 4) / 8;
    DWORD ex = 0;
    DWORD wstyle = style & ~(DS_SETFONT | DS_MODALFRAME | DS_SYSMODAL | DS_LOCALEDIT | DS_ABSALIGN | DS_NOIDLEMSG);
    if (style & DS_MODALFRAME) { ex |= WS_EX_DLGMODALFRAME; wstyle |= WS_DLGFRAME; }
    if (!(wstyle & WS_CHILD)) wstyle |= WS_POPUP;
    int visible = (wstyle & WS_VISIBLE) != 0;
    wstyle &= ~WS_VISIBLE;
    /* frame size for this style */
    struct W16Window fake;
    memset(&fake, 0, sizeof fake);
    fake.style = wstyle;
    fake.exstyle = ex;
    fake.parent = (wstyle & WS_CHILD) ? (HWND)1 : w16_desktop;
    RECT big = {0, 0, 1000, 1000}, inner;
    w16_nc_calc(&fake, &big, &inner);
    int fl = inner.left, ft = inner.top, fr = 1000 - inner.right, fb = 1000 - inner.bottom;
    if (menuord || (menuname && *menuname)) ft += GetSystemMetrics(SM_CYMENU) + 1;
    /* the template's x,y place the dialog's client area (in the owner's client coordinates unless
     * DS_ABSALIGN); the frame, caption and menu go around it (AdjustWindowRect) */
    int wx = px, wy = py;
    if (!(style & DS_ABSALIGN) && owner && w16_valid(owner) && !(wstyle & WS_CHILD)) {
        HWND o = owner;
        wx += o->rc.left;
        wy += o->rc.top;
    }
    if (!(wstyle & WS_CHILD)) { wx -= fl; wy -= ft; }
    int ww = rc.right + fl + fr, wh = rc.bottom + ft + fb;
    if (!(wstyle & WS_CHILD)) {
        /* keep on screen, USER seg24:043D: the bottom edge at most 4 pixels above the screen's
         * (less the Kanji window, none here), the top on it, the right edge at most 4 pixels inside,
         * the left on it - Calculator's template asks for x 620 and lands at 154 */
        if (wy + wh > w16_screen.h - 4) wy = w16_screen.h - 4 - wh;
        if (wy < 0) wy = 0;
        if (wx + ww > w16_screen.w - 4) wx = w16_screen.w - 4 - ww;
        if (wx < 0) wx = 0;
    }
    const char *cls = clsname && *clsname ? clsname : "#32770";
    W16Dialog *dd = calloc(1, sizeof *dd);
    dd->proc = proc;
    dd->font = font;
    dd->ownfont = ownfont;
    dd->cxchar = cxc;
    dd->cychar = cyc;
    dd->owner = owner;
    dd->defid = IDOK;
    HMENU menu = NULL;
    if (menuord) menu = LoadMenu(inst, MAKEINTRESOURCE(menuord));
    else if (menuname && *menuname) menu = LoadMenu(inst, menuname);
    /* create without WM_CREATE side effects of the dialog proc: the proc gets WM_INITDIALOG */
    W16Class *c = w16_find_class(cls, inst);
    if (!c) c = w16_find_class("#32770", NULL);
    WNDPROC saved = c->wc.lpfnWndProc;
    HWND h = CreateWindowEx(ex, c->name, caption, wstyle, wx, wy, ww, wh,
                            (wstyle & WS_CHILD) ? owner : owner, menu, inst, NULL);
    (void)saved;
    if (!h) { free(dd); return NULL; }
    h->is_dialog = 1;
    h->ctl = dd;
    if (font) SendMessage(h, WM_SETFONT, (WPARAM)font, 0);
    /* controls */
    HWND first = NULL;
    for (int i = 0; i < n; i++) {
        int ix = (SHORT)u16(p), iy = (SHORT)u16(p + 2), icx = (SHORT)u16(p + 4), icy = (SHORT)u16(p + 6);
        int id = u16(p + 8);
        DWORD is = u32(p + 10);
        p += 14;
        const char *ccls;
        static const char *pre[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX"};
        if (*p & 0x80) { int k = *p - 0x80; ccls = k < 6 ? pre[k] : "STATIC"; p++; }
        else ccls = sz(&p);
        const char *txt;
        int ord = name_or_ord(&p, &txt);
        int extra = *p;
        p += 1 + extra;
        /* position and size convert separately (rounded, as MulDiv): a 14-unit button is 23 px
         * high wherever it sits, as measured on real 3.11 */
        int cx_ = (icx * cxc + 2) / 4, cy_ = (icy * cyc + 4) / 8;
        HWND ch = CreateWindowEx(WS_EX_NOPARENTNOTIFY, ccls, ord ? "" : txt, (is | WS_CHILD) & ~WS_POPUP,
                                 (ix * cxc + 2) / 4, (iy * cyc + 4) / 8, cx_, cy_, h, (HMENU)(uintptr_t)id, inst, NULL);
        if (!ch) { W16_LOG("dialog: could not create control class %s\n", ccls); continue; }
        if (font) SendMessage(ch, WM_SETFONT, (WPARAM)font, 0);
        if (ord && !strcasecmp(ccls, "STATIC") && (is & 0xF) == SS_ICON) {
            /* a system icon (MAIN.CPL's restart box uses IDI_EXCLAMATION) when the module has none */
            HICON ic = LoadIcon(inst, MAKEINTRESOURCE(ord));
            if (!ic) ic = LoadIcon(NULL, MAKEINTRESOURCE(ord));
            SendMessage(ch, STM_SETICON, (WPARAM)ic, 0);
        }
        if (!first && (is & WS_TABSTOP) && !(is & WS_DISABLED) && (is & WS_VISIBLE)) first = ch;
        if (!strcasecmp(ccls, "BUTTON") && (is & 0xF) == BS_DEFPUSHBUTTON) dd->defid = id;
    }
    if (!first) first = GetNextDlgTabItem(h, NULL, FALSE);
    HWND param = first;
    dd->focus = first; /* given at the first activation (RestoreDlgFocus) */
    if (SendMessage(h, WM_INITDIALOG, (WPARAM)first, lp) && w16_valid(h)) {
        /* WM_INITDIALOG may have disabled or hidden the control picked before it ran */
        if (first && !tabbable(first)) first = GetNextDlgTabItem(h, first, FALSE);
        if (first) {
            /* USER seg24:0914: DlgSetFocus, then CheckDefPushButton - at once if WM_INITDIALOG
             * already showed and activated the dialog, else when it is */
            if (w16_active == h && IsWindowVisible(h)) dlg_set_focus(first);
            else dd->focus = first;
            check_def_push(h, param, first);
        }
    } else if (w16_valid(h)) {
        /* FALSE: the proc placed the focus itself. If it did not, activation gives the focus to the
         * first tab stop enabled by then (Sound disables its lists there and OK gets it) */
        if (w16_focus && IsChild(h, w16_focus)) dd->focus = w16_focus;
        else if (dd->focus && !tabbable(dd->focus)) dd->focus = GetNextDlgTabItem(h, NULL, FALSE);
    }
    if (!w16_valid(h)) return NULL;
    if (visible || modal) {
        HWND f0 = dd->focus; /* (activation hands it over and forgets it) */
        ShowWindow(h, SW_SHOWNORMAL);
        if (f0 && w16_valid(f0) && w16_focus != f0) SetFocus(f0);
        if (f0 && w16_valid(f0) && (SendMessage(f0, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL))
            SendMessage(f0, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
    }
    return h;
}

HWND CreateDialogIndirectParam(HINSTANCE h, const void *tmpl, HWND owner, DLGPROC proc, LPARAM lp)
{
    return create_dialog(h, tmpl, owner, proc, lp, 0);
}
HWND CreateDialogParam(HINSTANCE h, LPCSTR name, HWND owner, DLGPROC proc, LPARAM lp)
{
    const W16Res *r = w16_find_res(h, name, RT_DIALOG);
    if (!r) return NULL;
    return create_dialog(h, w16_res_data(h, r), owner, proc, lp, 0);
}
HWND CreateDialog(HINSTANCE h, LPCSTR name, HWND owner, DLGPROC proc) { return CreateDialogParam(h, name, owner, proc, 0); }

static int run_modal(HWND h, HWND owner)
{
    W16Dialog *d = w16_dlg(h);
    if (!d) return -1;
    HWND top = owner ? w16_top_level(owner) : NULL;
    int owner_was_enabled = top && IsWindowEnabled(top);
    if (top) EnableWindow(top, FALSE);
    if (top && owner_was_enabled) d->disabled_owner = top;
    MSG m;
    while (w16_valid(h) && !d->ended) {
        if (!GetMessage(&m, NULL, 0, 0)) { PostQuitMessage((int)m.wParam); break; }
        if (!IsDialogMessage(h, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    int result = d->result;
    if (d->disabled_owner && w16_valid(d->disabled_owner)) EnableWindow(d->disabled_owner, TRUE);
    if (w16_valid(h)) DestroyWindow(h);
    if (top && w16_valid(top)) w16_activate(top, WA_ACTIVE);
    if (d->ownfont) DeleteObject(d->font);
    free(d);
    return result;
}

int DialogBoxIndirectParam(HINSTANCE h, const void *tmpl, HWND owner, DLGPROC proc, LPARAM lp)
{
    HWND d = create_dialog(h, tmpl, owner, proc, lp, 1);
    if (!d) return -1;
    return run_modal(d, owner);
}
int DialogBoxParam(HINSTANCE h, LPCSTR name, HWND owner, DLGPROC proc, LPARAM lp)
{
    const W16Res *r = w16_find_res(h, name, RT_DIALOG);
    if (!r) return -1;
    return DialogBoxIndirectParam(h, w16_res_data(h, r), owner, proc, lp);
}
int DialogBox(HINSTANCE h, LPCSTR name, HWND owner, DLGPROC proc) { return DialogBoxParam(h, name, owner, proc, 0); }

void EndDialog(HWND h, int result)
{
    W16Dialog *d = w16_dlg(h);
    if (!d) return;
    d->ended = 1;
    d->result = result;
    /* as USER does: re-enable the owner first, so that hiding the dialog hands activation and the
     * focus back to it; 3.1 hides the dialog immediately and destroys it when the loop unwinds */
    if (d->disabled_owner) {
        HWND o = d->disabled_owner;
        d->disabled_owner = NULL;
        if (w16_valid(o)) EnableWindow(o, TRUE);
    }
    ShowWindow(h, SW_HIDE);
}

/* ------------------------------------------------------------------ dialog window procedure */
static LRESULT dlg_code(HWND h) { return w16_valid(h) ? SendMessage(h, WM_GETDLGCODE, 0, 0) : 0; }

/* USER seg25:0AB2: every default push button of the dialog becomes a plain one */
static void clear_defaults(HWND dlg)
{
    for (HWND c = dlg->child; c; c = c->next)
        if (dlg_code(c) & DLGC_DEFPUSHBUTTON) SendMessage(c, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
}

/* USER CheckDefPushButton (seg25:0B5B), run when the dialog manager moves the focus from old to
 * new (Tab, arrows, mnemonics, a click on a control, activation, WM_NEXTDLGCTL, DM_SETDEFID): a push
 * button getting the focus becomes the default, otherwise the dialog's default button is made the
 * default again - unless it is disabled. Focus moved by the application's own SetFocus leaves the
 * buttons as they are (measured: MAIN.CPL Color after "Color Palette >>"). */
static void check_def_push(HWND dlg, HWND old, HWND nw)
{
    if (!w16_valid(dlg)) return;
    LRESULT cn = nw ? dlg_code(nw) : 0;
    if (old == nw) {
        if (cn & DLGC_UNDEFPUSHBUTTON) SendMessage(nw, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        return;
    }
    if ((old && (dlg_code(old) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) ||
        (nw && (cn & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))))
        clear_defaults(dlg);
    if (cn & DLGC_UNDEFPUSHBUTTON) {
        SendMessage(nw, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        return;
    }
    LRESULT r = SendMessage(dlg, DM_GETDEFID, 0, 0);
    HWND def = GetDlgItem(dlg, HIWORD(r) == DC_HASDEFID ? LOWORD(r) : IDOK);
    if (!def) return;
    LRESULT cd = dlg_code(def);
    if ((cd & DLGC_DEFPUSHBUTTON) || !(cd & DLGC_UNDEFPUSHBUTTON) || (def->style & WS_DISABLED)) return;
    SendMessage(def, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
}

/* USER DlgSetFocus (seg25:0000): the focus, an edit's text selected */
static void dlg_set_focus(HWND c)
{
    SetFocus(c);
    if (w16_valid(c) && (dlg_code(c) & DLGC_HASSETSEL)) SendMessage(c, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
}

/* USER RestoreDlgFocus (seg25:03E3): the control that had the focus when the dialog was
 * deactivated gets it back (once); SaveDlgFocus (seg25:03A8) keeps it, when nothing is kept yet,
 * and makes the default buttons plain */
static int restore_dlg_focus(HWND dlg, W16Dialog *d)
{
    HWND f = d->focus;
    d->focus = NULL;
    if (!f || IsIconic(dlg) || !w16_valid(f) || !IsChild(dlg, f)) return 0;
    check_def_push(dlg, w16_focus, f);
    SetFocus(f);
    return 1;
}
static void save_dlg_focus(HWND dlg, W16Dialog *d)
{
    if (w16_focus && IsChild(dlg, w16_focus) && !d->focus) {
        d->focus = w16_focus;
        clear_defaults(dlg);
    }
}

LRESULT DefDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    W16Dialog *d = w16_dlg(h);
    switch (m) {
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        HBRUSH b = (HBRUSH)SendMessage(h, WM_CTLCOLOR, wp, MAKELPARAM(0, CTLCOLOR_DLG));
        if (!b) b = w16_sys_brush(COLOR_WINDOW);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, b);
        return 1;
    }
    case WM_CLOSE: {
        HWND c = GetDlgItem(h, IDCANCEL);
        if (!c || IsWindowEnabled(c)) PostMessage(h, WM_COMMAND, IDCANCEL, MAKELPARAM(0, BN_CLICKED));
        return 0;
    }
    case WM_ACTIVATE:
        /* USER seg25:050B: restore or save the focus; a dialog activated with nothing to restore
         * gets the focus itself, which DefDlgProc's WM_SETFOCUS hands to the first tab stop */
        if (d && LOWORD(wp) != WA_INACTIVE) {
            if (!restore_dlg_focus(h, d) && !(w16_focus && IsChild(h, w16_focus))) {
                HWND f = GetNextDlgTabItem(h, NULL, FALSE);
                if (f) dlg_set_focus(f);
            }
        } else if (d)
            /* while another window is active no button of the dialog is the default (measured on
             * 3.11: behind the Edit Pattern dialog the focused "Edit Pattern..." and the Desktop's
             * OK both have the thin border, and so has OK behind a message box with the focus in
             * an edit); the default comes back with the activation (CheckDefPushButton) */
            save_dlg_focus(h, d);
        return 0;
    case WM_SETFOCUS:
        /* seg25:0553 */
        if (d && !d->ended && !restore_dlg_focus(h, d)) {
            HWND f = GetNextDlgTabItem(h, NULL, FALSE);
            if (f) dlg_set_focus(f);
        }
        return 0;
    case WM_NEXTDLGCTL: {
        HWND n;
        if (LOWORD(lp)) n = (HWND)wp; /* only usable from libw16 code (pointer handle) */
        else n = GetNextDlgTabItem(h, w16_focus, wp != 0);
        if (n) {
            check_def_push(h, w16_focus, n);
            dlg_set_focus(n);
        }
        return 0;
    }
    case DM_GETDEFID: return d ? MAKELONG(d->defid, DC_HASDEFID) : 0;
    case DM_SETDEFID:
        if (d) {
            HWND old = GetDlgItem(h, d->defid), nw = GetDlgItem(h, (int)wp);
            check_def_push(h, old, nw);
            d->defid = (int)wp;
        }
        return TRUE;
    case WM_GETFONT: return d ? (LRESULT)d->font : 0;
    case WM_SETFONT: if (d) d->font = (HFONT)wp; return 0;
    case WM_SHOWWINDOW: return 0;
    case WM_PARENTNOTIFY: return 0;
    case WM_COMMAND:
        /* focus changes inside the dialog keep the default button in sync */
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

LRESULT w16_dialog_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    W16Dialog *d = w16_dlg(h);
    if (d && d->proc) {
        d->msgresult = 0;
        BOOL r = d->proc(h, m, wp, lp);
        if (!w16_valid(h)) return r;
        if (r) {
            if (m == WM_CTLCOLOR || m == WM_COMPAREITEM || m == WM_VKEYTOITEM || m == WM_CHARTOITEM ||
                m == WM_QUERYDRAGICON || m == WM_INITDIALOG)
                return r;
            return d->msgresult ? d->msgresult : r;
        }
    }
    if (m == WM_INITDIALOG) return TRUE;
    return DefDlgProc(h, m, wp, lp);
}

/* DWL_MSGRESULT etc. */
intptr_t w16_dlg_get(HWND h, int idx)
{
    W16Dialog *d = w16_dlg(h);
    if (!d) return 0;
    if (idx == DWL_MSGRESULT) return d->msgresult;
    if (idx == DWL_DLGPROC) return (intptr_t)d->proc;
    if (idx == DWL_USER) return d->user;
    return 0;
}

/* ------------------------------------------------------------------ item helpers */
HWND GetDlgItem(HWND h, int id)
{
    if (!w16_valid(h)) return NULL;
    for (HWND c = h->child; c; c = c->next)
        if ((int)(WORD)c->id == (int)(WORD)id) return c;
    return NULL;
}
int GetDlgCtrlID(HWND h) { return w16_valid(h) ? (int)h->id : 0; }
UINT GetDlgItemText(HWND h, int id, LPSTR buf, int cb) { HWND c = GetDlgItem(h, id); if (!c) { if (cb) buf[0] = 0; return 0; } return GetWindowText(c, buf, cb); }
void SetDlgItemText(HWND h, int id, LPCSTR s) { HWND c = GetDlgItem(h, id); if (c) SetWindowText(c, s); }
LRESULT SendDlgItemMessage(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessage(c, m, wp, lp) : 0; }
void SetDlgItemInt(HWND h, int id, UINT v, BOOL sign)
{
    char b[32];
    if (sign) wsprintf(b, "%d", (int)v); else wsprintf(b, "%u", v);
    SetDlgItemText(h, id, b);
}
UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign)
{
    char b[64];
    GetDlgItemText(h, id, b, sizeof b);
    char *p = b, *e;
    while (*p == ' ') p++;
    long v = strtol(p, &e, 10);
    while (*e == ' ') e++;
    int good = e != p && !*e && (sign || v >= 0) && v <= (sign ? 32767 : 65535) && v >= (sign ? -32768 : 0);
    if (ok) *ok = good;
    return good ? (UINT)v : 0;
}
void CheckDlgButton(HWND h, int id, UINT c) { SendDlgItemMessage(h, id, BM_SETCHECK, c, 0); }
UINT IsDlgButtonChecked(HWND h, int id) { return (UINT)SendDlgItemMessage(h, id, BM_GETCHECK, 0, 0); }
void CheckRadioButton(HWND h, int first, int last, int check)
{
    for (int i = first; i <= last; i++) SendDlgItemMessage(h, i, BM_SETCHECK, i == check, 0);
}

/* ------------------------------------------------------------------ navigation */
static int tabbable(HWND c)
{
    return (c->style & WS_VISIBLE) && !(c->style & WS_DISABLED) && (c->style & WS_TABSTOP);
}

/* the dialog's own child holding ctl: the focus can sit in a control's child window (the edit of a
 * combo box), and USER moves on from the control itself */
static HWND dlg_child(HWND dlg, HWND ctl)
{
    while (w16_valid(ctl) && ctl->parent && ctl->parent != dlg) ctl = ctl->parent;
    return w16_valid(ctl) && ctl->parent == dlg ? ctl : NULL;
}

HWND GetNextDlgTabItem(HWND dlg, HWND ctl, BOOL prev)
{
    if (!w16_valid(dlg) || !dlg->child) return NULL;
    if (ctl) ctl = dlg_child(dlg, ctl);
    HWND list[512];
    int n = 0, cur = -1;
    for (HWND c = dlg->child; c && n < 512; c = c->next) {
        if (c == ctl) cur = n;
        list[n++] = c;
    }
    if (n == 0) return NULL;
    for (int k = 1; k <= n; k++) {
        int i = cur < 0 ? (prev ? n - k : k - 1) : (cur + (prev ? -k : k) + n * 2) % n;
        if (tabbable(list[i])) return list[i];
    }
    return ctl;
}

HWND GetNextDlgGroupItem(HWND dlg, HWND ctl, BOOL prev)
{
    if (!w16_valid(dlg) || !w16_valid(ctl)) return NULL;
    ctl = dlg_child(dlg, ctl);
    if (!ctl) return NULL;
    /* group: from the last WS_GROUP item at or before ctl up to the next WS_GROUP item */
    HWND list[512];
    int n = 0, cur = -1;
    for (HWND c = dlg->child; c && n < 512; c = c->next) {
        if (c == ctl) cur = n;
        list[n++] = c;
    }
    if (cur < 0) return ctl;
    int start = cur;
    while (start > 0 && !(list[start]->style & WS_GROUP)) start--;
    int end = cur + 1;
    while (end < n && !(list[end]->style & WS_GROUP)) end++;
    int len = end - start;
    for (int k = 1; k < len; k++) {
        int i = start + ((cur - start) + (prev ? -k : k) + len * 2) % len;
        HWND c = list[i];
        if ((c->style & WS_VISIBLE) && !(c->style & WS_DISABLED)) return c;
    }
    return ctl;
}

static HWND find_mnemonic(HWND dlg, int ch, HWND from)
{
    ch = toupper(ch);
    HWND list[512];
    int n = 0, cur = -1;
    for (HWND c = dlg->child; c && n < 512; c = c->next) {
        if (c == from) cur = n;
        list[n++] = c;
    }
    for (int k = 1; k <= n; k++) {
        int i = (cur + k + n) % n;
        HWND c = list[i];
        if (!(c->style & WS_VISIBLE) || (c->style & WS_DISABLED)) continue;
        LRESULT code = SendMessage(c, WM_GETDLGCODE, 0, 0);
        if (!(code & (DLGC_STATIC | DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_RADIOBUTTON))) {
            if (strcasecmp(c->cls->name, "BUTTON") && strcasecmp(c->cls->name, "STATIC")) continue;
        }
        if ((c->style & SS_NOPREFIX) && !strcasecmp(c->cls->name, "STATIC")) continue;
        if (w16_mnemonic(c->text) == ch) return c;
    }
    return NULL;
}

static void activate_ctl(HWND dlg, HWND c)
{
    if (!strcasecmp(c->cls->name, "STATIC") || (SendMessage(c, WM_GETDLGCODE, 0, 0) & DLGC_STATIC)) {
        /* a label (or a group box: DLGC_STATIC) gives focus to the next tab-stop control */
        HWND n = c->next;
        while (n && !tabbable(n)) n = n->next;
        if (n) {
            SetFocus(n);
            if (SendMessage(n, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessage(n, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        }
        return;
    }
    LRESULT code = SendMessage(c, WM_GETDLGCODE, 0, 0);
    if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
        /* a push button's mnemonic flashes it and sends its command; the focus stays where it is
         * (measured on 3.11: "a" for the Edit Pattern dialog's Add with the focus on OK leaves OK
         * focused) */
        SendMessage(c, BM_SETSTATE, TRUE, 0);
        SendMessage(c, BM_SETSTATE, FALSE, 0);
        SendMessage(dlg, WM_COMMAND, GetDlgCtrlID(c), W16_CMD_LPARAM(c, BN_CLICKED));
        return;
    }
    if (code & (DLGC_BUTTON | DLGC_RADIOBUTTON)) {
        SetFocus(c);
        SendMessage(c, WM_KEYDOWN, VK_SPACE, 0); /* click */
        SendMessage(c, WM_KEYUP, VK_SPACE, 0);
        return;
    }
    SetFocus(c);
    (void)dlg;
}

BOOL IsDialogMessage(HWND dlg, LPMSG m)
{
    if (!w16_valid(dlg)) return FALSE;
    if (m->hwnd != dlg && !IsChild(dlg, m->hwnd)) return FALSE;
    W16Dialog *d = w16_dlg(dlg);
    HWND f = w16_focus && IsChild(dlg, w16_focus) ? w16_focus : NULL;
    LRESULT code = f ? SendMessage(f, WM_GETDLGCODE, m->wParam, (LPARAM)m) : 0;
    switch (m->message) {
    case WM_KEYDOWN:
        switch (m->wParam) {
        case VK_TAB:
            if (code & (DLGC_WANTTAB | DLGC_WANTALLKEYS)) break;
            {
                HWND n = GetNextDlgTabItem(dlg, f, (w16_keystate[VK_SHIFT] & 0x80) != 0);
                /* a group of radio buttons is entered at its checked button (measured on 3.11: Tab
                 * into the Desktop applet's Center / Tile pair lands on the checked Tile, which is
                 * not a tab stop) */
                if (n && (SendMessage(n, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON) && !SendMessage(n, BM_GETCHECK, 0, 0)) {
                    HWND g = n;
                    for (int k = 0; k < 64; k++) {
                        g = GetNextDlgGroupItem(dlg, g, FALSE);
                        if (!g || g == n) break;
                        if ((SendMessage(g, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON) && SendMessage(g, BM_GETCHECK, 0, 0)) {
                            n = g;
                            break;
                        }
                    }
                }
                if (n) {
                    dlg_set_focus(n);
                    check_def_push(dlg, f, n);
                }
            }
            return TRUE;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            if (code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) break;
            if (f) {
                HWND n = GetNextDlgGroupItem(dlg, f, m->wParam == VK_LEFT || m->wParam == VK_UP);
                if (n && n != f) {
                    dlg_set_focus(n);
                    check_def_push(dlg, f, n);
                    if (SendMessage(n, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON) {
                        /* auto radio buttons check themselves as focus arrives */
                        if ((n->style & 0xF) == BS_AUTORADIOBUTTON) SendMessage(n, WM_KEYDOWN, VK_SPACE, 0), SendMessage(n, WM_KEYUP, VK_SPACE, 0);
                        else w16_notify_parent(n, BN_CLICKED);
                    }
                }
            }
            return TRUE;
        case VK_RETURN:
            if (code & (DLGC_WANTALLKEYS)) break;
            if (f && (code & DLGC_DEFPUSHBUTTON)) {
                SendMessage(dlg, WM_COMMAND, GetDlgCtrlID(f), MAKELPARAM(0, BN_CLICKED));
                return TRUE;
            }
            {
                int id = d ? d->defid : IDOK;
                HWND b = GetDlgItem(dlg, id);
                if (!b || IsWindowEnabled(b)) SendMessage(dlg, WM_COMMAND, id, MAKELPARAM(0, BN_CLICKED));
                else MessageBeep(0);
            }
            return TRUE;
        case VK_ESCAPE:
            if (code & DLGC_WANTALLKEYS) break;
            {
                HWND b = GetDlgItem(dlg, IDCANCEL);
                if (!b || IsWindowEnabled(b)) SendMessage(dlg, WM_COMMAND, IDCANCEL, MAKELPARAM(0, BN_CLICKED));
            }
            return TRUE;
        }
        break;
    case WM_CHAR:
        if (code & (DLGC_WANTCHARS | DLGC_WANTALLKEYS)) break;
        if (m->wParam == '\t' || m->wParam == '\r' || m->wParam == 27) return TRUE;
        {
            HWND c = find_mnemonic(dlg, (int)m->wParam, f);
            if (c) {
                activate_ctl(dlg, c);
                if (w16_focus && IsChild(dlg, w16_focus)) check_def_push(dlg, f, w16_focus);
                return TRUE;
            }
        }
        break;
    case WM_SYSCHAR:
        {
            HWND c = find_mnemonic(dlg, (int)m->wParam, f);
            if (c) {
                activate_ctl(dlg, c);
                if (w16_focus && IsChild(dlg, w16_focus)) check_def_push(dlg, f, w16_focus);
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        /* seg25:0E98: a click on a control is a focus change for the default button */
        if (m->hwnd != dlg && w16_focus) check_def_push(dlg, w16_focus, m->hwnd);
        break;
    }
    /* (USER changes no default button after other messages, whatever focus moves they cause) */
    TranslateMessage(m);
    DispatchMessage(m);
    (void)d;
    return TRUE;
}

/* ------------------------------------------------------------------ MessageBox */
/* USER seg42:04F5 lays a message box out in pixels of the system font and builds a dialog template
 * from it (seg42:0299 header, 038E items, 01E2 buttons); seg42:0101 is its dialog procedure.
 * (MB_SYSTEMMODAL without an icon or with MB_ICONHAND is 3.1's system error box, seg1:9A86, not
 * ported: such boxes are laid out like the others.) */
typedef struct { UINT type; int nb; int def; } MbData;
static int mb_nest;   /* [0x1a2] message boxes open: each further one moves by a caption button */

static BOOL mb_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INITDIALOG) {
        MbData *mb = (MbData *)lp;
        /* the default button: the def-th child (the buttons come first) */
        HWND c = h->child;
        for (int i = mb->def; c && i > 0; i--) c = c->next;
        if (c) SetFocus(c);
        /* without a Cancel button the system menu has no Close */
        if (!GetDlgItem(h, IDCANCEL)) {
            HMENU m = GetSystemMenu(h, FALSE);
            if (m) DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
        }
        /* a lone OK answers Esc and Close as Cancel (its id becomes IDCANCEL; MessageBox still
         * returns IDOK) */
        if ((mb->type & MB_TYPEMASK) == MB_OK && GetDlgItem(h, IDOK)) GetDlgItem(h, IDOK)->id = IDCANCEL;
        return FALSE;
    }
    if (m == WM_COMMAND) {
        int id = (int)wp;
        if (id < IDOK || id > IDNO) return FALSE;
        if (id <= IDCANCEL && !GetDlgItem(h, id)) return TRUE; /* Esc without a Cancel button */
        EndDialog(h, id);
        return TRUE;
    }
    return FALSE;
}

/* one item of the template (seg42:038E): pixel geometry to dialog units of the system font; a text
 * static (SS_LEFT) gets a unit more each way */
static void mb_item(W16DlgTemplate *t, int cls, const char *text, int textlen, int id, DWORD style,
                    int x, int y, int cx, int cy, int bux, int buy)
{
    int ux = MulDiv(x, 4, bux), uy = MulDiv(y, 8, buy), ucx = MulDiv(cx, 4, bux), ucy = MulDiv(cy, 8, buy);
    if (cls == 0x82 && !(style & 0xF)) { ucx++; ucy++; }
    w16_dlgt_item(t, cls, text, textlen, id, style, ux, uy, ucx, ucy);
}

int MessageBox(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
    HINSTANCE user = w16_system_module("USER.EXE");
    /* USER's button texts ([0x226]) and per type: count [0x1fe], first text [0x204], ids [0x21e] */
    static const char *defaults[8] = {"OK", "Cancel", "&Abort", "&Retry", "&Ignore", "&Yes", "&No", "Error"};
    char s[8][16];
    for (int i = 0; i < 8; i++)
        if (!user || !LoadString(user, i < 7 ? 84 + i : 78, s[i], 16)) snprintf(s[i], 16, "%s", defaults[i]);
    static const BYTE count[6] = {1, 2, 3, 3, 2, 2};
    static const BYTE label[6][3] = {{0}, {0, 1}, {2, 3, 4}, {5, 6, 1}, {5, 6}, {3, 1}};
    static const BYTE ids[6][3] = {{IDOK}, {IDOK, IDCANCEL}, {IDABORT, IDRETRY, IDIGNORE}, {IDYES, IDNO, IDCANCEL},
                                   {IDYES, IDNO}, {IDRETRY, IDCANCEL}};
    if (!caption) caption = s[7];
    if (!text) text = "";
    int kind = type & MB_TYPEMASK;
    if (kind > MB_RETRYCANCEL) kind = MB_OK;
    MbData mb = {type, count[kind], (type & MB_DEFMASK) >> 8};
    if (mb.def >= mb.nb) mb.def = 0;
    int icon = 0;
    switch (type & MB_ICONMASK) { /* seg42:00C9 */
    case MB_ICONHAND: icon = 32513; break;
    case MB_ICONQUESTION: icon = 32514; break;
    case MB_ICONEXCLAMATION: icon = 32515; break;
    case MB_ICONASTERISK: icon = 32516; break;
    }

    /* the metrics: dialog base units of the system font ([0x522], [0x52a]), half the system-menu
     * bitmap ([0x602], [0x604]), border, caption, icon, screen */
    int bux = LOWORD(GetDialogBaseUnits()), buy = HIWORD(GetDialogBaseUnits());
    int cxs = GetSystemMetrics(SM_CXSIZE), cys = GetSystemMetrics(SM_CYSIZE);
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER), cyc = GetSystemMetrics(SM_CYCAPTION);
    int scrw = GetSystemMetrics(SM_CXSCREEN), scrh = GetSystemMetrics(SM_CYSCREEN);
    int iconw = 0, iconh = 0;
    if (icon) { iconw = GetSystemMetrics(SM_CXICON) + cxs; iconh = GetSystemMetrics(SM_CYICON); }
    HDC dc = GetDC(NULL);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    /* the button width, seg3:23BE: the longest text (by length) without its '&', and two "0"s */
    int longest = 0;
    for (int i = 1; i < 7; i++)
        if (strlen(s[i]) > strlen(s[longest])) longest = i;
    char plain[16];
    int k = 0;
    for (const char *p = s[longest]; *p && k < 15; p++)
        if (*p != '&') plain[k++] = *p;
    plain[k] = 0;
    int btnw = LOWORD(GetTextExtent(dc, plain, k)) + 2 * LOWORD(GetTextExtent(dc, "0", 1));
    int btnh = buy * 14 >> 3;
    int capw = LOWORD(GetTextExtent(dc, caption, strlen(caption)));
    int btnsw = btnw * mb.nb + (mb.nb - 1) * cxs;
    int minw = max(btnsw, capw + 2 * cxs);
    /* the text wraps at 5/8 of the screen less the margins (or what the buttons and caption need) */
    int wrap = minw - 2 * (cyb + cxs) - iconw;
    wrap = max(wrap, (scrw >> 3) * 5 - 2 * (cyb + cxs) - iconw);
    RECT tr = {0, 0, wrap, wrap};
    int texth = DrawText(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);
    ReleaseDC(NULL, dc);
    int textw = tr.right - tr.left;

    /* the window: centred, cascaded, kept on the screen */
    int w = max(textw, minw) + 2 * cxs + iconw;
    int hgt = max(iconh, texth) + 6 * buy;
    int x = ((scrw - w) >> 1) + cxs * mb_nest, y = ((scrh - hgt) >> 1) + cys * mb_nest;
    if (x + w > scrw) x = scrw - 2 * cxb - w;
    if (y + hgt > scrh) y = scrh - 2 * cyb - hgt;
    int bx = ((w - btnsw) >> 1) - cxb;
    int by = hgt - 2 * cyb - (buy >> 1) - cyc;
    int ty = ((max(iconh, texth) - texth) >> 1) + buy;
    W16_LOG("MessageBox: base %dx%d btn %dx%d text %dx%d wrap %d cap %d box %d,%d %dx%d\n", bux, buy, btnw, btnh, textw,
            texth, wrap, capw, x, y, w, hgt);

    /* the template: the client area (inside the border and caption) in dialog units, absolute;
     * buttons, then the icon, then the text */
    RECT r = {x + cxb, y + cyc, x + w - cxb, y + hgt - cyb};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_ABSALIGN | DS_NOIDLEMSG |
                  ((type & (MB_SYSTEMMODAL | MB_TASKMODAL)) == MB_SYSTEMMODAL ? DS_SYSMODAL : DS_MODALFRAME);
    W16DlgTemplate *t = w16_dlgt_new(style, MulDiv(r.left, 4, bux), MulDiv(r.top, 8, buy), MulDiv(r.right - r.left, 4, bux),
                                     MulDiv(r.bottom - r.top, 8, buy), caption, 0, NULL);
    for (int i = 0; i < mb.nb; i++, bx += btnw + cxs)
        mb_item(t, 0x80, s[label[kind][i]], -1, ids[kind][i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i ? 0 : WS_GROUP) | (i == mb.def ? BS_DEFPUSHBUTTON : 0), bx,
                by - btnh, btnw, btnh, bux, buy);
    if (icon) {
        char ord[3] = {(char)0xFF, (char)(icon & 0xFF), (char)(icon >> 8)};
        mb_item(t, 0x82, ord, 3, 0xFFFF, WS_CHILD | WS_VISIBLE | WS_GROUP | SS_ICON, cxs, ((texth - iconh) >> 1) + ty, 0, 0,
                bux, buy);
    }
    mb_item(t, 0x82, text, -1, 0xFFFF, WS_CHILD | WS_VISIBLE | WS_GROUP | SS_NOPREFIX, cxs + iconw, ty, textw, texth, bux, buy);

    HWND parent = owner && w16_valid(owner) ? w16_top_level(owner) : NULL;
    mb_nest++;
    /* (3.1's MessageBox plays no sound: programs call MessageBeep themselves) */
    int res = DialogBoxIndirectParam(NULL, w16_dlgt_data(t), parent, mb_proc, (LPARAM)&mb);
    if (mb_nest) mb_nest--;
    w16_dlgt_free(t);
    if (kind == MB_OK && res) res = IDOK;
    return res;
}

/* ------------------------------------------------------------------ DlgDirList / LB_DIR */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static int cmp_name(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* a sorted list keeps directory entries in USER's order: file names, then "[dir]"s, then "[-x-]"
 * drives, each group by lstrcmpi (as on real 3.11: "<none>", "chimes.wav", "[..]", "[system]",
 * "[-a-]"); an unsorted one gets them in the order listed */
static int dir_rank(const char *s) { return s[0] != '[' ? 0 : s[1] == '-' ? 2 : 1; }
static int dir_insert(HWND lb, const char *s, int combo)
{
    BOOL sorted = combo ? (lb->style & CBS_SORT) != 0 : (lb->style & LBS_SORT) != 0;
    if (!sorted) return (int)SendMessage(lb, combo ? CB_ADDSTRING : LB_ADDSTRING, 0, (LPARAM)s);
    int n = (int)SendMessage(lb, combo ? CB_GETCOUNT : LB_GETCOUNT, 0, 0), i;
    for (i = 0; i < n; i++) {
        char t[300] = "";
        SendMessage(lb, combo ? CB_GETLBTEXT : LB_GETTEXT, i, (LPARAM)t);
        int a = dir_rank(s), b = dir_rank(t);
        if (a < b || (a == b && lstrcmpi(s, t) < 0)) break;
    }
    return (int)SendMessage(lb, combo ? CB_INSERTSTRING : LB_INSERTSTRING, i, (LPARAM)s);
}

/* LB_DIR and CB_DIR: adds what DOS lists for `spec` ("*.WAV", "C:\WINDOWS\*.*"): file names in
 * lower case, "[name]" for directories (DDL_DIRECTORY; "[..]" below a drive's root), "[-x-]" for
 * drives (DDL_DRIVES). DDL_EXCLUSIVE leaves ordinary files out; Linux dot files count as hidden
 * and need DDL_HIDDEN. Returns the index of the last entry added, or LB_ERR. */
int w16_dir_add(HWND lb, UINT attr, LPCSTR spec, int combo)
{
    char dir[300] = "", pat[260] = "*.*", full[300], host[1024];
    const char *bs = strrchr(spec, '\\');
    if (!bs && spec[0] && spec[1] == ':') bs = spec + 1;
    if (bs) {
        snprintf(dir, sizeof dir, "%.*s", (int)(bs - spec + 1), spec);
        if (bs[1]) snprintf(pat, sizeof pat, "%s", bs + 1);
    } else if (*spec)
        snprintf(pat, sizeof pat, "%s", spec);
    w16_dos_fullpath(dir[0] ? dir : ".", full, sizeof full);
    DIR *d = w16_dos_to_host(full, host, sizeof host) ? NULL : opendir(host);
    if (!d) return LB_ERR;
    char **names = NULL;
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1400], name[300];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", host, e->d_name);
        if (stat(path, &st) || (e->d_name[0] == '.' && !(attr & DDL_HIDDEN)) || !w16_wildmatch(pat, e->d_name))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!(attr & DDL_DIRECTORY)) continue;
            snprintf(name, sizeof name, "[%s]", e->d_name);
        } else {
            /* every file carries the archive bit, as DOS sets it on each write */
            int ro = access(path, W_OK) != 0;
            if ((attr & DDL_EXCLUSIVE) && !((attr & DDL_ARCHIVE) || ((attr & DDL_READONLY) && ro) ||
                                            ((attr & DDL_HIDDEN) && e->d_name[0] == '.')))
                continue;
            snprintf(name, sizeof name, "%s", e->d_name);
        }
        names = realloc(names, sizeof *names * (n + 1));
        names[n++] = strdup(name);
    }
    closedir(d);
    if (attr & DDL_DIRECTORY) {
        /* directories mounted here from elsewhere (C:\WINDOWS) */
        char sub[16][64];
        int ns = w16_mount_children(full, sub, 16);
        for (int i = 0; i < ns; i++) {
            char name[80];
            int dup = 0;
            snprintf(name, sizeof name, "[%s]", sub[i]);
            for (int k = 0; k < n && !dup; k++) dup = !strcasecmp(names[k], name);
            if (dup || !w16_wildmatch(pat, sub[i])) continue;
            names = realloc(names, sizeof *names * (n + 1));
            names[n++] = strdup(name);
        }
    }
    qsort(names, n, sizeof *names, cmp_name);
    int last = LB_ERR;
    if ((attr & DDL_DIRECTORY) && full[3] && w16_wildmatch(pat, ".."))
        last = dir_insert(lb, "[..]", combo);
    for (int i = 0; i < n; i++) {
        AnsiLower(names[i]);
        last = dir_insert(lb, names[i], combo);
        free(names[i]);
    }
    free(names);
    if (attr & DDL_DRIVES)
        for (char c = 'a'; c <= 'z'; c++) {
            if (w16_drive_root(c, NULL, 0)) continue;
            char nm[8];
            snprintf(nm, sizeof nm, "[-%c-]", c);
            last = dir_insert(lb, nm, combo);
        }
    return last;
}

static int dir_fill(HWND dlg, LPSTR path, int idlist, int idstatic, UINT attr, int combo)
{
    char spec[260] = "*.*", dir[260] = "";
    if (path && *path) {
        char *bs = strrchr(path, '\\');
        if (!bs && path[0] && path[1] == ':') bs = path + 1;
        if (strchr(path, '*') || strchr(path, '?')) {
            if (bs) { snprintf(spec, sizeof spec, "%s", bs + 1); snprintf(dir, sizeof dir, "%.*s", (int)(bs - path + 1), path); }
            else snprintf(spec, sizeof spec, "%s", path);
        } else snprintf(dir, sizeof dir, "%s", path);
    }
    /* as in 3.1, the directory named in the path becomes the current one */
    if (dir[0] && w16_chdir(dir)) return 0;
    HWND lb = idlist ? GetDlgItem(dlg, idlist) : NULL;
    if (lb) {
        SendMessage(lb, combo ? CB_RESETCONTENT : LB_RESETCONTENT, 0, 0);
        w16_dir_add(lb, attr, spec, combo);
    }
    if (idstatic) {
        char cwd[300];
        w16_getcwd(cwd, sizeof cwd);
        AnsiLower(cwd);
        SetDlgItemText(dlg, idstatic, cwd);
    }
    if (path) snprintf(path, 260, "%s", spec);
    return 1;
}
int DlgDirList(HWND dlg, LPSTR path, int idlist, int idstatic, UINT attr) { return dir_fill(dlg, path, idlist, idstatic, attr, 0); }
int DlgDirListComboBox(HWND dlg, LPSTR path, int idc, int ids, UINT attr) { return dir_fill(dlg, path, idc, ids, attr, 1); }
/* the selection as a path: "[dir]" -> "dir\", "[-c-]" -> "c:", a file name without an extension
 * gets the "." 3.1 appends; returns TRUE for a directory or drive */
static BOOL dir_select(HWND dlg, LPSTR buf, int id, int combo)
{
    char t[300] = "";
    int sel = (int)SendDlgItemMessage(dlg, id, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
    if (sel < 0) { buf[0] = 0; return FALSE; }
    SendDlgItemMessage(dlg, id, combo ? CB_GETLBTEXT : LB_GETTEXT, sel, (LPARAM)t);
    if (t[0] == '[') {
        if (t[1] == '-') snprintf(buf, 260, "%c:", t[2]);
        else { t[strlen(t) - 1] = 0; snprintf(buf, 260, "%s\\", t + 1); }
        return TRUE;
    }
    snprintf(buf, 260, "%s%s", t, strchr(t, '.') ? "" : ".");
    return FALSE;
}
BOOL DlgDirSelect(HWND dlg, LPSTR buf, int id) { return dir_select(dlg, buf, id, 0); }
BOOL DlgDirSelectComboBox(HWND dlg, LPSTR buf, int id) { return dir_select(dlg, buf, id, 1); }

int w16_wildmatch(const char *p, const char *s)
{
    /* DOS-style: "*.*" matches everything, '?' one char, case-insensitive */
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return 1;
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            for (; *s; s++) if (w16_wildmatch(p, s)) return 1;
            return !*p;
        }
        if (!*s) return *p == '.' && p[1] == '*' && !p[2];
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++; s++;
    }
    return !*s;
}
