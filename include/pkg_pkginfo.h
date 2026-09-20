/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* `.pkginfo`: what a port says about the package it is. A port installs the
 * file with its documentation, and `PUBLISH ... INFO <file>` reads the name,
 * the version, the kind, the catalogue fields, the dependencies and which
 * installed paths belong to the package, so that packaging it needs no
 * hand-written table. tools/contrib/PKGINFO.md describes the format.
 *
 *   Format: pkginfo 1
 *   Name: mbedtls
 *   Version: 3.6.7
 *   Kind: sdk
 *   Files: Developer/lib/libmbedtls.a
 *   Description: Mbed TLS is a C library ...
 *
 * The syntax is the manifest's, so there is no second one: "Key: value" per
 * line, a blank line and a line starting with '#' ignored, and a key Pkg does
 * not know ignored too, so that a port may carry lines for its own tools
 * (Upstream-Archive, Port-Maintainer). What Pkg does know is checked with the
 * manifest's own checks, and a bad value is a refusal naming the line.
 */

#ifndef PKG_PKGINFO_H
#define PKG_PKGINFO_H

#include "pkg_manifest.h"

struct pkg_pkginfo {
    char            *name;
    char            *version;
    char            *kind;
    char            *depends;       /* "a >= 1.0, b", as PUBLISH DEPENDS takes it */
    struct pkg_about about;         /* the catalogue fields */
    struct pkg_strs  files;         /* the installed paths that make the package */
    struct pkg_strs  config;        /* those of them a person may edit */
    char            *files_line;    /* the same two, comma-separated, as FILES */
    char            *config_line;   /* and CONFIG take them */
};

void pkg_pkginfo_free(struct pkg_pkginfo *pi);

/* Parse `len` bytes of a .pkginfo. 0, or -1 with the reason in err (always
 * NUL-terminated) and the line it is on in *line, 0 when it is the file as a
 * whole. The caller frees *pi either way. */
int pkg_pkginfo_parse(const char *text, size_t len, struct pkg_pkginfo *pi,
                      char *err, size_t errlen, int *line);

#endif
