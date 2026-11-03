/* dsp.h — ZzzTop shared DSP core: 10 sounds, stereo, two-sound mixing.
 * One implementation (dsp.c) serves the native programs and the wasm app. */
#ifndef ZZZTOP_DSP_H
#define ZZZTOP_DSP_H

void   dsp_init(double rate);          /* set sample rate, reset all state  */
void   dsp_set_sound(int i);           /* primary sound, 0..9               */
void   dsp_set_overlay(int i);         /* second mixed sound, -1 = none     */
void   dsp_set_overlay_gain(double g); /* overlay level 0..1                */
void   dsp_set_param(int i, double v); /* flat param table, see dsp.c       */
double dsp_get_param(int i);
int    dsp_num_params(void);
int    dsp_num_sounds(void);
float *dsp_get_buf_l(void);            /* filled by dsp_render              */
float *dsp_get_buf_r(void);
void   dsp_render(int n);              /* n <= 4096 frames, stereo          */

#endif
