/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The package manifest: text, one "Key: value" per line, in the manner of an
 * Aminet .readme header so that the two convert without a second syntax.
 *
 *   Format: pkg-manifest 1
 *   Name: hello
 *   Version: 1.2
 *   Architecture: generic
 *   Kind: application
 *   Depends: hello-lib >= 1.0
 *   Payload: <sha256 of the .pkg container, hex>
 *   File: <sha256 hex> <size> <path>
 *
 * File lines are sorted by path, byte order, so that the same drawer always
 * produces the same manifest. Paths are relative to the root the package is
 * installed into, '/'-separated, and may contain spaces: the path is the rest
 * of the line after the size.
 *
 * Depends is optional and repeatable: a package name, alone or followed by
 * ">= <version>", the lowest version that will do. Depends lines are sorted by
 * name, name each package once, and never the package itself.
 *
 * Parsing is strict. An unknown key, a duplicate key, a missing required key,
 * an unsafe path or a malformed digest is a refusal, never a guess.
 */

#ifndef PKG_MANIFEST_H
#define PKG_MANIFEST_H

#include "pkg_sha256.h"
#include <stddef.h>

struct pkg_file {
    char               *path;
    char                digest[PKG_SHA256_HEXLEN + 1];
    unsigned long long  size;
};

struct pkg_dep {
    char *name;
    char *min;                      /* NULL: any version */
};

struct pkg_manifest {
    char            *name;
    char            *version;
    char            *architecture;
    char            *kind;
    char            *payload;       /* NULL until the container exists */
    struct pkg_file *files;
    size_t           nfiles;
    size_t           cap;
    struct pkg_dep  *deps;
    size_t           ndeps;
};

void pkg_manifest_init(struct pkg_manifest *m);
void pkg_manifest_free(struct pkg_manifest *m);

/* Setters copy their argument. Return 0 on success, -1 on allocation failure. */
int pkg_manifest_set(char **field, const char *value);
int pkg_manifest_add_file(struct pkg_manifest *m, const char *path,
                          const char *digest_hex, unsigned long long size);
void pkg_manifest_sort(struct pkg_manifest *m);   /* files by path, deps by name */

/* Add a dependency; min may be NULL. Both are copied. 0, or -1 on allocation
 * failure. Validity is checked by pkg_check_deps once all are added. */
int pkg_manifest_add_dep(struct pkg_manifest *m, const char *name, const char *min);

/* Parse "name" or "name >= version" into the two buffers; min becomes "" when
 * absent. NULL when well formed, or why not. */
const char *pkg_parse_dep(const char *text, char *name, size_t name_len,
                          char *min, size_t min_len);

/* Sorted, unique, and not the package itself. NULL, or why not. */
const char *pkg_check_deps(const struct pkg_manifest *m);

/* Caller frees *out. Return 0, or -1 on allocation failure. */
int pkg_manifest_emit(const struct pkg_manifest *m, char **out, size_t *out_len);

/* Return 0, or -1 with a reason in err (always NUL-terminated). */
int pkg_manifest_parse(const char *text, size_t len, struct pkg_manifest *m,
                       char *err, size_t errlen);

/* Validation, shared by publish and install. Each returns NULL when the value
 * is acceptable, or a static string saying why it is not. */
const char *pkg_check_path(const char *path);
const char *pkg_check_name(const char *name);
const char *pkg_check_version(const char *version);
const char *pkg_check_arch(const char *arch);
const char *pkg_check_kind(const char *kind);

/* Dotted-numeric comparison, missing components read as zero, so 1.2 equals
 * 1.2.0. Both arguments must have passed pkg_check_version. */
int pkg_version_cmp(const char *a, const char *b);

#endif
