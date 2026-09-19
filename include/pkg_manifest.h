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
 * Parsing is strict for what Pkg knows: a duplicate key, a missing required
 * key, an unsafe path or a malformed digest is a refusal, never a guess. A
 * key Pkg does not know is ignored, so that a package can carry lines for
 * other distribution systems: it stays in the signed text, is never acted
 * on, and its name is kept in `ignored` so that SHOW can say so. A change
 * Pkg must understand gets a new Format number, which older Pkg refuses.
 */

#ifndef PKG_MANIFEST_H
#define PKG_MANIFEST_H

#include "pkg_sha256.h"
#include <stddef.h>

struct pkg_file {
    char               *path;
    char                digest[PKG_SHA256_HEXLEN + 1];
    unsigned long long  size;
    unsigned long long  prot;       /* the AROS protection word; 0 is the default, rwed */
    char               *comment;    /* the AROS file comment, UTF-8; NULL for none */
    int                 config;     /* a configuration file: a person's edit survives upgrades */
};

struct pkg_dep {
    char *name;
    char *min;                      /* NULL: any version */
};

/* A list of strings, for the catalogue fields that repeat. */
struct pkg_strs {
    char  **v;
    size_t  n;
};

/* What a catalogue shows about a package: all optional, all signed, so a
 * change is a new version. Description and Changes are one line each,
 * joined in order; an empty line is a paragraph break. */
struct pkg_about {
    char           *short_desc;     /* "Short:", at most 40 characters */
    struct pkg_strs description;    /* "Description:" */
    char           *category;       /* "Category: type/sub", Aminet's types */
    struct pkg_strs tags;           /* "Tags: a, b": lowercase words */
    struct pkg_strs authors;        /* "Author:", one each; not the packager */
    char           *homepage;       /* "Homepage:", http or https */
    char           *repository;     /* "Repository:", http or https */
    char           *license;        /* "License:", an SPDX expression */
    char           *distribution;   /* "Distribution:", one of pkg_distributions */
    struct pkg_strs changes;        /* "Changes:", what this version changes */
    char           *icon;           /* "Icon:", a path of the package */
    struct pkg_strs screenshots;    /* "Screenshot:", paths of the package */
};

extern const char *const pkg_categories[];     /* Aminet's top-level types, NULL-ended */
extern const char *const pkg_distributions[];  /* NULL-ended */

/* Checks for the publish side, the same the parser applies: NULL if fine,
 * else why. */
const char *pkg_check_about_text(const char *field, const char *s, size_t max_chars);
const char *pkg_check_category(const char *s);
const char *pkg_check_tag(const char *s);
const char *pkg_check_url(const char *field, const char *s);
const char *pkg_check_license(const char *s);
const char *pkg_check_distribution(const char *s);
int pkg_strs_add(struct pkg_strs *l, const char *s);
void pkg_strs_free(struct pkg_strs *l);

struct pkg_manifest {
    char            *name;
    char            *version;
    char            *architecture;
    char            *kind;
    char            *payload;       /* NULL until the container exists */
    char            *source;        /* "<archive>!/<prefix>": the files come from an
                                       archive someone else published; no payload */
    char            *archive_sha;   /* "Archive:", with Source: where the archive is */
    unsigned long long archive_size;/*   published upstream: its SHA-256, its size, */
    char            *archive_url;   /*   and the http(s) URL it is downloaded from */
    struct pkg_file *files;
    size_t           nfiles;
    size_t           cap;
    struct pkg_dep  *deps;
    size_t           ndeps;
    struct pkg_file *content;       /* an image: the files inside it, "Content:" */
    size_t           ncontent;
    size_t           ccap;
    struct pkg_about about;         /* the catalogue fields */
    struct pkg_strs  ignored;       /* the keys Pkg does not know, each once */
    struct pkg_strs  provides;      /* "Provides: SDL2.library": the libraries and devices
                                       the package ships in Libs/ and Devs/, sorted */
};

/* "SDL2.library", "serial.device": NULL if the name can be a Provides line. */
const char *pkg_check_libname(const char *s);

void pkg_manifest_init(struct pkg_manifest *m);
void pkg_manifest_free(struct pkg_manifest *m);

/* Setters copy their argument. Return 0 on success, -1 on allocation failure. */
int pkg_manifest_set(char **field, const char *value);
int pkg_manifest_add_file(struct pkg_manifest *m, const char *path,
                          const char *digest_hex, unsigned long long size);
int pkg_manifest_add_content(struct pkg_manifest *m, const char *path,
                             const char *digest_hex, unsigned long long size);
/* An AROS file comment: at most 79 characters, all of them Latin-1, since
 * the comment an AROS file system stores is 79 bytes of it. */
#define PKG_COMMENT_MAX 79
/* The comment in Latin-1 bytes, NUL-terminated in out; its length, or -1
 * when it is not UTF-8, holds a control character or a character outside
 * Latin-1, or does not fit outsz. */
long pkg_comment_latin1(const char *utf8, char *out, size_t outsz);

/* The file or image content entry named path; NULL when there is none. */
struct pkg_file *pkg_manifest_attr_target(struct pkg_manifest *m, const char *path);
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
