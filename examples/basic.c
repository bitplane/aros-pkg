/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * basic: the shortest useful program on libpkg. It makes a signing key,
 * publishes a one-file drawer into a channel, installs it into a root,
 * lists the root, and shows how a refusal reads. Everything happens in a
 * directory given on the command line, which must not exist yet.
 *
 *   cc -Iinclude examples/basic.c build/libpkg.a -o basic
 *   ./basic /tmp/pkg-basic
 *
 * Three things to take from it:
 *   - every operation takes a sink and a pkg_options, and returns 0 or the
 *     class of its refusal;
 *   - with `structured` set, the answer arrives as key/value records, the
 *     same ones `pkg ... MACHINE` prints;
 *   - a refusal carries `next`, what to do now; pkg_next_words says it in
 *     words for a person.
 * examples/browse.c goes further: items with fields apart, suggestions,
 * cancelling, the trace.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The sink: print each record, and keep the two a refusal needs. */
struct last { char reason[512], next[32]; };

static void record(void *user, const char *key, const char *value)
{
    struct last *l = (struct last *)user;
    printf("    %s: %s\n", key, value);
    if (strcmp(key, "reason") == 0) snprintf(l->reason, sizeof l->reason, "%s", value);
    if (strcmp(key, "next") == 0)   snprintf(l->next, sizeof l->next, "%s", value);
}

static int step(const char *what, int rc, const struct last *l)
{
    if (rc == PKG_RC_OK)
        printf("  %s: done\n", what);
    else
        printf("  %s: refused, %s (%d)\n    what now: %s\n", what, pkg_class_name(rc), rc,
               pkg_next_words(l->next));
    return rc;
}

int main(int argc, char **argv)
{
    char key[512], drawer[512], channel[512], root[512], file[600];
    struct last last;
    struct pkg_sink sink;
    struct pkg_options o;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: basic <new directory>\n");
        return PKG_RC_USAGE;
    }
    if (mkdir(argv[1], 0700) != 0) {
        fprintf(stderr, "basic: %s must not exist yet\n", argv[1]);
        return PKG_RC_USAGE;
    }
    snprintf(key, sizeof key, "%s/publisher.key", argv[1]);
    snprintf(drawer, sizeof drawer, "%s/drawer", argv[1]);
    snprintf(channel, sizeof channel, "%s/channel", argv[1]);
    snprintf(root, sizeof root, "%s/root", argv[1]);

    /* A drawer laid out as it installs: one command in C/. */
    mkdir(drawer, 0755);
    snprintf(file, sizeof file, "%s/C", drawer);
    mkdir(file, 0755);
    snprintf(file, sizeof file, "%s/C/Hello", drawer);
    if ((f = fopen(file, "wb")) == NULL) return PKG_RC_IO;
    fputs("hello\n", f);
    fclose(f);

    memset(&sink, 0, sizeof sink);
    sink.record = record;
    sink.user = &last;
    sink.structured = 1;

    printf("keygen\n");
    memset(&o, 0, sizeof o);
    o.file = key;
    if (step("keygen", pkg_keygen(&sink, &o), &last) != 0) return 1;

    printf("publish\n");
    memset(&o, 0, sizeof o);
    o.target = drawer;
    o.channel = channel;
    o.sign = key;
    o.name = "hello";
    o.version = "1.0";
    o.kind = "application";
    if (step("publish", pkg_publish(&sink, &o), &last) != 0) return 1;

    printf("install\n");
    memset(&o, 0, sizeof o);
    o.target = "hello";
    o.root = root;
    o.channel = channel;
    if (step("install", pkg_install(&sink, &o), &last) != 0) return 1;

    printf("list\n");
    memset(&o, 0, sizeof o);
    o.root = root;
    if (step("list", pkg_list(&sink, &o), &last) != 0) return 1;

    /* A refusal, on purpose: a name the channel does not have. */
    printf("a refusal\n");
    memset(&o, 0, sizeof o);
    o.target = "helo";
    o.root = root;
    o.channel = channel;
    memset(&last, 0, sizeof last);
    if (step("install helo", pkg_install(&sink, &o), &last) != PKG_RC_NOTFOUND) return 1;
    return 0;
}
