/* Network back end for the Network applet: NetworkManager through nmcli, host name through
 * hostnamectl. */
#ifndef ARCH311_NET_H
#define ARCH311_NET_H

typedef struct {
    char name[64], uuid[40], type[32], device[32];
    int active;
} NetConn;      /* a saved connection profile */

typedef struct {
    char ssid[64], security[32];
    int signal, in_use;
} WifiAp;       /* a visible wireless network */

typedef struct {
    int dhcp;
    char address[16], mask[16], gateway[16], dns[64];
} NetIp;

int net_available(void);
int net_list_connections(NetConn *c, int max);
int net_list_wifi(WifiAp *a, int max, int rescan);
int net_wifi_ssid(const char *uuid, char *ssid, int cb);   /* SSID of a Wi-Fi profile */
int net_connect(const char *uuid);
int net_disconnect(const char *uuid);
int net_wifi_connect(const char *ssid, const char *password); /* password may be NULL (open network) */
int net_get_ipv4(const char *uuid, NetIp *ip);
int net_set_ipv4(const char *uuid, const NetIp *ip);
int net_device_address(const char *device, char *addr, int cb);
int net_get_hostname(char *name, int cb);
int net_set_hostname(const char *name);
const char *net_last_error(void);   /* nmcli's message for the last failure */

/* address helpers */
int net_valid_ipv4(const char *s);
int net_mask_to_prefix(const char *mask);  /* -1 if not a contiguous mask */
void net_prefix_to_mask(int prefix, char *mask, int cb);
#endif
