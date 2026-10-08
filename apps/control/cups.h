/* CUPS back end of the Printers applet (prntcpl.c): queues, default printer, devices and drivers
 * through lpstat, lpinfo, lpadmin and lpoptions (sysexec.c: no shell, every value its own argv
 * element, C locale output).
 *
 * ARCH311_SIMULATE=1 replaces CUPS with sample data shaped like the reference PC (tools/ref311),
 * produced as lpstat/lpinfo text so the parsers run too:
 *   ARCH311_SIM_PRINTERS  the queues, "[*]NAME=URI;..." ('*': the default). Unset: the two printers
 *                         the reference runs install, *HP_LaserJet_4 on parallel:/dev/lp0 and
 *                         Text_Only on file:///dev/null. Empty: no printer (the pristine install).
 *   ARCH311_SIM_NETWORK   1: a network is there (the reference PC has none)
 *   ARCH311_SIM_ERROR     text: every change (lpadmin, lpoptions) fails with it as its stderr
 * The devices are the reference PC's ports (LPT1, COM1, COM2); the drivers (lpinfo -m) are the
 * printer names of the user's CONTROL.INF [io.device], so List of Printers shows what 3.1 showed. */
#ifndef ARCH311_CUPS_H
#define ARCH311_CUPS_H

typedef struct {
    char name[128];     /* queue name (printer-name) */
    char uri[512];      /* device-uri */
} CupsQueue;

typedef struct {
    char uri[512];      /* device-uri */
    char cls[16];       /* device-class: direct, network, serial, file */
    char info[128];     /* device-info, "" when not known */
} CupsDevice;

typedef struct {
    char ppd[256];      /* ppd-name, as lpadmin -m takes it */
    char model[128];    /* ppd-make-and-model */
} CupsModel;

/* queries: the count, or -1 when the command cannot run or fails (see cups_last_error) */
int cups_list_queues(CupsQueue *q, int max);                  /* lpstat -v */
int cups_get_default(char *name, int cb);                     /* lpstat -d: 1 with a default, 0 none */
int cups_list_devices(CupsDevice *d, int max);                /* lpinfo -l -v (CUPS admin rights) */
int cups_list_models(CupsModel **m);                          /* lpinfo -l -m; *m is malloc'd */
int cups_get_option(const char *name, const char *key, char *val, int cb); /* lpoptions -p NAME: 1 found */
int cups_network_available(void);

/* changes: 1 done, 0 failed (cups_last_error has the tool's message) */
int cups_add_queue(const char *name, const char *ppd, int ppd_is_file, const char *uri);
int cups_set_ppd(const char *name, const char *ppd_file);    /* lpadmin -p NAME -P FILE */
int cups_set_device(const char *name, const char *uri);      /* lpadmin -p NAME -v URI */
int cups_delete_queue(const char *name);                     /* lpadmin -x NAME */
int cups_set_default(const char *name);                      /* lpoptions -d NAME (no admin rights) */
int cups_set_options(const char *name, const char *const *opts, int n); /* lpoptions -p NAME -o OPT... */
const char *cups_last_error(void);

/* the text parsers (unit-tested by tests/cups_test.c on sample output) */
int cups_parse_lpstat_v(char *text, CupsQueue *q, int max);
int cups_parse_lpstat_d(const char *text, char *name, int cb);
int cups_parse_lpinfo_v(char *text, CupsDevice *d, int max);
int cups_parse_lpinfo_m(char *text, CupsModel *m, int max);
int cups_parse_option(const char *text, const char *key, char *val, int cb);
/* a valid queue name made from a make-and-model ("Generic / Text Only" -> "Generic_Text_Only") */
void cups_queue_name(const char *model, char *name, int cb);
#endif
