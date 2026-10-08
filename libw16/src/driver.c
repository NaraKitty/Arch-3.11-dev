/* USER's installable drivers: OpenDriver, CloseDriver, SendDriverMessage, GetDriverModuleHandle and
 * DefDriverProc, ported from USER seg41:0000-06AC and seg2:04CC/07F3.
 *
 * In 3.1 an installable driver is a 16-bit DLL exporting DriverProc. USER keeps one table entry per
 * open driver (the alias it was opened by, its module, the instance id DRV_OPEN returned); the first
 * entry of a module sends DRV_LOAD and DRV_ENABLE, the last one to close DRV_DISABLE and DRV_FREE.
 * arch311 never runs 16-bit code, so the module of an entry is native: a driver registered for that
 * file name (w16_register_driver), else a stand-in that answers as the real 3.11 driver does. The
 * name resolution, the table and the message sequence are USER's; OpenDriver fails, as in 3.1, when
 * the driver file cannot be found or is not a driver (an NE module without a DriverProc export). */
#include "w16int.h"
#include <ctype.h>

/* ------------------------------------------------------------------ the stand-ins */
/* How the 3.11 drivers' DriverProcs answer the messages a configuring program sends (read from each
 * driver's DriverProc; Setup... enabled/disabled and the restart prompts were checked on real 3.11 in
 * the reference rig, arch311-ref/shots/drivers-*). Every other message succeeds as USER's
 * DefDriverProc would let it (DRV_LOAD/ENABLE/DISABLE/FREE 1) or with 1 (DRV_OPEN/DRV_CLOSE: a
 * non-zero instance id / success). DRV_CONFIGURE runs the driver's own setup dialog in 3.1; that
 * dialog lives in the 16-bit driver, so the stand-in shows nothing and answers DRVCNF_CANCEL
 * (TODO: native ports of those dialogs, registered with w16_register_driver). */
typedef struct {
    const char *file;
    LRESULT query;      /* DRV_QUERYCONFIGURE: 1 = the driver has a setup dialog */
    LRESULT install;    /* DRV_INSTALL */
    LRESULT remove;     /* DRV_REMOVE (DRIVERS.CPL ignores the answer) */
} StandIn;

static const StandIn standins[] = {
    /* MCIWAVE.DRV seg1:00AC: setup = "MCI Waveform Driver Setup" (seg5:0264, SYSTEM.INI
     * [mciwave.drv] Seconds=). Setup enabled on 3.11 (drivers-open/05). TODO: native dialog */
    {"MCIWAVE.DRV", 1, DRVCNF_OK, DRVCNF_OK},
    /* MCISEQ.DRV seg2:0D88: no setup; Setup disabled on 3.11 (drivers-open/04) */
    {"MCISEQ.DRV", 0, DRVCNF_OK, DRVCNF_OK},
    /* MCICDA.DRV seg1:10B0: setup seg1:2335 (a message when MSCDEX reports no CD-ROM drive, else
     * the drive dialog 1); DRV_INSTALL/DRV_REMOVE go to DefDriverProc. UNTESTED on 3.11 (the
     * reference machine has no MCICDA.DRV installed). TODO: native dialog */
    {"MCICDA.DRV", 1, DRVCNF_OK, DRVCNF_CANCEL},
    /* TIMER.DRV seg2:0038 (message table seg2:0000): no setup (drivers-open/03); DRV_INSTALL asks
     * for a restart (drivers-addtimer/06); DRV_REMOVE is not handled (0) */
    {"TIMER.DRV", 0, DRVCNF_RESTART, DRVCNF_CANCEL},
    /* MIDIMAP.DRV seg2:003C: no setup (drivers-open/02), restart after install and remove */
    {"MIDIMAP.DRV", 0, DRVCNF_RESTART, DRVCNF_RESTART},
    /* MSADLIB.DRV seg2:0628: no setup, restart after install (drivers-addlib/06, 07); DRV_REMOVE
     * goes to DefDriverProc */
    {"MSADLIB.DRV", 0, DRVCNF_RESTART, DRVCNF_CANCEL},
    /* MPU401.DRV seg1:0000: setup seg2:05B1 (port and interrupt). UNTESTED on 3.11. TODO: dialog */
    {"MPU401.DRV", 1, DRVCNF_RESTART, DRVCNF_RESTART},
    /* SNDBLST.DRV / SNDBLST2.DRV seg3:0000: setup seg2:0A88 / seg2:0AE4 (port and interrupt).
     * UNTESTED on 3.11 (the reference machine has no Sound Blaster). TODO: native dialogs */
    {"SNDBLST.DRV", 1, DRVCNF_RESTART, DRVCNF_RESTART},
    {"SNDBLST2.DRV", 1, DRVCNF_RESTART, DRVCNF_RESTART},
};

/* any other driver: no setup dialog, installs and removes without a restart. UNTESTED (a 3.11
 * driver not in the table answers whatever its DriverProc does) */
static const StandIn generic = {NULL, 0, DRVCNF_OK, DRVCNF_OK};

/* ------------------------------------------------------------------ the driver table */
typedef struct {
    int used;              /* +06 the entry holds a module */
    int loaded;            /* +00 bit 1: the entry that sent DRV_LOAD/DRV_ENABLE for its module */
    ULONG_PTR id;          /* +08 dwDriverID: DRV_OPEN's answer */
    char alias[0x80];      /* +0C the name the driver was opened by */
    char module[16];       /* the driver file's name, upper case: arch311's "module" */
    DRIVERPROC proc;       /* +8C */
} Drv;

static Drv *drv;           /* [0x17E] */
static int ndrv;           /* [0x17C] */

typedef struct Native { char file[16]; DRIVERPROC proc; struct Native *next; } Native;
static Native *natives;

/* handles are table index + 1, as in USER, so a stale handle is caught by the table check */
static HDRVR handle_of(int i) { return (HDRVR)(uintptr_t)(i + 1); }
static int index_of(HDRVR h)
{
    intptr_t i = (intptr_t)(uintptr_t)h - 1;
    return i >= 0 && i < ndrv && drv[i].used ? (int)i : -1;
}

static void upper_name(const char *path, char *out, size_t cb)
{
    const char *b = path;
    for (const char *c = path; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':') b = c + 1;
    snprintf(out, cb, "%s", b);
    for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
}

static const StandIn *standin(const char *module)
{
    for (size_t i = 0; i < sizeof standins / sizeof standins[0]; i++)
        if (!strcmp(standins[i].file, module)) return &standins[i];
    return &generic;
}

static LRESULT StandInProc(ULONG_PTR id, HDRVR h, UINT msg, LPARAM lp1, LPARAM lp2)
{
    int i = index_of(h);
    const StandIn *s = standin(i >= 0 ? drv[i].module : "");
    switch (msg) {
    case DRV_OPEN:
    case DRV_CLOSE:
        return 1;
    case DRV_QUERYCONFIGURE:
        return s->query;
    case DRV_CONFIGURE:
        return DRVCNF_CANCEL;
    case DRV_INSTALL:
        return s->install;
    case DRV_REMOVE:
        return s->remove;
    }
    return DefDriverProc(id, h, msg, lp1, lp2);
}

BOOL w16_register_driver(LPCSTR file, DRIVERPROC proc)
{
    Native *n = calloc(1, sizeof *n);
    if (!n) return FALSE;
    upper_name(file, n->file, sizeof n->file);
    n->proc = proc;
    n->next = natives;
    natives = n;
    return TRUE;
}

/* KERNEL's GetProcAddress(LoadLibrary(file), "DriverProc") for a file on disk: an NE module whose
 * resident or non-resident name table exports DRIVERPROC */
static int exports_driverproc(LPCSTR dos)
{
    char host[2048];
    if (w16_dos_to_host(dos, host, sizeof host)) return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    uint8_t mz[0x40], ne[0x40];
    int ok = 0;
    if (fread(mz, 1, sizeof mz, f) == sizeof mz && mz[0] == 'M' && mz[1] == 'Z') {
        long off = mz[0x3C] | mz[0x3D] << 8 | (long)mz[0x3E] << 16 | (long)mz[0x3F] << 24;
        if (!fseek(f, off, SEEK_SET) && fread(ne, 1, sizeof ne, f) == sizeof ne && ne[0] == 'N' && ne[1] == 'E') {
            long tables[2] = {off + (ne[0x26] | ne[0x27] << 8),     /* resident names */
                              ne[0x2C] | ne[0x2D] << 8 | (long)ne[0x2E] << 16 | (long)ne[0x2F] << 24};
            for (int t = 0; t < 2 && !ok; t++) {
                if (!tables[t] || fseek(f, tables[t], SEEK_SET)) continue;
                int len;
                for (int first = 1; !ok && (len = fgetc(f)) > 0; first = 0) {
                    char name[256];
                    if (fread(name, 1, len, f) != (size_t)len || fgetc(f) == EOF || fgetc(f) == EOF) break;
                    name[len] = 0;
                    /* entry 0 of each table is the module name / description, not an export */
                    if (!first && !strcasecmp(name, "DRIVERPROC")) ok = 1;
                }
            }
        }
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ seg41:0000 */
/* the driver's file: the SYSTEM.INI [section] value for name (the name itself when there is none)
 * up to its first blank; the rest is the parameter string DRV_OPEN gets. FALSE when there is no such
 * driver (3.1: OpenFile, LoadLibrary or GetProcAddress fails) */
static BOOL FindDriver(LPCSTR name, LPCSTR section, LPSTR params, int cb, char *module, DRIVERPROC *proc)
{
    char buf[0x80];
    OFSTRUCT of;
    if (!name || !*name) return FALSE;
    GetPrivateProfileString(section, name, name, buf, sizeof buf, "SYSTEM.INI");
    char *p = buf;
    while (*p && *p != ' ') p++;
    if (*p == ' ') *p++ = 0;
    upper_name(buf, module, 16);
    for (Native *n = natives; n; n = n->next)
        if (!strcmp(n->file, module)) { *proc = n->proc; goto found; }
    if (OpenFile(buf, &of, OF_EXIST | OF_SHARE_DENY_NONE) == HFILE_ERROR) return FALSE;
    /* LoadLibrary(buf) finds the same file; arch311 loads no 16-bit code: the module is the stand-in
     * of that file name */
    if (!exports_driverproc(of.szPathName)) return FALSE;
    *proc = StandInProc;
found:
    if (params && cb > 0) snprintf(params, cb, "%s", p);
    return TRUE;
}

/* seg41:00D8: how many entries hold module `module` */
static int ModuleUsage(const char *module)
{
    int n = 0;
    for (int i = 0; i < ndrv; i++)
        if (drv[i].used && !strcmp(drv[i].module, module)) n++;
    return n;
}

/* ------------------------------------------------------------------ seg41:0165 */
/* a new entry for the driver; the first entry of a module loads it (DRV_LOAD, then DRV_ENABLE when
 * fEnable). Returns the entry, -1 if the driver cannot be loaded */
static int LoadDriver(LPCSTR name, LPCSTR section, LPSTR params, int cb, BOOL fEnable)
{
    char module[16];
    DRIVERPROC proc;
    int i;
    /* USER strings 102 "DRIVERS" (the default section) and 74 "SYSTEM.INI" */
    if (!FindDriver(name, section ? section : "DRIVERS", params, cb, module, &proc)) return -1;
    for (i = 0; i < ndrv && drv[i].used; i++) ;      /* the first free entry, else a new one */
    if (i == ndrv) {
        Drv *t = realloc(drv, sizeof *drv * (ndrv + 1));
        if (!t) return -1;
        drv = t;
        ndrv++;
    }
    Drv *d = &drv[i];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->proc = proc;
    snprintf(d->alias, sizeof d->alias, "%s", name);
    snprintf(d->module, sizeof d->module, "%s", module);
    if (ModuleUsage(module) == 1) {
        /* seg41:012C first calls DriverProc with message 0 to check its stack frame (a native
         * DriverProc needs no such check) */
        if (!d->proc(d->id, handle_of(i), DRV_LOAD, 0, 0)) {
            memset(d, 0, sizeof *d);
            return -1;
        }
        d->loaded = 1;
    }
    if (fEnable && d->loaded) SendDriverMessage(handle_of(i), DRV_ENABLE, 0, 0);
    return i;
}

/* ------------------------------------------------------------------ seg41:038A */
/* frees entry i; the module's last entry sends DRV_DISABLE (when fDisable) and DRV_FREE. Returns how
 * many entries still hold the module */
static int FreeDriver(int i, BOOL fDisable)
{
    if (i < 0 || i >= ndrv || !drv[i].used) return 0;
    drv[i].id = 0;
    int n = ModuleUsage(drv[i].module);
    if (n == 1) {
        if (fDisable) SendDriverMessage(handle_of(i), DRV_DISABLE, 0, 0);
        SendDriverMessage(handle_of(i), DRV_FREE, 0, 0);
    }
    drv[i].used = drv[i].loaded = 0;
    drv[i].proc = NULL;
    return n - 1;
}

/* ------------------------------------------------------------------ seg41:04DF */
HDRVR OpenDriver(LPCSTR name, LPCSTR section, LPARAM lParam2)
{
    char params[0x80];
    int i = LoadDriver(name, section, params, sizeof params, TRUE);
    if (i < 0) return NULL;
    drv[i].id = (ULONG_PTR)(i + 1);
    LRESULT r = SendDriverMessage(handle_of(i), DRV_OPEN, (LPARAM)params, lParam2);
    if (!r) {
        FreeDriver(i, TRUE);
        return NULL;
    }
    drv[i].id = (ULONG_PTR)r;
    return handle_of(i);
}

/* ------------------------------------------------------------------ seg41:0579 */
/* DRV_CLOSE; the entry is freed only when the driver agrees (non-zero). An entry that loaded its
 * module hands that role to another entry still holding it */
LRESULT CloseDriver(HDRVR h, LPARAM lParam1, LPARAM lParam2)
{
    int i = index_of(h);
    if (i < 0) return 0;
    LRESULT r = SendDriverMessage(h, DRV_CLOSE, lParam1, lParam2);
    if (r) {
        int was = drv[i].loaded;
        char module[16];
        snprintf(module, sizeof module, "%s", drv[i].module);
        if (FreeDriver(i, TRUE) && was)
            for (int k = 0; k < ndrv; k++)
                if (drv[k].used && !strcmp(drv[k].module, module) && !drv[k].loaded) {
                    drv[k].loaded = 1;
                    break;
                }
    }
    return r;
}

/* ------------------------------------------------------------------ seg2:0621 -> seg2:04CC */
LRESULT SendDriverMessage(HDRVR h, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    int i = index_of(h);
    if (i < 0) return 0;
    return drv[i].proc(drv[i].id, h, msg, lParam1, lParam2);
}

/* ------------------------------------------------------------------ seg41:067C */
/* the driver's module: here the user's ripped copy of the driver file (its resources: dialogs,
 * strings for a native port of its setup), NULL when there is none */
HINSTANCE GetDriverModuleHandle(HDRVR h)
{
    int i = index_of(h);
    return i < 0 ? NULL : w16_module_open(drv[i].module);
}

/* ------------------------------------------------------------------ seg2:07F3 */
LRESULT DefDriverProc(ULONG_PTR dwDriverID, HDRVR h, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    (void)dwDriverID; (void)h; (void)lParam1; (void)lParam2;
    switch (msg) {
    case DRV_LOAD:
    case DRV_ENABLE:
    case DRV_DISABLE:
    case DRV_FREE:
    case DRV_INSTALL:
        return 1;
    }
    return 0;
}

/* VER.DLL's "file in use" test (seg3:113A asks whether the module is loaded): a driver file that the
 * driver table holds open */
int w16_driver_in_use(LPCSTR file)
{
    char module[16];
    upper_name(file, module, sizeof module);
    return ModuleUsage(module) > 0;
}
