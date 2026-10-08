/* COMM: the serial-port calls of USER/COMM.DRV that ported programs use. COM1..COM4 are Linux's
 * ttyS0..ttyS3. Only EscapeCommFunction's queries exist so far (no port is opened yet). */
#include "w16int.h"
#include <stdio.h>
#include <stdlib.h>

/* the BIOS data area's COM table (0040:0000) as COMM.DRV reads it: the I/O address where Linux found
 * a UART. ARCH311_SIMULATE gives the reference PC's two ports (DOSBox-X: 3F8 and 2F8) for tests. */
static WORD bios_com_base(int p)
{
    const char *sim = getenv("ARCH311_SIMULATE");
    if (sim && *sim && *sim != '0') return p == 0 ? 0x3F8 : p == 1 ? 0x2F8 : 0;
    char path[64];
    int type = 0;
    unsigned base = 0;
    snprintf(path, sizeof path, "/sys/class/tty/ttyS%d/type", p);
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%d", &type) != 1) type = 0; fclose(f); }
    if (!type) return 0; /* PORT_UNKNOWN: no UART there */
    snprintf(path, sizeof path, "/sys/class/tty/ttyS%d/port", p);
    f = fopen(path, "r");
    if (f) { if (fscanf(f, "%x", &base) != 1) base = 0; fclose(f); }
    return (WORD)base;
}

/* COMM.DRV seg2:0AE7: up to 4 upper-case hex digits, stopping at anything else */
static WORD parse_hex(const char *s)
{
    WORD v = 0;
    for (int i = 0; i < 4 && s[i]; i++) {
        int c = (unsigned char)s[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = (WORD)(v << 4 | d);
    }
    return v;
}

/* COMM.DRV seg2:0BD1: the base address from the BIOS table, else SYSTEM.INI [386Enh] COMnBase, else
 * 3E8 for COM3 only; the IRQ from COMnIrq (default 4, 3, 4, 3). DX:AX = IRQ:base, or FFFF:FFFF when
 * there is no base or the IRQ is not 1..15. */
static LONG get_base_irq(int p)
{
    static const BYTE irq_default[5] = {4, 3, 4, 3, 0};
    char key[16], buf[8] = "";
    WORD base = p < 4 ? bios_com_base(p) : 0;
    if (!base) {
        wsprintf(key, "COM%dBase", p + 1);
        GetPrivateProfileString("386Enh", key, "", buf, 5, "SYSTEM.INI");
        base = parse_hex(buf);
        if (!base && p == 2) base = 0x3E8;
        if (!base) return -1;
    }
    wsprintf(key, "COM%dIrq", p + 1);
    BYTE irq = (BYTE)GetPrivateProfileInt("386Enh", key, irq_default[p < 4 ? p : 4], "SYSTEM.INI");
    if (irq == 0 || irq > 15) return -1;
    return (LONG)MAKELONG(base, irq);
}

LONG EscapeCommFunction(int cid, int func)
{
    /* COMM.DRV seg2:0AB5: 0..3 are COM1..COM4, 80h..82h LPT1..LPT3 (which take only RESETDEV) */
    if (cid & 0x80) return cid > 0x82 ? 0x8000 : 0;
    if (cid > 3 || cid < 0) return 0x8000;
    switch (func) {
    case GETMAXLPT: return 0x82;
    case GETMAXCOM: return 3;
    case GETBASEIRQ: return get_base_irq(cid);
    }
    /* SETXOFF..RESETDEV act on an open port; ports cannot be opened yet (UNTESTED: real UARTs) */
    return 0;
}
