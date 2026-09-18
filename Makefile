# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude

# Portable C99: everything except the host filesystem layer.
CORE = src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c \
       src/pkg_manifest.c
# The host layer. POSIX covers macOS and Linux; AROS gets its own.
HOST = src/pkg_fs_posix.c src/pkg_out.c
HDR  = $(wildcard include/*.h)

UNITS = test_container test_sha256 test_manifest test_ed25519

.PHONY: all test test-ubsan check-portability check-m68k check check-aros clean

all: build/pkg

# Every check this tree knows how to run.
check: check-portability test test-ubsan check-m68k

build/pkg: src/pkg_main.c $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(CORE) $(HOST)

build/test_%: tests/test_%.c $(CORE) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CORE)

# `test` removes every binary first, deliberately. Editing a source and running
# the test inside the same second left a stale binary here once, and the test
# then reported the previous state of the code. A build this small buys nothing
# by being incremental, and a test that can report the wrong state is worse
# than a slow one.
test:
	@rm -f build/pkg $(UNITS:%=build/%)
	@$(MAKE) --no-print-directory build/pkg $(UNITS:%=build/%)
	@for t in $(UNITS); do echo "== $$t"; ./build/$$t || exit 1; done
	@echo "== e2e"
	@PKG=./build/pkg sh tests/e2e.sh

# Byte order is expressed in pkg_be32_get and pkg_be32_put and nowhere else.
# This refuses the constructs that would quietly reintroduce a host-order
# dependency. Verified to be able to fail: adding one of these to a source
# makes it report that file and exit non-zero.
FORBIDDEN = (hton[sl]|ntoh[sl]|__bswap|__builtin_bswap|BYTE_ORDER|BIG_END|LITTLE_END)

check-portability:
	@if grep -rnE '$(FORBIDDEN)' src include tests; then \
		echo "check-portability: FAIL, host byte order reached the sources"; \
		exit 1; \
	else \
		echo "check-portability: PASS, byte order lives only in the accessors"; \
	fi

# Unit tests and the end-to-end run again, under the sanitizers. The
# misaligned-buffer test is the point for the container: a struct mapped over
# the stream would pass the plain build and fail here, the way it would fault
# on a 68000.
SAN = -O1 -g -fsanitize=undefined,address -fno-omit-frame-pointer

test-ubsan:
	@mkdir -p build/san
	@for t in $(UNITS); do \
		$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
			-o build/san/$$t tests/$$t.c $(CORE) || exit 1; \
		./build/san/$$t > /dev/null || { echo "test-ubsan: $$t FAILED"; exit 1; }; \
	done
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/pkg src/pkg_main.c $(CORE) $(HOST)
	@PKG=./build/san/pkg sh tests/e2e.sh > build/san/e2e.log 2>&1 \
		|| { tail -20 build/san/e2e.log; echo "test-ubsan: e2e FAILED"; exit 1; }
	@echo "test-ubsan: PASS, units and e2e under -fsanitize=undefined,address"

# A big-endian COMPILE of the portable core. Running on a big-endian target is
# separate work and is not claimed here.
M68K_CC ?= $(HOME)/aros-m68k-build/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc

check-m68k:
	@if [ -x "$(M68K_CC)" ]; then \
		mkdir -p build/m68k; \
		for f in $(CORE); do \
			$(M68K_CC) -std=c99 -Wall -Wextra -Werror -O2 $(CPPFLAGS) \
				-c $$f -o build/m68k/$$(basename $$f .c).o || exit 1; \
		done; \
		echo "check-m68k: PASS, the portable core builds for a big-endian target"; \
	else \
		echo "check-m68k: SKIP, no m68k compiler at $(M68K_CC)"; \
	fi

# The client on hosted AROS: needs the AROS build under ~/aros-build, the
# crosstools under ~/aros-crosstools, and no hosted instance already running.
# Kept out of `check` because it boots an operating system.
check-aros: build/pkg
	@sh tools/build-aros.sh
	@sh tests/aros-smoke.sh

clean:
	rm -rf build
