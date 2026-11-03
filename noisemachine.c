/* noisemachine.c — real-time stereo sleep-sound player via SDL2, with
 * two-sound mixing.
 *
 * Build:  cc -O2 -o noisemachine noisemachine.c dsp.c $(sdl2-config --cflags --libs) -lm
 * Usage:  noisemachine SOUND[+OVERLAY[:GAIN]] [volume 0..1]
 *         e.g.  noisemachine rain 0.4
 *               noisemachine rain+airplane 0.4
 *               noisemachine sea+crickets:0.3 0.5
 *         Sounds: white pink brown deep|airplane rain sea wind stream
 *                 birds crickets.  Ctrl-C to stop.
 */
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

#define RATE 44100

static double vol = 0.3;

static int sound_index(const char *s)
{
    static const struct { const char *n; int i; } tab[] = {
        {"white",0},{"pink",1},{"brown",2},{"deep",3},{"airplane",3},
        {"rain",4},{"sea",5},{"wind",6},{"stream",7},{"birds",8},
        {"crickets",9},
    };
    for (size_t k = 0; k < sizeof tab / sizeof *tab; k++)
        if (strcmp(s, tab[k].n) == 0) return tab[k].i;
    return -1;
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    Sint16 *out = (Sint16 *)stream;
    int frames = len / (2 * (int)sizeof(Sint16));
    while (frames > 0) {
        int k = frames > 4096 ? 4096 : frames;
        dsp_render(k);
        const float *L = dsp_get_buf_l(), *R = dsp_get_buf_r();
        for (int i = 0; i < k; i++) {
            out[2 * i]     = (Sint16)lrint(L[i] * vol * 32767.0);
            out[2 * i + 1] = (Sint16)lrint(R[i] * vol * 32767.0);
        }
        out += 2 * k;
        frames -= k;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr,
            "usage: %s sound[+overlay[:gain]] [volume 0..1]\n"
            "sounds: white pink brown deep|airplane rain sea wind stream birds crickets\n"
            "e.g.:   %s rain+airplane:0.6 0.4\n", argv[0], argv[0]);
        return 1;
    }

    char spec[128];
    snprintf(spec, sizeof spec, "%s", argv[1]);
    double ov_gain = 0.5;
    int ov = -1;

    char *plus = strchr(spec, '+');
    if (plus) {
        *plus = '\0';
        char *colon = strchr(plus + 1, ':');
        if (colon) { *colon = '\0'; ov_gain = atof(colon + 1); }
        ov = sound_index(plus + 1);
        if (ov < 0) { fprintf(stderr, "unknown overlay '%s'\n", plus + 1); return 1; }
    }
    int snd = sound_index(spec);
    if (snd < 0) { fprintf(stderr, "unknown sound '%s'\n", spec); return 1; }

    if (argc == 3) {
        vol = atof(argv[2]);
        if (vol < 0.0) vol = 0.0;
        if (vol > 1.0) vol = 1.0;
    }

    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 4096;
    want.callback = audio_cb;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    dsp_init(have.freq > 0 ? have.freq : RATE);
    dsp_set_sound(snd);
    dsp_set_overlay(ov);
    dsp_set_overlay_gain(ov_gain);

    SDL_PauseAudioDevice(dev, 0);
    for (;;) SDL_Delay(1000);

    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
