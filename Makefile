# webOS PDK Runner X11 — Build system
# Requires: gcc, pkg-config, libsdl2-dev, libgl-dev, librt

HOST_CC = gcc
CFLAGS  = -std=gnu99 -O2

.PHONY: all clean gl_relay audio_relay font_redirect

all: gl_relay audio_relay libs/font_redirect.so

gl_relay: gl_relay.c pdk_gl_cmd.h pdk_input.h pvrtc_decode.h
	$(HOST_CC) $(CFLAGS) -o $@ gl_relay.c \
		$$(pkg-config --cflags --libs sdl2) \
		-lGL -lrt -lm

audio_relay: audio_relay.c
	$(HOST_CC) $(CFLAGS) -o $@ audio_relay.c -lasound -lrt

libs/font_redirect.so: font_redirect.c
	arm-linux-gnueabi-gcc $(CFLAGS) -shared -fPIC -o $@ $< -ldl

clean:
	rm -f gl_relay audio_relay libs/font_redirect.so
