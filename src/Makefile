# src/Makefile — Build all claw-linux C binaries
#
# Usage:
#   make               # build all binaries into bin/
#   make clean         # remove build artifacts
#   make claw-shell    # build only claw-shell
#   make claw-fs       # build only claw-fs
#   make claw-fetch    # build only claw-fetch (requires libcurl-dev)
#   make claw-gateway  # build only claw-gateway (requires libcurl-dev)
#   make claw-channel  # build only claw-channel (requires libcurl-dev)
#   make claw-cron     # build only claw-cron
#   make claw-daemon   # build only claw-daemon
#   make claw-tui      # build only claw-tui (requires libcurl-dev)
#   make claw-log      # build only claw-log
#   make claw-mem      # build only claw-mem
#   make claw-plugin   # build only claw-plugin
#   make claw-term     # build only claw-term
#   make claw-md       # build only claw-md
#   make claw-link     # build only claw-link (requires libcurl-dev)
#   make claw-tts      # build only claw-tts
#   make claw-browser  # build only claw-browser
#   make claw-media    # build only claw-media
#   make claw-canvas   # build only claw-canvas
#   make claw-mu       # build only claw-mu (requires libcurl-dev)
#   make claw-pair     # build only claw-pair
#
# Override CC or CFLAGS as needed:
#   make CC=musl-gcc CFLAGS="-Os -static"

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
BINDIR  := bin

# libcurl for claw-fetch, claw-channel, claw-gateway, claw-tui, claw-link, claw-mu
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs   libcurl 2>/dev/null || echo -lcurl)

BINS := $(BINDIR)/claw-shell \
        $(BINDIR)/claw-fs \
        $(BINDIR)/claw-fetch \
        $(BINDIR)/claw-gateway \
        $(BINDIR)/claw-channel \
        $(BINDIR)/claw-cron \
        $(BINDIR)/claw-daemon \
        $(BINDIR)/claw-tui \
        $(BINDIR)/claw-log \
        $(BINDIR)/claw-mem \
        $(BINDIR)/claw-plugin \
        $(BINDIR)/claw-term \
        $(BINDIR)/claw-md \
        $(BINDIR)/claw-link \
        $(BINDIR)/claw-tts \
        $(BINDIR)/claw-browser \
        $(BINDIR)/claw-media \
        $(BINDIR)/claw-canvas \
        $(BINDIR)/claw-mu \
        $(BINDIR)/claw-pair

.PHONY: all clean

all: $(BINDIR) $(BINS)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/claw-shell: claw-shell/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-shell/main.c

$(BINDIR)/claw-fs: claw-fs/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-fs/main.c

$(BINDIR)/claw-fetch: claw-fetch/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-fetch/main.c $(CURL_LIBS)

$(BINDIR)/claw-gateway: claw-gateway/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-gateway/main.c $(CURL_LIBS)

$(BINDIR)/claw-channel: claw-channel/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-channel/main.c $(CURL_LIBS)

$(BINDIR)/claw-cron: claw-cron/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-cron/main.c

$(BINDIR)/claw-daemon: claw-daemon/main.c
	$(CC) $(CFLAGS) -o $@ claw-daemon/main.c

$(BINDIR)/claw-tui: claw-tui/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-tui/main.c $(CURL_LIBS)

$(BINDIR)/claw-log: claw-log/main.c
	$(CC) $(CFLAGS) -o $@ claw-log/main.c

$(BINDIR)/claw-mem: claw-mem/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-mem/main.c

$(BINDIR)/claw-plugin: claw-plugin/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-plugin/main.c

$(BINDIR)/claw-term: claw-term/main.c
	$(CC) $(CFLAGS) -o $@ claw-term/main.c

$(BINDIR)/claw-md: claw-md/main.c
	$(CC) $(CFLAGS) -o $@ claw-md/main.c

$(BINDIR)/claw-link: claw-link/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-link/main.c $(CURL_LIBS)

$(BINDIR)/claw-tts: claw-tts/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-tts/main.c

$(BINDIR)/claw-browser: claw-browser/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-browser/main.c

$(BINDIR)/claw-media: claw-media/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-media/main.c

$(BINDIR)/claw-canvas: claw-canvas/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-canvas/main.c

$(BINDIR)/claw-mu: claw-mu/main.c common/claw_json.h
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -Icommon -o $@ claw-mu/main.c $(CURL_LIBS)

$(BINDIR)/claw-pair: claw-pair/main.c common/claw_json.h
	$(CC) $(CFLAGS) -Icommon -o $@ claw-pair/main.c

clean:
	rm -rf $(BINDIR)
