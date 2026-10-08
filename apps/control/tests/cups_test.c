/* Unit test of the CUPS output parsers (control/cupsparse.c): make -C apps check
 * The samples are written in the formats CUPS 2.x prints with LC_ALL=C (lpstat -v / -d, lpinfo -v,
 * lpinfo -l -v, lpinfo -m, lpinfo -l -m, lpoptions -p). UNTESTED: output captured from a real CUPS
 * server (the WSL build host has none). */
#include "cups.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void expect_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want)) {
        failures++;
        printf("FAIL %s: got '%s', want '%s'\n", what, got, want);
    }
}

static void expect_int(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %d, want %d\n", what, got, want);
    }
}

static void test_lpstat_v(void)
{
    char text[] = "device for HP_LaserJet_4: usb://HP/LaserJet%204?serial=00XY1234\n"
                  "device for Office:Color: ipp://10.0.0.5/ipp/print\r\n"
                  "device for PDF: cups-pdf:/\n"
                  "lpstat: Unknown printer\n"
                  "device for Text_Only: file:///dev/null";
    CupsQueue q[8];
    int n = cups_parse_lpstat_v(text, q, 8);
    expect_int("lpstat -v count", n, 4);
    expect_str("lpstat -v name 0", q[0].name, "HP_LaserJet_4");
    expect_str("lpstat -v uri 0", q[0].uri, "usb://HP/LaserJet%204?serial=00XY1234");
    expect_str("lpstat -v name with ':'", q[1].name, "Office:Color");
    expect_str("lpstat -v uri, CRLF", q[1].uri, "ipp://10.0.0.5/ipp/print");
    expect_str("lpstat -v uri 2", q[2].uri, "cups-pdf:/");
    expect_str("lpstat -v last line", q[3].name, "Text_Only");
    char two[] = "device for A: socket://a\ndevice for B: socket://b\n";
    expect_int("lpstat -v max", cups_parse_lpstat_v(two, q, 1), 1);
}

static void test_lpstat_d(void)
{
    char name[64];
    expect_int("lpstat -d", cups_parse_lpstat_d("system default destination: HP_LaserJet_4\n", name, sizeof name), 1);
    expect_str("lpstat -d name", name, "HP_LaserJet_4");
    expect_int("lpstat -d none", cups_parse_lpstat_d("no system default destination\n", name, sizeof name), 0);
    expect_str("lpstat -d none name", name, "");
    cups_parse_lpstat_d("system default destination: Office/duplex\n", name, sizeof name);
    expect_str("lpstat -d instance", name, "Office");
}

static void test_lpinfo_v(void)
{
    char text[] = "Device: uri = socket\n"
                  "        class = network\n"
                  "        info = AppSocket/HP JetDirect\n"
                  "        make-and-model = Unknown\n"
                  "        device-id = \n"
                  "        location = \n"
                  "Device: uri = usb://HP/LaserJet%201020?serial=00CNBW123456\n"
                  "        class = direct\n"
                  "        info = HP LaserJet 1020\n"
                  "        make-and-model = HP LaserJet 1020\n"
                  "        device-id = MFG:Hewlett-Packard;CMD:ZJS;MDL:HP LaserJet 1020;\n"
                  "        location = \n"
                  "Device: uri = parallel:/dev/lp0\n"
                  "        class = direct\n"
                  "        info = LPT #1\n"
                  "        make-and-model = Unknown\n"
                  "        device-id = \n"
                  "        location = \n"
                  "Device: uri = ipp\n"
                  "        class = network\n"
                  "        info = Internet Printing Protocol (ipp)\n"
                  "        make-and-model = Unknown\n"
                  "        device-id = \n"
                  "        location = \n";
    CupsDevice d[8];
    int n = cups_parse_lpinfo_v(text, d, 8);
    expect_int("lpinfo -l -v count (backends without an address left out)", n, 2);
    expect_str("lpinfo -l -v uri 0", d[0].uri, "usb://HP/LaserJet%201020?serial=00CNBW123456");
    expect_str("lpinfo -l -v class 0", d[0].cls, "direct");
    expect_str("lpinfo -l -v info 0", d[0].info, "HP LaserJet 1020");
    expect_str("lpinfo -l -v uri 1", d[1].uri, "parallel:/dev/lp0");
    expect_str("lpinfo -l -v info 1", d[1].info, "LPT #1");

    char shortform[] = "network socket\n"
                       "direct usb://Canon/MG3600%20series?serial=1A2B3C\n"
                       "serial serial:/dev/ttyS0?baud=115200\n"
                       "network lpd\n"
                       "file cups-pdf:/\n";
    n = cups_parse_lpinfo_v(shortform, d, 8);
    expect_int("lpinfo -v count", n, 3);
    expect_str("lpinfo -v class", d[0].cls, "direct");
    expect_str("lpinfo -v uri", d[0].uri, "usb://Canon/MG3600%20series?serial=1A2B3C");
    expect_str("lpinfo -v serial", d[1].uri, "serial:/dev/ttyS0?baud=115200");
    expect_str("lpinfo -v file", d[2].uri, "cups-pdf:/");
    expect_str("lpinfo -v no info", d[2].info, "");
}

static void test_lpinfo_m(void)
{
    char text[] = "Model:  name = drv:///sample.drv/generic.ppd\n"
                  "        natural_language = en\n"
                  "        make-and-model = Generic PostScript Printer\n"
                  "        device-id = MFG:Generic;CMD:PJL,PS;MDL:PostScript Printer;\n"
                  "Model:  name = lsb/usr/HP/hp-laserjet_4_plus-ps.ppd.gz\n"
                  "        natural_language = en\n"
                  "        make-and-model = HP LaserJet 4 Plus Postscript (recommended)\n"
                  "        device-id = \n"
                  "Model:  name = everywhere\n"
                  "        natural_language = en\n"
                  "        make-and-model = IPP Everywhere\n"
                  "        device-id = \n";
    CupsModel m[8];
    int n = cups_parse_lpinfo_m(text, m, 8);
    expect_int("lpinfo -l -m count", n, 3);
    expect_str("lpinfo -l -m ppd 0", m[0].ppd, "drv:///sample.drv/generic.ppd");
    expect_str("lpinfo -l -m model 0", m[0].model, "Generic PostScript Printer");
    expect_str("lpinfo -l -m ppd 1", m[1].ppd, "lsb/usr/HP/hp-laserjet_4_plus-ps.ppd.gz");
    expect_str("lpinfo -l -m model 1", m[1].model, "HP LaserJet 4 Plus Postscript (recommended)");
    expect_str("lpinfo -l -m model 2", m[2].model, "IPP Everywhere");

    char shortform[] = "drv:///sample.drv/generpcl.ppd Generic PCL Laser Printer\n"
                       "drv:///hpcups.drv/hp-laserjet_1020.ppd HP LaserJet 1020, hpcups 3.22.10\n"
                       "everywhere IPP Everywhere\n";
    n = cups_parse_lpinfo_m(shortform, m, 2);
    expect_int("lpinfo -m count (max)", n, 2);
    expect_str("lpinfo -m ppd", m[1].ppd, "drv:///hpcups.drv/hp-laserjet_1020.ppd");
    expect_str("lpinfo -m model", m[1].model, "HP LaserJet 1020, hpcups 3.22.10");
}

static void test_lpoptions(void)
{
    const char *text = "copies=1 device-uri=usb://HP/LaserJet%204 finishings=3 media=A4 "
                       "orientation-requested=4 printer-info='HP LaserJet 4' "
                       "printer-make-and-model='HP LaserJet 4 Plus, hpcups 3.22.10' printer-is-shared=false job-sheets=none,none";
    char v[128];
    expect_int("lpoptions media found", cups_parse_option(text, "media", v, sizeof v), 1);
    expect_str("lpoptions media", v, "A4");
    cups_parse_option(text, "orientation-requested", v, sizeof v);
    expect_str("lpoptions orientation", v, "4");
    cups_parse_option(text, "printer-info", v, sizeof v);
    expect_str("lpoptions quoted", v, "HP LaserJet 4");
    cups_parse_option(text, "job-sheets", v, sizeof v);
    expect_str("lpoptions last", v, "none,none");
    expect_int("lpoptions missing", cups_parse_option(text, "sides", v, sizeof v), 0);
    expect_int("lpoptions prefix is not the key", cups_parse_option(text, "printer", v, sizeof v), 0);
}

static void test_queue_name(void)
{
    char n[128], longmodel[300];
    cups_queue_name("Generic / Text Only", n, sizeof n);
    expect_str("queue name", n, "Generic_Text_Only");
    cups_queue_name("HP LaserJet 4 Plus, hpcups 3.22.10", n, sizeof n);
    expect_str("queue name punctuation", n, "HP_LaserJet_4_Plus_hpcups_3.22.10");
    cups_queue_name("  (Agfa) 9000 Series PS  ", n, sizeof n);
    expect_str("queue name ends", n, "Agfa_9000_Series_PS");
    cups_queue_name("///", n, sizeof n);
    expect_str("queue name empty", n, "Printer");
    memset(longmodel, 'x', sizeof longmodel - 1);
    longmodel[sizeof longmodel - 1] = 0;
    cups_queue_name(longmodel, n, sizeof n);
    expect_int("queue name at most 127", (int)strlen(n), 127);
}

int main(void)
{
    test_lpstat_v();
    test_lpstat_d();
    test_lpinfo_v();
    test_lpinfo_m();
    test_lpoptions();
    test_queue_name();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
