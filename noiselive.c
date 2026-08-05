/* noiselive.c — play white, pink, or brown noise in real time via ALSA.
 *
 * Build:  cc -O2 -o noiselive noiselive.c -lasound -lm
 *         (needs: sudo apt install libasound2-dev)
 * Usage:  ./noiselive white|pink|brown [volume 0..1]
 *         Ctrl-C to stop.
 */
#include <alsa/asoundlib.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE   44100
#define CHUNK  1024          /* frames per write: ~23 ms of audio */

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static double white(void)
{
    return 2.0 * ((double)rand() / ((double)RAND_MAX + 1.0)) - 1.0;
}

static double pink(void)                     /* Kellet filter, as before */
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

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s white|pink|brown [volume 0..1]\n", argv[0]);
        return 1;
    }
    double (*gen)(void) =
        strcmp(argv[1], "white") == 0 ? white :
        strcmp(argv[1], "pink")  == 0 ? pink  :
        strcmp(argv[1], "brown") == 0 ? brown : NULL;
    if (!gen) { fprintf(stderr, "unknown noise type '%s'\n", argv[1]); return 1; }

    double vol = (argc == 3) ? atof(argv[2]) : 0.3;
    if (vol < 0.0) vol = 0.0;
    if (vol > 1.0) vol = 1.0;

    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(err));
        return 1;
    }

    /* One call sets format, rate, channels, and asks ALSA for a
     * 500 ms hardware buffer — big buffer = no underruns, and for a
     * noise machine nobody cares about latency. */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1 /* mono */, RATE,
                             1 /* allow resampling */,
                             500000 /* latency, usec */);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_set_params: %s\n", snd_strerror(err));
        return 1;
    }

    signal(SIGINT, on_sigint);
    srand((unsigned)time(NULL));

    int16_t buf[CHUNK];
    while (running) {
        for (int i = 0; i < CHUNK; i++) {
            double s = gen() * vol;
            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;
            buf[i] = (int16_t)lrint(s * 32767.0);
        }
        /* Blocks while the card's buffer is full — this is the clock. */
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, CHUNK);
        if (n == -EPIPE) {                 /* underrun: recover and go on */
            snd_pcm_prepare(pcm);
        } else if (n < 0) {
            fprintf(stderr, "writei: %s\n", snd_strerror((int)n));
            break;
        }
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return 0;
}
