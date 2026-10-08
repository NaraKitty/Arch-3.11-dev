/* MMSYSTEM: sndPlaySound, the wave device count, and the system sounds behind MessageBeep.
 *
 * 3.1's MMSYSTEM.DLL plays one waveform at a time through the wave driver. Here a sound is a player
 * process of the Linux sound server (pw-play, paplay or aplay from PATH, started without a shell).
 * The rules are MMSYSTEM's (seg3:0000, 0090, 0229, 05B1):
 *  - a name listed in WIN.INI [sounds] plays the file named before the comma there; any other name
 *    is a file name. The file is looked for as OpenFile does (current, Windows, system directory).
 *  - a file that is missing or not a RIFF WAVE with "fmt " and "data" chunks plays SystemDefault
 *    instead, unless SND_NODEFAULT
 *  - NULL stops the sound, "" succeeds without playing, SND_NOSTOP fails while a sound plays,
 *    SND_LOOP needs SND_ASYNC, unknown flags fail
 * MessageBeep plays through here as MMSOUND.DRV's DoBeep does (seg1:000A).
 *
 * Headless runs (W16_HEADLESS, the UI tests) never start a player: they log the file that would play.
 * ARCH311_WAVEDEVS overrides the device count (tests that must match a reference machine). */
#include "w16int.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int worker_on;            /* a worker thread waits for the player (joined by stop_sound) */
static pid_t child;              /* the player now running, 0 if none */
static int stopping, looping;
static char playing[1100];       /* the Linux path being played */
static int mem_fd = -1;          /* holds an SND_MEMORY image while it plays */

static int headless(void) { return getenv("W16_HEADLESS") != NULL; }

/* the first of the sound server's players found in PATH */
static const char *player(void)
{
    static const char *found;
    static int probed;
    if (!probed) {
        static const char *const names[] = {"pw-play", "paplay", "aplay"};
        const char *path = getenv("PATH");
        probed = 1;
        for (int i = 0; i < 3 && !found && path; i++) {
            char *dirs = strdup(path), *save;
            for (char *d = strtok_r(dirs, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
                char f[1100];
                snprintf(f, sizeof f, "%s/%s", d, names[i]);
                if (access(f, X_OK) == 0) found = names[i];
            }
            free(dirs);
        }
    }
    return found;
}

UINT waveOutGetNumDevs(void)
{
    const char *e = getenv("ARCH311_WAVEDEVS");
    if (e && *e) return (UINT)atoi(e);
    return player() ? 1 : 0;
}

static pid_t spawn_player(const char *path)
{
    const char *argv[] = {player(), path, NULL};
    pid_t p;
    if (!argv[0] || posix_spawnp(&p, argv[0], NULL, NULL, (char *const *)argv, environ)) return 0;
    return p;
}

/* waits for the player; a looping sound starts again until it is stopped */
static void *worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        pid_t p = child;
        pthread_mutex_unlock(&lock);
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        pthread_mutex_lock(&lock);
        child = looping && !stopping ? spawn_player(playing) : 0;
        int again = child != 0;
        pthread_mutex_unlock(&lock);
        if (!again) return NULL;
    }
}

static int sound_playing(void)
{
    pthread_mutex_lock(&lock);
    int on = child != 0;
    pthread_mutex_unlock(&lock);
    return on;
}

static void stop_sound(void)
{
    pthread_mutex_lock(&lock);
    stopping = 1;
    if (child) kill(child, SIGTERM);
    pthread_mutex_unlock(&lock);
    if (worker_on) pthread_join(worker, NULL);
    worker_on = stopping = looping = 0;
    if (mem_fd >= 0) close(mem_fd);
    mem_fd = -1;
}

/* seg3:05B1: "RIFF" .. "WAVE", a "fmt " chunk, then a "data" chunk, all inside the RIFF size */
static int riff_wave(const uint8_t *d, size_t n)
{
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    size_t riff = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24;
    if (riff > n) return 0;
    int fmt = 0;
    for (size_t o = 12; o + 8 <= riff + 8 && o + 8 <= n;) {
        size_t len = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (size_t)d[o + 7] << 24;
        if (!memcmp(d + o, "fmt ", 4)) fmt = 1;
        else if (fmt && !memcmp(d + o, "data", 4)) return 1;
        o += len + 8;
    }
    return 0;
}

/* seg3:0000: the [sounds] entry for `name` (or the name itself) up to the first blank or comma,
 * then where OpenFile finds it; returns the Linux path of a playable file, or 0 */
static int resolve(LPCSTR name, char *host, size_t cb)
{
    char f[128];
    OFSTRUCT of;
    GetProfileString("sounds", name, name, f, sizeof f);
    f[strcspn(f, " \t,")] = 0;
    if (!f[0] || OpenFile(f, &of, OF_EXIST | OF_SHARE_DENY_NONE) == HFILE_ERROR) return 0;
    OemToAnsi(of.szPathName, of.szPathName);
    if (w16_dos_to_host(of.szPathName, host, cb)) return 0;
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0;
    uint8_t *d = NULL;
    size_t n = 0, got;
    uint8_t buf[65536];
    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) {
        d = realloc(d, n + got);
        memcpy(d + n, buf, got);
        n += got;
    }
    fclose(fp);
    int ok = d && riff_wave(d, n);
    free(d);
    if (ok && headless()) fprintf(stderr, "arch311: sndPlaySound plays %s\n", of.szPathName);
    return ok;
}

/* starts `host` (stopping what plays); a synchronous sound returns when it has finished */
static BOOL play(const char *host, UINT flags)
{
    stop_sound();
    if (headless()) return TRUE;
    snprintf(playing, sizeof playing, "%s", host);
    pid_t p = spawn_player(playing);
    if (!p) return FALSE;
    if (!(flags & SND_ASYNC)) {
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        return TRUE;
    }
    pthread_mutex_lock(&lock);
    child = p;
    looping = (flags & SND_LOOP) != 0;
    pthread_mutex_unlock(&lock);
    worker_on = pthread_create(&worker, NULL, worker_main, NULL) == 0;
    if (!worker_on) stop_sound();
    return worker_on;
}

/* SND_MEMORY: the image in memory, handed to the player as a file descriptor */
static BOOL play_memory(const uint8_t *d, UINT flags)
{
    size_t n = 8 + (d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24);
    if (!riff_wave(d, n)) return FALSE;
    stop_sound();
    if (headless()) { fprintf(stderr, "arch311: sndPlaySound plays a %zu-byte image\n", n); return TRUE; }
    int fd = memfd_create("arch311-sound", 0);
    if (fd < 0) return FALSE;
    if (write(fd, d, n) != (ssize_t)n) { close(fd); return FALSE; }
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    BOOL ok = play(path, flags);
    if (ok && (flags & SND_ASYNC)) mem_fd = fd;
    else close(fd);
    return ok;
}

BOOL sndPlaySound(LPCSTR name, UINT flags)
{
    char host[1100];
    if ((flags & ~0x1F) || ((flags & SND_LOOP) && !(flags & SND_ASYNC))) return FALSE;
    if ((flags & SND_NOSTOP) && sound_playing()) return FALSE;
    if (!waveOutGetNumDevs()) return FALSE;
    if (!name) { stop_sound(); return TRUE; }
    if (flags & SND_MEMORY) return play_memory((const uint8_t *)name, flags);
    if (!*name) return TRUE;
    if (!resolve(name, host, sizeof host) && ((flags & SND_NODEFAULT) || !resolve("SystemDefault", host, sizeof host)))
        return FALSE;
    return play(host, flags);
}

/* ------------------------------------------------------------------ MessageBeep */
/* MMSOUND.DRV's speaker beep when no sound plays: PIT divisor 123FE0h / 750 = 1594 (about 750 Hz)
 * for a delay loop of 0F000h iterations, about 20 ms on a 33 MHz 386 */
static BOOL speaker_beep(void)
{
    enum { RATE = 11025, N = RATE / 50 };
    static uint8_t wav[44 + N];
    static const uint8_t hdr[36] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                    16, 0, 0, 0, 1, 0, 1, 0, RATE & 0xFF, RATE >> 8, 0, 0, RATE & 0xFF, RATE >> 8, 0, 0,
                                    1, 0, 8, 0};
    memcpy(wav, hdr, sizeof hdr);
    uint32_t riff = sizeof wav - 8, data = N;
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data, 4);
    for (int i = 0; i < N; i++) wav[44 + i] = (i * 750 * 2 / RATE) & 1 ? 0x40 : 0xC0;
    return sndPlaySound((LPCSTR)wav, SND_ASYNC | SND_MEMORY);
}

void MessageBeep(UINT type)
{
    static const char *const names[5] = {"SystemDefault", "SystemHand", "SystemQuestion", "SystemExclamation",
                                         "SystemAsterisk"};
    if (!w16_beep) return;
    UINT i = (type & 0xF0) >> 4;
    if (i >= 5) i = 0;
    if (!sndPlaySound(names[i], SND_ASYNC)) speaker_beep();
}
