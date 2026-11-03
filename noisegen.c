/* noisegen.c — render any ZzzTop sound to a 16-bit stereo WAV.
 *
 * Build:  cc -O2 -o noisegen noisegen.c dsp.c -lm
 * Usage:  noisegen SOUND seconds out.wav [loop]
 *         SOUND: white pink brown deep|airplane rain sea wind stream
 *                birds crickets
 *         loop:  crossfade the last second into the first so the file
 *                loops seamlessly in any player.
 *
 * (Historically this program made only the colored noises and
 * soundscape made the nature sounds; both are now thin front ends to
 * the shared DSP core and accept every sound.)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp.h"

#define RATE 44100

int zz_sound_index(const char *s)
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

static void wav_header(FILE *f, uint32_t nframes)
{
    uint32_t data_bytes = nframes * 4;        /* 16-bit stereo */
    uint32_t byte_rate  = RATE * 4;
    uint32_t chunk = 36 + data_bytes;
    uint16_t u16; uint32_t u32;
    fwrite("RIFF", 1, 4, f); u32 = chunk; fwrite(&u32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u32 = 16;   fwrite(&u32, 4, 1, f);
    u16 = 1;    fwrite(&u16, 2, 1, f);        /* PCM */
    u16 = 2;    fwrite(&u16, 2, 1, f);        /* stereo */
    u32 = RATE; fwrite(&u32, 4, 1, f);
    u32 = byte_rate; fwrite(&u32, 4, 1, f);
    u16 = 4;    fwrite(&u16, 2, 1, f);        /* block align */
    u16 = 16;   fwrite(&u16, 2, 1, f);
    fwrite("data", 1, 4, f); u32 = data_bytes; fwrite(&u32, 4, 1, f);
}

static void put_frames(FILE *f, const float *l, const float *r, int n)
{
    static int16_t pcm[2 * 4096];
    while (n > 0) {
        int k = n > 4096 ? 4096 : n;
        for (int i = 0; i < k; i++) {
            pcm[2 * i]     = (int16_t)lrintf(l[i] * 32767.0f);
            pcm[2 * i + 1] = (int16_t)lrintf(r[i] * 32767.0f);
        }
        fwrite(pcm, 2, 2 * (size_t)k, f);
        l += k; r += k; n -= k;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "loop") != 0)) {
        fprintf(stderr,
            "usage: %s sound seconds out.wav [loop]\n"
            "sounds: white pink brown deep|airplane rain sea wind stream birds crickets\n",
            argv[0]);
        return 1;
    }
    int snd = zz_sound_index(argv[1]);
    if (snd < 0) { fprintf(stderr, "unknown sound '%s'\n", argv[1]); return 1; }

    double seconds = atof(argv[2]);
    if (seconds <= 0) { fprintf(stderr, "bad duration\n"); return 1; }
    uint32_t n = (uint32_t)(seconds * RATE);
    int looping = (argc == 5);
    uint32_t F = looping ? (n / 4 < RATE ? n / 4 : RATE) : 0;

    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 1; }

    dsp_init(RATE);
    dsp_set_sound(snd);
    wav_header(f, n);

    float *headL = NULL, *headR = NULL;
    if (looping) {
        headL = malloc(F * sizeof *headL);
        headR = malloc(F * sizeof *headR);
        if (!headL || !headR) { fprintf(stderr, "oom\n"); return 1; }
    }
    long data_pos = ftell(f);

    /* pass 1: stream all n frames; remember the first F for the loop mix */
    uint32_t done = 0;
    while (done < n) {
        int k = (n - done > 4096) ? 4096 : (int)(n - done);
        dsp_render(k);
        const float *L = dsp_get_buf_l(), *R = dsp_get_buf_r();
        if (looping && done < F) {
            uint32_t c = (F - done > (uint32_t)k) ? (uint32_t)k : F - done;
            memcpy(headL + done, L, c * sizeof *L);
            memcpy(headR + done, R, c * sizeof *R);
        }
        put_frames(f, L, R, k);
        done += k;
    }

    if (looping) {
        /* pass 2: render F extra frames (the "tail past the end") and
         * equal-power crossfade them under the head, so sample n-1
         * flows into sample 0 with no seam. */
        float *mixL = malloc(F * sizeof *mixL), *mixR = malloc(F * sizeof *mixR);
        if (!mixL || !mixR) { fprintf(stderr, "oom\n"); return 1; }
        uint32_t got = 0;
        while (got < F) {
            int k = (F - got > 4096) ? 4096 : (int)(F - got);
            dsp_render(k);
            const float *L = dsp_get_buf_l(), *R = dsp_get_buf_r();
            for (int i = 0; i < k; i++) {
                double th = 0.5 * M_PI * (double)(got + i) / (double)F;
                double in = sin(th), out = cos(th);
                mixL[got + i] = (float)(headL[got + i] * in + L[i] * out);
                mixR[got + i] = (float)(headR[got + i] * in + R[i] * out);
            }
            got += k;
        }
        fseek(f, data_pos, SEEK_SET);
        put_frames(f, mixL, mixR, (int)F);
        free(mixL); free(mixR);
    }
    free(headL); free(headR);
    fclose(f);
    return 0;
}
