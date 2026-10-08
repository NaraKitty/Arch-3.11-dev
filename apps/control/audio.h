/* Sound volume back end for the Volume applet: PipeWire through wpctl (WirePlumber). */
#ifndef ARCH311_AUDIO_H
#define ARCH311_AUDIO_H

enum { AUDIO_OUT, AUDIO_IN };   /* default sink (speakers) / default source (microphone) */

typedef struct {
    int id;
    char name[96];
    int is_default;
} AudioDev;

int audio_available(void);                       /* wpctl can talk to PipeWire */
int audio_list_outputs(AudioDev *d, int max);    /* sinks, in wpctl order */
int audio_get(int which, int *percent, int *muted);
int audio_set_volume(int which, int percent);    /* 0..100 */
int audio_set_mute(int which, int muted);
int audio_set_default_output(int id);
int audio_play_test(void);                       /* the ripped DING.WAV (or CHIMES.WAV) */
#endif
