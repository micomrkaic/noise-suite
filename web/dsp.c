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
static double params[12] = {
    20000.0, 20000.0, 0.997, 0.997,
    60.0, 600.0, 1.6, 0.15,
    9.0, 3.0, 5500.0, 2.6,
};

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

static double (*gens[6])(void) = { s_white, s_pink, s_brown, s_deep, s_rain, s_sea };
static int cur = 2;
static float outbuf[BUF_N];

void dsp_init(double rate)
{
    g_rate = (rate > 8000.0 && rate < 384000.0) ? rate : 44100.0;
}

void dsp_set_sound(int i)
{
    if (i >= 0 && i < 6) cur = i;
}

void dsp_set_param(int i, double v)
{
    if (i >= 0 && i < 12) params[i] = v;
}

double dsp_get_param(int i)
{
    return (i >= 0 && i < 12) ? params[i] : 0.0;
}

int dsp_num_params(void) { return 12; }

float *dsp_get_buf(void) { return outbuf; }

void dsp_render(int n)
{
    if (n > BUF_N) n = BUF_N;
    for (int i = 0; i < n; i++) {
        double s = gens[cur]();
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        outbuf[i] = (float)s;
    }
}
