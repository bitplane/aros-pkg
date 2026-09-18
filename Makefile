# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude -Ithird_party/bzip2

# Portable C99: everything except the host filesystem layer.
LIB  = src/pkg_lib.c
CORE = src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c \
       src/pkg_manifest.c src/pkg_image.c src/pkg_ameta.c src/pkg_archive.c src/pkg_bzip2.c
# The host layer. POSIX covers macOS and Linux; AROS gets its own.
HOST = src/pkg_fs_posix.c src/pkg_out.c src/pkg_port.c
HDR  = $(wildcard include/*.h)

UNITS = test_container test_sha256 test_manifest test_ed25519 test_image test_ameta

.PHONY: all test test-ubsan check-portability check-m68k check-image check check-aros clean install aros-channel

all: build/pkg

# Every check this tree knows how to run.
check: check-portability test test-ubsan check-m68k check-image

build/pkg: src/pkg_main.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(LIB) $(CORE) $(HOST)

# libpkg for other programs: a static library and pkg.h. The host layer is
# part of it, since every operation touches files.
build/libpkg.a: $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build/lib-obj
	@for f in $(LIB) $(CORE) $(HOST); do \
		$(CC) $(CFLAGS) $(CPPFLAGS) -c $$f -o build/lib-obj/$$(basename $$f .c).o || exit 1; \
	done
	ar rcs $@ build/lib-obj/*.o

# Install the tool, the library and the agents' skill: `make install`, or
# `make install PREFIX=/usr/local`. Nothing else is needed at run time.
PREFIX ?= $(HOME)/.local

install: build/pkg build/libpkg.a
	@mkdir -p $(PREFIX)/bin $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/share/pkg/skills/pkg
	cp build/pkg $(PREFIX)/bin/pkg
	cp include/pkg.h $(PREFIX)/include/pkg.h
	cp build/libpkg.a $(PREFIX)/lib/libpkg.a
	cp skills/pkg/SKILL.md $(PREFIX)/share/pkg/skills/pkg/SKILL.md
	@echo "installed pkg into $(PREFIX)/bin; if that is not on PATH, add it"

# A channel that puts Pkg itself on AROS machines: `make aros-channel
# CHANNEL=<dir>`. See tools/make-aros-channel.sh.
aros-channel: build/pkg
	sh tools/make-aros-channel.sh "$(CHANNEL)"

# The examples, built against libpkg.a as another program would.
build/example-%: examples/%.c build/libpkg.a include/pkg.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

# Windows, cross-built with mingw-w64. tools/make-windows-kit.sh wraps it in a
# test kit to run on a Windows machine.
WINCC ?= x86_64-w64-mingw32-gcc
WINHOST = src/pkg_fs_win32.c src/pkg_out.c src/pkg_port.c

build/pkg.exe: src/pkg_main.c $(LIB) $(CORE) $(WINHOST) $(HDR)
	@mkdir -p build
	$(WINCC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(LIB) $(CORE) $(WINHOST) \
		-lbcrypt -ladvapi32 -lshell32

# macOS, one universal binary for Apple silicon and Intel.
build/pkg-macos: src/pkg_main.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	cc $(CFLAGS) $(CPPFLAGS) -arch arm64 -arch x86_64 -o $@ src/pkg_main.c $(LIB) $(CORE) $(HOST)

# Linux, static against musl, cross-built with zig so no Linux toolchain is
# needed here.
build/pkg-linux-%: src/pkg_main.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	zig cc -target $*-linux-musl $(CFLAGS) $(CPPFLAGS) -static -o $@ src/pkg_main.c $(LIB) $(CORE) $(HOST)

# The library through pkg.h alone, as another program would use it.
build/test_api: tests/test_api.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_api.c $(LIB) $(CORE) $(HOST)

build/test_%: tests/test_%.c $(CORE) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CORE)

# `test` removes every binary first, deliberately. Editing a source and running
# the test inside the same second left a stale binary here once, and the test
# then reported the previous state of the code. A build this small buys nothing
# by being incremental, and a test that can report the wrong state is worse
# than a slow one.
test:
	@rm -f build/pkg build/test_api $(UNITS:%=build/%)
	@$(MAKE) --no-print-directory build/pkg build/test_api $(UNITS:%=build/%)
	@for t in $(UNITS) test_api; do echo "== $$t"; ./build/$$t || exit 1; done
	@echo "== e2e"
	@PKG=./build/pkg sh tests/e2e.sh
	@echo "== deps"
	@PKG=./build/pkg sh tests/deps.sh
	@echo "== crossarch"
	@PKG=./build/pkg sh tests/crossarch.sh
	@echo "== status"
	@PKG=./build/pkg sh tests/status.sh
	@echo "== network"
	@PKG=./build/pkg sh tests/network.sh
	@echo "== hostile"
	@PKG=./build/pkg sh tests/hostile.sh
	@echo "== archive"
	@rm -f build/test_archive
	@$(MAKE) --no-print-directory build/test_archive
	@sh tests/archive.sh
	@echo "== examples"
	@rm -f build/libpkg.a build/example-basic build/example-browse
	@$(MAKE) --no-print-directory build/example-basic build/example-browse
	@PKG=./build/pkg sh tests/examples.sh

# The image writer judged by amitools, an FFS written apart from it. Needs the
# amitools virtualenv described at the top of tests/image.sh.
check-image: build/pkg
	@PKG=./build/pkg sh tests/image.sh

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

# Unit tests and the host runs again, under the sanitizers. The
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
		-o build/san/test_api tests/test_api.c $(LIB) $(CORE) $(HOST)
	@./build/san/test_api > /dev/null || { echo "test-ubsan: test_api FAILED"; exit 1; }
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/pkg src/pkg_main.c $(LIB) $(CORE) $(HOST)
	@PKG=./build/san/pkg sh tests/e2e.sh > build/san/e2e.log 2>&1 \
		|| { tail -20 build/san/e2e.log; echo "test-ubsan: e2e FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/deps.sh > build/san/deps.log 2>&1 \
		|| { tail -20 build/san/deps.log; echo "test-ubsan: deps FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/status.sh > build/san/status.log 2>&1 \
		|| { tail -20 build/san/status.log; echo "test-ubsan: status FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/image.sh > build/san/image.log 2>&1 \
		|| { tail -20 build/san/image.log; echo "test-ubsan: image FAILED"; exit 1; }
	@echo "test-ubsan: PASS, units, e2e, deps, status and image under -fsanitize=undefined,address"

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
