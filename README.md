# noise-suite (ZzzTop)

ZzzTop: sleep-sound synthesis in plain C: colored noise (white / pink / brown)
and procedural soundscapes (rain / sea), as both WAV generators and
real-time players.

## Programs

| Program        | Sounds                          | Output      | Dependencies    |
|----------------|---------------------------------|-------------|-----------------|
| `noisegen`     | white, pink, brown, deep        | WAV file    | libm only       |
| `soundscape`   | rain, sea                       | WAV file    | libm only       |
| `noisemachine` | all six                         | real-time   | SDL2            |
| `noisesdl`     | white, pink, brown              | real-time   | SDL2            |
| `noiselive`    | white, pink, brown              | real-time   | ALSA (Linux)    |

`noisemachine` is the one to use day-to-day; `noisesdl` and `noiselive`
are kept as minimal single-backend references.

## Build

    sudo apt install libsdl2-dev libasound2-dev   # Debian/Ubuntu
    make            # builds everything
    make noisegen   # or any single target
    make samples    # regenerate 30 s demo WAVs into samples/

On macOS, `noisegen`, `soundscape`, and the SDL programs build as-is
(SDL2 via Homebrew); skip `noiselive`.

## Usage

    ./noisemachine brown            # play at default volume 0.3
    ./noisemachine sea 0.5          # play at volume 0.5, Ctrl-C stops
    ./noisegen pink 3600 pink.wav   # one hour of pink noise to a file
    ./soundscape rain 600 rain.wav  # ten minutes of rain

## Web / phone version (web/)

`web/dsp.c` is the DSP core alone (own PRNG, runtime sample rate, no
libc beyond libm) compiled to a zero-import WebAssembly module, with
the UI as plain HTML/JS: native touch sliders, Web Audio output, and
the browser's AnalyserNode for the live spectrum. `web/build.sh`
(needs wasi-sdk) produces `noisemachine.html` — a single
self-contained file with the wasm embedded base64. Copy it to a
phone, open it, "add to home screen"; it works offline, over plain
http, or from file://, because output uses ScriptProcessorNode rather
than AudioWorklet (which requires a secure context).

## How the synthesis works — the mathematics

Notation: sample rate $f_s$ (44.1 or 48 kHz), discrete time $n$, and
$w[n] \sim \mathrm{iid}\ U(-1,1)$ the white-noise source, with variance
$\sigma_w^2 = 1/3$. All spectra below are power spectral densities;
"dB/octave" slopes refer to power.

### Building block: the one-pole low-pass

Everything is assembled from the leaky integrator

$$y[n] = (1-\alpha)\,y[n-1] + \alpha\, x[n], \qquad
\alpha = 1 - e^{-2\pi f_c / f_s},$$

a first-order IIR filter with $-3$ dB point at $f_c$ and a $-6$
dB/octave rolloff above it. Its complement $x[n]-y[n]$ is the matching
high-pass. Band-passes are a high-pass followed by low-passes; each
extra pole steepens the skirt by another $-6$ dB/octave.

### White noise ($S(f) \propto f^0$)

$x[n] = w[n]$: a flat spectrum, $S(f) = \sigma_w^2 / f_s$ up to
Nyquist. The GUI's tone control is one low-pass on top.

### Pink noise ($S(f) \propto 1/f$, $-3$ dB/octave)

An exact $1/f$ spectrum has no finite-order rational transfer
function, so it must be approximated. The Kellet filter drives a
parallel bank of first-order (AR(1)) sections from common input:

$$b_k[n] = p_k\, b_k[n-1] + g_k\, w[n], \qquad k = 1,\dots,6,$$
$$y[n] = \sum_k b_k[n] + g_0\, w[n],$$

with poles $p_k$ spread geometrically from $\approx 0.55$ to
$0.99886$. Each section contributes a $1/f^2$ shoulder at a different
corner; their staggered sum approximates the $1/f$ slope to within a
few hundredths of a dB across the audio band.

For the econometrician: this is Granger's (1980) aggregation result
in miniature — a sum of AR(1) processes with dispersed coefficients
exhibits long memory that no single finite AR process can produce.
Pink noise is the acoustic face of fractional integration.

### Brown noise ($S(f) \propto 1/f^2$, $-6$ dB/octave)

A single AR(1) — leaky integration of white noise:

$$a[n] = L\, a[n-1] + g\, w[n], \qquad
S(f) = \frac{g^2 \sigma_w^2 / f_s}{\left|1 - L e^{-i 2\pi f/f_s}\right|^2}.$$

For $f$ well above the corner, $S(f) \propto 1/f^2$; the leak $L < 1$
flattens the spectrum below the corner frequency

$$f_{\mathrm{corner}} \approx \frac{-\ln L}{2\pi} f_s$$

($L = 0.997 \Rightarrow \approx 21$ Hz), which is what keeps the
random walk from wandering off to infinity. The stationary variance is
$\sigma_a^2 = g^2 \sigma_w^2 / (1 - L^2)$, so when the GUI slider
changes $L$, the output is rescaled by
$\sqrt{(1-L_0^2)/(1-L^2)}$ (reference $L_0 = 0.997$) to hold loudness
constant — otherwise the leak slider would double as a volume knob.

### Airplane noise ($S(f) \propto 1/f^4$, $-12$ dB/octave)

Brown noise fed through a second AR(1): a two-pole cascade,

$$a_1[n] = p_1 a_1[n-1] + w[n], \qquad a_2[n] = p_2 a_2[n-1] + a_1[n],$$

whose spectrum is the product of the two AR(1) spectra, hence
$1/f^4$ above both corners. The second stage's input is *correlated*,
so single-stage variance formulas do not compose; the exact stationary
variance of the cascade is

$$\mathrm{Var}(a_2) = \sigma_w^2\,
\frac{1 + p_1 p_2}{(1 - p_1 p_2)(1 - p_1^2)(1 - p_2^2)},$$

(verified against simulation) and the output is normalized by its
square root. Nearly all the energy lies below $\sim$100 Hz — the
spectral shape of turbulent-boundary-layer cabin noise, hence the name.

### Rain: filtered Poisson shot noise

Drop onsets $\{t_k\}$ form a Poisson process of rate $\lambda$
(implemented as per-sample spawn probability $\lambda/f_s$). Each drop
is a burst of low-passed noise under a smooth envelope:

$$v_k[n] = A_k \left( d_s^{\,n} - d_f^{\,n} \right)\,
\mathrm{LP}_{\phi_k}\!\big(w\big)[n],$$

where $d_s = e^{-1/(\tau_s f_s)}$ and $d_f = e^{-1/(\tau_f f_s)}$ are
slow (8–30 ms) and fast (1–3 ms) decay factors — the difference of
exponentials rises smoothly and then decays, avoiding the
broadband click of an instantaneous attack — and the per-drop cutoff
$\phi_k \sim U(T/2,\, 2.5\,T)$ randomizes brightness around the drop
tone $T$. Amplitudes $A_k$ are drawn skewed ($\propto U^2$): many
quiet drops, few loud ones.

By Campbell's theorem, the drop layer's spectrum is
$S(f) = \lambda\, \mathbb{E}\!\left[ A^2 |V(f)|^2 \right]$ — rate
scales power, the envelope-and-filter shape $V(f)$ sets the color.
On top sits the hiss bed, band-passed white noise
($400$ Hz high-pass, two-pole $4$ kHz low-pass) whose gain is slowly
modulated by low-passed noise ($f_c = 0.3$ Hz), mimicking drifting
rainfall intensity.

### Sea: co-modulated noise

A slow deterministic envelope built from two incommensurate swells,

$$e(t) = \mathrm{clip}_{[0,1]}\!\left( 0.5 + 0.3 \sin\frac{2\pi t}{T}
+ 0.2 \sin\!\left(\frac{2\pi t}{1.522\,T} + 1\right) \right),
\qquad c(t) = e(t)^{\gamma},$$

drives *both* the gain and the cutoff of a time-varying low-pass on
white noise:

$$\mathrm{surf}[n] = \big(0.15 + 0.85\, c\big)\;
\mathrm{LP}_{f(t)}(w)[n], \qquad f(t) = 250 + B\, c(t)\ \mathrm{Hz}.$$

The exponent $\gamma$ (crash sharpness) peaks the envelope: crashes
are simultaneously *louder and brighter*, which is what the ear reads
as breaking surf. Underneath, a constant two-pole 120 Hz low-passed
noise supplies the deep-water rumble. The process is cyclostationary
with periods $T$ and $1.522\,T$; the irrational-looking ratio keeps
successive waves from repeating exactly.

### Measured spectral slopes

FFT measurements of generated output agree with theory: white
$-0.1$, pink $-3.0$, brown $-6.0$, airplane $-11.8$ dB/octave. The
rain and sea spectra were compared against the acoustics literature
(Nystuen's underwater rainfall spectra; surf-zone bubble noise
peaking near 300–500 Hz) — see the repository history for the
comparison.

## Tuning knobs

- Rain density: the `25.0 / RATE` spawn probability (6 = sparse
  dripping, 80 = downpour).
- Drop character: `chirp` closer to 1.0 = hard-surface ticks,
  smaller = bloops into water.
- Sea mood: the two envelope periods, and the cube in
  `e*e*e` (higher power = violent surf, lower = lazy lapping).
- For a never-repeating sea, replace the two-sine envelope with
  very-low-pass-filtered noise.
