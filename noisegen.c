/* noisegen.c — generate white, pink, or brown noise as a 16-bit mono WAV.
 *
 * Build:  cc -O2 -o noisegen noisegen.c -lm
 * Usage:  ./noisegen white|pink|brown|deep seconds out.wav
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define RATE 44100

/* uniform white sample in [-1, 1) */
static double white(void)
{
    return 2.0 * ((double)rand() / ((double)RAND_MAX + 1.0)) - 1.0;
}

/* Paul Kellet's "refined" pink noise filter: cascade of leaky
 * integrators whose sum approximates a -3 dB/octave slope to
 * within ~0.05 dB over the audio band. State persists in statics. */
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
    return out * 0.11; /* scale roughly into [-1, 1] */
}

/* Brown (red) noise: leaky integration of white noise.
 * The 0.997 leak is a gentle high-pass that stops DC drift. */
static double brown(void)
{
    static double acc;
    acc = 0.997 * acc + 0.02 * white();
    return acc * 3.5; /* rough loudness match */
}

/* 1/f^4 ("black"): brown through a second AR(1) stage, -12 dB/octave */
static double deep(void)
{
    static double a1, a2;
    const double p = 0.997;
    a1 = p * a1 + white();
    a2 = p * a2 + a1;
    const double var = (1.0 + p * p) /
                       ((1.0 - p * p) * (1.0 - p * p) * (1.0 - p * p)) / 3.0;
    return a2 / sqrt(var) * 0.5;
}

static void wav_header(FILE *f, uint32_t nsamples)
{
    uint32_t data_bytes = nsamples * 2;      /* 16-bit mono */
    uint32_t byte_rate  = RATE * 2;
    uint32_t chunk_size = 36 + data_bytes;
    uint16_t u16; uint32_t u32;

    fwrite("RIFF", 1, 4, f);
    u32 = chunk_size;  fwrite(&u32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u32 = 16;          fwrite(&u32, 4, 1, f);  /* PCM fmt chunk size */
    u16 = 1;           fwrite(&u16, 2, 1, f);  /* PCM */
    u16 = 1;           fwrite(&u16, 2, 1, f);  /* mono */
    u32 = RATE;        fwrite(&u32, 4, 1, f);
    u32 = byte_rate;   fwrite(&u32, 4, 1, f);
    u16 = 2;           fwrite(&u16, 2, 1, f);  /* block align */
    u16 = 16;          fwrite(&u16, 2, 1, f);  /* bits/sample */
    fwrite("data", 1, 4, f);
    u32 = data_bytes;  fwrite(&u32, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s white|pink|brown|deep seconds out.wav\n", argv[0]);
        return 1;
    }
    double (*gen)(void) =
        strcmp(argv[1], "white") == 0 ? white :
        strcmp(argv[1], "pink")  == 0 ? pink  :
        strcmp(argv[1], "brown") == 0 ? brown :
        strcmp(argv[1], "deep")  == 0 ? deep  : NULL;
    if (!gen) { fprintf(stderr, "unknown noise type '%s'\n", argv[1]); return 1; }

    double seconds = atof(argv[2]);
    if (seconds <= 0) { fprintf(stderr, "bad duration\n"); return 1; }
    uint32_t n = (uint32_t)(seconds * RATE);

    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 1; }

    srand((unsigned)time(NULL));
    wav_header(f, n);

    const double amp = 0.5; /* headroom: peak at -6 dBFS-ish */
    for (uint32_t i = 0; i < n; i++) {
        double s = gen() * amp;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        int16_t pcm = (int16_t)lrint(s * 32767.0);
        fwrite(&pcm, 2, 1, f);
    }
    fclose(f);
    return 0;
}
