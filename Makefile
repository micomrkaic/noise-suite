CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
SDLFLAGS = $(shell sdl2-config --cflags --libs)

ALL = noisegen soundscape noisemachine noisegui noisesdl noiselive

all: $(ALL)

# ---- no dependencies beyond libm ----
noisegen: noisegen.c
	$(CC) $(CFLAGS) -o $@ $< -lm

soundscape: soundscape.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# ---- needs libsdl2-dev ----
noisemachine: noisemachine.c
	$(CC) $(CFLAGS) -o $@ $< $(SDLFLAGS) -lm

noisesdl: noisesdl.c
	$(CC) $(CFLAGS) -o $@ $< $(SDLFLAGS) -lm

# ---- needs libasound2-dev (Linux only) ----
noiselive: noiselive.c
	$(CC) $(CFLAGS) -o $@ $< -lasound -lm

samples: noisegen soundscape
	mkdir -p samples
	./noisegen white 30 samples/white.wav
	./noisegen pink  30 samples/pink.wav
	./noisegen brown 30 samples/brown.wav
	./noisegen deep  30 samples/deep.wav
	./soundscape rain 30 samples/rain.wav
	./soundscape sea  30 samples/sea.wav

clean:
	rm -f $(ALL)

.PHONY: all samples clean

noisegui: noisegui.c
	$(CC) $(CFLAGS) -o $@ $< $(SDLFLAGS) -lSDL2_ttf -lm
