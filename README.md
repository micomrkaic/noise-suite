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

## How the synthesis works

**White** — uniform random samples: equal power at all frequencies.

**Pink** (−3 dB/octave) — Paul Kellet's cascade of leaky one-pole
filters whose sum approximates a 1/f spectrum within ~0.05 dB across
the audio band. An exact 1/f slope has no finite IIR realization.

**Brown** (−6 dB/octave) — leaky integration of white noise:
`acc = 0.997*acc + 0.02*white()`. The 0.997 leak is a gentle
high-pass that stops the random walk from drifting into clipping.
The leak L maps to a corner frequency f = −ln(L)·RATE/2π: L=0.997
is ~21 Hz (full rumble), L=0.99 ~70 Hz, L=0.90 ~740 Hz (tight,
tending toward white as L falls further).

**Deep** (−12 dB/octave, 1/f⁴, sometimes called "black" noise) —
brown noise fed through a second AR(1) integrator: a two-pole
cascade. Loudness is normalized analytically from the closed-form
variance of the cascade, so the GUI leak slider doesn't double as a
volume control. Nearly all its energy sits below ~100 Hz: expect
silence on laptop speakers, a heavy earthquake-rumble on headphones
or a subwoofer.

**Rain** — a Poisson process (~60/s) of "drop" voices, each a burst of
low-passed noise (300–1500 Hz) with a smooth 1–3 ms attack and an
8–30 ms decay; the envelope is a difference of two exponentials, since
an instantaneous attack reads as static crackle and a damped sine with
a pitch glide reads as birdsong. Underneath, a band-passed white-noise
hiss bed (400 Hz – 4 kHz, two-pole top end) whose level wobbles slowly
like real rainfall intensity.

**Sea** — white noise through a one-pole low-pass whose cutoff AND
gain are driven by a slow envelope (two summed sines with
incommensurate 9 s / 13.7 s periods, clamped and cubed for peaky
crashes), so waves get louder and brighter together, over a doubly
low-passed 120 Hz rumble bed.

All generators are stateful pull-style functions (one sample per
call), so they are indifferent to their sink: WAV writer, ALSA
blocking write, or SDL audio callback.

Verified spectral slopes (FFT over generated output): white
−0.1 dB/oct, pink −3.1, brown −6.0.

## Tuning knobs

- Rain density: the `25.0 / RATE` spawn probability (6 = sparse
  dripping, 80 = downpour).
- Drop character: `chirp` closer to 1.0 = hard-surface ticks,
  smaller = bloops into water.
- Sea mood: the two envelope periods, and the cube in
  `e*e*e` (higher power = violent surf, lower = lazy lapping).
- For a never-repeating sea, replace the two-sine envelope with
  very-low-pass-filtered noise.
