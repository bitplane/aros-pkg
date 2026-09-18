/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* The archive reader, as tests/archive.sh drives it:
 *   test_archive <archive>          every file: "<size> <mode-octal> <path>"
 *   test_archive <archive> <path>   that file's bytes on stdout
 * Exit 1 with the reason on stderr when the archive is refused. */

#include "pkg_archive.h"

#include <stdio.h>
#include <string.h>

static const char *target;
static int found;

static int want(const struct pkg_archive_entry *e, void *ctx)
{
    (void)ctx;
    if (target == NULL) {
        if (!e->is_dir) printf("%llu %o %s\n", e->size, e->mode, e->path);
        return 0;
    }
    if (strcmp(e->path, target) == 0) { found = 1; return 1; }
    return 0;
}

static int data(const struct pkg_archive_entry *e, const unsigned char *b, size_t n, void *ctx)
{
    (void)e; (void)ctx;
    fwrite(b, 1, n, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    char err[300];
    int rc;
    if (argc < 2) return 2;
    target = argc > 2 ? argv[2] : NULL;
    rc = pkg_archive_walk(argv[1], want, data, NULL, err, sizeof err);
    if (rc != 0) { fprintf(stderr, "refused: %s\n", err); return 1; }
    if (target && !found) { fprintf(stderr, "not found\n"); return 1; }
    return 0;
}
