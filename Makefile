CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
SDLFLAGS = $(shell sdl2-config --cflags --libs)

# install prefix: binaries in $(PREFIX)/bin, man page in $(PREFIX)/share/man/man1
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin
MANDIR   = $(PREFIX)/share/man/man1

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

# ---- needs libsdl2-dev + libsdl2-ttf-dev ----
noisegui: noisegui.c
	$(CC) $(CFLAGS) -o $@ $< $(SDLFLAGS) -lSDL2_ttf -lm

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
	./soundscape wind 30 samples/wind.wav
	./soundscape stream 30 samples/stream.wav
	./soundscape birds 30 samples/birds.wav

install: all
	install -d $(BINDIR) $(MANDIR)
	install -m 755 $(ALL) $(BINDIR)
	ln -sf noisegui $(BINDIR)/zzztop
	install -m 644 man/zzztop.1 $(MANDIR)/zzztop.1
	for p in $(ALL); do ln -sf zzztop.1 $(MANDIR)/$$p.1; done

uninstall:
	rm -f $(addprefix $(BINDIR)/,$(ALL) zzztop)
	rm -f $(addprefix $(MANDIR)/,$(addsuffix .1,$(ALL)) zzztop.1)

clean:
	rm -f $(ALL)

.PHONY: all samples install uninstall clean
