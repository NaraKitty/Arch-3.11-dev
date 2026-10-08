/* SOUND: OpenSound .. StopSound, the 3.x voice interface, as 3.11's MMSOUND.DRV implements it (the
 * driver SYSTEM.INI [boot] sound.drv= names; its module name is SOUND): one voice, the PC speaker.
 *  - OpenSound (seg1:03F0): one owner at a time, queue of 0C0h bytes (32 notes of 6 bytes), tempo 120,
 *    mode S_NORMAL, pitch 0
 *  - SetVoiceAccent (seg1:0677): tempo 32..255, mode 0..2, pitch 0..83, each checked in that order
 *    after the previous one was stored; the volume is ignored
 *  - SetVoiceNote (seg1:054B): a note lasts 96000 / (tempo * length) ticks, the two multiplied as
 *    bytes; the dots are ignored (seg1:058D compares bx with itself); a legato note sounds for all of
 *    it, a normal one for 7/8 and a staccato one for 3/4, then silence; note 1..84 plus the pitch
 *    (past 84 it wraps) is octave (n - 1) / 12, step (n - 1) % 12 of the equal-tempered octave 8
 *    table (C8 = 4186 Hz) shifted down 6 - octave times, rounding up the last bit out (seg1:05DD);
 *    the speaker's PIT divisor is 1234DCh / f; note 0 is a rest
 *  - ticks become timer ticks of 5 ms rounded up from halves (seg1:0396, the virtual-timer path of
 *    386 enhanced mode), so a tick is 2.5 ms
 *  - StartSound plays the queue, StopSound stops it and empties it, CloseSound gives the voice back.
 * Here StartSound renders the queued notes as a square wave and hands the image to the sound server
 * as the speaker beep does (mmsystem.c); headless runs log it. UNTESTED by ear: the test machine has no
 * sound server. */
#include "w16int.h"

static const WORD octave8[12] = {4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902};

typedef struct { WORD divisor; WORD on, off; } Note; /* divisor 0: silence; on/off in timer ticks */
static Note *queue;
static int nq, capq;
static int owner;
static BYTE tempo = 120, mode = S_NORMAL, pitch;
static DWORD sounding_until; /* GetTickCount when the last StartSound finishes */

int OpenSound(void)
{
    if (owner) return S_SERDVNA;
    capq = 0xC0 / 6;
    queue = calloc(capq, sizeof *queue);
    if (!queue) return S_SEROFM;
    owner = 1;
    nq = 0;
    tempo = 120;
    mode = S_NORMAL;
    pitch = 0;
    return 1;
}

int StopSound(void)
{
    if (!owner) return 0;
    nq = 0;
    if ((int)(sounding_until - GetTickCount()) > 0) sndPlaySound(NULL, 0);
    sounding_until = GetTickCount();
    return 0;
}

void CloseSound(void)
{
    if (!owner) return;
    StopSound();
    free(queue);
    queue = NULL;
    capq = 0;
    owner = 0;
}

int CountVoiceNotes(int voice) { return owner && voice == 1 ? nq : 0; }

/* seg1:04D6: only while nothing is queued; a whole number of 6-byte notes */
int SetVoiceQueueSize(int voice, int bytes)
{
    if (!owner || voice != 1) return 0;
    if (nq) return S_SERMACT;
    Note *q = calloc((unsigned)bytes / 6 ? (unsigned)bytes / 6 : 1, sizeof *q);
    if (!q) { capq = 0; return S_SEROFM; }
    free(queue);
    queue = q;
    capq = (unsigned)bytes / 6;
    return 0;
}

int SetVoiceAccent(int voice, int t, int volume, int m, int p)
{
    (void)volume;
    if (!owner || voice != 1) return 0;
    if (t > 255 || t < 32) return S_SERDTP;
    tempo = (BYTE)t;
    if ((unsigned)m > 2) return S_SERDMD;
    mode = (BYTE)m;
    if ((unsigned)p > 83) return S_SERDPT;
    pitch = (BYTE)p;
    return 0;
}

/* seg1:0396 (virtual timer): driver ticks -> 5 ms timer ticks */
static WORD timer_ticks(unsigned t) { return (WORD)((t + 1) >> 1); }

/* seg1:0338: queue a note; freq 0 is silence for all of it */
static int queue_note(unsigned freq, unsigned on, unsigned off)
{
    if (nq >= capq) return S_SERQFUL;
    Note n = {0, 0, 0};
    if (!freq || !on) {
        n.off = timer_ticks(on + off);
    } else {
        if (freq < 0x25 || freq > 0x7FFF) return S_SERDFQ;
        n.divisor = (WORD)(0x1234DCul / freq);
        n.on = timer_ticks(on);
        n.off = timer_ticks(off);
    }
    queue[nq++] = n;
    return 0;
}

int SetVoiceNote(int voice, int value, int length, int cdots)
{
    (void)cdots;
    if (!owner || voice != 1) return 0;
    unsigned prod = (unsigned)tempo * (BYTE)length;
    if (!prod) return value;
    unsigned ticks = 0x17700u / prod;
    if (!ticks) return value;
    if (!value) return queue_note(0, ticks, 0);
    unsigned on = mode == S_LEGATO ? ticks : mode == S_STACCATO ? (ticks * 3) >> 2 : (ticks * 7) >> 3;
    if (!on) on = 1;
    if ((unsigned)value > 84) return S_SERBDNT;
    unsigned n = (BYTE)(value + pitch);
    if (n > 84) n -= 84;
    n--;
    int shift = 6 - (int)(n / 12);
    unsigned f = octave8[n % 12];
    f = shift > 0 ? (f >> shift) + ((f >> (shift - 1)) & 1) : f;
    return queue_note(f, on, ticks - on);
}

/* the queued notes as an 8-bit mono WAV image, square waves at the speaker's frequency */
int StartSound(void)
{
    enum { RATE = 22050 };
    if (!owner || !nq) return 0;
    size_t samples = 0;
    for (int i = 0; i < nq; i++) samples += (size_t)(queue[i].on + queue[i].off) * 5 * RATE / 1000;
    uint8_t *wav = malloc(44 + samples);
    if (!wav) return 0;
    static const uint8_t hdr[36] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                    16, 0, 0, 0, 1, 0, 1, 0, RATE & 0xFF, RATE >> 8, 0, 0, RATE & 0xFF, RATE >> 8, 0, 0,
                                    1, 0, 8, 0};
    memcpy(wav, hdr, sizeof hdr);
    uint32_t riff = (uint32_t)(36 + samples), data = (uint32_t)samples;
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data, 4);
    uint8_t *p = wav + 44;
    DWORD ms = 0;
    for (int i = 0; i < nq; i++) {
        size_t on = (size_t)queue[i].on * 5 * RATE / 1000, off = (size_t)queue[i].off * 5 * RATE / 1000;
        double hz = queue[i].divisor ? 1193180.0 / queue[i].divisor : 0;
        for (size_t k = 0; k < on; k++) *p++ = (unsigned)(k * hz * 2 / RATE) & 1 ? 0x40 : 0xC0;
        memset(p, 0x80, off);
        p += off;
        ms += (DWORD)(queue[i].on + queue[i].off) * 5;
    }
    nq = 0;
    if (sndPlaySound((LPCSTR)wav, SND_ASYNC | SND_MEMORY)) sounding_until = GetTickCount() + ms;
    free(wav);
    return 0;
}
