/* noisegui.c — ZzzTop: sleep-sound machine with per-sound tuning, mixing,
 * sleep timer, presets, and a live FFT spectrum. Stereo, via the shared
 * DSP core (dsp.c).
 *
 * Build:  cc -O2 -o noisegui noisegui.c dsp.c $(sdl2-config --cflags --libs) -lSDL2_ttf -lm
 *         (needs: sudo apt install libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core)
 *
 * Keys: 1-9,0 select sound · o cycle overlay · space play/pause ·
 *       up/down volume · r reset sound params · s save preset · Esc quit.
 * Preset file: $XDG_CONFIG_HOME/zzztop.conf (default ~/.config/zzztop.conf),
 * loaded at startup, written by the Save button or the s key.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

#define TWO_PI 6.28318530717958647692

/* ================= sounds and parameter metadata ================= */

#define NSOUNDS 10

static const char *names[NSOUNDS] = {
    "White", "Pink", "Brown", "Airplane", "Rain",
    "Sea", "Wind", "Stream", "Birds", "Crickets",
};
static const SDL_Color accents[NSOUNDS] = {
    { 200, 200, 200, 255 }, { 235, 140, 180, 255 }, { 170, 120,  70, 255 },
    { 110, 100, 140, 255 }, { 110, 170, 230, 255 }, {  60, 130, 160, 255 },
    { 127, 174, 107, 255 }, { 110, 200, 176, 255 }, { 230, 200, 110, 255 },
    { 160, 180,  80, 255 },
};

typedef struct { const char *name; int idx; double lo, hi, def; const char *fmt; } pmeta;

static const pmeta P_WHITE[]  = { { "Low-pass tone (Hz)",  0, 500, 20000, 20000, "%.0f" } };
static const pmeta P_PINK[]   = { { "Low-pass tone (Hz)",  1, 500, 20000, 20000, "%.0f" } };
static const pmeta P_BROWN[]  = { { "Leak (tight <-> deep)", 2, 0.90, 0.995, 0.995, "%.4f" } };
static const pmeta P_DEEP[]   = { { "2nd-stage leak",      3, 0.90, 0.9995, 0.997, "%.4f" } };
static const pmeta P_RAIN[] = {
    { "Drop density (/s)",   4, 5,   150,  60,   "%.0f" },
    { "Drop tone (Hz)",      5, 150, 3000, 600,  "%.0f" },
    { "Drop level",          6, 0,   3,    1.6,  "%.2f" },
    { "Hiss level",          7, 0,   0.5,  0.15, "%.2f" },
};
static const pmeta P_SEA[] = {
    { "Wave period (s)",     8, 4,    20,   9.0,  "%.1f" },
    { "Crash sharpness",     9, 1,    5,    3.0,  "%.1f" },
    { "Surf brightness (Hz)",10, 1000, 8000, 5500, "%.0f" },
    { "Rumble level",        11, 0,   5,    2.6,  "%.2f" },
};
static const pmeta P_WIND[] = {
    { "Gustiness",           12, 0,   1,    0.6,  "%.2f" },
    { "Leaf rustle",         13, 0,   1.5,  0.6,  "%.2f" },
    { "Body tone (Hz)",      14, 100, 700,  250,  "%.0f" },
};
static const pmeta P_STREAM[] = {
    { "Bubble rate (/s)",    15, 10,  150,  50,   "%.0f" },
    { "Bubble pitch (Hz)",   16, 300, 2000, 900,  "%.0f" },
    { "Bubble level",        17, 0,   2,    0.9,  "%.2f" },
    { "Flow level",          18, 0,   0.4,  0.12, "%.2f" },
};
static const pmeta P_BIRDS[] = {
    { "Songs per 10 s",      19, 0.5, 10,   2.5,  "%.1f" },
    { "Pitch (Hz)",          20, 1500,5000, 2800, "%.0f" },
    { "Ambience",            21, 0,   0.25, 0.03, "%.2f" },
};
static const pmeta P_CRICK[] = {
    { "Chirps per s",        22, 0.3, 4,    1.2,  "%.1f" },
    { "Pitch (Hz)",          23, 3000,6000, 4300, "%.0f" },
    { "Ambience",            24, 0,   0.25, 0.02, "%.2f" },
};

static const pmeta *psets[NSOUNDS] = {
    P_WHITE, P_PINK, P_BROWN, P_DEEP, P_RAIN,
    P_SEA, P_WIND, P_STREAM, P_BIRDS, P_CRICK,
};
static const int pcounts[NSOUNDS] = { 1, 1, 1, 1, 4, 4, 3, 4, 3, 3 };

/* ================= shared state (UI <-> audio thread) ================= */

/* Written by the UI only inside SDL_LockAudioDevice(dev); read by the
 * callback. vol_slider is the knob position; the applied gain is its
 * cube (an audio taper: sleep volumes live in the lower half of a
 * linear slider's bottom, the cube spreads them across the travel). */
static double vol_slider = 0.75;
static double timer_total, timer_left;   /* in samples; 0 = off */

static SDL_atomic_t rms_milli;
static SDL_atomic_t timer_expired;

/* spectrum tap: pre-volume mono mix of the core's output */
#define FFT_N 4096
static double spec_ring[FFT_N];
static SDL_atomic_t spec_widx;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    Sint16 *out = (Sint16 *)stream;
    int frames = len / (2 * (int)sizeof(Sint16));
    static int wpos;
    double g = vol_slider * vol_slider * vol_slider;
    double sumsq = 0.0;

    while (frames > 0) {
        int k = frames > 4096 ? 4096 : frames;
        dsp_render(k);
        const float *L = dsp_get_buf_l(), *R = dsp_get_buf_r();
        for (int i = 0; i < k; i++) {
            double fade = 1.0;
            if (timer_total > 0.0) {
                fade = timer_left / timer_total;
                if (fade < 0.0) fade = 0.0;
                timer_left -= 1.0;
                if (timer_left <= 0.0) {
                    timer_total = 0.0;
                    SDL_AtomicSet(&timer_expired, 1);
                }
            }
            double m = 0.5 * (L[i] + R[i]);
            spec_ring[wpos++ & (FFT_N - 1)] = m;
            double l = L[i] * g * fade, r = R[i] * g * fade;
            sumsq += 0.5 * (l * l + r * r);
            out[2 * i]     = (Sint16)lrint(l * 32767.0);
            out[2 * i + 1] = (Sint16)lrint(r * 32767.0);
        }
        out += 2 * k;
        frames -= k;
    }
    SDL_AtomicSet(&spec_widx, wpos);
    SDL_AtomicSet(&rms_milli, (int)(sqrt(sumsq * 2.0 * sizeof(Sint16) / len) * 1000.0));
}

/* ================= preset file ================= */

static void conf_path(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/zzztop.conf", xdg);
    else snprintf(buf, n, "%s/.config/zzztop.conf", getenv("HOME") ? getenv("HOME") : ".");
}

static void preset_save(int sel, int ovl, double ovgain)
{
    char p[512];
    conf_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# ZzzTop preset (written by noisegui)\n");
    fprintf(f, "sel %d\noverlay %d\novgain %.4f\nvol %.4f\n", sel, ovl, ovgain, vol_slider);
    for (int i = 0; i < dsp_num_params(); i++)
        fprintf(f, "p%d %.6g\n", i, dsp_get_param(i));
    fclose(f);
}

static void preset_load(int *sel, int *ovl, double *ovgain)
{
    char p[512], key[32];
    double val;
    conf_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fscanf(f, "%31s %lf", key, &val) == 2) {
        if (strcmp(key, "sel") == 0 && val >= 0 && val < NSOUNDS) *sel = (int)val;
        else if (strcmp(key, "overlay") == 0 && val >= -1 && val < NSOUNDS) *ovl = (int)val;
        else if (strcmp(key, "ovgain") == 0) *ovgain = val;
        else if (strcmp(key, "vol") == 0 && val >= 0 && val <= 1) vol_slider = val;
        else if (key[0] == 'p') dsp_set_param(atoi(key + 1), val);
    }
    fclose(f);
}

/* ================= spectrum analysis (UI thread) ================= */

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -TWO_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int j = 0; j < len / 2; j++) {
                double ur = re[i + j], ui = im[i + j];
                double vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                double vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                re[i + j] = ur + vr;           im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

#define NBARS  60
#define SPEC_F_LO 30.0
#define SPEC_F_HI 16000.0
#define SPEC_DB_LO -70.0
#define SPEC_DB_HI -15.0

static double spec_bars[NBARS];

static void spectrum_update(int playing, int rate)
{
    static double hann[FFT_N];
    static int init;
    if (!init) {
        for (int i = 0; i < FFT_N; i++)
            hann[i] = 0.5 * (1.0 - cos(TWO_PI * i / (FFT_N - 1)));
        init = 1;
    }

    double re[FFT_N], im[FFT_N];
    int w = SDL_AtomicGet(&spec_widx);
    for (int i = 0; i < FFT_N; i++) {
        re[i] = spec_ring[(w - FFT_N + i) & (FFT_N - 1)] * hann[i];
        im[i] = 0.0;
    }
    fft(re, im, FFT_N);

    for (int b = 0; b < NBARS; b++) {
        double f0 = SPEC_F_LO * pow(SPEC_F_HI / SPEC_F_LO, (double)b / NBARS);
        double f1 = SPEC_F_LO * pow(SPEC_F_HI / SPEC_F_LO, (double)(b + 1) / NBARS);
        int k0 = (int)(f0 * FFT_N / rate), k1 = (int)(f1 * FFT_N / rate);
        if (k1 <= k0) k1 = k0 + 1;
        double peak = 0.0;
        for (int k = k0; k < k1 && k < FFT_N / 2; k++) {
            double m2 = re[k] * re[k] + im[k] * im[k];
            if (m2 > peak) peak = m2;
        }
        double db = 10.0 * log10(peak + 1e-18) - 20.0 * log10(FFT_N / 4.0);
        double h = (db - SPEC_DB_LO) / (SPEC_DB_HI - SPEC_DB_LO);
        if (h < 0.0) h = 0.0;
        if (h > 1.0) h = 1.0;
        if (!playing) h = 0.0;
        spec_bars[b] = (h > spec_bars[b]) ? h : spec_bars[b] * 0.90;
    }
}

/* ================= tiny widget layer ================= */

#define WIN_W 640
#define WIN_H 770

static SDL_Rect sound_btn(int i)   { return (SDL_Rect){ 20, 20 + i * 40, 150, 32 }; }
static SDL_Rect param_track(int j) { return (SDL_Rect){ 190, 52 + j * 58, 330, 8 }; }
static SDL_Rect reset_btn(void)    { return (SDL_Rect){ 190, 288, 150, 28 }; }
static SDL_Rect ovl_btn(void)      { return (SDL_Rect){ 190, 356, 130, 28 }; }
static SDL_Rect ovl_track(void)    { return (SDL_Rect){ 350, 366, 170, 8 }; }
static SDL_Rect timer_track(void)  { return (SDL_Rect){ 190, 428, 330, 8 }; }
static SDL_Rect vol_track(void)    { return (SDL_Rect){ 20, 490, 290, 8 }; }
static SDL_Rect save_btn(void)     { return (SDL_Rect){ 400, 478, 130, 30 }; }
static SDL_Rect play_btn(void)     { return (SDL_Rect){ 20, 524, 120, 40 }; }
static SDL_Rect vu_rect(void)      { return (SDL_Rect){ 160, 536, 460, 16 }; }
static SDL_Rect spec_rect(void)    { return (SDL_Rect){ 20, 580, 600, 170 }; }

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

static void track_draw(SDL_Renderer *ren, SDL_Rect tr, double frac, SDL_Color accent)
{
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    SDL_SetRenderDrawColor(ren, 60, 64, 72, 255);
    SDL_RenderFillRect(ren, &tr);
    SDL_Rect fill = tr; fill.w = (int)(tr.w * frac);
    SDL_SetRenderDrawColor(ren, accent.r, accent.g, accent.b, 255);
    SDL_RenderFillRect(ren, &fill);
    SDL_Rect knob = { tr.x + (int)(tr.w * frac) - 5, tr.y - 7, 10, 22 };
    SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
    SDL_RenderFillRect(ren, &knob);
}

static double track_frac(SDL_Rect tr, int mx)
{
    double v = (double)(mx - tr.x) / tr.w;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return v;
}

static int track_grabbed(SDL_Rect tr, int x, int y)
{
    SDL_Rect g = { tr.x - 8, tr.y - 12, tr.w + 16, tr.h + 24 };
    return hit(g, x, y);
}

/* ================= main ================= */

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

    SDL_Window *win = SDL_CreateWindow("ZzzTop",
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
    want.freq = 44100; want.format = AUDIO_S16SYS;
    want.channels = 2; want.samples = 4096; want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 1;
    }
    int rate = have.freq > 0 ? have.freq : 44100;

    int sel = 2, ovl = -1, playing = 1;
    double ovgain = 0.5, timer_min = 0.0;

    dsp_init(rate);
    preset_load(&sel, &ovl, &ovgain);
    dsp_set_sound(sel);
    dsp_set_overlay(ovl);
    dsp_set_overlay_gain(ovgain);

    SDL_PauseAudioDevice(dev, 0);

    /* drag targets: 0..3 param sliders, 100 volume, 101 overlay gain,
     * 102 timer, -1 none */
    int drag = -1;
    int run = 1;
    while (run) {
        const pmeta *ps = psets[sel];
        int np = pcounts[sel];

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
                    for (int j = 0; j < np; j++) dsp_set_param(ps[j].idx, ps[j].def);
                    SDL_UnlockAudioDevice(dev);
                    break;
                case SDLK_s:
                    preset_save(sel, ovl, ovgain);
                    break;
                case SDLK_o:
                    ovl = (ovl + 2 <= NSOUNDS) ? ovl + 1 : -1;
                    if (ovl == sel) ovl = (ovl + 1 < NSOUNDS) ? ovl + 1 : -1;
                    SDL_LockAudioDevice(dev);
                    dsp_set_overlay(ovl);
                    SDL_UnlockAudioDevice(dev);
                    break;
                case SDLK_UP: case SDLK_DOWN: {
                    double d = (ev.key.keysym.sym == SDLK_UP) ? 0.05 : -0.05;
                    SDL_LockAudioDevice(dev);
                    vol_slider += d;
                    if (vol_slider < 0.0) vol_slider = 0.0;
                    if (vol_slider > 1.0) vol_slider = 1.0;
                    SDL_UnlockAudioDevice(dev);
                    break;
                }
                default: {
                    int k = -1;
                    if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_9)
                        k = ev.key.keysym.sym - SDLK_1;
                    else if (ev.key.keysym.sym == SDLK_0)
                        k = 9;
                    if (k >= 0 && k < NSOUNDS) {
                        sel = k;
                        SDL_LockAudioDevice(dev);
                        dsp_set_sound(sel);
                        SDL_UnlockAudioDevice(dev);
                    }
                }
                }
                break;

            case SDL_MOUSEBUTTONDOWN: {
                int x = ev.button.x, y = ev.button.y;
                for (int i = 0; i < NSOUNDS; i++)
                    if (hit(sound_btn(i), x, y)) {
                        sel = i;
                        SDL_LockAudioDevice(dev);
                        dsp_set_sound(sel);
                        SDL_UnlockAudioDevice(dev);
                    }
                if (hit(play_btn(), x, y)) {
                    playing = !playing;
                    SDL_PauseAudioDevice(dev, !playing);
                }
                if (hit(reset_btn(), x, y)) {
                    SDL_LockAudioDevice(dev);
                    for (int j = 0; j < np; j++) dsp_set_param(ps[j].idx, ps[j].def);
                    SDL_UnlockAudioDevice(dev);
                }
                if (hit(save_btn(), x, y))
                    preset_save(sel, ovl, ovgain);
                if (hit(ovl_btn(), x, y)) {
                    ovl = (ovl + 2 <= NSOUNDS) ? ovl + 1 : -1;
                    if (ovl == sel) ovl = (ovl + 1 < NSOUNDS) ? ovl + 1 : -1;
                    SDL_LockAudioDevice(dev);
                    dsp_set_overlay(ovl);
                    SDL_UnlockAudioDevice(dev);
                }
                for (int j = 0; j < np; j++)
                    if (track_grabbed(param_track(j), x, y)) drag = j;
                if (track_grabbed(vol_track(), x, y))   drag = 100;
                if (track_grabbed(ovl_track(), x, y))   drag = 101;
                if (track_grabbed(timer_track(), x, y)) drag = 102;
            } /* FALLTHROUGH */
            case SDL_MOUSEMOTION:
                if (drag >= 0) {
                    int mx = (ev.type == SDL_MOUSEMOTION) ? ev.motion.x : ev.button.x;
                    SDL_LockAudioDevice(dev);
                    if (drag == 100)
                        vol_slider = track_frac(vol_track(), mx);
                    else if (drag == 101) {
                        ovgain = track_frac(ovl_track(), mx);
                        dsp_set_overlay_gain(ovgain);
                    } else if (drag == 102) {
                        timer_min = 120.0 * track_frac(timer_track(), mx);
                        timer_min = 5.0 * floor(timer_min / 5.0 + 0.5);
                        timer_total = timer_left = timer_min * 60.0 * rate;
                        if (timer_min <= 0.0) timer_total = timer_left = 0.0;
                    } else if (drag < np) {
                        const pmeta *p = &ps[drag];
                        dsp_set_param(p->idx,
                            p->lo + track_frac(param_track(drag), mx) * (p->hi - p->lo));
                    }
                    SDL_UnlockAudioDevice(dev);
                }
                break;

            case SDL_MOUSEBUTTONUP: drag = -1; break;
            }
        }

        if (SDL_AtomicGet(&timer_expired)) {
            SDL_AtomicSet(&timer_expired, 0);
            playing = 0;
            timer_min = 0.0;
            SDL_PauseAudioDevice(dev, 1);
        }

        /* ---------- draw ---------- */
        SDL_SetRenderDrawColor(ren, 24, 26, 30, 255);
        SDL_RenderClear(ren);

        for (int i = 0; i < NSOUNDS; i++) {
            SDL_Rect r = sound_btn(i);
            if (i == sel)
                SDL_SetRenderDrawColor(ren, accents[i].r / 2, accents[i].g / 2,
                                       accents[i].b / 2, 255);
            else
                SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, accents[i].r, accents[i].g, accents[i].b, 255);
            SDL_RenderDrawRect(ren, &r);
            draw_text(ren, font, names[i], r.x + 14, r.y + 6,
                      (SDL_Color){ 230, 230, 230, 255 });
        }

        /* parameter panel */
        char buf[64];
        for (int j = 0; j < np; j++) {
            const pmeta *p = &ps[j];
            SDL_Rect tr = param_track(j);
            double val = dsp_get_param(p->idx);
            draw_text(ren, small, p->name, tr.x, tr.y - 22,
                      (SDL_Color){ 170, 175, 185, 255 });
            track_draw(ren, tr, (val - p->lo) / (p->hi - p->lo), accents[sel]);
            snprintf(buf, sizeof buf, p->fmt, val);
            draw_text(ren, font, buf, tr.x + tr.w + 10, tr.y - 6,
                      (SDL_Color){ 200, 200, 200, 255 });
        }
        SDL_Rect rb = reset_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &rb);
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDrawRect(ren, &rb);
        draw_text(ren, small, "Reset to defaults (r)", rb.x + 14, rb.y + 6,
                  (SDL_Color){ 210, 210, 210, 255 });

        /* overlay mixer */
        draw_text(ren, small, "Overlay sound (o)  +  level", 190, 334,
                  (SDL_Color){ 170, 175, 185, 255 });
        SDL_Rect ob = ovl_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &ob);
        if (ovl >= 0)
            SDL_SetRenderDrawColor(ren, accents[ovl].r, accents[ovl].g, accents[ovl].b, 255);
        else
            SDL_SetRenderDrawColor(ren, 100, 104, 112, 255);
        SDL_RenderDrawRect(ren, &ob);
        draw_text(ren, font, ovl >= 0 ? names[ovl] : "none", ob.x + 14, ob.y + 4,
                  (SDL_Color){ 230, 230, 230, 255 });
        track_draw(ren, ovl_track(), ovgain,
                   ovl >= 0 ? accents[ovl] : (SDL_Color){ 100, 104, 112, 255 });
        snprintf(buf, sizeof buf, "%.0f%%", ovgain * 100.0);
        draw_text(ren, font, buf, ovl_track().x + ovl_track().w + 10,
                  ovl_track().y - 6, (SDL_Color){ 200, 200, 200, 255 });

        /* sleep timer */
        draw_text(ren, small, "Sleep timer (fades to silence; 0 = off)", 190, 406,
                  (SDL_Color){ 170, 175, 185, 255 });
        track_draw(ren, timer_track(), timer_min / 120.0,
                   (SDL_Color){ 180, 140, 200, 255 });
        if (timer_total > 0.0) {
            double left_min = timer_left / (60.0 * rate);
            snprintf(buf, sizeof buf, "%.0f min", left_min + 0.5);
        } else
            snprintf(buf, sizeof buf, timer_min > 0 ? "%.0f min" : "off", timer_min);
        draw_text(ren, font, buf, timer_track().x + timer_track().w + 10,
                  timer_track().y - 6, (SDL_Color){ 200, 200, 200, 255 });

        /* volume (cube taper) */
        draw_text(ren, small, "Volume", vol_track().x, vol_track().y - 22,
                  (SDL_Color){ 170, 175, 185, 255 });
        track_draw(ren, vol_track(), vol_slider, (SDL_Color){ 120, 180, 120, 255 });
        snprintf(buf, sizeof buf, "%.0f%%", vol_slider * 100.0);
        draw_text(ren, font, buf, vol_track().x + vol_track().w + 10,
                  vol_track().y - 6, (SDL_Color){ 200, 200, 200, 255 });

        /* save preset */
        SDL_Rect sb = save_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &sb);
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDrawRect(ren, &sb);
        draw_text(ren, small, "Save preset (s)", sb.x + 16, sb.y + 7,
                  (SDL_Color){ 210, 210, 210, 255 });

        /* play / pause + VU */
        SDL_Rect pb = play_btn();
        SDL_SetRenderDrawColor(ren, 45, 48, 54, 255);
        SDL_RenderFillRect(ren, &pb);
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDrawRect(ren, &pb);
        draw_text(ren, font, playing ? "Pause" : "Play", pb.x + 34, pb.y + 9,
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

        /* spectrum */
        spectrum_update(playing, rate);
        SDL_Rect sp = spec_rect();
        SDL_SetRenderDrawColor(ren, 30, 32, 38, 255);
        SDL_RenderFillRect(ren, &sp);
        const double marks[3] = { 100.0, 1000.0, 10000.0 };
        const char *mlab[3] = { "100", "1k", "10k" };
        for (int m = 0; m < 3; m++) {
            double fx = log(marks[m] / SPEC_F_LO) / log(SPEC_F_HI / SPEC_F_LO);
            int x = sp.x + (int)(fx * sp.w);
            SDL_SetRenderDrawColor(ren, 55, 58, 66, 255);
            SDL_RenderDrawLine(ren, x, sp.y, x, sp.y + sp.h);
            draw_text(ren, small, mlab[m], x + 3, sp.y + sp.h - 18,
                      (SDL_Color){ 130, 135, 145, 255 });
        }
        int bw = sp.w / NBARS;
        for (int b = 0; b < NBARS; b++) {
            int h = (int)(spec_bars[b] * (sp.h - 4));
            if (h < 1) continue;
            SDL_Rect bar = { sp.x + b * bw + 1, sp.y + sp.h - 2 - h, bw - 2, h };
            SDL_SetRenderDrawColor(ren, accents[sel].r, accents[sel].g,
                                   accents[sel].b, 255);
            SDL_RenderFillRect(ren, &bar);
        }

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
