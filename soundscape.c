/* soundscape.c — identical to noisegen: both are thin WAV front ends to
 * the shared DSP core (dsp.c) and accept every sound. The two names are
 * kept so existing scripts and the historical split (colored noise vs
 * nature soundscapes) keep working.
 *
 * Build:  cc -O2 -o soundscape soundscape.c dsp.c -lm
 */
#include "noisegen.c"
