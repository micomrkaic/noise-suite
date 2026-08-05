/* noisesdl.c — play white, pink, or brown noise in real time via SDL2.
 *
 * Build:  cc -O2 -o noisesdl noisesdl.c $(sdl2-config --cflags --libs) -lm
 *         (needs: sudo apt install libsdl2-dev)
 * Usage:  ./noisesdl white|pink|brown [volume 0..1]
 *         Ctrl-C to stop.
 */
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 44100

static double vol = 0.3;

static double white(void)
{
    return 2.0 * ((double)rand() / ((double)RAND_MAX + 1.0)) - 1.0;
}

static double pink(void)
{
    static double b0, b1, b2, b3, b4, b5, b6;
    double w = white();
    b0 = 0.99886 * b0 + w * 0.0555179;
    b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.96900 * b2 + w * 0.1538520;
    b3 = 0.86650 * b3 + w * 0.3104856;
    b4 = 0.55000 * b4 + w * 0.5329522;
    b5 = -0.7616 * b5 - w * 0.0168980;
    double out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
    b6 = w * 0.115926;
    return out * 0.11;
}

static double brown(void)
{
    static double acc;
    acc = 0.997 * acc + 0.02 * white();
    return acc * 3.5;
}

static double (*gen)(void);

/* Runs on SDL's audio thread whenever the device needs more samples.
 * stream is raw bytes; we asked for AUDIO_S16SYS mono, so cast it. */
static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    Sint16 *out = (Sint16 *)stream;
    int n = len / (int)sizeof(Sint16);
    for (int i = 0; i < n; i++) {
        double s = gen() * vol;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        out[i] = (Sint16)lrint(s * 32767.0);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s white|pink|brown [volume 0..1]\n", argv[0]);
        return 1;
    }
    gen = strcmp(argv[1], "white") == 0 ? white :
          strcmp(argv[1], "pink")  == 0 ? pink  :
          strcmp(argv[1], "brown") == 0 ? brown : NULL;
    if (!gen) { fprintf(stderr, "unknown noise type '%s'\n", argv[1]); return 1; }

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
    want.channels = 1;
    want.samples  = 4096;          /* frames per callback (~93 ms) */
    want.callback = audio_cb;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    srand((unsigned)time(NULL));
    SDL_PauseAudioDevice(dev, 0);  /* devices start paused: unpause = play */

    /* Main thread just idles; audio thread does the work. */
    for (;;) SDL_Delay(1000);

    /* unreachable, but the tidy shutdown would be: */
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
