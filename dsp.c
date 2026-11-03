/* dsp.c — ZzzTop DSP core: the one implementation of all ten sounds.
 *
 * Shared by every target: the native CLI programs and GUI link it,
 * web/build.sh compiles this same file to WebAssembly. No libc beyond
 * libm; own PRNG; runtime sample rate.
 *
 * Stereo and mixing: all generator state lives in per-voice structs.
 * Four voices run at once — primary sound L/R and overlay sound L/R —
 * each with its own PRNG stream, so the two ears are decorrelated
 * (a diffuse, wide image) while deterministic envelopes (sea swell)
 * stay in sync across channels.
 *
 * Param table (flat indices):
 *   0 white low-pass Hz | 1 pink low-pass Hz | 2 brown leak
 *   3 deep (airplane) 2nd-stage leak
 *   4..7   rain:     density /s, drop tone Hz, drop level, hiss level
 *   8..11  sea:      period s, crash sharpness, surf Hz, rumble level
 *   12..14 wind:     gustiness, leaf rustle, body tone Hz
 *   15..18 stream:   bubble rate /s, bubble pitch Hz, bubble lvl, flow lvl
 *   19..21 birds:    songs per 10 s, pitch Hz, ambience
 *   22..24 crickets: chirps per s, pitch Hz, ambience
 */
#include <math.h>
#include <stdint.h>
#include "dsp.h"

#define TWO_PI 6.28318530717958647692
#define BUF_N  4096
#define NSOUNDS 10
#define NPARAMS 25

static double g_rate = 44100.0;

static double params[NPARAMS] = {
    20000.0, 20000.0, 0.995, 0.997,
    60.0, 600.0, 1.6, 0.15,
    9.0, 3.0, 5500.0, 2.6,
    0.6, 0.6, 250.0,
    50.0, 900.0, 0.9, 0.12,
    2.5, 2800.0, 0.03,
    1.2, 4300.0, 0.02,
};

#define P_WGUST  params[12]
#define P_WRUST  params[13]
#define P_WTONE  params[14]
#define P_SRATE  params[15]
#define P_SPITCH params[16]
#define P_SBLVL  params[17]
#define P_SFLOW  params[18]
#define P_BRATE  params[19]
#define P_BPITCH params[20]
#define P_BAMB   params[21]
#define P_CKRATE params[22]
#define P_CKPITCH params[23]
#define P_CKAMB  params[24]

/* ---- primitives ---- */

typedef struct { double y; } lp1;
static double lp1_run(lp1 *f, double x, double a) { f->y += a * (x - f->y); return f->y; }
static double lp_coef(double fc) { return 1.0 - exp(-TWO_PI * fc / g_rate); }

/* ---- per-voice state ---- */

#define MAX_DROPS   64
#define MAX_BUBBLES 24
#define MAX_BIRDS   3
#define MAX_CRICKETS 4

typedef struct { double ed, dd, ea, da, coef, amp; lp1 f; int alive; } drop;
typedef struct { double ph, f, c, ed, dd, ea, da, amp; int alive; } bubble;
typedef struct {
    int active, chirping, chirps_left;
    double gap, ph, f0, f, glide, trill_m, trill_f, trill_ph;
    double ed, dd, ea, da, amp, dur, t;
} birdv;
typedef struct {
    int active, on, syl_left;
    double gap, t, dur, ph, f, ed, dd, ea, da, amp;
} cricket;

typedef struct {
    uint64_t rng;
    /* white / pink */
    lp1 tone_w, tone_p;
    double pb[7];
    /* brown / deep */
    double bacc, a1, a2;
    /* rain */
    drop drops[MAX_DROPS];
    lp1 hiss_hp, bed_lp, bed_lp2, wob_lp;
    /* sea */
    double t;
    lp1 surf_lp, rum1, rum2;
    /* wind */
    lp1 wg_lp, wf_lp, wb_lp1, wb_lp2, wr_hp, wr_lp;
    /* stream */
    bubble bubbles[MAX_BUBBLES];
    lp1 st_hp, st_lp, st_wob;
    /* birds */
    birdv birds[MAX_BIRDS];
    lp1 ba_hp, ba_lp;
    /* crickets */
    cricket crickets[MAX_CRICKETS];
    lp1 ck_hp, ck_lp;
} voice;

/* voices: [0] primary L, [1] primary R, [2] overlay L, [3] overlay R */
static voice V[4];

/* ---- PRNG: xorshift64*, one independent stream per voice ---- */

static double frand(voice *v)
{
    v->rng ^= v->rng >> 12; v->rng ^= v->rng << 25; v->rng ^= v->rng >> 27;
    return (double)((v->rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}
static double wht(voice *v) { return 2.0 * frand(v) - 1.0; }

/* ---- generators ---- */

static double s_white(voice *v)
{
    return lp1_run(&v->tone_w, wht(v), lp_coef(params[0]));
}

static double s_pink(voice *v)
{
    double *b = v->pb;
    double w = wht(v);
    b[0] = 0.99886 * b[0] + w * 0.0555179;
    b[1] = 0.99332 * b[1] + w * 0.0750759;
    b[2] = 0.96900 * b[2] + w * 0.1538520;
    b[3] = 0.86650 * b[3] + w * 0.3104856;
    b[4] = 0.55000 * b[4] + w * 0.5329522;
    b[5] = -0.7616 * b[5] - w * 0.0168980;
    double out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
    b[6] = w * 0.115926;
    return lp1_run(&v->tone_p, out * 0.11, lp_coef(params[1]));
}

static double s_brown(voice *v)
{
    double L = params[2];
    v->bacc = L * v->bacc + 0.02 * wht(v);
    double comp = sqrt((1.0 - 0.997 * 0.997) / (1.0 - L * L));
    return v->bacc * 3.5 * comp;
}

static double s_deep(voice *v)
{
    double p1 = 0.997, p2 = params[3];
    v->a1 = p1 * v->a1 + wht(v);
    v->a2 = p2 * v->a2 + v->a1;
    double var = (1.0 + p1 * p2) /
                 ((1.0 - p1 * p2) * (1.0 - p1 * p1) * (1.0 - p2 * p2)) / 3.0;
    return v->a2 / sqrt(var) * 0.5;
}

static void spawn_drop(voice *v)
{
    for (int i = 0; i < MAX_DROPS; i++)
        if (!v->drops[i].alive) {
            drop *d = &v->drops[i];
            d->alive = 1;
            d->f.y   = 0.0;
            double dec_ms = 8.0 + frand(v) * 22.0;
            double atk_ms = 1.0 + frand(v) * 2.0;
            d->dd  = exp(-1.0 / (dec_ms * 0.001 * g_rate));
            d->da  = exp(-1.0 / (atk_ms * 0.001 * g_rate));
            d->ed  = 1.0;
            d->ea  = 1.0;
            double T = params[5];
            d->coef = lp_coef(T * 0.5 + frand(v) * T * 2.0);
            d->amp  = 0.20 + frand(v) * frand(v) * 0.60;
            return;
        }
}

static double s_rain(voice *v)
{
    if (frand(v) < params[4] / g_rate) spawn_drop(v);

    double s = 0.0;
    for (int i = 0; i < MAX_DROPS; i++) {
        drop *d = &v->drops[i];
        if (!d->alive) continue;
        s += d->amp * (d->ed - d->ea) * lp1_run(&d->f, wht(v), d->coef);
        d->ed *= d->dd;
        d->ea *= d->da;
        if (d->ed < 1e-4) d->alive = 0;
    }

    double w = wht(v);
    double hp  = w - lp1_run(&v->hiss_hp, w, lp_coef(400.0));
    double bed = lp1_run(&v->bed_lp2,
                         lp1_run(&v->bed_lp, hp, lp_coef(4000.0)),
                         lp_coef(4000.0));
    double wob = 1.0 + 80.0 * lp1_run(&v->wob_lp, wht(v), lp_coef(0.3));
    if (wob < 0.5) wob = 0.5;
    if (wob > 1.5) wob = 1.5;
    return params[6] * s + params[7] * bed * wob;
}

static double s_sea(voice *v)
{
    v->t += 1.0 / g_rate;

    double T = params[8];
    double e = 0.5 + 0.30 * sin(TWO_PI * v->t / T)
                   + 0.20 * sin(TWO_PI * v->t / (1.522 * T) + 1.0);
    if (e < 0.0) e = 0.0;
    if (e > 1.0) e = 1.0;
    double crash = pow(e, params[9]);

    double fc = 250.0 + params[10] * crash;
    double surf = lp1_run(&v->surf_lp, wht(v), lp_coef(fc)) * (0.15 + 0.85 * crash);

    double r = lp1_run(&v->rum2,
                       lp1_run(&v->rum1, wht(v), lp_coef(120.0)),
                       lp_coef(120.0));
    return surf + params[11] * r;
}

static double s_wind(voice *v)
{
    double g = 0.5 + 400.0 * P_WGUST * lp1_run(&v->wg_lp, wht(v), lp_coef(0.12));
    if (g < 0.05) g = 0.05;
    if (g > 1.0)  g = 1.0;
    double fl = 1.0 + 30.0 * lp1_run(&v->wf_lp, wht(v), lp_coef(1.5));
    if (fl < 0.7) fl = 0.7;
    if (fl > 1.3) fl = 1.3;
    double fc = P_WTONE * (0.6 + 1.8 * g);
    double body = lp1_run(&v->wb_lp2, lp1_run(&v->wb_lp1, wht(v), lp_coef(fc)), lp_coef(fc));
    double w = wht(v);
    double hp = w - lp1_run(&v->wr_hp, w, lp_coef(2000.0));
    double rust = lp1_run(&v->wr_lp, hp, lp_coef(6000.0));
    return (2.6 * body * (0.15 + 0.85 * g) + 0.55 * P_WRUST * g * g * rust) * fl;
}

static void spawn_bubble(voice *v)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (!v->bubbles[i].alive) {
            bubble *b = &v->bubbles[i];
            b->alive = 1;
            b->ph = 0.0;
            b->f  = P_SPITCH * (0.6 + 1.2 * frand(v));
            b->c  = 1.0 + (0.3 + frand(v)) * 0.00045;
            double dec_ms = 10.0 + frand(v) * 30.0, atk_ms = 1.0 + frand(v) * 2.0;
            b->dd = exp(-1.0 / (dec_ms * 0.001 * g_rate));
            b->da = exp(-1.0 / (atk_ms * 0.001 * g_rate));
            b->ed = 1.0; b->ea = 1.0;
            b->amp = 0.05 + frand(v) * frand(v) * 0.25;
            return;
        }
}

static double s_stream(voice *v)
{
    if (frand(v) < P_SRATE / g_rate) spawn_bubble(v);
    double s = 0.0;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubble *b = &v->bubbles[i];
        if (!b->alive) continue;
        s += b->amp * (b->ed - b->ea) * sin(b->ph);
        b->ph += TWO_PI * b->f / g_rate;
        b->f *= b->c;
        b->ed *= b->dd; b->ea *= b->da;
        if (b->ed < 1e-4) b->alive = 0;
    }
    double w = wht(v);
    double hp = w - lp1_run(&v->st_hp, w, lp_coef(700.0));
    double bed = lp1_run(&v->st_lp, hp, lp_coef(3000.0));
    double wob = 1.0 + 40.0 * lp1_run(&v->st_wob, wht(v), lp_coef(2.0));
    if (wob < 0.6) wob = 0.6;
    if (wob > 1.4) wob = 1.4;
    return P_SBLVL * s + P_SFLOW * bed * wob;
}

static void start_chirp(voice *v, birdv *b)
{
    b->chirping = 1;
    b->t = 0.0;
    b->ph = 0.0;
    b->f0 = P_BPITCH * (0.8 + 0.5 * frand(v));
    b->f  = b->f0;
    b->glide = (frand(v) < 0.5 ? 1.0 : -1.0) * (0.10 + 0.25 * frand(v));
    b->trill_m = frand(v) * 0.10;
    b->trill_f = 20.0 + frand(v) * 40.0;
    b->trill_ph = 0.0;
    b->dur = (0.04 + frand(v) * 0.11) * g_rate;
    double atk_ms = 3.0 + frand(v) * 6.0, dec_ms = 30.0 + frand(v) * 90.0;
    b->dd = exp(-1.0 / (dec_ms * 0.001 * g_rate));
    b->da = exp(-1.0 / (atk_ms * 0.001 * g_rate));
    b->ed = 1.0; b->ea = 1.0;
    b->amp = 0.10 + frand(v) * 0.20;
}

static double s_birds(voice *v)
{
    if (frand(v) < (P_BRATE / 10.0) / g_rate) {
        for (int i = 0; i < MAX_BIRDS; i++)
            if (!v->birds[i].active) {
                v->birds[i].active = 1;
                v->birds[i].chirps_left = 2 + (int)(frand(v) * 5.0);
                v->birds[i].gap = 0.0;
                v->birds[i].chirping = 0;
                break;
            }
    }
    double s = 0.0;
    for (int i = 0; i < MAX_BIRDS; i++) {
        birdv *b = &v->birds[i];
        if (!b->active) continue;
        if (!b->chirping) {
            b->gap -= 1.0;
            if (b->gap <= 0.0) {
                if (b->chirps_left-- > 0) start_chirp(v, b);
                else { b->active = 0; continue; }
            }
        }
        if (b->chirping) {
            double frac = b->t / b->dur;
            if (frac > 1.0) frac = 1.0;
            s += b->amp * (b->ed - b->ea) * sin(b->ph);
            b->ph += TWO_PI * b->f * (1.0 + b->trill_m * sin(b->trill_ph)) / g_rate;
            b->trill_ph += TWO_PI * b->trill_f / g_rate;
            b->f = b->f0 * (1.0 + b->glide * frac);
            b->ed *= b->dd; b->ea *= b->da;
            b->t += 1.0;
            if (b->t >= b->dur && b->ed < 5e-3) {
                b->chirping = 0;
                b->gap = (0.06 + frand(v) * 0.14) * g_rate;
            }
        }
    }
    double w = wht(v);
    double hp = w - lp1_run(&v->ba_hp, w, lp_coef(400.0));
    double amb = lp1_run(&v->ba_lp, hp, lp_coef(2500.0));
    return s + P_BAMB * amb;
}

/* ---- crickets: a night field. Each voice = one chirp group: 2-4
 * syllables of ~25 ms narrowband tone around P_CKPITCH, separated by
 * ~30 ms gaps — the syllable train is the familiar "chirp... chirp".
 * Group onsets are Poisson at P_CKRATE per second. Over it, a faint
 * band-limited night-air hiss. ---- */

static void start_syllable(voice *v, cricket *c)
{
    c->on = 1;
    c->t = 0.0;
    c->ph = 0.0;
    c->dur = (0.020 + frand(v) * 0.012) * g_rate;
    c->f = P_CKPITCH * (0.95 + 0.10 * frand(v));
    double atk_ms = 2.0 + frand(v) * 2.0, dec_ms = 12.0 + frand(v) * 8.0;
    c->dd = exp(-1.0 / (dec_ms * 0.001 * g_rate));
    c->da = exp(-1.0 / (atk_ms * 0.001 * g_rate));
    c->ed = 1.0; c->ea = 1.0;
    c->amp = 0.10 + frand(v) * 0.10;
}

static double s_crickets(voice *v)
{
    if (frand(v) < P_CKRATE / g_rate) {
        for (int i = 0; i < MAX_CRICKETS; i++)
            if (!v->crickets[i].active) {
                v->crickets[i].active = 1;
                v->crickets[i].syl_left = 2 + (int)(frand(v) * 3.0);
                v->crickets[i].gap = 0.0;
                v->crickets[i].on = 0;
                break;
            }
    }
    double s = 0.0;
    for (int i = 0; i < MAX_CRICKETS; i++) {
        cricket *c = &v->crickets[i];
        if (!c->active) continue;
        if (!c->on) {
            c->gap -= 1.0;
            if (c->gap <= 0.0) {
                if (c->syl_left-- > 0) start_syllable(v, c);
                else { c->active = 0; continue; }
            }
        }
        if (c->on) {
            s += c->amp * (c->ed - c->ea) * sin(c->ph);
            c->ph += TWO_PI * c->f / g_rate;
            c->ed *= c->dd; c->ea *= c->da;
            c->t += 1.0;
            if (c->t >= c->dur && c->ed < 5e-3) {
                c->on = 0;
                c->gap = (0.025 + frand(v) * 0.015) * g_rate;
            }
        }
    }
    /* night air: faint 1-6 kHz band */
    double w = wht(v);
    double hp = w - lp1_run(&v->ck_hp, w, lp_coef(1000.0));
    double amb = lp1_run(&v->ck_lp, hp, lp_coef(6000.0));
    return s + P_CKAMB * amb;
}

/* ---- public interface ---- */

static double (*gens[NSOUNDS])(voice *) = {
    s_white, s_pink, s_brown, s_deep, s_rain,
    s_sea, s_wind, s_stream, s_birds, s_crickets,
};

static int cur = 2;        /* primary sound   */
static int ovl = -1;       /* overlay, -1 off */
static double ovl_gain = 0.5;
static float bufL[BUF_N], bufR[BUF_N];

void dsp_init(double rate)
{
    g_rate = (rate > 8000.0 && rate < 384000.0) ? rate : 44100.0;
    static const uint64_t seeds[4] = {
        0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull,
        0x8CB92BA72F3D8DD7ull, 0xEB44ACCAB455D165ull,
    };
    for (int i = 0; i < 4; i++) {
        voice z = {0};
        V[i] = z;
        V[i].rng = seeds[i];
    }
}

void dsp_set_sound(int i)   { if (i >= 0 && i < NSOUNDS) cur = i; }
void dsp_set_overlay(int i) { ovl = (i >= 0 && i < NSOUNDS) ? i : -1; }

void dsp_set_overlay_gain(double g)
{
    if (g < 0.0) g = 0.0;
    if (g > 1.0) g = 1.0;
    ovl_gain = g;
}

void dsp_set_param(int i, double v)
{
    if (i >= 0 && i < NPARAMS) params[i] = v;
}

double dsp_get_param(int i)
{
    return (i >= 0 && i < NPARAMS) ? params[i] : 0.0;
}

int dsp_num_params(void) { return NPARAMS; }
int dsp_num_sounds(void) { return NSOUNDS; }

float *dsp_get_buf_l(void) { return bufL; }
float *dsp_get_buf_r(void) { return bufR; }

void dsp_render(int n)
{
    if (n > BUF_N) n = BUF_N;
    for (int i = 0; i < n; i++) {
        double l = gens[cur](&V[0]);
        double r = gens[cur](&V[1]);
        if (ovl >= 0) {
            l += ovl_gain * gens[ovl](&V[2]);
            r += ovl_gain * gens[ovl](&V[3]);
        }
        /* 0.35 headroom: raw generator peaks (esp. brown/deep) exceed
         * +-1, and this clamp sits BEFORE the volume gain downstream —
         * without headroom they hard-clip audibly. */
        l *= 0.35;
        r *= 0.35;
        if (l > 1.0) l = 1.0;
        if (l < -1.0) l = -1.0;
        if (r > 1.0) r = 1.0;
        if (r < -1.0) r = -1.0;
        bufL[i] = (float)l;
        bufR[i] = (float)r;
    }
}
