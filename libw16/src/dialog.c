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
        /* keep on screen */
        if (wx + ww > w16_screen.w) wx = w16_screen.w - ww;
        if (wy + wh > w16_screen.h) wy = w16_screen.h - wh;
        if (wx < 0) wx = 0;
        if (wy < 0) wy = 0;
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
    dd->focus = first;
    if (SendMessage(h, WM_INITDIALOG, (WPARAM)first, lp) && w16_valid(h)) {
        /* WM_INITDIALOG may have disabled or hidden the control picked before it ran */
        if (first && !tabbable(first)) first = GetNextDlgTabItem(h, first, FALSE);
        if (first) {
            dd->focus = first;
            if (visible || modal) { /* focus is set when shown/activated */ }
        }
    } else if (w16_valid(h)) {
        /* FALSE: the proc placed the focus itself. If it did not, activation gives the focus to the
         * first tab stop enabled by then (Sound disables its lists there and OK gets it) */
        if (w16_focus && IsChild(h, w16_focus)) dd->focus = w16_focus;
        else if (dd->focus && !tabbable(dd->focus)) dd->focus = GetNextDlgTabItem(h, NULL, FALSE);
    }
    if (!w16_valid(h)) return NULL;
    if (visible || modal) {
        ShowWindow(h, SW_SHOWNORMAL);
        if (dd->focus && w16_valid(dd->focus)) SetFocus(dd->focus);
        if (dd->focus && w16_valid(dd->focus) && (SendMessage(dd->focus, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL))
            SendMessage(dd->focus, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
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
    /* USER seg25:08EB: the dialog box of a child window belongs to that window's top-level window */
    if (owner && w16_valid(owner) && (owner->style & WS_CHILD)) owner = w16_top_level(owner);
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
static void set_default_button(HWND dlg, HWND focus)
{
    W16Dialog *d = w16_dlg(dlg);
    if (!d) return;
    /* the focused push button becomes the default; otherwise the template default */
    int want = d->defid;
    if (focus && (SendMessage(focus, WM_GETDLGCODE, 0, 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) want = GetDlgCtrlID(focus);
    for (HWND c = dlg->child; c; c = c->next) {
        LRESULT code = SendMessage(c, WM_GETDLGCODE, 0, 0);
        if (code & DLGC_DEFPUSHBUTTON && GetDlgCtrlID(c) != want) SendMessage(c, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        else if (code & DLGC_UNDEFPUSHBUTTON && GetDlgCtrlID(c) == want) SendMessage(c, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
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
        if (d && LOWORD(wp) != WA_INACTIVE) {
            if (d->focus && w16_valid(d->focus) && IsChild(h, d->focus)) SetFocus(d->focus);
            else { HWND f = GetNextDlgTabItem(h, NULL, FALSE); if (f) SetFocus(f); }
            if (w16_focus && IsChild(h, w16_focus)) set_default_button(h, w16_focus);
        } else if (d) {
            if (w16_focus && IsChild(h, w16_focus)) d->focus = w16_focus;
            /* while another window is active no button of the dialog is the default (measured on
             * 3.11: behind the Edit Pattern dialog the focused "Edit Pattern..." and the Desktop's
             * OK both have the thin border, and so has OK behind a message box with the focus in
             * an edit); the default comes back with the activation */
            for (HWND c = h->child; c; c = c->next)
                if (SendMessage(c, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON) SendMessage(c, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        }
        return 0;
    case WM_SETFOCUS:
        if (d && d->focus && w16_valid(d->focus)) SetFocus(d->focus);
        return 0;
    case WM_NEXTDLGCTL: {
        HWND n;
        if (LOWORD(lp)) n = (HWND)wp; /* only usable from libw16 code (pointer handle) */
        else n = GetNextDlgTabItem(h, w16_focus, wp != 0);
        if (n) {
            SetFocus(n);
            if (SendMessage(n, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessage(n, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        }
        return 0;
    }
    case DM_GETDEFID: return d ? MAKELONG(d->defid, DC_HASDEFID) : 0;
    case DM_SETDEFID:
        if (d) { d->defid = (int)wp; set_default_button(h, w16_focus); }
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
        /* USER's DefDlgProc returns the dialog procedure's own answer to WM_INITDIALOG (seg25:051C):
         * FALSE when it placed the focus itself */
        if (m == WM_INITDIALOG) return FALSE;
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
                    SetFocus(n);
                    if (SendMessage(n, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessage(n, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                }
            }
            return TRUE;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            if (code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) break;
            if (f) {
                HWND n = GetNextDlgGroupItem(dlg, f, m->wParam == VK_LEFT || m->wParam == VK_UP);
                if (n && n != f) {
                    SetFocus(n);
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
            if (c) { activate_ctl(dlg, c); return TRUE; }
        }
        break;
    case WM_SYSCHAR:
        {
            HWND c = find_mnemonic(dlg, (int)m->wParam, f);
            if (c) { activate_ctl(dlg, c); return TRUE; }
        }
        break;
    }
    TranslateMessage(m);
    DispatchMessage(m);
    if (w16_valid(dlg) && w16_focus && IsChild(dlg, w16_focus)) {
        if (d) d->focus = w16_focus;
        set_default_button(dlg, w16_focus);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ MessageBox */
/* USER's MessageBox (seg1:9B91) and SoftModalMessageBox (seg42:04F5): the box is a dialog template
 * that USER builds in memory from pixel sizes measured in the system font, run by
 * DialogBoxIndirectParam with USER's own dialog procedure (seg42:0101). Sizes and places pass through
 * dialog units (client x = 2 * units with the system font's 8-px average), and the dialog class is
 * CS_BYTEALIGNWINDOW, so the frame's left edge then moves to the nearest multiple of 8: boxes 384 and
 * 388 px wide both start at x=128. The text is wrapped by DrawText's DT_CALCRECT, whose width of a
 * wrapped line includes the blank it broke at. It does not beep: 3.1 applications call MessageBeep
 * themselves. The tables are USER's: buttons per MB_ type, where each type starts in the button
 * list, and for each entry its label and command ID. */
static const BYTE mb_count[6] = {1, 2, 3, 3, 2, 2};
static const BYTE mb_first[6] = {0, 0, 2, 5, 5, 8};
static const BYTE mb_label[10] = {1, 2, 6, 5, 7, 3, 4, 2, 5, 2};
static const BYTE mb_cmd[10] = {IDOK, IDCANCEL, IDABORT, IDRETRY, IDIGNORE, IDYES, IDNO, IDCANCEL, IDRETRY, IDCANCEL};
/* labels 1..8 are USER strings 84, 85, 89, 90, 87, 86, 88 and 114; the default caption is 78 */
static char mb_labels[9][16] = {"", "OK", "Cancel", "&Yes", "&No", "&Retry", "&Abort", "&Ignore", "&Close"};
static char mb_error[16] = "Error";
static int mb_btnw;    /* button width (USER measures it at start-up, seg3:23BE) */
static int mb_nesting; /* boxes up: each new one cascades below and right of the last */

typedef struct {
    UINT type;
    HWND owner;
    int def;           /* the default button's place in the list */
    HWND task[64];     /* windows a task-modal box disabled */
    int ntask;
} MbState;

/* USER's own MulDiv (seg1:39AB): a * b / c, adding half of c first */
static int mb_muldiv(int a, int b, int c)
{
    if (!c) return a;
    return (int)(((long)a * b + (long)((unsigned)c >> 1)) / c);
}

/* the extent of a label without its '&' prefix ("&&" is one '&'): PSMGetTextExtent, seg1:1292 */
static int mb_label_extent(HDC dc, const char *s)
{
    char t[64];
    int n = 0;
    for (; *s && n < (int)sizeof t; s++) {
        if (*s == '&' && *++s != '&') { s--; continue; }
        t[n++] = *s;
    }
    return LOWORD(GetTextExtent(dc, t, n));
}

static void mb_init(void)
{
    static const int ids[9] = {0, 84, 85, 89, 90, 87, 86, 88, 114};
    static const int cch[9] = {0, 10, 15, 10, 10, 15, 15, 15, 15};
    HINSTANCE user = w16_system_module("USER.EXE");
    char s[16];
    if (user) {
        for (int i = 1; i <= 8; i++)
            if (LoadString(user, ids[i], s, cch[i])) strcpy(mb_labels[i], s);
        if (LoadString(user, 78, s, 10)) strcpy(mb_error, s);
    }
    /* the longest label by length (the first of equals), measured without its prefix, plus the
     * width of "0" on each side */
    int longest = 1, len = 0;
    for (int i = 1; i <= 8; i++)
        if ((int)strlen(mb_labels[i]) > len) { len = (int)strlen(mb_labels[i]); longest = i; }
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, GetStockObject(SYSTEM_FONT));
    mb_btnw = mb_label_extent(dc, mb_labels[longest]) + 2 * LOWORD(GetTextExtent(dc, "0", 1));
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
}

/* template writers: the header (seg42:0299) takes the window rectangle in pixels and stores the
 * client rectangle USER expects (one border and the caption inside it) in dialog units; an item
 * (seg42:038E) converts its pixel rectangle, and a left-aligned static gets one unit more each way */
static BYTE *mb_u16(BYTE *p, int v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); return p + 2; }
static BYTE *mb_u32(BYTE *p, DWORD v) { return mb_u16(mb_u16(p, LOWORD(v)), HIWORD(v)); }

static BYTE *mb_header(BYTE *p, DWORD style, int count, int x, int y, int w, int h, const char *caption,
                       int cxc, int cyc)
{
    RECT r;
    SetRect(&r, x, y, x + w, y + h);
    InflateRect(&r, -GetSystemMetrics(SM_CXBORDER), -GetSystemMetrics(SM_CYBORDER));
    r.top += GetSystemMetrics(SM_CYCAPTION) - GetSystemMetrics(SM_CYBORDER);
    p = mb_u32(p, style);
    *p++ = (BYTE)count;
    p = mb_u16(p, mb_muldiv(r.left, 4, cxc));
    p = mb_u16(p, mb_muldiv(r.top, 8, cyc));
    p = mb_u16(p, mb_muldiv(r.right - r.left, 4, cxc));
    p = mb_u16(p, mb_muldiv(r.bottom - r.top, 8, cyc));
    *p++ = 0; /* no menu */
    *p++ = 0; /* the dialog class */
    size_t n = strlen(caption);
    memcpy(p, caption, n);
    p += n;
    *p++ = 0;
    return p;
}

static BYTE *mb_item(BYTE *p, int cls, const char *text, int len, int x, int y, int cx, int cy, DWORD style,
                     int id, int cxc, int cyc)
{
    int dcx = mb_muldiv(cx, 4, cxc), dcy = mb_muldiv(cy, 8, cyc);
    if (cls == 0x82 && !(style & 0xF)) { dcx++; dcy++; } /* SS_LEFT */
    p = mb_u16(p, mb_muldiv(x, 4, cxc));
    p = mb_u16(p, mb_muldiv(y, 8, cyc));
    p = mb_u16(p, dcx);
    p = mb_u16(p, dcy);
    p = mb_u16(p, id);
    p = mb_u32(p, style);
    *p++ = (BYTE)cls;
    memcpy(p, text, len);
    p += len;
    if (!(len == 3 && (BYTE)text[0] == 0xFF)) *p++ = 0; /* an icon's 0xFF + ordinal has no NUL */
    *p++ = 0;                                            /* no creation data */
    return p;
}

/* seg42:0101 */
static BOOL mb_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    MbState *st = (MbState *)GetProp(h, "W16MBOX");
    if (m == WM_INITDIALOG) {
        st = (MbState *)lp;
        SetProp(h, "W16MBOX", st);
        /* TODO: a DS_SYSMODAL box becomes the system-modal window (SetSysModalWindow), which libw16
         * lacks (UNTESTED) */
        if (!st->owner && (st->type & 0x3000) == MB_TASKMODAL) {
            /* seg42:0000: with no owner, the task's enabled top-level windows are disabled */
            for (HWND c = w16_desktop->child; c && st->ntask < 64; c = c->next)
                if (c != h && !(c->style & WS_DISABLED)) st->task[st->ntask++] = c;
            for (int i = 0; i < st->ntask; i++) EnableWindow(st->task[i], FALSE);
        }
        /* the buttons are the first controls: the default one takes the focus */
        HWND c = h->child;
        for (int i = st->def; i > 0 && c; i--) c = c->next;
        if (c) SetFocus(c);
        if (!GetDlgItem(h, IDCANCEL)) {
            HMENU sm = GetSystemMenu(h, FALSE);
            if (sm) DeleteMenu(sm, SC_CLOSE, MF_BYCOMMAND);
        }
        if (!(st->type & MB_TYPEMASK)) {
            /* a lone OK also answers Esc: it takes IDCANCEL's ID (MessageBox still returns IDOK) */
            HWND ok = GetDlgItem(h, IDOK);
            if (ok) ok->id = IDCANCEL;
        }
        return FALSE;
    }
    if (m == WM_COMMAND) {
        int id = (int)wp;
        if (id < IDOK || id > IDNO) return FALSE;
        if (id <= IDCANCEL && !GetDlgItem(h, id)) return FALSE;
        if (st) {
            for (int i = 0; i < st->ntask; i++)
                if (w16_valid(st->task[i])) EnableWindow(st->task[i], TRUE);
            st->ntask = 0;
        }
        RemoveProp(h, "W16MBOX");
        EndDialog(h, id);
        return TRUE;
    }
    return FALSE;
}

int MessageBox(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
    static int inited;
    if (owner && !w16_valid(owner)) return 0; /* USER's parameter check (seg1:AB5D) fails the call */
    if (!inited) { mb_init(); inited = 1; }
    if (!caption) caption = mb_error;
    int kind = type & MB_TYPEMASK;
    if (kind > MB_RETRYCANCEL) kind = MB_OK; /* USER reads past its tables there */
    int nb = mb_count[kind];
    int def = (type & MB_DEFMASK) >> 8;
    if (def >= nb) def = 0;
    /* TODO: a system-modal box with no icon or the stop icon is USER's hard error box (seg1:9A86),
     * painted without a dialog; libw16 shows the dialog box instead (UNTESTED) */
    int icon = 0;
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: icon = 32513; break;        /* IDI_HAND */
    case MB_ICONQUESTION: icon = 32514; break;    /* IDI_QUESTION */
    case MB_ICONEXCLAMATION: icon = 32515; break; /* IDI_EXCLAMATION */
    case MB_ICONASTERISK: icon = 32516; break;    /* IDI_ASTERISK */
    }
    DWORD units = GetDialogBaseUnits();
    int cxc = LOWORD(units), cyc = HIWORD(units);
    int cxb = GetSystemMetrics(SM_CXBORDER), cyb = GetSystemMetrics(SM_CYBORDER);
    int gap = GetSystemMetrics(SM_CXSIZE), cap = GetSystemMetrics(SM_CYCAPTION);
    int cxs = GetSystemMetrics(SM_CXSCREEN), cys = GetSystemMetrics(SM_CYSCREEN);
    int iconw = 0, iconh = 0;
    if (icon) { iconw = GetSystemMetrics(SM_CXICON) + gap; iconh = GetSystemMetrics(SM_CYICON); }

    /* the width wanted by the buttons or the caption, and the text wrapped to that less the margins
     * and icon, but to no less than 5/8 of the screen less them */
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, GetStockObject(SYSTEM_FONT));
    int caplen = (int)strlen(caption);
    int capw = LOWORD(GetTextExtent(dc, caption, caplen));
    int btnsw = mb_btnw * nb + (nb - 1) * gap;
    int minw = max(btnsw, 2 * gap + capw);
    int margins = 2 * (cyb + gap);
    int tw = max(minw - margins - iconw, (cxs >> 3) * 5 - margins - iconw);
    RECT r;
    SetRect(&r, 0, 0, tw, tw);
    int texth = DrawText(dc, text ? text : "", -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
    int textw = r.right - r.left;

    /* the box: centred (each open box moves it on by a system-menu box), kept on the screen */
    int w = max(textw, minw) + 2 * gap + iconw;
    int bodyh = max(iconh, texth);
    int h = bodyh + 6 * cyc;
    int x = ((cxs - w) >> 1) + gap * mb_nesting;
    int y = ((cys - h) >> 1) + GetSystemMetrics(SM_CYSIZE) * mb_nesting;
    if (x + w > cxs) x = cxs - 2 * cxb - w;
    if (y + h > cys) y = cys - 2 * cyb - h;
    int bx = ((w - btnsw) >> 1) - cxb;           /* first button, client coordinates */
    int bottom = h - 2 * cyb - (cyc >> 1) - cap; /* where the buttons end */
    int texty = ((bodyh - texth) >> 1) + cyc;

    /* the template: buttons first, then the icon and the text */
    size_t size = 16 + caplen + (icon ? 19 : 0) + (text ? 17 + strlen(text) : 0);
    for (int i = 0; i < nb; i++) size += 17 + strlen(mb_labels[mb_label[mb_first[kind] + i]]);
    BYTE *tmpl = malloc(size), *p;
    if (!tmpl) return 0;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_NOIDLEMSG | DS_ABSALIGN |
                  ((type & 0x3000) == MB_SYSTEMMODAL ? DS_SYSMODAL : DS_MODALFRAME);
    p = mb_header(tmpl, style, nb + (icon != 0) + (text != NULL), x, y, w, h, caption, cxc, cyc);
    int btnh = (cyc * 14) >> 3;
    for (int i = 0; i < nb; i++) {
        int k = mb_first[kind] + i;
        const char *s = mb_labels[mb_label[k]];
        DWORD bs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 0 ? WS_GROUP : 0) | (i == def ? BS_DEFPUSHBUTTON : 0);
        p = mb_item(p, 0x80, s, (int)strlen(s), bx + i * (mb_btnw + gap), bottom - btnh, mb_btnw, btnh, bs,
                    mb_cmd[k], cxc, cyc);
    }
    if (icon) {
        char ord[3] = {(char)0xFF, (char)(icon & 0xFF), (char)(icon >> 8)};
        p = mb_item(p, 0x82, ord, 3, gap, ((texth - iconh) >> 1) + texty, 0, 0,
                    WS_CHILD | WS_VISIBLE | WS_GROUP | SS_ICON, -1, cxc, cyc);
    }
    if (text)
        p = mb_item(p, 0x82, text, (int)strlen(text), gap + iconw, texty, textw, texth,
                    WS_CHILD | WS_VISIBLE | WS_GROUP | SS_NOPREFIX | SS_LEFT, -1, cxc, cyc);

    MbState st;
    memset(&st, 0, sizeof st);
    st.type = type;
    st.owner = owner;
    st.def = def;
    mb_nesting++;
    HCURSOR oldcur = SetCursor(LoadCursor(NULL, IDC_ARROW));
    int ret = DialogBoxIndirectParam(NULL, tmpl, owner, mb_proc, (LPARAM)&st);
    if (ret == -1) ret = 0;
    if (!(type & MB_TYPEMASK) && ret) ret = IDOK;
    if (mb_nesting) mb_nesting--;
    free(tmpl);
    if (oldcur) SetCursor(oldcur);
    return ret;
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
