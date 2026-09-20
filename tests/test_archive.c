/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* The archive reader, as tests/archive.sh and tests/fastarchive.sh drive it:
 *   test_archive <archive>                  every file: "<size> <mode-octal> <path>"
 *   test_archive <archive> <path>           that file's bytes on stdout
 *   test_archive MAP <archive> <mapfile>    walk it and write the block map
 *   test_archive READ <archive> <mapfile> <path>[,<path>...]
 *                                           those files' bytes, from the map alone
 * Exit 1 with the reason on stderr when the archive is refused, 3 when the
 * map cannot serve the read and the caller would walk instead. */

#include "pkg_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *target;
static int found;

static int wanted(const char *path)
{
    const char *p = target;
    size_t n = strlen(path);
    while (p != NULL && *p) {
        const char *end = strchr(p, ',');
        size_t k = end ? (size_t)(end - p) : strlen(p);
        if (k == n && memcmp(p, path, n) == 0) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static int want(const struct pkg_archive_entry *e, void *ctx)
{
    (void)ctx;
    if (target == NULL) {
        if (!e->is_dir) printf("%llu %o %s\n", e->size, e->mode, e->path);
        return 0;
    }
    if (!e->is_dir && wanted(e->path)) { found++; return 1; }
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
    if (strcmp(argv[1], "MAP") == 0) {
        char *map = NULL;
        FILE *out;
        if (argc < 4) return 2;
        target = "";                       /* a silent walk: only the map is wanted */
        rc = pkg_archive_walk_map(argv[2], want, data, NULL, &map, err, sizeof err);
        if (rc != 0) { fprintf(stderr, "refused: %s\n", err); return 1; }
        if (map == NULL) { fprintf(stderr, "no map\n"); return 1; }
        out = fopen(argv[3], "wb");
        if (out == NULL) return 2;
        fputs(map, out);
        fclose(out);
        free(map);
        return 0;
    }
    if (strcmp(argv[1], "READ") == 0) {
        char *map;
        long len;
        FILE *in;
        if (argc < 5) return 2;
        in = fopen(argv[3], "rb");
        if (in == NULL) { fprintf(stderr, "no map file\n"); return 3; }
        fseek(in, 0, SEEK_END);
        len = ftell(in);
        rewind(in);
        map = (char *)malloc((size_t)len + 1);
        if (map == NULL || fread(map, 1, (size_t)len, in) != (size_t)len) { free(map); fclose(in); return 2; }
        map[len] = '\0';
        fclose(in);
        target = argv[4];
        rc = pkg_archive_read_mapped(argv[2], map, want, data, NULL, err, sizeof err);
        free(map);
        if (rc != 0) { fprintf(stderr, "map not used\n"); return 3; }
        return 0;
    }
    target = argc > 2 ? argv[2] : NULL;
    rc = pkg_archive_walk(argv[1], want, data, NULL, err, sizeof err);
    if (rc != 0) { fprintf(stderr, "refused: %s\n", err); return 1; }
    if (target && !found) { fprintf(stderr, "not found\n"); return 1; }
    return 0;
}
