# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude

SRC = src/pkg_container.c
HDR = include/pkg_container.h

.PHONY: all test clean

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

clean:
	rm -rf build
