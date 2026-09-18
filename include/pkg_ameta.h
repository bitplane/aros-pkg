/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* .ameta: the Amiga metadata of the entries of one host directory, one text
 * file per directory. The format is specified in the planning repository,
 * docs/features/file-metadata/ameta.md; tests/ameta-corpus holds its
 * reference cases. Portable C99, no file access: callers read and write. */

#ifndef PKG_AMETA_H
#define PKG_AMETA_H

#include <stddef.h>

#define PKG_AMETA_HOST_MASK     0xEE0Eull  /* bits a live host volume takes from the mode */
#define PKG_AMETA_RECORD_MASK   0xEE02ull  /* bits outside it are recorded */
#define PKG_AMETA_OWNER_EXECUTE 0x2ull

struct pkg_ameta_kv {
    char *key, *value;               /* an unknown key, kept as it was */
};

struct pkg_ameta_entry {
    unsigned char       *name;       /* raw bytes, unescaped */
    size_t               name_len;
    int                  has_prot;
    unsigned long long   prot;
    unsigned char       *comment;    /* raw bytes; NULL or empty: none */
    size_t               comment_len;
    int                  has_uid, has_gid;
    unsigned long        uid, gid;
    struct pkg_ameta_kv *unknown;
    size_t               nunknown;
    int                  stale;      /* names nothing present: ignored, dropped on write */
};

struct pkg_ameta_report {
    unsigned line;                   /* 0 for the file as a whole */
    char     reason[24];             /* malformed-escape, bad-number, prot-too-long,
                                        key-before-file, line-too-long, bad-name,
                                        not-utf8, duplicate, bad-header,
                                        file-too-large */
};

struct pkg_ameta {
    int                      usable; /* 0: ignored as a whole, see the report */
    struct pkg_ameta_entry  *e;
    size_t                   n, cap;
    struct pkg_ameta_report *r;
    size_t                   nr;
};

void pkg_ameta_init(struct pkg_ameta *a);
void pkg_ameta_free(struct pkg_ameta *a);

/* Parse a file's bytes. Returns 0, or -1 when out of memory. A file that is
 * not `ameta 1` leaves usable at 0; malformed lines are reported and
 * skipped, and the rest applies. */
int pkg_ameta_parse(const unsigned char *data, size_t len, struct pkg_ameta *a);

/* Mark the entries whose name present() does not find; returns how many. */
size_t pkg_ameta_mark_stale(struct pkg_ameta *a,
                            int (*present)(const unsigned char *name, size_t len, void *ctx),
                            void *ctx);

struct pkg_ameta_entry *pkg_ameta_find(struct pkg_ameta *a, const unsigned char *name,
                                       size_t len);
/* The entry for name, created empty when absent; NULL when out of memory. */
struct pkg_ameta_entry *pkg_ameta_get(struct pkg_ameta *a, const unsigned char *name,
                                      size_t len);
void pkg_ameta_delete(struct pkg_ameta *a, const unsigned char *name, size_t len);
int  pkg_ameta_rename(struct pkg_ameta *a, const unsigned char *old, size_t ol,
                      const unsigned char *nw, size_t nl);
int  pkg_ameta_set_comment(struct pkg_ameta_entry *e, const unsigned char *c, size_t len);

/* The canonical file for the non-stale entries, NUL-terminated: *out is
 * NULL (and 0 is returned) when the directory must have no .ameta. -1: out of memory. */
int pkg_ameta_emit(const struct pkg_ameta *a, char **out, size_t *len);

/* The protection word a publishing tool derives: owner Execute from the
 * host mode when the host has one, every other bit from prot, AROS
 * defaults (0) otherwise. e may be NULL. */
unsigned long long pkg_ameta_publish_word(const struct pkg_ameta_entry *e, int host_has_mode,
                                          int host_executable);

/* Percent escaping over bytes; out needs 3 * len + 1. Unescape returns the
 * length, or -1 on a malformed escape; out needs len bytes. */
void pkg_ameta_escape(const unsigned char *in, size_t len, char *out);
long pkg_ameta_unescape(const char *in, size_t len, unsigned char *out);

/* Parse a prot value ("0x" and 1 to 16 hex digits); NULL on success, else
 * the report reason. */
const char *pkg_ameta_parse_prot(const char *v, size_t len, unsigned long long *out);

#endif
