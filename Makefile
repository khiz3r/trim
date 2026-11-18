CC       ?= cc
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion
# Fastest, but the binary may SIGILL on older CPUs than the build machine.
CFLAGS   ?= -O3 -march=native -std=gnu11 -D_FILE_OFFSET_BITS=64 $(WARN)
# Portable build: same speed class, runs on any x86-64 / aarch64.
PORTABLE := -O3 -std=gnu11 -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=2 $(WARN)
PREFIX   ?= /usr/local

all: trim

trim: trim.c
	$(CC) $(CFLAGS) -o $@ $<

portable: trim.c
	$(CC) $(PORTABLE) -o trim $<

# Tiny buffers force every chunk-boundary code path; used by the fuzzer.
trim_small: trim.c
	$(CC) -O1 -g -std=gnu11 -DBUF_SIZE=5 -DOBUF_SIZE=7 $(WARN) -fsanitize=address,undefined -o $@ $<

test: trim trim_small
	python3 tests/fuzz.py ./trim ./trim_small

install: trim
	install -D -m 0755 trim $(DESTDIR)$(PREFIX)/bin/trim

clean:
	rm -f trim trim_small

.PHONY: all portable test install clean
