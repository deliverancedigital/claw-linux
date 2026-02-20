# src/Makefile — Build all claw-linux C skill binaries
#
# Usage:
#   make              # build all binaries into bin/
#   make clean        # remove build artifacts
#   make claw-shell   # build only claw-shell
#   make claw-fs      # build only claw-fs
#   make claw-fetch   # build only claw-fetch (requires libcurl-dev)
#
# Override CC or CFLAGS as needed:
#   make CC=musl-gcc CFLAGS="-Os -static"

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
BINDIR  := bin

# libcurl for claw-fetch
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs   libcurl 2>/dev/null || echo -lcurl)

BINS := $(BINDIR)/claw-shell $(BINDIR)/claw-fs $(BINDIR)/claw-fetch

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

clean:
	rm -rf $(BINDIR)
