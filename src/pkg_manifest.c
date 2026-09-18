/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_manifest.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

void pkg_manifest_init(struct pkg_manifest *m)
{
    memset(m, 0, sizeof *m);
}

void pkg_manifest_free(struct pkg_manifest *m)
{
    size_t i;
    free(m->name); free(m->version); free(m->architecture);
    free(m->kind); free(m->payload);
    for (i = 0; i < m->nfiles; i++)
        free(m->files[i].path);
    free(m->files);
    pkg_manifest_init(m);
}

int pkg_manifest_set(char **field, const char *value)
{
    char *p = dupstr(value);
    if (p == NULL)
        return -1;
    free(*field);
    *field = p;
    return 0;
}

int pkg_manifest_add_file(struct pkg_manifest *m, const char *path,
                          const char *digest_hex, unsigned long long size)
{
    struct pkg_file *f;
    if (m->nfiles == m->cap) {
        size_t ncap = m->cap ? m->cap * 2u : 16u;
        struct pkg_file *g = (struct pkg_file *)realloc(m->files, ncap * sizeof *g);
        if (g == NULL)
            return -1;
        m->files = g;
        m->cap = ncap;
    }
    f = &m->files[m->nfiles];
    f->path = dupstr(path);
    if (f->path == NULL)
        return -1;
    memcpy(f->digest, digest_hex, PKG_SHA256_HEXLEN);
    f->digest[PKG_SHA256_HEXLEN] = '\0';
    f->size = size;
    m->nfiles++;
    return 0;
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const struct pkg_file *)a)->path,
                  ((const struct pkg_file *)b)->path);
}

void pkg_manifest_sort(struct pkg_manifest *m)
{
    if (m->nfiles > 1u)
        qsort(m->files, m->nfiles, sizeof m->files[0], by_path);
}

/* ---- validation ------------------------------------------------------- */

const char *pkg_check_path(const char *p)
{
    const char *seg;
    size_t n;

    if (p == NULL || *p == '\0')
        return "the path is empty";
    if (*p == '/')
        return "the path is absolute";
    for (seg = p; *seg; seg++) {
        unsigned char ch = (unsigned char)*seg;
        if (ch < 0x20u || ch == 0x7Fu)
            return "the path contains a control character";
        if (ch == '\\')
            return "the path contains a backslash";
        if (ch == ':')
            return "the path names a volume or assign, which a package may not do yet";
    }
    n = strlen(p);
    if (p[n - 1u] == '/')
        return "the path ends with a separator";

    seg = p;
    for (;;) {
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        if (len == 0u)
            return "the path contains an empty component";
        if ((len == 1u && seg[0] == '.') || (len == 2u && seg[0] == '.' && seg[1] == '.'))
            return "the path contains a '.' or '..' component";
        if (seg == p && len == 4u && memcmp(seg, ".pkg", 4) == 0)
            return "the path is inside .pkg, which holds the package database";
        if (end == NULL)
            break;
        seg = end + 1;
    }
    return NULL;
}

const char *pkg_check_name(const char *s)
{
    size_t i, n;
    if (s == NULL || *s == '\0')
        return "the name is empty";
    n = strlen(s);
    if (n > 64u)
        return "the name is longer than 64 characters";
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
        return "the name must start with a lowercase letter or a digit";
    for (i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '+' || c == '.' || c == '_' || c == '-'))
            return "the name may contain only a-z, 0-9, '+', '.', '_' and '-'";
    }
    return NULL;
}

const char *pkg_check_version(const char *s)
{
    size_t digits = 0;
    if (s == NULL || *s == '\0')
        return "the version is empty";
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            if (++digits > 9u)
                return "a version component is longer than 9 digits";
        } else if (*s == '.') {
            if (digits == 0u)
                return "the version has an empty component";
            digits = 0;
        } else {
            return "the version must be dotted numbers, such as 40.1 or 1.2.3";
        }
    }
    if (digits == 0u)
        return "the version ends with a separator";
    return NULL;
}

const char *pkg_check_arch(const char *s)
{
    if (s == NULL || *s == '\0')
        return "the architecture is empty";
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')
              || *s == '-' || *s == '_' || *s == '.'))
            return "the architecture may contain only a-z, 0-9, '-', '_' and '.'";
    return NULL;
}

const char *pkg_check_kind(const char *s)
{
    static const char *const kinds[] = {
        "application", "library", "device", "boot", "class", "font",
        "catalog", "startup", "data", "slave", "sdk"
    };
    size_t i;
    if (s == NULL || *s == '\0')
        return "the kind is empty";
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (strcmp(s, kinds[i]) == 0)
            return NULL;
    return "unknown kind; expected one of application, library, device, boot, "
           "class, font, catalog, startup, data, slave, sdk";
}

int pkg_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        unsigned long x = 0, y = 0;
        while (*a >= '0' && *a <= '9') x = x * 10u + (unsigned long)(*a++ - '0');
        while (*b >= '0' && *b <= '9') y = y * 10u + (unsigned long)(*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* ---- emit ------------------------------------------------------------- */

struct sb { char *p; size_t len, cap; int bad; };

static void sb_printf(struct sb *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    if (b->bad)
        return;
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { b->bad = 1; return; }
    if (b->len + (size_t)need + 1u > b->cap) {
        size_t ncap = b->cap ? b->cap : 256u;
        char *q;
        while (ncap < b->len + (size_t)need + 1u)
            ncap *= 2u;
        q = (char *)realloc(b->p, ncap);
        if (q == NULL) { b->bad = 1; return; }
        b->p = q;
        b->cap = ncap;
    }
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

int pkg_manifest_emit(const struct pkg_manifest *m, char **out, size_t *out_len)
{
    struct sb b = { NULL, 0, 0, 0 };
    size_t i;

    sb_printf(&b, "Format: pkg-manifest 1\n");
    sb_printf(&b, "Name: %s\n", m->name);
    sb_printf(&b, "Version: %s\n", m->version);
    sb_printf(&b, "Architecture: %s\n", m->architecture);
    sb_printf(&b, "Kind: %s\n", m->kind);
    if (m->payload)
        sb_printf(&b, "Payload: %s\n", m->payload);
    for (i = 0; i < m->nfiles; i++)
        sb_printf(&b, "File: %s %llu %s\n", m->files[i].digest,
                  m->files[i].size, m->files[i].path);
    if (b.bad) {
        free(b.p);
        return -1;
    }
    *out = b.p;
    *out_len = b.len;
    return 0;
}

/* ---- parse ------------------------------------------------------------ */

static void seterr(char *err, size_t errlen, unsigned line, const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(err, errlen, "manifest line %u: ", line);
    if (n < 0 || (size_t)n >= errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(err + n, errlen - (size_t)n, fmt, ap);
    va_end(ap);
}

static int is_hex64(const char *s, size_t n)
{
    size_t i;
    if (n != PKG_SHA256_HEXLEN)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

int pkg_manifest_parse(const char *text, size_t len, struct pkg_manifest *m,
                       char *err, size_t errlen)
{
    size_t at = 0;
    unsigned line = 0;
    int seen_format = 0;
    const char *why;

    if (errlen)
        err[0] = '\0';
    pkg_manifest_init(m);

    while (at < len) {
        const char *ls = text + at;
        const char *nl = (const char *)memchr(ls, '\n', len - at);
        size_t ll = nl ? (size_t)(nl - ls) : len - at;
        const char *colon;
        char key[32], *val;
        size_t kl, vl;

        at += ll + (nl ? 1u : 0u);
        line++;
        if (nl == NULL) {
            seterr(err, errlen, line, "the last line has no newline");
            goto fail;
        }
        if (memchr(ls, '\0', ll) != NULL) {
            seterr(err, errlen, line, "contains a NUL byte");
            goto fail;
        }
        colon = (const char *)memchr(ls, ':', ll);
        if (colon == NULL || colon + 1 >= ls + ll || colon[1] != ' ') {
            seterr(err, errlen, line, "expected \"Key: value\"");
            goto fail;
        }
        kl = (size_t)(colon - ls);
        if (kl == 0u || kl >= sizeof key) {
            seterr(err, errlen, line, "the key is empty or too long");
            goto fail;
        }
        memcpy(key, ls, kl);
        key[kl] = '\0';
        vl = ll - kl - 2u;
        val = (char *)malloc(vl + 1u);
        if (val == NULL) {
            seterr(err, errlen, line, "out of memory");
            goto fail;
        }
        memcpy(val, colon + 2, vl);
        val[vl] = '\0';

#define ONCE(field, check)                                                   \
        do {                                                                 \
            if (m->field != NULL) {                                          \
                seterr(err, errlen, line, "%s appears twice", key);          \
                free(val); goto fail;                                        \
            }                                                                \
            if ((why = check(val)) != NULL) {                                \
                seterr(err, errlen, line, "%s", why);                        \
                free(val); goto fail;                                        \
            }                                                                \
            m->field = val;                                                  \
        } while (0)

        if (strcmp(key, "Format") == 0) {
            if (seen_format || line != 1u || strcmp(val, "pkg-manifest 1") != 0) {
                seterr(err, errlen, line,
                       "the first line must be \"Format: pkg-manifest 1\"");
                free(val); goto fail;
            }
            seen_format = 1;
            free(val);
        } else if (!seen_format) {
            seterr(err, errlen, line, "the first line must be \"Format: pkg-manifest 1\"");
            free(val); goto fail;
        } else if (strcmp(key, "Name") == 0) {
            ONCE(name, pkg_check_name);
        } else if (strcmp(key, "Version") == 0) {
            ONCE(version, pkg_check_version);
        } else if (strcmp(key, "Architecture") == 0) {
            ONCE(architecture, pkg_check_arch);
        } else if (strcmp(key, "Kind") == 0) {
            ONCE(kind, pkg_check_kind);
        } else if (strcmp(key, "Payload") == 0) {
            if (m->payload != NULL || !is_hex64(val, vl)) {
                seterr(err, errlen, line, "Payload must appear once, as 64 lowercase hex digits");
                free(val); goto fail;
            }
            m->payload = val;
        } else if (strcmp(key, "File") == 0) {
            char *sp1 = strchr(val, ' '), *sp2, *endnum;
            unsigned long long size;
            if (sp1 == NULL || !is_hex64(val, (size_t)(sp1 - val))) {
                seterr(err, errlen, line, "File must start with a 64-digit lowercase hex digest");
                free(val); goto fail;
            }
            sp2 = strchr(sp1 + 1, ' ');
            if (sp2 == NULL || sp2 == sp1 + 1) {
                seterr(err, errlen, line, "File must be \"<digest> <size> <path>\"");
                free(val); goto fail;
            }
            *sp2 = '\0';
            size = strtoull(sp1 + 1, &endnum, 10);
            if (*endnum != '\0' || sp1[1] == '-' || sp1[1] == '+') {
                seterr(err, errlen, line, "the File size is not a plain decimal number");
                free(val); goto fail;
            }
            if ((why = pkg_check_path(sp2 + 1)) != NULL) {
                seterr(err, errlen, line, "%s: \"%s\"", why, sp2 + 1);
                free(val); goto fail;
            }
            if (m->nfiles > 0 && strcmp(m->files[m->nfiles - 1].path, sp2 + 1) >= 0) {
                seterr(err, errlen, line, "File lines must be sorted by path with no repeat: \"%s\"", sp2 + 1);
                free(val); goto fail;
            }
            if (pkg_manifest_add_file(m, sp2 + 1, val, size) != 0) {
                seterr(err, errlen, line, "out of memory");
                free(val); goto fail;
            }
            free(val);
        } else {
            seterr(err, errlen, line, "unknown key \"%s\"", key);
            free(val); goto fail;
        }
#undef ONCE
    }

    if (!seen_format)       { seterr(err, errlen, line, "the manifest is empty"); goto fail; }
    if (m->name == NULL)    { seterr(err, errlen, line, "Name is missing"); goto fail; }
    if (m->version == NULL) { seterr(err, errlen, line, "Version is missing"); goto fail; }
    if (m->architecture == NULL) { seterr(err, errlen, line, "Architecture is missing"); goto fail; }
    if (m->kind == NULL)    { seterr(err, errlen, line, "Kind is missing"); goto fail; }
    return 0;

fail:
    pkg_manifest_free(m);
    return -1;
}
