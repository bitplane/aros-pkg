/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * What pkg shows while it works, gone through case by case, with no network
 * and no large file: the work is pretended, the drawing is the real one.
 *
 *     make build/example-activity && ./build/example-activity
 *     ./build/example-activity 3         one case only
 *
 * docs/activity.md says what each case is expected to look like.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg_activity.h"
#include "pkg_style.h"
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nap(long ms)
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
}

/* The two places text goes, as the command itself wires them. */
static void write_styled(int is_error, const char *styled, const char *plain)
{
    (void)plain;
    fputs(styled, is_error ? stderr : stdout);
    fflush(is_error ? stderr : stdout);
}

static void show(void *user, const char *text)
{
    (void)user;
    pkg_style_line(write_styled, PKG_LINE_PROGRESS, 0, text);
}

static void say(void *user, const char *text)
{
    (void)user;
    pkg_style_line(write_styled, PKG_LINE_NOTE, 0, text);
}

static void result(const char *text)
{
    pkg_style_line(write_styled, PKG_LINE_RESULT, 0, text);
}

static void title(int n, const char *what)
{
    printf("\n%d. %s\n", n, what);
    fflush(stdout);
}

/* 1. A size known in advance, in bytes: the line is there from the first
 *    byte, the figure climbs from 0, and a rate and the time left join it. */
static void bytes_known(void)
{
    long long done, total = 637ll << 20;
    title(1, "a file of a known size: the figure from 0, then a rate and the time left");
    pkg_activity_step("reading the archive", "contrib.tar.bz2", total, PKG_ACTIVITY_BYTES, NULL);
    for (done = 0; done <= total; done += 3 << 20) {
        pkg_activity_bytes(done, total);
        nap(20);
    }
    pkg_activity_done();
    result("installed zip 1.0: 3 files");
}

/* 2. A count known in advance. */
static void count_known(void)
{
    unsigned i, n = 208;
    title(2, "a number of things known in advance");
    pkg_activity_step("checking", "contrib-nightly", (long long)n, PKG_ACTIVITY_THINGS, "version");
    for (i = 0; i <= n; i++) {
        pkg_activity_count(i, n, NULL);
        nap(15);
    }
    pkg_activity_done();
    result("208 versions, all of them sound");
}

/* 3. Nothing to measure: the mark, after half a second, pulsing. */
static void unknown_length(void)
{
    int i;
    title(3, "nothing to measure: half a second of silence, then the mark pulses");
    pkg_activity_step("waiting for", "aros-pkg.azurewebsites.net", 0, PKG_ACTIVITY_NOTHING, NULL);
    for (i = 0; i < 60; i++) {
        pkg_activity_waiting("aros-pkg.azurewebsites.net");
        nap(100);
    }
    pkg_activity_done();
    result("the channel answered");
}

/* 4. Quicker than half a second and of unknown size: not a word. */
static void quick(void)
{
    int i;
    title(4, "a quick step of unknown size: nothing is shown at all");
    pkg_activity_step("waiting for", "127.0.0.1", 0, PKG_ACTIVITY_NOTHING, NULL);
    for (i = 0; i < 3; i++) {
        pkg_activity_waiting("127.0.0.1");
        nap(100);
    }
    pkg_activity_done();
    result("done in a third of a second");
}

/* 5. A long step made of small ones: each small one draws over the line and
 *    gives it back, so the count is never lost. */
static void nested(void)
{
    unsigned i, n = 12;
    title(5, "small steps inside a long one: the count comes back after each");
    pkg_activity_step("checking", "pkg", (long long)n, PKG_ACTIVITY_THINGS, "version");
    for (i = 0; i < n; i++) {
        long long got, size = 800 << 10;
        pkg_activity_count(i, n, NULL);
        nap(250);
        pkg_activity_step("downloading", "395a2fb60299.pkg", size, PKG_ACTIVITY_BYTES, NULL);
        for (got = 0; got <= size; got += 40 << 10) {
            pkg_activity_bytes(got, size);
            nap(25);
        }
        pkg_activity_done();
    }
    pkg_activity_count(n, n, NULL);
    pkg_activity_done();
    result("12 versions checked");
}

/* 6. A wait in the middle of a measured step: the mark takes the line, and
 *    the figure takes it back when the bytes arrive. */
static void wait_then_bytes(void)
{
    long long got, size = 24ll << 20;
    int i;
    title(6, "a wait, then the bytes: the mark, then the figure");
    pkg_activity_step("downloading", "pkg-1.7.1.pkg", size, PKG_ACTIVITY_BYTES, NULL);
    for (i = 0; i < 25; i++) {
        pkg_activity_waiting("aros-pkg.azurewebsites.net");
        nap(100);
    }
    pkg_activity_waiting(NULL);
    for (got = 0; got <= size; got += 256 << 10) {
        pkg_activity_bytes(got, size);
        nap(30);
    }
    pkg_activity_done();
    result("downloaded pkg-1.7.1.pkg");
}

int main(int argc, char **argv)
{
    static void (*const cases[])(void) = {
        bytes_known, count_known, unknown_length, quick, nested, wait_then_bytes
    };
    int only = argc > 1 ? atoi(argv[1]) : 0, i;

    pkg_style_init(1, 1, 0);
    pkg_activity_charset(PKG_ACTIVITY_UTF8);
    pkg_activity_to(show, say, NULL);
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
        if (only == 0 || only == i + 1)
            cases[i]();
    pkg_activity_to(NULL, NULL, NULL);
    printf("\n");
    return 0;
}
