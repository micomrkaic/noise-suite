/* noisegui.c — sleep-sound machine with per-sound tuning controls.
 * Left: pick a sound. Right: live sliders for that sound's synthesis
 * parameters. Bottom: master volume, play/pause, VU meter.
 * Keys: 1-5 select sound, space play/pause, up/down volume, r resets
 * the selected sound's parameters, Esc quits.
 *
 * Build:  cc -O2 -o noisegui noisegui.c $(sdl2-config --cflags --libs) -lSDL2_ttf -lm
 *         (needs: sudo apt install libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core)
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 44100
#define TWO_PI 6.28318530717958647692

/* ================= parameters =================
 * All slider-tunable values live in these tables. The UI thread writes
 * .val only inside SDL_LockAudioDevice(); the audio thread reads freely. */

typedef struct {
    const char *name;
    double lo, hi, val, def;
    const char *fmt;              /* printf format for the value label */
} param;

static param p_white[] = {
    { "Low-pass tone (Hz)", 500, 20000, 20000, 20000, "%.0f" },
};
static param p_pink[] = {
    { "Low-pass tone (Hz)", 500, 20000, 20000, 20000, "%.0f" },
};
static param p_brown[] = {
    { "Leak (deep <-> tight)", 0.9900, 0.9995, 0.997, 0.997, "%.4f" },
};
static param p_rain[] = {
    { "Drop density (/s)",   5,   150,   60,   60,  "%.0f" },
    { "Drop tone (Hz)",      150, 3000,  600,  600,  "%.0f" },
    { "Drop level",          0.0, 3.0,   1.6,  1.6,  "%.2f" },
    { "Hiss level",          0.0, 0.5,   0.15, 0.15, "%.2f" },
};
static param p_sea[] = {
    { "Wave period (s)",     4,    20,   9.0,  9.0,  "%.1f" },
    { "Crash sharpness",     1.0,  5.0,  3.0,  3.0,  "%.1f" },
    { "Surf brightness (Hz)",1000, 8000, 5500, 5500, "%.0f" },
    { "Rumble level",        0.0,  5.0,  2.6,  2.6,  "%.2f" },
};
static param p_vol = { "Volume", 0.0, 1.0, 0.30, 0.30, "%.0f%%" };

static param *param_sets[5] = { p_white, p_pink, p_brown, p_rain, p_sea };
static const int param_counts[5] = { 1, 1, 1, 4, 4 };

/* ================= synthesis ================= */

static double frand(void) { return (double)rand() / ((double)RAND_MAX + 1.0); }
static double white_raw(void) { return 2.0 * frand() - 1.0; }

typedef struct { double y; } lp1;
static double lp1_run(lp1 *f, double x, double a) { f->y += a * (x - f->y); return f->y; }
static double lp_coef(double fc) { return 1.0 - exp(-TWO_PI * fc / RATE); }

static lp1 tone_w, tone_p;

static double white(void)
{
    return lp1_run(&tone_w, white_raw(), lp_coef(p_white[0].val));
}

static double pink(void)
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
    return lp1_run(&tone_p, out * 0.11, lp_coef(p_pink[0].val));
}

static double brown(void)
{
    static double acc;
    double L = p_brown[0].val;
    acc = L * acc + 0.02 * white_raw();
    /* loudness compensation: keep RMS constant as the leak changes */
    double comp = sqrt((1.0 - 0.997 * 0.997) / (1.0 - L * L));
    return acc * 3.5 * comp;
}

#define MAX_DROPS 64

/* Drop = burst of low-passed noise with a smooth 1-3 ms attack
 * (difference of two exponentials). Instant attacks sound like static
 * crackle; tonal (sine) drops with pitch glides sound like birds. */
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
            d->dd  = exp(-1.0 / (dec_ms * 0.001 * RATE));
            d->da  = exp(-1.0 / (atk_ms * 0.001 * RATE));
            d->ed  = 1.0;
            d->ea  = 1.0;
            /* tone T: cutoff drawn in [T/2, 2.5T] — default T=600 gives
             * the 300-1500 Hz "pat" range; slide up for tin-roof ticks */
            double T = p_rain[1].val;
            d->coef = lp_coef(T * 0.5 + frand() * T * 2.0);
            d->amp  = 0.20 + frand() * frand() * 0.60;
            return;
        }
}

static double rain(void)
{
    if (frand() < p_rain[0].val / RATE) spawn_drop();

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
    return p_rain[2].val * s + p_rain[3].val * bed * wob;
}

static lp1 surf_lp, rumble_lp1, rumble_lp2;

static double sea(void)
{
    static double t;
    t += 1.0 / RATE;

    double T = p_sea[0].val;
    double e = 0.5 + 0.30 * sin(TWO_PI * t / T)
                   + 0.20 * sin(TWO_PI * t / (1.522 * T) + 1.0);
    if (e < 0.0) e = 0.0;
    if (e > 1.0) e = 1.0;
    double crash = pow(e, p_sea[1].val);

    double fc = 250.0 + p_sea[2].val * crash;
    double surf = lp1_run(&surf_lp, white_raw(), lp_coef(fc)) * (0.15 + 0.85 * crash);

    double r = lp1_run(&rumble_lp2,
                       lp1_run(&rumble_lp1, white_raw(), lp_coef(120.0)),
                       lp_coef(120.0));
    return surf + p_sea[3].val * r;
}

/* ================= audio plumbing ================= */

static double (*gen)(void) = brown;
static SDL_atomic_t rms_milli;

static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    Sint16 *out = (Sint16 *)stream;
    int n = len / (int)sizeof(Sint16);
    double sumsq = 0.0;
    for (int i = 0; i < n; i++) {
        double s = gen() * p_vol.val;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        sumsq += s * s;
        out[i] = (Sint16)lrint(s * 32767.0);
    }
    SDL_AtomicSet(&rms_milli, (int)(sqrt(sumsq / n) * 1000.0));
}

/* ================= GUI ================= */

#define WIN_W 640
#define WIN_H 480

static const char *names[5] = { "White", "Pink", "Brown", "Rain", "Sea" };
static double (*gens[5])(void) = { white, pink, brown, rain, sea };
static const SDL_Color accents[5] = {
    { 200, 200, 200, 255 }, { 235, 140, 180, 255 }, { 170, 120, 70, 255 },
    { 110, 170, 230, 255 }, {  60, 130, 160, 255 },
};

static SDL_Rect sound_btn(int i)  { return (SDL_Rect){ 20, 20 + i * 58, 150, 48 }; }
static SDL_Rect param_track(int j){ return (SDL_Rect){ 190, 52 + j * 66, 330, 8 }; }
static SDL_Rect reset_btn(void)   { return (SDL_Rect){ 190, 320, 150, 32 }; }
static SDL_Rect vol_track(void)   { return (SDL_Rect){ 20, 388, 300, 8 }; }
static SDL_Rect play_btn(void)    { return (SDL_Rect){ 20, 416, 120, 44 }; }
static SDL_Rect vu_rect(void)     { return (SDL_Rect){ 160, 430, 460, 16 }; }

static int hit(SDL_Rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *s,
                      int x, int y, SDL_Color c)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_FreeSurface(surf);
    if (tex) { SDL_RenderCopy(ren, tex, NULL, &dst); SDL_DestroyTexture(tex); }
}

static TTF_Font *load_font(int pt)
{
    const char *paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
    };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], pt);
        if (f) return f;
    }
    return NULL;
}

static void slider_draw(SDL_Renderer *ren, TTF_Font *font, SDL_Rect tr,
                        const param *p, SDL_Color accent)
{
    double frac = (p->val - p->lo) / (p->hi - p->lo);
    SDL_SetRenderDrawColor(ren, 60, 64, 72, 255);
    SDL_RenderFillRect(ren, &tr);
    SDL_Rect fill = tr; fill.w = (int)(tr.w * frac);
    SDL_SetRenderDrawColor(ren, accent.r, accent.g, accent.b, 255);
    SDL_RenderFillRect(ren, &fill);
    SDL_Rect knob = { tr.x + (int)(tr.w * frac) - 5, tr.y - 7, 10, 22 };
    SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
    SDL_RenderFillRect(ren, &knob);

    char buf[64];
    if (strcmp(p->fmt, "%.0f%%") == 0)
        snprintf(buf, sizeof buf, "%.0f%%", p->val * 100.0);
    else
        snprintf(buf, sizeof buf, p->fmt, p->val);
    draw_text(ren, font, buf, tr.x + tr.w + 10, tr.y - 6,
              (SDL_Color){ 200, 200, 200, 255 });
}

static void slider_set_from_x(param *p, SDL_Rect tr, int mx)
{
    double v = (double)(mx - tr.x) / tr.w;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    p->val = p->lo + v * (p->hi - p->lo);
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        return 1;
    }
    TTF_Font *font  = load_font(16);
    TTF_Font *small = load_font(13);
    if (!font || !small) {
        fprintf(stderr, "no usable font found (install fonts-dejavu-core)\n");
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("noise machine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    int have_vsync = 1;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        have_vsync = 0;
    }
    if (!win || !ren) {
        fprintf(stderr, "window/renderer: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = RATE; want.format = AUDIO_S16SYS;
    want.channels = 1; want.samples = 4096; want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 1;
    }

    srand((unsigned)time(NULL));

    int sel = 2, playing = 1;
    int drag = -1;   /* -1 none, 0..n-1 param slider j, 100 = volume */
    SDL_PauseAudioDevice(dev, 0);

    int run = 1;
    while (run) {
        param *ps = param_sets[sel];
        int np = param_counts[sel];

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: run = 0; break;

            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: run = 0; break;
                case SDLK_SPACE:
                    playing = !playing;
                    SDL_PauseAudioDevice(dev, !playing);
                    break;
                case SDLK_r:
                    SDL_LockAudioDevice(dev);
                    for (int j = 0; j < np; j++) ps[j].val = ps[j].def;
                    SDL_UnlockAudioDevice(dev);
                    break;
                case SDLK_UP: case SDLK_DOWN: {
                    double d = (ev.key.keysym.sym == SDLK_UP) ? 0.05 : -0.05;
                    SDL_LockAudioDevice(dev);
                    p_vol.val += d;
                    if (p_vol.val < 0.0) p_vol.val = 0.0;
                    if (p_vol.val > 1.0) p_vol.val = 1.0;
                    SDL_UnlockAudioDevice(dev);
                    break;
                }
                default:
                    if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_5) {
                        sel = ev.key.keysym.sym - SDLK_1;
                        SDL_LockAudioDevice(dev);
                        gen = gens[sel];
                        SDL_UnlockAudioDevice(dev);
                    }
                }
                break;

            case SDL_MOUSEBUTTONDOWN: {
                int x = ev.button.x, y = ev.button.y;
                for (int i = 0; i < 5; i++)
                    if (hit(sound_btn(i), x, y)) {
                        sel = i;
                        SDL_LockAudioDevice(dev);
                        gen = gens[i];
                        SDL_UnlockAudioDevice(dev);
                    }
                if (hit(play_btn(), x, y)) {
                    playing = !playing;
                    SDL_PauseAudioDevice(dev, !playing);
                }
                if (hit(reset_btn(), x, y)) {
                    SDL_LockAudioDevice(dev);
                    for (int j = 0; j < np; j++) ps[j].val = ps[j].def;
                    SDL_UnlockAudioDevice(dev);
                }
                for (int j = 0; j < np; j++) {
                    SDL_Rect tr = param_track(j);
                    SDL_Rect grab = { tr.x - 8, tr.y - 12, tr.w + 16, tr.h + 24 };
                    if (hit(grab, x, y)) drag = j;
                }
                SDL_Rect vt = vol_track();
                SDL_Rect vg = { vt.x - 8, vt.y - 12, vt.w + 16, vt.h + 24 };
                if (hit(vg, x, y)) drag = 100;
            } /* FALLTHROUGH */
            case SDL_MOUSEMOTION:
                if (drag >= 0) {
                    int mx = (ev.type == SDL_MOUSEMOTION) ? ev.motion.x : ev.button.x;
                    SDL_LockAudioDevice(dev);
                    if (drag == 100)
                        slider_set_from_x(&p_vol, vol_track(), mx);
                    else if (drag < np)
                        slider_set_from_x(&ps[drag], param_track(drag), mx);
                    SDL_UnlockAudioDevice(dev);
                }
                break;

            case SDL_MOUSEBUTTONUP: drag = -1; break;
            }
        }

        /* ---------- draw ---------- */
        SDL_SetRenderDrawColor(ren, 24, 26, 30, 255);
        SDL_RenderClear(ren);

        for (int i = 0; i < 5; i++) {
            SDL_Rect r = sound_btn(i);
            if (i == sel)
                SDL_SetRenderDrawColor(ren, accents[i].r / 2, accents[i].g / 2,
                                       accents[i].b / 2, 255);
            else
                SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, accents[i].r, accents[i].g, accents[i].b, 255);
            SDL_RenderDrawRect(ren, &r);
            draw_text(ren, font, names[i], r.x + 16, r.y + 13,
                      (SDL_Color){ 230, 230, 230, 255 });
        }

        /* parameter panel for the selected sound */
        for (int j = 0; j < np; j++) {
            SDL_Rect tr = param_track(j);
            draw_text(ren, small, ps[j].name, tr.x, tr.y - 22,
                      (SDL_Color){ 170, 175, 185, 255 });
            slider_draw(ren, font, tr, &ps[j], accents[sel]);
        }
        SDL_Rect rb = reset_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &rb);
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDrawRect(ren, &rb);
        draw_text(ren, small, "Reset to defaults (r)", rb.x + 14, rb.y + 8,
                  (SDL_Color){ 210, 210, 210, 255 });

        /* master volume + play + VU */
        draw_text(ren, small, "Volume", vol_track().x, vol_track().y - 22,
                  (SDL_Color){ 170, 175, 185, 255 });
        slider_draw(ren, font, vol_track(), &p_vol,
                    (SDL_Color){ 120, 180, 120, 255 });

        SDL_Rect pb = play_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &pb);
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDrawRect(ren, &pb);
        draw_text(ren, font, playing ? "Pause" : "Play", pb.x + 34, pb.y + 11,
                  (SDL_Color){ 230, 230, 230, 255 });

        double rms = SDL_AtomicGet(&rms_milli) / 1000.0;
        double db = (rms > 1e-4) ? 20.0 * log10(rms) : -80.0;
        double frac = (db + 60.0) / 60.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        if (!playing) frac = 0.0;
        SDL_Rect vu = vu_rect();
        SDL_SetRenderDrawColor(ren, 60, 64, 72, 255);
        SDL_RenderFillRect(ren, &vu);
        SDL_Rect vf = vu; vf.w = (int)(vu.w * frac);
        SDL_SetRenderDrawColor(ren, accents[sel].r, accents[sel].g, accents[sel].b, 255);
        SDL_RenderFillRect(ren, &vf);

        SDL_RenderPresent(ren);
        if (!have_vsync) SDL_Delay(16);
    }

    SDL_CloseAudioDevice(dev);
    TTF_CloseFont(font);
    TTF_CloseFont(small);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
