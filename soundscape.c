/* soundscape.c — synthesize rain or ocean waves to a 16-bit mono WAV.
 *
 * Build:  cc -O2 -o soundscape soundscape.c -lm
 * Usage:  ./soundscape rain|sea|wind|stream|birds seconds out.wav
 *
 * The generators are pull-style (one sample per call), so they drop
 * straight into the SDL callback or the ALSA loop from noiselive.c.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define RATE 44100
#define TWO_PI 6.28318530717958647692

static double frand(void)                 /* uniform [0,1) */
{
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

static double white(void)                 /* uniform [-1,1) */
{
    return 2.0 * frand() - 1.0;
}

/* One-pole low-pass. 'a' from cutoff: a = 1 - exp(-2*pi*fc/fs) */
typedef struct { double y; } lp1;
static double lp1_run(lp1 *f, double x, double a)
{
    f->y += a * (x - f->y);
    return f->y;
}
static double lp_coef(double fc) { return 1.0 - exp(-TWO_PI * fc / RATE); }

/* ---------------- rain ---------------- */

#define MAX_DROPS 64

/* A drop is a burst of low-passed noise with a SMOOTH attack: the
 * envelope is a difference of two exponentials (slow minus fast), so
 * it rises over ~1-3 ms and then decays. An instant attack is heard
 * as a static-electricity crack; the ramp turns it into a "pat".
 * Cutoffs sit low (300-1500 Hz): ground impacts thump, not tick. */
typedef struct { double ed, dd, ea, da, coef, amp; lp1 f; int alive; } drop;

static drop drops[MAX_DROPS];
static lp1 hiss_hp, bed_lp, bed_lp2, wob_lp;

static void spawn_drop(void)
{
    for (int i = 0; i < MAX_DROPS; i++)
        if (!drops[i].alive) {
            drop *d = &drops[i];
            d->alive = 1;
            d->f.y   = 0.0;
            double dec_ms = 8.0 + frand() * 22.0;         /* body: 8-30 ms */
            double atk_ms = 1.0 + frand() * 2.0;          /* rise: 1-3 ms  */
            d->dd  = exp(-1.0 / (dec_ms * 0.001 * RATE));
            d->da  = exp(-1.0 / (atk_ms * 0.001 * RATE));
            d->ed  = 1.0;
            d->ea  = 1.0;
            d->coef = lp_coef(300.0 + frand() * 1200.0);  /* pat, not tick */
            d->amp  = 0.20 + frand() * frand() * 0.60;
            return;
        }
}

static double rain(void)
{
    if (frand() < 60.0 / RATE) spawn_drop();

    double s = 0.0;
    for (int i = 0; i < MAX_DROPS; i++) {
        drop *d = &drops[i];
        if (!d->alive) continue;
        double env = d->ed - d->ea;                       /* smooth rise+fall */
        s += d->amp * env * lp1_run(&d->f, white(), d->coef);
        d->ed *= d->dd;
        d->ea *= d->da;
        if (d->ed < 1e-4) d->alive = 0;
    }

    /* Hiss bed: band-passed white noise (400 Hz - 4 kHz) carrying the
     * distant-rain wash, its level wobbling slowly like real rainfall. */
    double w = white();
    double hp  = w - lp1_run(&hiss_hp, w, lp_coef(400.0));
    double bed = lp1_run(&bed_lp2,
                     lp1_run(&bed_lp, hp, lp_coef(4000.0)),
                     lp_coef(4000.0));   /* two poles: softer top end */
    double wob = 1.0 + 80.0 * lp1_run(&wob_lp, white(), lp_coef(0.3));
    if (wob < 0.5) wob = 0.5;
    if (wob > 1.5) wob = 1.5;
    return 1.6 * s + 0.15 * bed * wob;
}


#define P_WGUST  0.6
#define P_WRUST  0.6
#define P_WTONE  250.0
#define P_SRATE  50.0
#define P_SPITCH 900.0
#define P_SBLVL  0.9
#define P_SFLOW  0.12
#define P_BRATE  2.5
#define P_BPITCH 2800.0
#define P_BAMB   0.03

/* ---- wind in the forest ---- */
static lp1 wg_lp, wf_lp, wb_lp1, wb_lp2, wr_hp, wr_lp;

static double snd_wind(void)
{
    /* gust: very slow filtered noise scales gain AND brightness */
    double g = 0.5 + 400.0 * P_WGUST * lp1_run(&wg_lp, white(), lp_coef(0.12));
    if (g < 0.05) g = 0.05;
    if (g > 1.0)  g = 1.0;
    double fl = 1.0 + 30.0 * lp1_run(&wf_lp, white(), lp_coef(1.5));   /* flutter */
    if (fl < 0.7) fl = 0.7;
    if (fl > 1.3) fl = 1.3;
    double fc = P_WTONE * (0.6 + 1.8 * g);
    double body = lp1_run(&wb_lp2, lp1_run(&wb_lp1, white(), lp_coef(fc)), lp_coef(fc));
    double w = white();
    double hp = w - lp1_run(&wr_hp, w, lp_coef(2000.0));
    double rust = lp1_run(&wr_lp, hp, lp_coef(6000.0));   /* leaf band 2-6 kHz */
    return (2.6 * body * (0.15 + 0.85 * g) + 0.55 * P_WRUST * g * g * rust) * fl;
}

/* ---- stream: bubbles are damped sines with an UPWARD chirp
 * (downward chirps read as birdsong; rising pitch reads as water) ---- */
#define MAX_BUBBLES 24
typedef struct { double ph, f, c, ed, dd, ea, da, amp; int alive; } bubble;
static bubble bubbles[MAX_BUBBLES];
static lp1 st_hp, st_lp, st_wob;

static void spawn_bubble(void)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (!bubbles[i].alive) {
            bubble *b = &bubbles[i];
            b->alive = 1;
            b->ph = 0.0;
            b->f  = P_SPITCH * (0.6 + 1.2 * frand());
            b->c  = 1.0 + (0.3 + frand()) * 0.00045;
            double dec_ms = 10.0 + frand() * 30.0, atk_ms = 1.0 + frand() * 2.0;
            b->dd = exp(-1.0 / (dec_ms * 0.001 * RATE));
            b->da = exp(-1.0 / (atk_ms * 0.001 * RATE));
            b->ed = 1.0; b->ea = 1.0;
            b->amp = 0.05 + frand() * frand() * 0.25;
            return;
        }
}

static double snd_stream(void)
{
    if (frand() < P_SRATE / RATE) spawn_bubble();
    double s = 0.0;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubble *b = &bubbles[i];
        if (!b->alive) continue;
        s += b->amp * (b->ed - b->ea) * sin(b->ph);
        b->ph += TWO_PI * b->f / RATE;
        b->f *= b->c;
        b->ed *= b->dd; b->ea *= b->da;
        if (b->ed < 1e-4) b->alive = 0;
    }
    double w = white();
    double hp = w - lp1_run(&st_hp, w, lp_coef(700.0));
    double bed = lp1_run(&st_lp, hp, lp_coef(3000.0));
    double wob = 1.0 + 40.0 * lp1_run(&st_wob, white(), lp_coef(2.0));
    if (wob < 0.6) wob = 0.6;
    if (wob > 1.4) wob = 1.4;
    return P_SBLVL * s + P_SFLOW * bed * wob;
}

/* ---- birds: Poisson songs, each a burst of gliding, trilling chirps ---- */
#define MAX_BIRDS 3
typedef struct {
    int active, chirping, chirps_left;
    double gap, ph, f0, f, glide, trill_m, trill_f, trill_ph;
    double ed, dd, ea, da, amp, dur, t;
} birdv;
static birdv birds_v[MAX_BIRDS];
static lp1 ba_hp, ba_lp;

static void start_chirp(birdv *b)
{
    b->chirping = 1;
    b->t = 0.0;
    b->ph = 0.0;
    b->f0 = P_BPITCH * (0.8 + 0.5 * frand());
    b->f  = b->f0;
    b->glide = (frand() < 0.5 ? 1.0 : -1.0) * (0.10 + 0.25 * frand());
    b->trill_m = frand() * 0.10;
    b->trill_f = 20.0 + frand() * 40.0;
    b->trill_ph = 0.0;
    b->dur = (0.04 + frand() * 0.11) * RATE;
    double atk_ms = 3.0 + frand() * 6.0, dec_ms = 30.0 + frand() * 90.0;
    b->dd = exp(-1.0 / (dec_ms * 0.001 * RATE));
    b->da = exp(-1.0 / (atk_ms * 0.001 * RATE));
    b->ed = 1.0; b->ea = 1.0;
    b->amp = 0.10 + frand() * 0.20;
}

static double snd_birds(void)
{
    if (frand() < (P_BRATE / 10.0) / RATE) {
        for (int i = 0; i < MAX_BIRDS; i++)
            if (!birds_v[i].active) {
                birds_v[i].active = 1;
                birds_v[i].chirps_left = 2 + (int)(frand() * 5.0);
                birds_v[i].gap = 0.0;
                birds_v[i].chirping = 0;
                break;
            }
    }
    double s = 0.0;
    for (int i = 0; i < MAX_BIRDS; i++) {
        birdv *b = &birds_v[i];
        if (!b->active) continue;
        if (!b->chirping) {
            b->gap -= 1.0;
            if (b->gap <= 0.0) {
                if (b->chirps_left-- > 0) start_chirp(b);
                else { b->active = 0; continue; }
            }
        }
        if (b->chirping) {
            double frac = b->t / b->dur;
            if (frac > 1.0) frac = 1.0;
            s += b->amp * (b->ed - b->ea) * sin(b->ph);
            b->ph += TWO_PI * b->f * (1.0 + b->trill_m * sin(b->trill_ph)) / RATE;
            b->trill_ph += TWO_PI * b->trill_f / RATE;
            b->f = b->f0 * (1.0 + b->glide * frac);
            b->ed *= b->dd; b->ea *= b->da;
            b->t += 1.0;
            if (b->t >= b->dur && b->ed < 5e-3) {
                b->chirping = 0;
                b->gap = (0.06 + frand() * 0.14) * RATE;
            }
        }
    }
    double w = white();
    double hp = w - lp1_run(&ba_hp, w, lp_coef(400.0));
    double amb = lp1_run(&ba_lp, hp, lp_coef(2500.0));
    return s + P_BAMB * amb;
}

static lp1 surf_lp, rumble_lp1, rumble_lp2;

static double sea(void)
{
    static double t;
    t += 1.0 / RATE;

    /* Slow envelope: two incommensurate swells (9 s and 13.7 s periods)
     * summed, normalized to [0,1], then cubed so crashes are peaky
     * and the troughs between them are long and calm. */
    double e = 0.5 + 0.30 * sin(TWO_PI * t / 9.0)
                   + 0.20 * sin(TWO_PI * t / 13.7 + 1.0);
    if (e < 0.0) e = 0.0;
    if (e > 1.0) e = 1.0;
    double crash = e * e * e;

    /* Surf: white noise through a low-pass whose cutoff AND gain track
     * the envelope — quiet+muffled in troughs, loud+bright at the peak. */
    double fc = 250.0 + 5500.0 * crash;
    double surf = lp1_run(&surf_lp, white(), lp_coef(fc)) * (0.15 + 0.85 * crash);

    /* Constant bed: heavily low-passed noise = distant deep-water rumble */
    double r = lp1_run(&rumble_lp2,
                       lp1_run(&rumble_lp1, white(), lp_coef(120.0)),
                       lp_coef(120.0));
    return 1.0 * surf + 2.6 * r;
}

/* ---------------- WAV output ---------------- */

static void wav_header(FILE *f, uint32_t nsamples)
{
    uint32_t data_bytes = nsamples * 2, byte_rate = RATE * 2;
    uint32_t chunk = 36 + data_bytes;
    uint16_t u16; uint32_t u32;
    fwrite("RIFF", 1, 4, f); u32 = chunk; fwrite(&u32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u32 = 16; fwrite(&u32, 4, 1, f);
    u16 = 1;  fwrite(&u16, 2, 1, f);
    u16 = 1;  fwrite(&u16, 2, 1, f);
    u32 = RATE; fwrite(&u32, 4, 1, f);
    u32 = byte_rate; fwrite(&u32, 4, 1, f);
    u16 = 2;  fwrite(&u16, 2, 1, f);
    u16 = 16; fwrite(&u16, 2, 1, f);
    fwrite("data", 1, 4, f); u32 = data_bytes; fwrite(&u32, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s rain|sea|wind|stream|birds seconds out.wav\n", argv[0]);
        return 1;
    }
    double (*gen)(void) =
        strcmp(argv[1], "rain") == 0 ? rain :
        strcmp(argv[1], "sea")  == 0 ? sea  :
        strcmp(argv[1], "wind")  == 0 ? snd_wind   :
        strcmp(argv[1], "stream")== 0 ? snd_stream :
        strcmp(argv[1], "birds") == 0 ? snd_birds  : NULL;
    if (!gen) { fprintf(stderr, "unknown scene '%s'\n", argv[1]); return 1; }

    double seconds = atof(argv[2]);
    if (seconds <= 0) { fprintf(stderr, "bad duration\n"); return 1; }
    uint32_t n = (uint32_t)(seconds * RATE);

    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 1; }

    srand((unsigned)time(NULL));
    wav_header(f, n);

    const double amp = 0.6;
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
