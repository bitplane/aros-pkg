# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude

SRC = src/pkg_container.c
HDR = include/pkg_container.h

.PHONY: all test test-ubsan check-portability check-m68k check clean

# Every check this tree knows how to run.
check: check-portability test test-ubsan check-m68k

all: build/test_container

# `test` removes the binary first, deliberately. Editing a source and running
# the test inside the same second left a stale binary here once, and the test
# then reported the previous state of the code. A build this small buys nothing
# by being incremental, and a test that can report the wrong state is worse
# than a slow one.
test:
	@rm -f build/test_container
	@$(MAKE) --no-print-directory build/test_container
	./build/test_container

build/test_container: tests/test_container.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_container.c $(SRC)

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

# The misaligned-buffer test is the point of this one: a struct mapped over the
# stream would pass the plain build and fail here, the way it would fault on a
# 68000.
test-ubsan:
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O1 -g \
		-fsanitize=undefined,address -fno-omit-frame-pointer \
		$(CPPFLAGS) -o build/test_ubsan tests/test_container.c $(SRC)
	./build/test_ubsan

# A big-endian COMPILE. Running on a big-endian target is separate work and is
# not claimed here.
M68K_CC ?= $(HOME)/aros-m68k-build/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc

check-m68k:
	@if [ -x "$(M68K_CC)" ]; then \
		mkdir -p build; \
		$(M68K_CC) -std=c99 -Wall -Wextra -Werror -O2 $(CPPFLAGS) \
			-c $(SRC) -o build/pkg_container.m68k.o && \
		echo "check-m68k: PASS, builds for a big-endian target"; \
	else \
		echo "check-m68k: SKIP, no m68k compiler at $(M68K_CC)"; \
	fi

clean:
	rm -rf build
