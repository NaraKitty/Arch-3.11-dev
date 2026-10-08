/* Network - an arch311 Control Panel applet in the place of Windows for Workgroups' Network applet:
 * computer name, wired and wireless connections (connect, disconnect, Wi-Fi password) and TCP/IP
 * settings, through NetworkManager (net.c). Dialogs follow the 3.11 conventions (Helv 8, group
 * boxes, OK/Cancel and command buttons on the right). The icon is WfW's own Network icon, loaded
 * from the user's ripped MAIN.CPL (icon 34); without it the applet still works. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "cpl.h"
#include "net.h"

enum {
    IDC_NAME = 701, IDC_LIST = 702, IDC_STATUS = 703, IDC_CONNECT = 704, IDC_DISCONNECT = 705, IDC_SETTINGS = 706,
    IDC_REFRESH = 707,
    IDC_PASSWORD = 801, IDC_SSID = 802,
    IDC_DHCP = 901, IDC_MANUAL = 902, IDC_ADDR = 903, IDC_MASK = 904, IDC_GATEWAY = 905, IDC_DNS = 906, IDC_CONN = 910,
};

static const char szTitle[] = "Network";

/* ------------------------------------------------------------------ the entries of the list */
typedef struct {
    int kind;         /* 0 saved connection, 1 visible Wi-Fi network without a profile */
    NetConn conn;
    WifiAp ap;
    int signal;       /* -1: not a visible Wi-Fi network */
} Entry;

static Entry entries[96];
static int nentries;

static const char *TypeName(const char *t)
{
    if (strstr(t, "wireless")) return "Wi-Fi";
    if (strstr(t, "ethernet")) return "Ethernet";
    if (!strcmp(t, "gsm") || !strcmp(t, "cdma")) return "Mobile";
    if (strstr(t, "vpn") || !strcmp(t, "wireguard")) return "VPN";
    return t;
}

static void Collect(int rescan)
{
    static NetConn conns[64];
    static WifiAp aps[64];
    int nc = net_list_connections(conns, 64), na = net_list_wifi(aps, 64, rescan);
    int used[64] = {0};
    nentries = 0;
    for (int pass = 0; pass < 2; pass++) /* connections in use first */
        for (int i = 0; i < nc && nentries < 96; i++) {
            if (conns[i].active != !pass) continue;
            Entry *e = &entries[nentries++];
            memset(e, 0, sizeof *e);
            e->conn = conns[i];
            e->signal = -1;
            if (strstr(conns[i].type, "wireless")) {
                char ssid[64];
                if (!net_wifi_ssid(conns[i].uuid, ssid, sizeof ssid)) snprintf(ssid, sizeof ssid, "%s", conns[i].name);
                for (int k = 0; k < na; k++)
                    if (!strcmp(aps[k].ssid, ssid)) { e->signal = aps[k].signal; used[k] = 1; }
            }
        }
    for (int k = 0; k < na && nentries < 96; k++) {
        if (used[k]) continue;
        Entry *e = &entries[nentries++];
        memset(e, 0, sizeof *e);
        e->kind = 1;
        e->ap = aps[k];
        e->signal = aps[k].signal;
    }
}

static void FillList(HWND dlg, int keep)
{
    HWND lb = GetDlgItem(dlg, IDC_LIST);
    SendMessage(lb, WM_SETREDRAW, FALSE, 0);
    SendMessage(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < nentries; i++) {
        Entry *e = &entries[i];
        char line[160], state[32];
        if (e->kind == 0) {
            if (e->conn.active) lstrcpy(state, "Connected");
            else if (e->signal >= 0) wsprintf(state, "%d%%", e->signal);
            else state[0] = 0;
            wsprintf(line, "%s\t%s\t%s", e->conn.name, TypeName(e->conn.type), state);
        } else
            wsprintf(line, "%s\t%s\t%d%%", e->ap.ssid, e->ap.security[0] ? "Wi-Fi (secured)" : "Wi-Fi", e->signal);
        SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessage(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
    int sel = keep < nentries ? keep : 0;
    if (nentries) SendMessage(lb, LB_SETCURSEL, sel, 0);
}

static Entry *Selected(HWND dlg)
{
    int i = (int)SendDlgItemMessage(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
    return (i >= 0 && i < nentries) ? &entries[i] : NULL;
}

static void UpdateButtons(HWND dlg)
{
    Entry *e = Selected(dlg);
    int active = e && e->kind == 0 && e->conn.active;
    EnableWindow(GetDlgItem(dlg, IDC_CONNECT), e && !active);
    EnableWindow(GetDlgItem(dlg, IDC_DISCONNECT), active);
    EnableWindow(GetDlgItem(dlg, IDC_SETTINGS), e && e->kind == 0);
    char status[160] = "", addr[32];
    if (e && active) {
        if (net_device_address(e->conn.device, addr, sizeof addr)) wsprintf(status, "Connected.  IP address: %s", addr);
        else lstrcpy(status, "Connected.");
    } else if (e && e->kind == 1)
        wsprintf(status, "%s network, signal %d%%.", e->ap.security[0] ? "Secured" : "Open", e->signal);
    else if (e)
        lstrcpy(status, "Not connected.");
    SetDlgItemText(dlg, IDC_STATUS, status);
}

static void Failed(HWND dlg, const char *what)
{
    char t[700];
    const char *why = net_last_error();
    wsprintf(t, "%s\n\n%s", what, why && *why ? why : "NetworkManager reported an error.");
    MessageBox(dlg, t, szTitle, MB_OK | MB_ICONEXCLAMATION);
}

/* ------------------------------------------------------------------ Wi-Fi password */
static char szPassword[128];

static BOOL PasswordDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg, IDC_SSID, (const char *)lp);
        SendDlgItemMessage(dlg, IDC_PASSWORD, EM_LIMITTEXT, 63, 0);
        SetFocus(GetDlgItem(dlg, IDC_PASSWORD));
        return FALSE;
    case WM_COMMAND:
        if (wp == IDOK) {
            GetDlgItemText(dlg, IDC_PASSWORD, szPassword, sizeof szPassword);
            if (lstrlen(szPassword) < 8) {
                MessageBox(dlg, "The password must be at least 8 characters long.", "Connect to Network",
                           MB_OK | MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(dlg, IDC_PASSWORD));
                return TRUE;
            }
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        if (wp == IDCANCEL) { EndDialog(dlg, FALSE); return TRUE; }
        break;
    }
    return FALSE;
}

static int AskPassword(HWND owner, const char *ssid)
{
    W16DlgTemplate *t = w16_dlgt_new(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 30, 30, 200, 42, "Connect to Network", 8, "Helv");
    w16_dlgt_add(t, "STATIC", "Network:", -1, SS_LEFT, 6, 8, 40, 9);
    w16_dlgt_add(t, "STATIC", "", IDC_SSID, SS_LEFT | SS_NOPREFIX, 48, 8, 100, 9);
    w16_dlgt_add(t, "STATIC", "&Password:", -1, SS_LEFT, 6, 24, 40, 9);
    w16_dlgt_add(t, "EDIT", "", IDC_PASSWORD, WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 48, 22, 100, 12);
    w16_dlgt_add(t, "BUTTON", "OK", IDOK, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, 156, 6, 40, 14);
    w16_dlgt_add(t, "BUTTON", "Cancel", IDCANCEL, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 156, 22, 40, 14);
    szPassword[0] = 0;
    int r = DialogBoxIndirectParam(NULL, w16_dlgt_data(t), owner, PasswordDlgProc, (LPARAM)ssid);
    w16_dlgt_free(t);
    return r > 0;
}

/* ------------------------------------------------------------------ TCP/IP settings */
static NetIp ipEdit;
static const NetConn *ipConn;

static void EnableManual(HWND dlg)
{
    int on = IsDlgButtonChecked(dlg, IDC_MANUAL);
    for (int id = IDC_ADDR; id <= IDC_GATEWAY; id++) {
        EnableWindow(GetDlgItem(dlg, id), on);
        EnableWindow(GetDlgItem(dlg, id + 100), on); /* the labels */
    }
}

static BOOL IpDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg, IDC_CONN, ipConn->name);
        CheckRadioButton(dlg, IDC_DHCP, IDC_MANUAL, ipEdit.dhcp ? IDC_DHCP : IDC_MANUAL);
        SetDlgItemText(dlg, IDC_ADDR, ipEdit.address);
        SetDlgItemText(dlg, IDC_MASK, ipEdit.mask[0] ? ipEdit.mask : "255.255.255.0");
        SetDlgItemText(dlg, IDC_GATEWAY, ipEdit.gateway);
        SetDlgItemText(dlg, IDC_DNS, ipEdit.dns);
        EnableManual(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (wp) {
        case IDC_DHCP:
        case IDC_MANUAL:
            EnableManual(dlg);
            return TRUE;
        case IDOK: {
            NetIp ip;
            memset(&ip, 0, sizeof ip);
            ip.dhcp = IsDlgButtonChecked(dlg, IDC_DHCP);
            GetDlgItemText(dlg, IDC_ADDR, ip.address, sizeof ip.address);
            GetDlgItemText(dlg, IDC_MASK, ip.mask, sizeof ip.mask);
            GetDlgItemText(dlg, IDC_GATEWAY, ip.gateway, sizeof ip.gateway);
            GetDlgItemText(dlg, IDC_DNS, ip.dns, sizeof ip.dns);
            const char *bad = NULL;
            int focus = 0;
            if (!ip.dhcp) {
                if (!net_valid_ipv4(ip.address)) { bad = "The IP Address is not valid."; focus = IDC_ADDR; }
                else if (net_mask_to_prefix(ip.mask) < 1) { bad = "The Subnet Mask is not valid."; focus = IDC_MASK; }
                else if (ip.gateway[0] && !net_valid_ipv4(ip.gateway)) { bad = "The Default Gateway is not valid."; focus = IDC_GATEWAY; }
            }
            if (!bad) { /* DNS: addresses separated by spaces or commas */
                char tmp[64];
                lstrcpy(tmp, ip.dns);
                for (char *p = strtok(tmp, " ,"); p; p = strtok(NULL, " ,"))
                    if (!net_valid_ipv4(p)) { bad = "A DNS Server address is not valid."; focus = IDC_DNS; break; }
            }
            if (bad) {
                char t[200];
                wsprintf(t, "%s\n\nType four numbers from 0 to 255, separated by periods.", bad);
                MessageBox(dlg, t, "TCP/IP Settings", MB_OK | MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(dlg, focus));
                SendDlgItemMessage(dlg, focus, EM_SETSEL, 0, MAKELPARAM(0, 0x7FFF));
                return TRUE;
            }
            HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
            int ok = net_set_ipv4(ipConn->uuid, &ip);
            SetCursor(old);
            if (!ok) { Failed(dlg, "Cannot change the TCP/IP settings."); return TRUE; }
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void IpSettings(HWND owner, const NetConn *c)
{
    if (!net_get_ipv4(c->uuid, &ipEdit)) { Failed(owner, "Cannot read the TCP/IP settings."); return; }
    ipConn = c;
    W16DlgTemplate *t = w16_dlgt_new(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 24, 20, 226, 116, "TCP/IP Settings", 8, "Helv");
    w16_dlgt_add(t, "STATIC", "Connection:", -1, SS_LEFT, 6, 6, 44, 9);
    w16_dlgt_add(t, "STATIC", "", IDC_CONN, SS_LEFT | SS_NOPREFIX, 52, 6, 118, 9);
    w16_dlgt_add(t, "BUTTON", "&Obtain an address automatically", IDC_DHCP, BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 6, 20, 164, 12);
    w16_dlgt_add(t, "BUTTON", "&Use the following settings:", IDC_MANUAL, BS_AUTORADIOBUTTON, 6, 32, 164, 12);
    static const char *labels[] = {"&IP Address:", "Subnet &Mask:", "Default &Gateway:"};
    for (int i = 0; i < 3; i++) {
        w16_dlgt_add(t, "STATIC", labels[i], IDC_ADDR + 100 + i, SS_LEFT | WS_GROUP, 18, 50 + i * 16, 56, 9);
        w16_dlgt_add(t, "EDIT", "", IDC_ADDR + i, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 76, 48 + i * 16, 94, 12);
    }
    w16_dlgt_add(t, "STATIC", "&DNS Servers:", -1, SS_LEFT | WS_GROUP, 6, 100, 56, 9);
    w16_dlgt_add(t, "EDIT", "", IDC_DNS, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 76, 98, 94, 12);
    w16_dlgt_add(t, "BUTTON", "OK", IDOK, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, 178, 6, 44, 14);
    w16_dlgt_add(t, "BUTTON", "Cancel", IDCANCEL, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 178, 22, 44, 14);
    DialogBoxIndirectParam(NULL, w16_dlgt_data(t), owner, IpDlgProc, 0);
    w16_dlgt_free(t);
}

/* ------------------------------------------------------------------ the Network dialog */
static char szHostWas[64];

static void Reload(HWND dlg, int rescan)
{
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int keep = (int)SendDlgItemMessage(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
    Collect(rescan);
    FillList(dlg, keep < 0 ? 0 : keep);
    UpdateButtons(dlg);
    SetCursor(old);
    /* the button just used may have become disabled: keep the keyboard on the list */
    HWND f = GetFocus();
    if (!f || !IsWindowEnabled(f)) SetFocus(GetDlgItem(dlg, IDC_LIST));
}

static BOOL NetworkDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        int tabs[2] = {84, 146};
        SendDlgItemMessage(dlg, IDC_LIST, LB_SETTABSTOPS, 2, (LPARAM)tabs);
        SendDlgItemMessage(dlg, IDC_NAME, EM_LIMITTEXT, 63, 0);
        if (!net_get_hostname(szHostWas, sizeof szHostWas)) szHostWas[0] = 0;
        SetDlgItemText(dlg, IDC_NAME, szHostWas);
        if (!net_available()) {
            MessageBox(dlg, "Cannot find NetworkManager.\n\nThe Network settings need the NetworkManager service (nmcli).",
                       szTitle, MB_OK | MB_ICONEXCLAMATION);
            for (int id = IDC_CONNECT; id <= IDC_REFRESH; id++) EnableWindow(GetDlgItem(dlg, id), FALSE);
            return TRUE;
        }
        Reload(dlg, 0);
        return TRUE;
    }
    case WM_COMMAND: {
        Entry *e = Selected(dlg);
        switch (wp) {
        case IDC_LIST:
            if (HIWORD(lp) == LBN_SELCHANGE) UpdateButtons(dlg);
            else if (HIWORD(lp) == LBN_DBLCLK && e && !(e->kind == 0 && e->conn.active))
                PostMessage(dlg, WM_COMMAND, IDC_CONNECT, 0);
            return TRUE;
        case IDC_CONNECT: {
            if (!e) return TRUE;
            HCURSOR old;
            int ok;
            if (e->kind == 0) {
                old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                ok = net_connect(e->conn.uuid);
            } else {
                int secured = e->ap.security[0] != 0;
                if (secured && !AskPassword(dlg, e->ap.ssid)) return TRUE;
                old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                ok = net_wifi_connect(e->ap.ssid, secured ? szPassword : NULL);
                memset(szPassword, 0, sizeof szPassword);
            }
            SetCursor(old);
            if (!ok) Failed(dlg, "Cannot connect to the network.");
            Reload(dlg, 0);
            return TRUE;
        }
        case IDC_DISCONNECT:
            if (e && e->kind == 0) {
                HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                int ok = net_disconnect(e->conn.uuid);
                SetCursor(old);
                if (!ok) Failed(dlg, "Cannot disconnect from the network.");
                Reload(dlg, 0);
            }
            return TRUE;
        case IDC_SETTINGS:
            if (e && e->kind == 0) {
                NetConn c = e->conn;
                IpSettings(dlg, &c);
                Reload(dlg, 0);
            }
            return TRUE;
        case IDC_REFRESH:
            Reload(dlg, 1);
            return TRUE;
        case IDOK: {
            char name[64];
            GetDlgItemText(dlg, IDC_NAME, name, sizeof name);
            if (lstrcmp(name, szHostWas)) {
                int ok = name[0] && name[0] != '-';
                for (const char *p = name; *p; p++)
                    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) ok = 0;
                if (!ok) {
                    MessageBox(dlg, "The Computer Name is not valid.\n\nUse letters, numbers and hyphens only, and do not start "
                               "it with a hyphen.", szTitle, MB_OK | MB_ICONEXCLAMATION);
                    SetFocus(GetDlgItem(dlg, IDC_NAME));
                    return TRUE;
                }
                HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                ok = net_set_hostname(name);
                SetCursor(old);
                if (!ok) { Failed(dlg, "Cannot change the Computer Name."); return TRUE; }
            }
            EndDialog(dlg, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, FALSE);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static W16DlgTemplate *NetworkTemplate(void)
{
    W16DlgTemplate *t = w16_dlgt_new(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 12, 14, 264, 136, "Network", 8, "Helv");
    w16_dlgt_add(t, "STATIC", "Computer &Name:", -1, SS_LEFT | WS_GROUP, 6, 8, 58, 9);
    w16_dlgt_add(t, "EDIT", "", IDC_NAME, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 66, 6, 110, 12);
    w16_dlgt_add(t, "BUTTON", "C&onnections", -1, BS_GROUPBOX, 4, 24, 200, 108);
    w16_dlgt_add(t, "LISTBOX", "", IDC_LIST, WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_USETABSTOPS, 8, 36, 192, 72);
    w16_dlgt_add(t, "STATIC", "", IDC_STATUS, SS_LEFT | SS_NOPREFIX, 8, 112, 192, 16);
    w16_dlgt_add(t, "BUTTON", "OK", IDOK, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 6, 52, 14);
    w16_dlgt_add(t, "BUTTON", "Cancel", IDCANCEL, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 22, 52, 14);
    w16_dlgt_add(t, "BUTTON", "&Connect", IDC_CONNECT, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 44, 52, 14);
    w16_dlgt_add(t, "BUTTON", "&Disconnect", IDC_DISCONNECT, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 60, 52, 14);
    w16_dlgt_add(t, "BUTTON", "&Settings...", IDC_SETTINGS, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 76, 52, 14);
    w16_dlgt_add(t, "BUTTON", "&Refresh", IDC_REFRESH, BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 208, 92, 52, 14);
    return t;
}

/* ------------------------------------------------------------------ CPlApplet */
static HICON hIconNet;

LRESULT Network_CPlApplet(HWND hwndCPl, UINT msg, LPARAM l1, LPARAM l2)
{
    (void)l1;
    switch (msg) {
    case CPL_INIT: {
        HINSTANCE main = w16_load_module("MAIN.CPL");
        hIconNet = main ? LoadIcon(main, MAKEINTRESOURCE(34)) : NULL;
        if (!hIconNet) hIconNet = LoadIcon(NULL, IDI_APPLICATION);
        return TRUE;
    }
    case CPL_GETCOUNT:
        return 1;
    case CPL_NEWINQUIRE: {
        NEWCPLINFO *ni = (NEWCPLINFO *)l2;
        memset(ni, 0, sizeof *ni);
        ni->dwSize = sizeof *ni;
        ni->hIcon = hIconNet;
        lstrcpy(ni->szName, "&Network");
        lstrcpy(ni->szInfo, "Changes your network settings and connections");
        return 0;
    }
    case CPL_DBLCLK: {
        W16DlgTemplate *t = NetworkTemplate();
        DialogBoxIndirectParam(NULL, w16_dlgt_data(t), hwndCPl, NetworkDlgProc, 0);
        w16_dlgt_free(t);
        return 0;
    }
    }
    return 0;
}
