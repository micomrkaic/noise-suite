#!/bin/sh
# Build the single-file WASM noise machine from the shared DSP core (../dsp.c).
# Requires wasi-sdk: https://github.com/WebAssembly/wasi-sdk/releases
# Usage: WASI_SDK=/path/to/wasi-sdk sh build.sh
set -e
: "${WASI_SDK:?set WASI_SDK to your wasi-sdk directory}"
"$WASI_SDK/bin/clang" --target=wasm32-wasip1 -mcpu=mvp -O2 -mexec-model=reactor -I.. \
  -Wl,--export=dsp_init -Wl,--export=dsp_set_sound \
  -Wl,--export=dsp_set_overlay -Wl,--export=dsp_set_overlay_gain \
  -Wl,--export=dsp_set_param -Wl,--export=dsp_get_param \
  -Wl,--export=dsp_num_params -Wl,--export=dsp_num_sounds \
  -Wl,--export=dsp_render -Wl,--export=dsp_get_buf_l -Wl,--export=dsp_get_buf_r \
  -Wl,--no-entry -o dsp.wasm ../dsp.c
python3 - << 'PY'
import base64
b64 = base64.b64encode(open('dsp.wasm','rb').read()).decode()
html = open('template.html').read().replace('__WASM_B64__', b64)
open('noisemachine.html','w').write(html)
print('noisemachine.html written (%d KB)' % (len(html)//1024))
PY
