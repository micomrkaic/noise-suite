/* dsp.c — sleep-sound DSP core for WebAssembly (and anywhere else).
 * No libc dependencies beyond libm; own PRNG; runtime sample rate.
 *
 * Build (wasi-sdk):
 *   clang --target=wasm32-wasip1 -O2 -mexec-model=reactor \
 *     -Wl,--export=dsp_init -Wl,--export=dsp_set_sound \
 *     -Wl,--export=dsp_set_param -Wl,--export=dsp_get_param \
 *     -Wl,--export=dsp_num_params -Wl,--export=dsp_render \
 *     -Wl,--export=dsp_get_buf -Wl,--no-entry -o dsp.wasm dsp.c
 *
 * Exports:
 *   dsp_init(rate)          set sample rate, reset state
 *   dsp_set_sound(i)        0 white 1 pink 2 brown 3 deep 4 rain 5 sea
 *   dsp_set_param(i, v)     flat param index (see table below)
 *   dsp_get_param(i)        current value (use to read defaults)
 *   dsp_num_params()        size of the param table
 *   dsp_get_buf()           pointer to the float output buffer
 *   dsp_render(n)           fill buffer with n samples (n <= 4096)
 *
 * Param table (flat indices):
 *   0 white low-pass Hz | 1 pink low-pass Hz | 2 brown leak
 *   3 deep 2nd-stage leak
 *   4 rain density /s | 5 rain drop tone Hz | 6 rain drop level | 7 rain hiss level
 *   8 sea period s | 9 sea crash sharpness | 10 sea surf Hz | 11 sea rumble
 */
#include <math.h>
#include <stdint.h>

#define TWO_PI 6.28318530717958647692
#define BUF_N 4096

static double g_rate = 44100.0;

/* ---- params ---- */
static double params[22] = {
    20000.0, 20000.0, 0.995, 0.997,
    60.0, 600.0, 1.6, 0.15,
    9.0, 3.0, 5500.0, 2.6,
    0.6, 0.6, 250.0,            /* wind: gustiness, rustle, tone */
    50.0, 900.0, 0.9, 0.12,     /* stream: rate, pitch, bubble lvl, flow */
    2.5, 2800.0, 0.03,          /* birds: songs per 10 s, pitch, ambience */
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

/* ---- PRNG: xorshift64* — no libc rand ---- */
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static double frand(void)
{
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (double)((rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}
static double white_raw(void) { return 2.0 * frand() - 1.0; }

typedef struct { double y; } lp1;
static double lp1_run(lp1 *f, double x, double a) { f->y += a * (x - f->y); return f->y; }
static double lp_coef(double fc) { return 1.0 - exp(-TWO_PI * fc / g_rate); }

/* ---- generators (same synthesis as the desktop GUI) ---- */

static lp1 tone_w, tone_p;

static double s_white(void)
{
    return lp1_run(&tone_w, white_raw(), lp_coef(params[0]));
}

static double s_pink(void)
{
    static double b0, b1, b2, b3, b4, b5, b6;
    double w = white_raw();
    b0 = 0.99886 * b0 + w * 0.0555179;
    b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.96900 * b2 + w * 0.1538520;
    b3 = 0.86650 * b3 + w * 0.3104856;
    b4 = 0.55000 * b4 + w * 0.5329522;
    b5 = -0.7616 * b5 - w * 0.0168980;
    double out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
    b6 = w * 0.115926;
    return lp1_run(&tone_p, out * 0.11, lp_coef(params[1]));
}

static double s_brown(void)
{
    static double acc;
    double L = params[2];
    acc = L * acc + 0.02 * white_raw();
    double comp = sqrt((1.0 - 0.997 * 0.997) / (1.0 - L * L));
    return acc * 3.5 * comp;
}

static double s_deep(void)
{
    static double a1, a2;
    double p1 = 0.997, p2 = params[3];
    a1 = p1 * a1 + white_raw();
    a2 = p2 * a2 + a1;
    double var = (1.0 + p1 * p2) /
                 ((1.0 - p1 * p2) * (1.0 - p1 * p1) * (1.0 - p2 * p2)) / 3.0;
    return a2 / sqrt(var) * 0.5;
}

#define MAX_DROPS 64
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
            double dec_ms = 8.0 + frand() * 22.0;
            double atk_ms = 1.0 + frand() * 2.0;
            d->dd  = exp(-1.0 / (dec_ms * 0.001 * g_rate));
            d->da  = exp(-1.0 / (atk_ms * 0.001 * g_rate));
            d->ed  = 1.0;
            d->ea  = 1.0;
            double T = params[5];
            d->coef = lp_coef(T * 0.5 + frand() * T * 2.0);
            d->amp  = 0.20 + frand() * frand() * 0.60;
            return;
        }
}

static double s_rain(void)
{
    if (frand() < params[4] / g_rate) spawn_drop();

    double s = 0.0;
    for (int i = 0; i < MAX_DROPS; i++) {
        drop *d = &drops[i];
        if (!d->alive) continue;
        double env = d->ed - d->ea;
        s += d->amp * env * lp1_run(&d->f, white_raw(), d->coef);
        d->ed *= d->dd;
        d->ea *= d->da;
        if (d->ed < 1e-4) d->alive = 0;
    }

    double w = white_raw();
    double hp  = w - lp1_run(&hiss_hp, w, lp_coef(400.0));
    double bed = lp1_run(&bed_lp2,
                         lp1_run(&bed_lp, hp, lp_coef(4000.0)),
                         lp_coef(4000.0));
    double wob = 1.0 + 80.0 * lp1_run(&wob_lp, white_raw(), lp_coef(0.3));
    if (wob < 0.5) wob = 0.5;
    if (wob > 1.5) wob = 1.5;
    return params[6] * s + params[7] * bed * wob;
}


/* ---- wind in the forest ---- */
static lp1 wg_lp, wf_lp, wb_lp1, wb_lp2, wr_hp, wr_lp;

static double snd_wind(void)
{
    /* gust: very slow filtered noise scales gain AND brightness */
    double g = 0.5 + 400.0 * P_WGUST * lp1_run(&wg_lp, white_raw(), lp_coef(0.12));
    if (g < 0.05) g = 0.05;
    if (g > 1.0)  g = 1.0;
    double fl = 1.0 + 30.0 * lp1_run(&wf_lp, white_raw(), lp_coef(1.5));   /* flutter */
    if (fl < 0.7) fl = 0.7;
    if (fl > 1.3) fl = 1.3;
    double fc = P_WTONE * (0.6 + 1.8 * g);
    double body = lp1_run(&wb_lp2, lp1_run(&wb_lp1, white_raw(), lp_coef(fc)), lp_coef(fc));
    double w = white_raw();
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
            b->dd = exp(-1.0 / (dec_ms * 0.001 * g_rate));
            b->da = exp(-1.0 / (atk_ms * 0.001 * g_rate));
            b->ed = 1.0; b->ea = 1.0;
            b->amp = 0.05 + frand() * frand() * 0.25;
            return;
        }
}

static double snd_stream(void)
{
    if (frand() < P_SRATE / g_rate) spawn_bubble();
    double s = 0.0;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubble *b = &bubbles[i];
        if (!b->alive) continue;
        s += b->amp * (b->ed - b->ea) * sin(b->ph);
        b->ph += TWO_PI * b->f / g_rate;
        b->f *= b->c;
        b->ed *= b->dd; b->ea *= b->da;
        if (b->ed < 1e-4) b->alive = 0;
    }
    double w = white_raw();
    double hp = w - lp1_run(&st_hp, w, lp_coef(700.0));
    double bed = lp1_run(&st_lp, hp, lp_coef(3000.0));
    double wob = 1.0 + 40.0 * lp1_run(&st_wob, white_raw(), lp_coef(2.0));
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
    b->dur = (0.04 + frand() * 0.11) * g_rate;
    double atk_ms = 3.0 + frand() * 6.0, dec_ms = 30.0 + frand() * 90.0;
    b->dd = exp(-1.0 / (dec_ms * 0.001 * g_rate));
    b->da = exp(-1.0 / (atk_ms * 0.001 * g_rate));
    b->ed = 1.0; b->ea = 1.0;
    b->amp = 0.10 + frand() * 0.20;
}

static double snd_birds(void)
{
    if (frand() < (P_BRATE / 10.0) / g_rate) {
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
            b->ph += TWO_PI * b->f * (1.0 + b->trill_m * sin(b->trill_ph)) / g_rate;
            b->trill_ph += TWO_PI * b->trill_f / g_rate;
            b->f = b->f0 * (1.0 + b->glide * frac);
            b->ed *= b->dd; b->ea *= b->da;
            b->t += 1.0;
            if (b->t >= b->dur && b->ed < 5e-3) {
                b->chirping = 0;
                b->gap = (0.06 + frand() * 0.14) * g_rate;
            }
        }
    }
    double w = white_raw();
    double hp = w - lp1_run(&ba_hp, w, lp_coef(400.0));
    double amb = lp1_run(&ba_lp, hp, lp_coef(2500.0));
    return s + P_BAMB * amb;
}

static lp1 surf_lp, rumble_lp1, rumble_lp2;

static double s_sea(void)
{
    static double t;
    t += 1.0 / g_rate;

    double T = params[8];
    double e = 0.5 + 0.30 * sin(TWO_PI * t / T)
                   + 0.20 * sin(TWO_PI * t / (1.522 * T) + 1.0);
    if (e < 0.0) e = 0.0;
    if (e > 1.0) e = 1.0;
    double crash = pow(e, params[9]);

    double fc = 250.0 + params[10] * crash;
    double surf = lp1_run(&surf_lp, white_raw(), lp_coef(fc)) * (0.15 + 0.85 * crash);

    double r = lp1_run(&rumble_lp2,
                       lp1_run(&rumble_lp1, white_raw(), lp_coef(120.0)),
                       lp_coef(120.0));
    return surf + params[11] * r;
}

/* ---- public interface ---- */

static double (*gens[9])(void) = { s_white, s_pink, s_brown, s_deep, s_rain, s_sea, snd_wind, snd_stream, snd_birds };
static int cur = 2;
static float outbuf[BUF_N];

void dsp_init(double rate)
{
    g_rate = (rate > 8000.0 && rate < 384000.0) ? rate : 44100.0;
}

void dsp_set_sound(int i)
{
    if (i >= 0 && i < 9) cur = i;
}

void dsp_set_param(int i, double v)
{
    if (i >= 0 && i < 22) params[i] = v;
}

double dsp_get_param(int i)
{
    return (i >= 0 && i < 22) ? params[i] : 0.0;
}

int dsp_num_params(void) { return 22; }

float *dsp_get_buf(void) { return outbuf; }

void dsp_render(int n)
{
    if (n > BUF_N) n = BUF_N;
    for (int i = 0; i < n; i++) {
        /* 0.35 headroom: raw generator peaks (esp. brown/deep) exceed
         * +-1, and this clamp sits BEFORE the volume gain in the web
         * audio graph — without headroom they hard-clip audibly. */
        double s = gens[cur]() * 0.35;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        outbuf[i] = (float)s;
    }
}
