/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "pkg_activity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static long long clock_ms = 1000;
static char line[512];
static int draws;
long long pkg_fs_now_ms(void) { return clock_ms; }
static void show(void *u, const char *s)
{
    (void)u;
    snprintf(line, sizeof line, "%s", s);
    draws++;
}
int main(void)
{
    int before, i;
    char first[512];
    pkg_activity_to(show, NULL, NULL);
    pkg_activity_step("downloading", "archive", 456ll << 20, PKG_ACTIVITY_BYTES, NULL);
    assert(strstr(line, "downloading archive, 0.0/456 MB"));
    assert(!strstr(line, "(O)"));
    pkg_activity_done();
    assert(!line[0]);

    before = draws;
    pkg_activity_step("checking", "channel", 0, PKG_ACTIVITY_NOTHING, NULL);
    clock_ms += 499;
    pkg_activity_tick();
    assert(draws == before);
    clock_ms++;
    pkg_activity_tick();
    assert(strstr(line, "checking channel"));
    strcpy(first, line);
    clock_ms += 125;
    pkg_activity_tick();
    assert(strcmp(first, line));
    pkg_activity_done();
    assert(!line[0]);

    pkg_activity_step("downloading", "archive", 0, PKG_ACTIVITY_NOTHING, NULL);
    before = draws;
    pkg_activity_bytes(0, 456ll << 20);
    assert(draws == before + 1);
    assert(strstr(line, "0.0/456 MB"));
    clock_ms += 500;
    pkg_activity_bytes(1ll << 20, 456ll << 20);
    strcpy(first, line);
    pkg_activity_step("child", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    pkg_activity_done();
    assert(!strcmp(first, line));
    pkg_activity_done();

    pkg_activity_step("outer", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    pkg_activity_step("inner", NULL, 20, PKG_ACTIVITY_THINGS, NULL);
    assert(strstr(line, "0/20"));
    pkg_activity_done();
    assert(!line[0]);
    clock_ms += 500;
    pkg_activity_tick();
    assert(strstr(line, "outer"));
    pkg_activity_done();

    pkg_activity_step("outer", NULL, 20, PKG_ACTIVITY_THINGS, NULL);
    for (i = 0; i < 40; i++) pkg_activity_step("inner", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    for (i = 0; i < 40; i++) pkg_activity_done();
    assert(strstr(line, "outer, 0/20"));
    pkg_activity_step("middle", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    pkg_activity_step("inner", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    pkg_activity_done();
    assert(strstr(line, "outer, 0/20"));
    pkg_activity_done();
    pkg_activity_to(NULL, NULL, NULL);
    assert(!line[0]);
    pkg_activity_to(show, NULL, NULL);
    pkg_activity_step("fresh", NULL, 1, PKG_ACTIVITY_THINGS, NULL);
    pkg_activity_done();
    assert(!line[0]);
    pkg_activity_step("uploading", NULL, 0, PKG_ACTIVITY_NOTHING, NULL);
    pkg_activity_count(1, 10, NULL);
    strcpy(first, line);
    clock_ms += 125;
    pkg_activity_tick();
    assert(!strcmp(first, line));
    pkg_activity_done();
    puts("activity state tests passed");
    return 0;
}
