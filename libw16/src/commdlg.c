/* COMMDLG.DLL: File Open / Save As, Find / Replace, Print / Print Setup.
 *
 * Native reimplementation. The dialog templates (1536-1541), strings and the folder/drive
 * bitmap strip (576) are loaded at run time from the user's ripped COMMDLG.DLL, so the
 * dialogs look like the originals without any Microsoft material in this file.
 * Behaviour follows the documented 3.1 COMMDLG API; layout constants marked MEASURE are
 * to be confirmed against the DOSBox-X reference screenshots. */
#include "w16int.h"
#include "commdlg.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* control ids used by the COMMDLG templates (dlgs.h) */
enum {
    psh1 = 1024, psh2 = 1025, pshHelp = 1038, chx1 = 1040, chx2 = 1041,
    grp1 = 1072, rad1 = 1056, rad2 = 1057, rad3 = 1058, rad4 = 1059,
    stc1 = 1088, stc2 = 1089, stc3 = 1090, stc4 = 1091, stc5 = 1092,
    lst1 = 1120, lst2 = 1121, cmb1 = 1136, cmb2 = 1137, cmb3 = 1138,
    edt1 = 1152, edt2 = 1153, edt3 = 1154, ico1 = 1084,
};
enum { DLG_OPEN = 1536, DLG_SAVE = 1537, DLG_PRINT = 1538, DLG_SETUP = 1539, DLG_FIND = 1540, DLG_REPLACE = 1541 };

/* COMMDLG string ids */
enum {
    IDS_FILEEXISTS = 257, IDS_OPEN = 384, IDS_SAVEAS = 385, IDS_SAVETYPE = 386, IDS_NODRIVE = 387,
    IDS_FILENOTFOUND = 391, IDS_PATHNOTFOUND = 392, IDS_BADNAME = 393, IDS_READONLY = 396,
    IDS_CANTSELDRIVE = 404, IDS_PRINTERLABEL = 1089, IDS_PRNONPORT = 1090,
    IDS_FROMLOW = 1104, IDS_FROMHIGH = 1105, IDS_TOLOW = 1106, IDS_TOHIGH = 1107, IDS_FROMBAD = 1108,
    IDS_TOBAD = 1109, IDS_NOPAGES = 1110, IDS_COPIESEMPTY = 1111, IDS_COPIESBAD = 1112, IDS_COPIESZERO = 1113,
    IDS_NODEFPRN = 1114, IDS_QUALITY = 1072,
};

/* bitmap strip 576: 8 cells of 16x16, pure blue is transparent */
enum { BMP_OPENDIR, BMP_CURDIR, BMP_CLOSEDDIR, BMP_FLOPPY, BMP_HARDDRV, BMP_CDDRV, BMP_NETDRV, BMP_RAMDRV };
#define DX_BMP 16
#define DY_BMP 16
#define DX_INDENT 8 /* MEASURE: per-level indent in the Directories list */

static DWORD cd_err;
DWORD CommDlgExtendedError(void) { return cd_err; }

static HINSTANCE commdlg(void) { return w16_system_module("COMMDLG.DLL"); }

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

static int has_wild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

static uint32_t cref_to_px(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

/* ================================================================== File Open / Save As */
typedef struct {
    OPENFILENAME *ofn;
    int save;
    char spec[260];       /* current pattern(s), e.g. "*.txt" or "*.doc;*.wri" */
    char start_dir[300];  /* restored for OFN_NOCHANGEDIR / Cancel */
    HBITMAP bmp[2];       /* strip with transparency replaced by window / highlight colour */
    int item_h;
} FileDlg;
static FileDlg *fd_cur;

static void fd_load_bitmaps(FileDlg *fd)
{
    COLORREF bg[2] = {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_HIGHLIGHT)};
    for (int k = 0; k < 2; k++) {
        fd->bmp[k] = LoadBitmap(commdlg(), MAKEINTRESOURCE(576));
        if (!fd->bmp[k]) continue;
        W16Bitmap *b = &fd->bmp[k]->u.bmp;
        for (int i = 0; i < b->w * b->h; i++)
            if (b->px[i] == 0x0000FF) b->px[i] = cref_to_px(bg[k]);
    }
}

/* nth (0-based) "description\0pattern\0" pair of a filter list */
static const char *filter_pattern(const char *f, int n)
{
    for (int i = 0; f && *f; i++) {
        const char *pat = f + strlen(f) + 1;
        if (i == n) return pat;
        f = pat + strlen(pat) + 1;
    }
    return NULL;
}

static int spec_match(const char *spec, const char *name)
{
    char buf[260], *save;
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        while (*p == ' ') p++;
        if (w16_wildmatch(p, name)) return 1;
    }
    return 0;
}

static void fd_fill_files(HWND dlg, FileDlg *fd)
{
    HWND lb = GetDlgItem(dlg, lst1);
    char host[2048], cwd[300];
    SendMessage(lb, WM_SETREDRAW, FALSE, 0);
    SendMessage(lb, LB_RESETCONTENT, 0, 0);
    w16_getcwd(cwd, sizeof cwd);
    DIR *d = w16_dos_to_host(cwd, host, sizeof host) ? NULL : opendir(host);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue; /* also hides Linux dot files, like DOS hidden files */
        char full[2400];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        if (stat(full, &st) || !S_ISREG(st.st_mode) || !spec_match(fd->spec, e->d_name)) continue;
        char name[260];
        snprintf(name, sizeof name, "%s", e->d_name);
        AnsiLower(name);
        SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)name);
    }
    if (d) closedir(d);
    SendMessage(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void fd_fill_dirs(HWND dlg)
{
    HWND lb = GetDlgItem(dlg, lst2);
    char cwd[300], host[2048];
    w16_getcwd(cwd, sizeof cwd);
    SendMessage(lb, WM_SETREDRAW, FALSE, 0);
    SendMessage(lb, LB_RESETCONTENT, 0, 0);

    /* the open path: "c:\" then each component, the last one is the current directory */
    char root[4] = {cwd[0], ':', '\\', 0}, buf[300], *save;
    AnsiLower(root);
    int level = 0, cur;
    snprintf(buf, sizeof buf, "%s", cwd + 3);
    char *comp[64];
    int n = 0;
    for (char *c = strtok_r(buf, "\\", &save); c && n < 64; c = strtok_r(NULL, "\\", &save)) comp[n++] = c;
    cur = (int)SendMessage(lb, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)root);
    SendMessage(lb, LB_SETITEMDATA, cur, MAKELONG(0, n ? BMP_OPENDIR : BMP_CURDIR));
    for (int i = 0; i < n; i++) {
        char t[260];
        snprintf(t, sizeof t, "%s", comp[i]);
        AnsiLower(t);
        cur = (int)SendMessage(lb, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)t);
        SendMessage(lb, LB_SETITEMDATA, cur, MAKELONG(++level, i == n - 1 ? BMP_CURDIR : BMP_OPENDIR));
    }

    /* subdirectories of the current directory, sorted */
    char **sub = NULL;
    int ns = 0;
    DIR *d = w16_dos_to_host(cwd, host, sizeof host) ? NULL : opendir(host);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char full[2400];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        if (stat(full, &st) || !S_ISDIR(st.st_mode)) continue;
        sub = realloc(sub, sizeof *sub * (ns + 1));
        sub[ns++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    qsort(sub, ns, sizeof *sub, cmp_str);
    for (int i = 0; i < ns; i++) {
        AnsiLower(sub[i]);
        int k = (int)SendMessage(lb, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)sub[i]);
        SendMessage(lb, LB_SETITEMDATA, k, MAKELONG(level + 1, BMP_CLOSEDDIR));
        free(sub[i]);
    }
    free(sub);
    SendMessage(lb, LB_SETCURSEL, cur, 0);
    SendMessage(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);

    char shown[300];
    snprintf(shown, sizeof shown, "%s", cwd);
    AnsiLower(shown);
    SetDlgItemText(dlg, stc1, shown);
}

static int drive_bitmap(char letter)
{
    char root[1024];
    if (w16_drive_root(letter, root, sizeof root)) return BMP_HARDDRV;
    if (!strncmp(root, "/media/", 7) || !strncmp(root, "/run/media/", 11)) return BMP_FLOPPY;
    if (strstr(root, "cdrom") || strstr(root, "/sr0")) return BMP_CDDRV;
    if (!strncmp(root, "/net/", 5) || !strncmp(root, "/run/user/", 10)) return BMP_NETDRV;
    if (!strncmp(root, "/tmp", 4) || !strncmp(root, "/dev/shm", 8)) return BMP_RAMDRV;
    return BMP_HARDDRV;
}

static void fd_fill_drives(HWND dlg)
{
    HWND cb = GetDlgItem(dlg, cmb2);
    char cwd[300];
    w16_getcwd(cwd, sizeof cwd);
    SendMessage(cb, CB_RESETCONTENT, 0, 0);
    for (char c = 'A'; c <= 'Z'; c++) {
        char root[1024];
        if (w16_drive_root(c, root, sizeof root)) continue;
        /* "c: label"; Linux has no volume labels, so the label is the mapped folder's name */
        const char *label = strrchr(root, '/');
        label = label && label[1] ? label + 1 : "";
        char t[300];
        snprintf(t, sizeof t, "%c: %s", c, label);
        AnsiLower(t);
        int k = (int)SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)t);
        SendMessage(cb, CB_SETITEMDATA, k, MAKELONG(0, drive_bitmap(c)));
        if (toupper((unsigned char)cwd[0]) == c) SendMessage(cb, CB_SETCURSEL, k, 0);
    }
}

static void fd_refresh(HWND dlg, FileDlg *fd)
{
    fd_fill_files(dlg, fd);
    fd_fill_dirs(dlg);
    fd_fill_drives(dlg);
}

static void fd_set_edit_spec(HWND dlg, FileDlg *fd)
{
    char t[260];
    snprintf(t, sizeof t, "%s", fd->spec);
    AnsiLower(t);
    SetDlgItemText(dlg, edt1, t);
    SendDlgItemMessage(dlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
}

/* change to the directory built from the Directories list item `sel` */
static void fd_dir_from_list(HWND dlg, FileDlg *fd, int sel)
{
    HWND lb = GetDlgItem(dlg, lst2);
    char path[300] = "", t[260];
    DWORD data = (DWORD)SendMessage(lb, LB_GETITEMDATA, sel, 0);
    if (LOWORD(data) == 0) {
        SendMessage(lb, LB_GETTEXT, 0, (LPARAM)t);
        snprintf(path, sizeof path, "%s", t);
    } else {
        /* items 0..level-1 are the open path; `sel` is either on it or a subdirectory */
        int target = LOWORD(data);
        SendMessage(lb, LB_GETTEXT, 0, (LPARAM)path);
        for (int i = 1, n = (int)SendMessage(lb, LB_GETCOUNT, 0, 0); i < n; i++) {
            DWORD d = (DWORD)SendMessage(lb, LB_GETITEMDATA, i, 0);
            if (HIWORD(d) == BMP_CLOSEDDIR && i != sel) continue;
            if ((int)LOWORD(d) > target) break;
            SendMessage(lb, LB_GETTEXT, i, (LPARAM)t);
            size_t L = strlen(path);
            snprintf(path + L, sizeof path - L, "%s%s", path[L - 1] == '\\' ? "" : "\\", t);
            if (i == sel) break;
        }
    }
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (w16_chdir(path) == 0) fd_refresh(dlg, fd);
    else cd_msg(dlg, IDS_PATHNOTFOUND, path, MB_OK | MB_ICONEXCLAMATION);
    SetCursor(old);
    fd_set_edit_spec(dlg, fd);
}

static void fd_restore_dir(FileDlg *fd)
{
    if (fd->start_dir[0]) w16_chdir(fd->start_dir);
}

static BOOL host_exists(const char *dos, int *is_dir, int *ro)
{
    char host[2048];
    struct stat st;
    if (w16_dos_to_host(dos, host, sizeof host) || stat(host, &st)) return FALSE;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (ro) *ro = access(host, W_OK) != 0;
    return TRUE;
}

/* OK pressed (or a file double-clicked). Returns TRUE when the dialog should close. */
static BOOL fd_ok(HWND dlg, FileDlg *fd)
{
    OPENFILENAME *ofn = fd->ofn;
    if (GetFocus() == GetDlgItem(dlg, lst2)) {
        fd_dir_from_list(dlg, fd, (int)SendDlgItemMessage(dlg, lst2, LB_GETCURSEL, 0, 0));
        return FALSE;
    }
    char text[260];
    GetDlgItemText(dlg, edt1, text, sizeof text);
    char *t = text;
    while (*t == ' ') t++;
    for (size_t L = strlen(t); L && t[L - 1] == ' '; L--) t[L - 1] = 0;
    if (!*t) { MessageBeep(0); return FALSE; }

    /* split "dir\name" */
    char dir[260] = "", name[260];
    char *bs = strrchr(t, '\\');
    if (!bs && t[1] == ':') bs = t + 1;
    if (bs) { snprintf(dir, sizeof dir, "%.*s", (int)(bs - t + 1), t); snprintf(name, sizeof name, "%s", bs + 1); }
    else snprintf(name, sizeof name, "%s", t);
    if (dir[0] && strlen(dir) > 3 && dir[strlen(dir) - 1] == '\\') dir[strlen(dir) - 1] = 0;

    if (has_wild(name)) { /* a new pattern, maybe in another directory */
        if (dir[0]) {
            int r = w16_chdir(dir);
            if (r) { cd_msg(dlg, r == -2 ? IDS_NODRIVE : IDS_PATHNOTFOUND, r == -2 ? dir : t, MB_OK | MB_ICONEXCLAMATION); return FALSE; }
        }
        snprintf(fd->spec, sizeof fd->spec, "%s", name);
        fd_refresh(dlg, fd);
        fd_set_edit_spec(dlg, fd);
        return FALSE;
    }
    int is_dir = 0, ro = 0;
    if (host_exists(t, &is_dir, NULL) && is_dir) { /* a directory: go there */
        w16_chdir(t);
        fd_refresh(dlg, fd);
        fd_set_edit_spec(dlg, fd);
        return FALSE;
    }
    if (dir[0]) {
        int dd = 0;
        if (!host_exists(dir, &dd, NULL) || !dd) {
            cd_msg(dlg, w16_drive_root(dir[1] == ':' ? dir[0] : 'C', NULL, 0) ? IDS_NODRIVE : IDS_PATHNOTFOUND, t, MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
    }

    /* the full DOS path, with the default extension when the name has none */
    char full[300];
    OFSTRUCT of;
    OpenFile(t, &of, OF_PARSE);
    snprintf(full, sizeof full, "%s", of.szPathName);
    char *base = strrchr(full, '\\') + 1;
    if (!strchr(base, '.') && ofn->lpstrDefExt && *ofn->lpstrDefExt) {
        size_t L = strlen(full);
        snprintf(full + L, sizeof full - L, ".%s", ofn->lpstrDefExt);
    }
    AnsiUpper(full);
    base = strrchr(full, '\\') + 1;
    if (strlen(base) == 0 || strpbrk(base, "\"/[]:|<>+=;,")) {
        cd_msg(dlg, IDS_BADNAME, base, MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }

    int exists = host_exists(full, &is_dir, &ro);
    if (!fd->save && (ofn->Flags & OFN_FILEMUSTEXIST) && !exists) {
        cd_msg(dlg, IDS_FILENOTFOUND, base, MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }
    if (fd->save && exists) {
        if ((ofn->Flags & OFN_NOREADONLYRETURN) && ro) {
            cd_msg(dlg, IDS_READONLY, full, MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
        if ((ofn->Flags & OFN_OVERWRITEPROMPT)) {
            char fmt[200], msg[500], cap[80];
            cd_str(IDS_FILEEXISTS, fmt, sizeof fmt);
            snprintf(msg, sizeof msg, fmt, full);
            GetWindowText(dlg, cap, sizeof cap);
            if (MessageBox(dlg, msg, cap, MB_YESNO | MB_ICONEXCLAMATION) != IDYES) return FALSE;
        }
    }

    if (strlen(full) + 1 > ofn->nMaxFile) {
        cd_err = FNERR_BUFFERTOOSMALL;
        if (ofn->nMaxFile >= 2) { ofn->lpstrFile[0] = (char)strlen(full); ofn->lpstrFile[1] = 0; }
        EndDialog(dlg, FALSE);
        return TRUE;
    }
    strcpy(ofn->lpstrFile, full);
    ofn->nFileOffset = (UINT)(base - full);
    char *dot = strrchr(base, '.');
    ofn->nFileExtension = dot ? (UINT)(dot + 1 - full) : (UINT)strlen(full);
    if (dot && ofn->lpstrDefExt && strcasecmp(dot + 1, ofn->lpstrDefExt)) ofn->Flags |= OFN_EXTENSIONDIFFERENT;
    else ofn->Flags &= ~OFN_EXTENSIONDIFFERENT;
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) snprintf(ofn->lpstrFileTitle, ofn->nMaxFileTitle, "%s", base);
    if (IsDlgButtonChecked(dlg, chx1)) ofn->Flags |= OFN_READONLY;
    else ofn->Flags &= ~OFN_READONLY;
    int fi = (int)SendDlgItemMessage(dlg, cmb1, CB_GETCURSEL, 0, 0);
    if (fi >= 0) ofn->nFilterIndex = fi + 1;
    if (ofn->Flags & OFN_NOCHANGEDIR) fd_restore_dir(fd);
    EndDialog(dlg, TRUE);
    return TRUE;
}

static void fd_draw(FileDlg *fd, DRAWITEMSTRUCT *di)
{
    HWND ctl = di->hwndItem;
    int sel = (di->itemState & ODS_SELECTED) != 0;
    if (di->itemAction == ODA_FOCUS) { DrawFocusRect(di->hDC, &di->rcItem); return; }
    HFONT f = (HFONT)SendMessage(ctl, WM_GETFONT, 0, 0);
    HFONT of = f ? SelectObject(di->hDC, f) : NULL;
    FillRect(di->hDC, &di->rcItem, w16_sys_brush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    if ((int)di->itemID >= 0) {
        char t[300] = "";
        int combo = di->CtlType == ODT_COMBOBOX;
        SendMessage(ctl, combo ? CB_GETLBTEXT : LB_GETTEXT, di->itemID, (LPARAM)t);
        TEXTMETRIC tm;
        GetTextMetrics(di->hDC, &tm);
        int x = di->rcItem.left + 1, h = di->rcItem.bottom - di->rcItem.top;
        if (di->CtlID != lst1) {
            int level = LOWORD(di->itemData), b = HIWORD(di->itemData);
            x += level * DX_INDENT;
            HDC mdc = CreateCompatibleDC(di->hDC);
            HGDIOBJ ob = SelectObject(mdc, fd->bmp[sel]);
            BitBlt(di->hDC, x, di->rcItem.top + (h - DY_BMP) / 2, DX_BMP, DY_BMP, mdc, b * DX_BMP, 0, SRCCOPY);
            SelectObject(mdc, ob);
            DeleteDC(mdc);
            x += DX_BMP + 3; /* MEASURE */
        } else
            x += 1;
        SetBkMode(di->hDC, TRANSPARENT);
        COLORREF tc = sel ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                          : (fd->save && di->CtlID == lst1) ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_WINDOWTEXT);
        SetTextColor(di->hDC, tc);
        TextOut(di->hDC, x, di->rcItem.top + (h - tm.tmHeight) / 2, t, strlen(t));
    }
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
    if (of) SelectObject(di->hDC, of);
}

static BOOL FileDlgProc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    FileDlg *fd = fd_cur;
    OPENFILENAME *ofn = fd->ofn;
    if ((ofn->Flags & OFN_ENABLEHOOK) && ofn->lpfnHook && m != WM_INITDIALOG && ofn->lpfnHook(dlg, m, wp, lp))
        return TRUE;
    switch (m) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        HDC dc = GetDC(dlg);
        HFONT f = (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0);
        HFONT of = f ? SelectObject(dc, f) : NULL;
        TEXTMETRIC tm;
        GetTextMetrics(dc, &tm);
        if (of) SelectObject(dc, of);
        ReleaseDC(dlg, dc);
        mi->itemHeight = mi->CtlID == lst1 ? (UINT)tm.tmHeight : (UINT)max(tm.tmHeight, DY_BMP);
        return TRUE;
    }
    case WM_DRAWITEM:
        fd_draw(fd, (DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_INITDIALOG: {
        char t[260];
        if (ofn->lpstrTitle) SetWindowText(dlg, ofn->lpstrTitle);
        else { cd_str(fd->save ? IDS_SAVEAS : IDS_OPEN, t, sizeof t); SetWindowText(dlg, t); }
        if (fd->save) { cd_str(IDS_SAVETYPE, t, sizeof t); SetDlgItemText(dlg, stc2, t); }
        if (ofn->Flags & OFN_HIDEREADONLY) ShowWindow(GetDlgItem(dlg, chx1), SW_HIDE);
        else CheckDlgButton(dlg, chx1, (ofn->Flags & OFN_READONLY) != 0);
        if (!(ofn->Flags & OFN_SHOWHELP)) ShowWindow(GetDlgItem(dlg, pshHelp), SW_HIDE);
        SendDlgItemMessage(dlg, edt1, EM_LIMITTEXT, 127, 0);

        /* file types */
        HWND cb = GetDlgItem(dlg, cmb1);
        int idx = 0;
        for (const char *f = ofn->lpstrFilter; f && *f; idx++) {
            SendMessage(cb, CB_ADDSTRING, 0, (LPARAM)f);
            const char *pat = f + strlen(f) + 1;
            f = pat + strlen(pat) + 1;
        }
        int fi = ofn->nFilterIndex ? (int)ofn->nFilterIndex - 1 : 0;
        if (fi >= idx) fi = 0;
        SendMessage(cb, CB_SETCURSEL, fi, 0);
        const char *pat = filter_pattern(ofn->lpstrFilter, fi);
        if (ofn->lpstrCustomFilter && ofn->lpstrCustomFilter[0] && !ofn->nFilterIndex)
            pat = ofn->lpstrCustomFilter + strlen(ofn->lpstrCustomFilter) + 1;
        snprintf(fd->spec, sizeof fd->spec, "%s", pat && *pat ? pat : "*.*");

        /* initial directory and name */
        if (ofn->lpstrInitialDir && *ofn->lpstrInitialDir) w16_chdir(ofn->lpstrInitialDir);
        char file[260] = "";
        if (ofn->lpstrFile && *ofn->lpstrFile) {
            snprintf(file, sizeof file, "%s", ofn->lpstrFile);
            char *bs = strrchr(file, '\\');
            if (bs) {
                char d[260];
                snprintf(d, sizeof d, "%.*s", (int)(bs - file + (bs - file == 2 ? 1 : 0)), file);
                w16_chdir(d);
                memmove(file, bs + 1, strlen(bs + 1) + 1);
            }
            if (has_wild(file)) { snprintf(fd->spec, sizeof fd->spec, "%s", file); file[0] = 0; }
        }
        fd_refresh(dlg, fd);
        if (file[0]) {
            AnsiLower(file);
            SetDlgItemText(dlg, edt1, file);
            SendDlgItemMessage(dlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        } else
            fd_set_edit_spec(dlg, fd);
        if ((ofn->Flags & OFN_ENABLEHOOK) && ofn->lpfnHook) return ofn->lpfnHook(dlg, m, wp, (LPARAM)ofn);
        SetFocus(GetDlgItem(dlg, edt1));
        return FALSE;
    }
    case WM_COMMAND: {
        int code = HIWORD(lp);
        switch (wp) {
        case IDOK:
            fd_ok(dlg, fd);
            return TRUE;
        case IDCANCEL:
            fd_restore_dir(fd);
            EndDialog(dlg, FALSE);
            return TRUE;
        case pshHelp:
            if (ofn->hwndOwner) SendMessage(ofn->hwndOwner, RegisterWindowMessage(HELPMSGSTRING), (WPARAM)0, (LPARAM)ofn);
            return TRUE;
        case lst1:
            if (code == LBN_SELCHANGE || code == LBN_DBLCLK) {
                char t[260];
                int sel = (int)SendDlgItemMessage(dlg, lst1, LB_GETCURSEL, 0, 0);
                if (sel < 0) return TRUE;
                SendDlgItemMessage(dlg, lst1, LB_GETTEXT, sel, (LPARAM)t);
                SetDlgItemText(dlg, edt1, t);
                if (code == LBN_DBLCLK) fd_ok(dlg, fd);
            }
            return TRUE;
        case lst2:
            if (code == LBN_DBLCLK) fd_dir_from_list(dlg, fd, (int)SendDlgItemMessage(dlg, lst2, LB_GETCURSEL, 0, 0));
            return TRUE;
        case cmb1:
            if (code == CBN_SELCHANGE) {
                const char *p = filter_pattern(ofn->lpstrFilter, (int)SendDlgItemMessage(dlg, cmb1, CB_GETCURSEL, 0, 0));
                if (p) {
                    snprintf(fd->spec, sizeof fd->spec, "%s", p);
                    fd_fill_files(dlg, fd);
                    fd_set_edit_spec(dlg, fd);
                }
            }
            return TRUE;
        case cmb2:
            if (code == CBN_SELCHANGE) {
                char t[300];
                int sel = (int)SendDlgItemMessage(dlg, cmb2, CB_GETCURSEL, 0, 0);
                SendDlgItemMessage(dlg, cmb2, CB_GETLBTEXT, sel, (LPARAM)t);
                char drv[3] = {t[0], ':', 0};
                if (w16_chdir(drv)) cd_msg(dlg, IDS_CANTSELDRIVE, drv, MB_OK | MB_ICONEXCLAMATION);
                fd_refresh(dlg, fd);
                fd_set_edit_spec(dlg, fd);
            }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static BOOL file_dialog(OPENFILENAME *ofn, int save)
{
    cd_err = 0;
    if (!ofn || ofn->lStructSize != sizeof *ofn) { cd_err = CDERR_STRUCTSIZE; return FALSE; }
    if (!ofn->lpstrFile) { cd_err = CDERR_INITIALIZATION; return FALSE; }
    HINSTANCE inst = commdlg();
    LPCSTR tmpl = MAKEINTRESOURCE(save ? DLG_SAVE : DLG_OPEN);
    if (ofn->Flags & OFN_ENABLETEMPLATE) { inst = ofn->hInstance; tmpl = ofn->lpTemplateName; }
    if (!inst || !w16_find_res(inst, tmpl, RT_DIALOG)) { cd_err = inst ? CDERR_FINDRESFAILURE : CDERR_LOADRESFAILURE; return FALSE; }

    FileDlg fd = {ofn, save};
    w16_getcwd(fd.start_dir, sizeof fd.start_dir);
    fd_load_bitmaps(&fd);
    FileDlg *outer = fd_cur;
    fd_cur = &fd;
    int r = DialogBoxParam(inst, tmpl, ofn->hwndOwner, FileDlgProc, (LPARAM)ofn);
    fd_cur = outer;
    for (int k = 0; k < 2; k++) if (fd.bmp[k]) DeleteObject(fd.bmp[k]);
    if (r < 0) { cd_err = CDERR_DIALOGFAILURE; return FALSE; }
    return r > 0 && !cd_err;
}

BOOL GetOpenFileName(OPENFILENAME *ofn) { return file_dialog(ofn, 0); }
BOOL GetSaveFileName(OPENFILENAME *ofn) { return file_dialog(ofn, 1); }

int GetFileTitle(LPCSTR file, LPSTR title, UINT cb)
{
    const char *b = file;
    for (const char *p = file; *p; p++) if (*p == '\\' || *p == ':' || *p == '/') b = p + 1;
    if (!*b || strpbrk(b, "*?")) return -1;
    if (strlen(b) + 1 > cb) return (int)strlen(b) + 1;
    strcpy(title, b);
    return 0;
}

/* ================================================================== Find / Replace (modeless) */
static UINT fr_msg(void) { return RegisterWindowMessage(FINDMSGSTRING); }

static void fr_update_buttons(HWND dlg)
{
    int has = GetWindowTextLength(GetDlgItem(dlg, edt1)) > 0;
    EnableWindow(GetDlgItem(dlg, IDOK), has);
    if (GetDlgItem(dlg, psh1)) EnableWindow(GetDlgItem(dlg, psh1), has);
    if (GetDlgItem(dlg, psh2)) EnableWindow(GetDlgItem(dlg, psh2), has);
}

static void fr_notify(HWND dlg, FINDREPLACE *fr, DWORD action)
{
    GetDlgItemText(dlg, edt1, fr->lpstrFindWhat, fr->wFindWhatLen);
    if (fr->lpstrReplaceWith && GetDlgItem(dlg, edt2)) GetDlgItemText(dlg, edt2, fr->lpstrReplaceWith, fr->wReplaceWithLen);
    fr->Flags &= ~(FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL | FR_DIALOGTERM | FR_MATCHCASE | FR_WHOLEWORD);
    if (IsDlgButtonChecked(dlg, chx2)) fr->Flags |= FR_MATCHCASE;
    if (IsDlgButtonChecked(dlg, chx1)) fr->Flags |= FR_WHOLEWORD;
    if (GetDlgItem(dlg, rad2)) {
        if (IsDlgButtonChecked(dlg, rad2)) fr->Flags |= FR_DOWN;
        else fr->Flags &= ~FR_DOWN;
    }
    fr->Flags |= action;
    SendMessage(fr->hwndOwner, fr_msg(), 0, (LPARAM)fr);
}

static BOOL FindDlgProc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    FINDREPLACE *fr = (FINDREPLACE *)GetProp(dlg, "W16FR");
    switch (m) {
    case WM_INITDIALOG:
        fr = (FINDREPLACE *)lp;
        SetProp(dlg, "W16FR", (HANDLE)fr);
        SendDlgItemMessage(dlg, edt1, EM_LIMITTEXT, fr->wFindWhatLen ? fr->wFindWhatLen - 1 : 0, 0);
        SetDlgItemText(dlg, edt1, fr->lpstrFindWhat);
        if (GetDlgItem(dlg, edt2) && fr->lpstrReplaceWith) {
            SendDlgItemMessage(dlg, edt2, EM_LIMITTEXT, fr->wReplaceWithLen ? fr->wReplaceWithLen - 1 : 0, 0);
            SetDlgItemText(dlg, edt2, fr->lpstrReplaceWith);
        }
        CheckDlgButton(dlg, chx2, (fr->Flags & FR_MATCHCASE) != 0);
        CheckDlgButton(dlg, chx1, (fr->Flags & FR_WHOLEWORD) != 0);
        if (GetDlgItem(dlg, rad1)) CheckRadioButton(dlg, rad1, rad2, (fr->Flags & FR_DOWN) ? rad2 : rad1);
        if (fr->Flags & FR_HIDEWHOLEWORD) ShowWindow(GetDlgItem(dlg, chx1), SW_HIDE);
        else if (fr->Flags & FR_NOWHOLEWORD) EnableWindow(GetDlgItem(dlg, chx1), FALSE);
        if (fr->Flags & FR_HIDEMATCHCASE) ShowWindow(GetDlgItem(dlg, chx2), SW_HIDE);
        else if (fr->Flags & FR_NOMATCHCASE) EnableWindow(GetDlgItem(dlg, chx2), FALSE);
        if (GetDlgItem(dlg, grp1)) {
            if (fr->Flags & FR_HIDEUPDOWN) {
                ShowWindow(GetDlgItem(dlg, grp1), SW_HIDE);
                ShowWindow(GetDlgItem(dlg, rad1), SW_HIDE);
                ShowWindow(GetDlgItem(dlg, rad2), SW_HIDE);
            } else if (fr->Flags & FR_NOUPDOWN) {
                EnableWindow(GetDlgItem(dlg, rad1), FALSE);
                EnableWindow(GetDlgItem(dlg, rad2), FALSE);
            }
        }
        if (!(fr->Flags & FR_SHOWHELP)) ShowWindow(GetDlgItem(dlg, pshHelp), SW_HIDE);
        fr_update_buttons(dlg);
        SendDlgItemMessage(dlg, edt1, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
        SetFocus(GetDlgItem(dlg, edt1));
        return FALSE;
    case WM_COMMAND:
        if (!fr) break;
        switch (wp) {
        case IDOK: fr_notify(dlg, fr, FR_FINDNEXT); return TRUE;
        case psh1: fr_notify(dlg, fr, FR_REPLACE); return TRUE;
        case psh2: fr_notify(dlg, fr, FR_REPLACEALL); return TRUE;
        case IDCANCEL:
            fr->Flags &= ~(FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL);
            fr->Flags |= FR_DIALOGTERM;
            SendMessage(fr->hwndOwner, fr_msg(), 0, (LPARAM)fr);
            RemoveProp(dlg, "W16FR");
            DestroyWindow(dlg);
            return TRUE;
        case pshHelp:
            SendMessage(fr->hwndOwner, RegisterWindowMessage(HELPMSGSTRING), 0, (LPARAM)fr);
            return TRUE;
        case edt1:
            if (HIWORD(lp) == EN_CHANGE) fr_update_buttons(dlg);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        SendMessage(dlg, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    return FALSE;
}

static HWND find_dialog(FINDREPLACE *fr, int id)
{
    cd_err = 0;
    if (!fr || fr->lStructSize != sizeof *fr) { cd_err = CDERR_STRUCTSIZE; return NULL; }
    if (!fr->lpstrFindWhat || !fr->wFindWhatLen) { cd_err = FRERR_BUFFERLENGTHZERO; return NULL; }
    HINSTANCE inst = commdlg();
    LPCSTR tmpl = MAKEINTRESOURCE(id);
    if (fr->Flags & FR_ENABLETEMPLATE) { inst = fr->hInstance; tmpl = fr->lpTemplateName; }
    if (!inst || !w16_find_res(inst, tmpl, RT_DIALOG)) { cd_err = inst ? CDERR_FINDRESFAILURE : CDERR_LOADRESFAILURE; return NULL; }
    HWND h = CreateDialogParam(inst, tmpl, fr->hwndOwner, FindDlgProc, (LPARAM)fr);
    if (!h) { cd_err = CDERR_DIALOGFAILURE; return NULL; }
    ShowWindow(h, SW_SHOWNORMAL);
    return h;
}

HWND FindText(FINDREPLACE *fr) { return find_dialog(fr, DLG_FIND); }
HWND ReplaceText(FINDREPLACE *fr) { return find_dialog(fr, DLG_REPLACE); }

/* ================================================================== Print / Print Setup */
/* Printers come from CUPS (lpstat). TODO(T-PRN-01): the printer DC itself does not render yet. */
typedef struct { char name[64], port[64]; } Printer;
static Printer printers[32];
static int nprinters, def_printer = -1;
static W16PRINTERENUMPROC printer_enum;

void w16_set_printer_enum(W16PRINTERENUMPROC fn) { printer_enum = fn; }

void w16_printer_port(LPCSTR uri, LPSTR port, int cb)
{
    const char *dev = NULL, *pre = NULL;
    if (!strncmp(uri, "parallel:/dev/lp", 16)) { dev = uri + 16; pre = "LPT"; }
    else if (!strncmp(uri, "serial:/dev/ttyS", 16)) { dev = uri + 16; pre = "COM"; }
    if (dev && *dev >= '0' && *dev <= '9') {
        int n = 0;
        const char *d = dev;
        while (*d >= '0' && *d <= '9' && n < 1000) n = n * 10 + (*d++ - '0');
        if (!*d || *d == '?') { snprintf(port, cb, "%s%d:", pre, n + 1); return; }
    }
    size_t k = 0;
    if (isalpha((unsigned char)uri[0]))
        while (isalnum((unsigned char)uri[k]) || uri[k] == '+' || uri[k] == '-' || uri[k] == '.') k++;
    if (k && uri[k] == ':') {
        snprintf(port, cb, "%.*s:", (int)k, uri);
        AnsiUpper(port);
    } else
        GetProfileString("windows", "NullPort", "None", port, cb);
}

static void enum_printers(void)
{
    nprinters = 0;
    def_printer = -1;
    if (printer_enum) {
        char name[32][64], port[32][64];
        nprinters = printer_enum(name, port, 32, &def_printer);
        if (nprinters < 0) nprinters = 0;
        for (int i = 0; i < nprinters; i++) {
            snprintf(printers[i].name, sizeof printers[i].name, "%s", name[i]);
            snprintf(printers[i].port, sizeof printers[i].port, "%s", port[i]);
        }
        if (def_printer >= nprinters) def_printer = -1;
        if (def_printer < 0 && nprinters) def_printer = 0;
        return;
    }
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
        w16_printer_port(uri, pr->port, sizeof pr->port);
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
