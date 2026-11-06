# Host-side bridge: serenity_compat /dev/audio/0 -> ALSA default device.
#
# Deliberately not part of the Lagom build: it is plain C and only needs
# libasound, so cross-building it in the VM stays trivial.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
ALSA_CFLAGS := $(shell pkg-config --cflags alsa)
ALSA_LIBS := $(shell pkg-config --libs alsa)

serenity-audio-bridge: serenity_audio_bridge.c
	$(CC) $(CFLAGS) $(ALSA_CFLAGS) -o $@ $< $(ALSA_LIBS)

install: serenity-audio-bridge
	install -D -m 755 serenity-audio-bridge $(DESTDIR)/usr/local/bin/serenity-audio-bridge

clean:
	rm -f serenity-audio-bridge

.PHONY: install clean
