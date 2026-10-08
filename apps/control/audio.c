/* PipeWire volume control through wpctl. UNTESTED against a real PipeWire session: the WSL build
 * host has none; ARCH311_SIMULATE=1 exercises the applet with sample devices. */
#include "audio.h"
#include "sysexec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *w16_assets_dir(void);

static const char *target(int which) { return which == AUDIO_IN ? "@DEFAULT_AUDIO_SOURCE@" : "@DEFAULT_AUDIO_SINK@"; }

/* simulated system */
static struct { int vol[2], mute[2], def; } sim = {{70, 40}, {0, 0}, 47};
static const AudioDev sim_devs[] = {{47, "Built-in Audio Analog Stereo", 0}, {52, "HDMI / DisplayPort 1 Output", 0},
                                    {58, "USB Headset", 0}};

int audio_available(void)
{
    if (sys_simulated()) return 1;
    const char *argv[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
    char out[128];
    return sys_run(argv, NULL, out, sizeof out, NULL, 0, 3000) == 0;
}

int audio_get(int which, int *percent, int *muted)
{
    if (sys_simulated()) { *percent = sim.vol[which]; *muted = sim.mute[which]; return 1; }
    const char *argv[] = {"wpctl", "get-volume", target(which), NULL};
    char out[128];
    if (sys_run(argv, NULL, out, sizeof out, NULL, 0, 3000) != 0) return 0;
    /* "Volume: 0.40" or "Volume: 0.40 [MUTED]" */
    double v = 0;
    if (sscanf(out, "Volume: %lf", &v) != 1) return 0;
    *percent = (int)(v * 100 + 0.5);
    if (*percent > 100) *percent = 100;
    *muted = strstr(out, "[MUTED]") != NULL;
    return 1;
}

int audio_set_volume(int which, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (sys_simulated()) { sim.vol[which] = percent; return 1; }
    char v[16];
    snprintf(v, sizeof v, "%d%%", percent);
    const char *argv[] = {"wpctl", "set-volume", target(which), v, NULL};
    return sys_run(argv, NULL, NULL, 0, NULL, 0, 3000) == 0;
}

int audio_set_mute(int which, int muted)
{
    if (sys_simulated()) { sim.mute[which] = muted != 0; return 1; }
    const char *argv[] = {"wpctl", "set-mute", target(which), muted ? "1" : "0", NULL};
    return sys_run(argv, NULL, NULL, 0, NULL, 0, 3000) == 0;
}

int audio_list_outputs(AudioDev *d, int max)
{
    int n = 0;
    if (sys_simulated()) {
        for (size_t i = 0; i < sizeof sim_devs / sizeof sim_devs[0] && n < max; i++) {
            d[n] = sim_devs[i];
            d[n].is_default = d[n].id == sim.def;
            n++;
        }
        return n;
    }
    const char *argv[] = {"wpctl", "status", NULL};
    static char out[32768];
    if (sys_run(argv, NULL, out, sizeof out, NULL, 0, 5000) != 0) return 0;
    /* the "Sinks:" block of the Audio section:  " │  *   46. Built-in Audio Analog Stereo [vol: 0.40]" */
    int in_audio = 0, in_sinks = 0;
    for (char *line = strtok(out, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "Audio", 5)) { in_audio = 1; continue; }
        if (!strncmp(line, "Video", 5) || !strncmp(line, "Settings", 8)) { in_audio = 0; in_sinks = 0; continue; }
        if (!in_audio) continue;
        if (strstr(line, "Sinks:")) { in_sinks = 1; continue; }
        if (!in_sinks) continue;
        if (strstr(line, "Sources:") || strstr(line, "Filters:") || strstr(line, "Streams:")) { in_sinks = 0; continue; }
        char *p = line;
        int def = 0;
        while (*p && !(*p >= '0' && *p <= '9')) { if (*p == '*') def = 1; p++; }
        if (!*p) { in_sinks = 0; continue; } /* the blank " │" line ends the block */
        int id = atoi(p);
        char *name = strchr(p, '.');
        if (!name) continue;
        name++;
        while (*name == ' ') name++;
        char *vol = strstr(name, "[vol:");
        if (vol) *vol = 0;
        for (size_t L = strlen(name); L && name[L - 1] == ' '; L--) name[L - 1] = 0;
        d[n].id = id;
        d[n].is_default = def;
        snprintf(d[n].name, sizeof d[n].name, "%s", name);
        n++;
    }
    return n;
}

int audio_set_default_output(int id)
{
    if (sys_simulated()) { sim.def = id; return 1; }
    char v[16];
    snprintf(v, sizeof v, "%d", id);
    const char *argv[] = {"wpctl", "set-default", v, NULL};
    return sys_run(argv, NULL, NULL, 0, NULL, 0, 3000) == 0;
}

int audio_play_test(void)
{
    if (sys_simulated()) return 1;
    static const char *names[] = {"DING.WAV", "CHIMES.WAV", "TADA.WAV"};
    for (int i = 0; i < 3; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/files/%s", w16_assets_dir(), names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        const char *argv[] = {"pw-play", path, NULL};
        return sys_run(argv, NULL, NULL, 0, NULL, 0, 10000) == 0;
    }
    return 0;
}
