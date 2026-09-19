/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_manifest.h"
#include "pkg_ameta.h"

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

/* ---- the catalogue fields ---------------------------------------------- */

const char *const pkg_categories[] = {
    "biz", "comm", "demo", "dev", "disk", "docs", "driver", "game", "gfx", "hard",
    "misc", "mods", "mus", "pix", "text", "util", NULL
};

const char *const pkg_distributions[] = {
    "open-source", "freeware", "shareware", "public-domain", "commercial", "demo", "other", NULL
};

int pkg_strs_add(struct pkg_strs *l, const char *s)
{
    char **g = (char **)realloc(l->v, (l->n + 1) * sizeof *g);
    if (g == NULL) return -1;
    l->v = g;
    if ((l->v[l->n] = dupstr(s)) == NULL) return -1;
    l->n++;
    return 0;
}

void pkg_strs_free(struct pkg_strs *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

static void about_free(struct pkg_about *a)
{
    free(a->short_desc); free(a->category); free(a->homepage); free(a->repository);
    free(a->license); free(a->distribution); free(a->icon);
    pkg_strs_free(&a->description); pkg_strs_free(&a->tags); pkg_strs_free(&a->authors);
    pkg_strs_free(&a->changes); pkg_strs_free(&a->screenshots);
}

/* Characters of UTF-8 text, or -1 when it is not UTF-8. */
static long utf8_chars(const char *s)
{
    long n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2
              : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return -1;
        p++;
        while (k-- > 0) {
            if ((*p & 0xC0) != 0x80) return -1;
            p++;
        }
        n++;
    }
    return n;
}

const char *pkg_check_about_text(const char *field, const char *s, size_t max_chars)
{
    static char why[160];
    const unsigned char *p = (const unsigned char *)s;
    long n = utf8_chars(s);
    for (; *p; p++)
        if (*p < 0x20 || *p == 0x7F) {
            snprintf(why, sizeof why, "%s holds a control character", field);
            return why;
        }
    if (n < 0) {
        snprintf(why, sizeof why, "%s is not UTF-8 text", field);
        return why;
    }
    if (s[0] == ' ' || (s[0] && s[strlen(s) - 1] == ' ')) {
        snprintf(why, sizeof why, "%s starts or ends with a space", field);
        return why;
    }
    if ((size_t)n > max_chars) {
        snprintf(why, sizeof why, "%s is %ld characters long; it takes %lu at most", field, n,
                 (unsigned long)max_chars);
        return why;
    }
    return NULL;
}

const char *pkg_check_category(const char *s)
{
    static char why[300];
    const char *slash = strchr(s, '/');
    size_t tl = slash ? (size_t)(slash - s) : strlen(s), i, k;
    for (i = 0; pkg_categories[i]; i++)
        if (strlen(pkg_categories[i]) == tl && strncmp(pkg_categories[i], s, tl) == 0)
            break;
    if (slash == NULL || pkg_categories[i] == NULL) {
        size_t at = 0;
        at += (size_t)snprintf(why, sizeof why, "Category is an Aminet type and its sub-directory, "
                               "such as util/arc or game/think; the types are");
        for (k = 0; pkg_categories[k] && at < sizeof why; k++)
            at += (size_t)snprintf(why + at, sizeof why - at, " %s", pkg_categories[k]);
        return why;
    }
    for (k = 1; slash[k]; k++)
        if (!((slash[k] >= 'a' && slash[k] <= 'z') || (slash[k] >= '0' && slash[k] <= '9')))
            break;
    if (k == 1 || slash[k] != '\0' || k > 9)
        return "the sub-directory of Category is 1 to 8 lowercase letters or digits, such as util/arc";
    return NULL;
}

const char *pkg_check_tag(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 24)
        return "a tag is 1 to 24 characters";
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9')
              || s[i] == '+' || s[i] == '.' || s[i] == '-'))
            return "a tag is lowercase letters, digits, + . or -, such as rexx or 3d";
    return NULL;
}

const char *pkg_check_url(const char *field, const char *s)
{
    static char why[120];
    const unsigned char *p = (const unsigned char *)s;
    if (strncmp(s, "https://", 8) != 0 && strncmp(s, "http://", 7) != 0) {
        snprintf(why, sizeof why, "%s is an http:// or https:// address", field);
        return why;
    }
    for (; *p; p++)
        if (*p <= ' ' || *p == 0x7F) {
            snprintf(why, sizeof why, "%s holds a space or a control character; write a space as %%20", field);
            return why;
        }
    if (strlen(s) > 500) {
        snprintf(why, sizeof why, "%s is longer than 500 characters", field);
        return why;
    }
    return NULL;
}

const char *pkg_check_license(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 100)
        return "License is an SPDX expression of 1 to 100 characters, such as MIT or GPL-2.0-or-later";
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')
              || s[i] == '.' || s[i] == '-' || s[i] == '+' || s[i] == ' ' || s[i] == '(' || s[i] == ')'
              || s[i] == ':'))
            return "License is an SPDX expression, such as MIT, GPL-2.0-or-later or (MIT OR Apache-2.0)";
    if (s[0] == ' ' || s[n - 1] == ' ')
        return "License starts or ends with a space";
    return NULL;
}

const char *pkg_check_libname(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 9 || n > 64)
        return "a library or device name is name.library or name.device, 64 characters at most";
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')
              || s[i] == '_' || s[i] == '.' || s[i] == '-' || s[i] == '+'))
            return "a library or device name is letters, digits and _ . - +";
    if (!(n > 8 && strcmp(s + n - 8, ".library") == 0) && !(n > 7 && strcmp(s + n - 7, ".device") == 0))
        return "a library or device name ends in .library or .device";
    return NULL;
}

const char *pkg_check_distribution(const char *s)
{
    size_t i;
    for (i = 0; pkg_distributions[i]; i++)
        if (strcmp(pkg_distributions[i], s) == 0)
            return NULL;
    return "Distribution is one of open-source, freeware, shareware, public-domain, commercial, "
           "demo or other";
}

void pkg_manifest_init(struct pkg_manifest *m)
{
    memset(m, 0, sizeof *m);
}

void pkg_manifest_free(struct pkg_manifest *m)
{
    size_t i;
    free(m->name); free(m->version); free(m->architecture);
    free(m->kind); free(m->payload); free(m->source);
    free(m->archive_sha); free(m->archive_url);
    about_free(&m->about);
    pkg_strs_free(&m->provides);
    pkg_strs_free(&m->ignored);
    for (i = 0; i < m->nfiles; i++) {
        free(m->files[i].path);
        free(m->files[i].comment);
    }
    free(m->files);
    for (i = 0; i < m->ncontent; i++) {
        free(m->content[i].path);
        free(m->content[i].comment);
    }
    free(m->content);
    for (i = 0; i < m->ndeps; i++) {
        free(m->deps[i].name);
        free(m->deps[i].min);
    }
    free(m->deps);
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

static int add_to(struct pkg_file **v, size_t *n, size_t *cap, const char *path,
                  const char *digest_hex, unsigned long long size)
{
    struct pkg_file *f;
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2u : 16u;
        struct pkg_file *g = (struct pkg_file *)realloc(*v, ncap * sizeof *g);
        if (g == NULL)
            return -1;
        *v = g;
        *cap = ncap;
    }
    f = &(*v)[*n];
    f->path = dupstr(path);
    if (f->path == NULL)
        return -1;
    memcpy(f->digest, digest_hex, PKG_SHA256_HEXLEN);
    f->digest[PKG_SHA256_HEXLEN] = '\0';
    f->size = size;
    f->prot = 0;
    f->comment = NULL;
    f->config = 0;
    (*n)++;
    return 0;
}

long pkg_comment_latin1(const char *utf8, char *out, size_t outsz)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t o = 0;
    while (*p) {
        unsigned c;
        if (p[0] < 0x80) { c = p[0]; p++; }
        else if ((p[0] == 0xC2 || p[0] == 0xC3) && (p[1] & 0xC0) == 0x80) {
            c = (unsigned)((p[0] & 0x1F) << 6 | (p[1] & 0x3F));
            p += 2;
        } else {
            return -1;                  /* outside Latin-1, or not UTF-8 */
        }
        if (c < 0x20 || (c >= 0x7F && c < 0xA0) || o + 1 >= outsz)
            return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return (long)o;
}

struct pkg_file *pkg_manifest_attr_target(struct pkg_manifest *m, const char *path)
{
    size_t i;
    for (i = 0; i < m->nfiles; i++)
        if (strcmp(m->files[i].path, path) == 0)
            return &m->files[i];
    for (i = 0; i < m->ncontent; i++)
        if (strcmp(m->content[i].path, path) == 0)
            return &m->content[i];
    return NULL;
}

int pkg_manifest_add_file(struct pkg_manifest *m, const char *path,
                          const char *digest_hex, unsigned long long size)
{
    return add_to(&m->files, &m->nfiles, &m->cap, path, digest_hex, size);
}

int pkg_manifest_add_content(struct pkg_manifest *m, const char *path,
                             const char *digest_hex, unsigned long long size)
{
    return add_to(&m->content, &m->ncontent, &m->ccap, path, digest_hex, size);
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const struct pkg_file *)a)->path,
                  ((const struct pkg_file *)b)->path);
}

static int by_dep(const void *a, const void *b)
{
    return strcmp(((const struct pkg_dep *)a)->name, ((const struct pkg_dep *)b)->name);
}

void pkg_manifest_sort(struct pkg_manifest *m)
{
    if (m->nfiles > 1u)
        qsort(m->files, m->nfiles, sizeof m->files[0], by_path);
    if (m->ncontent > 1u)
        qsort(m->content, m->ncontent, sizeof m->content[0], by_path);
    if (m->ndeps > 1u)
        qsort(m->deps, m->ndeps, sizeof m->deps[0], by_dep);
}

int pkg_manifest_add_dep(struct pkg_manifest *m, const char *name, const char *min)
{
    struct pkg_dep *g = (struct pkg_dep *)realloc(m->deps, (m->ndeps + 1u) * sizeof *g);
    if (g == NULL)
        return -1;
    m->deps = g;
    g[m->ndeps].name = dupstr(name);
    g[m->ndeps].min = (min != NULL && *min) ? dupstr(min) : NULL;
    if (g[m->ndeps].name == NULL || (min != NULL && *min && g[m->ndeps].min == NULL)) {
        free(g[m->ndeps].name);
        free(g[m->ndeps].min);
        return -1;
    }
    m->ndeps++;
    return 0;
}

const char *pkg_parse_dep(const char *text, char *name, size_t name_len,
                          char *min, size_t min_len)
{
    const char *sp = strchr(text, ' ');
    size_t nl = sp ? (size_t)(sp - text) : strlen(text);
    const char *why;

    if (nl == 0u || nl >= name_len)
        return "a dependency must name a package";
    memcpy(name, text, nl);
    name[nl] = '\0';
    if ((why = pkg_check_name(name)) != NULL)
        return why;
    min[0] = '\0';
    if (sp == NULL)
        return NULL;
    if (strncmp(sp, " >= ", 4) != 0)
        return "a dependency is \"name\" or \"name >= version\"";
    if (strlen(sp + 4) >= min_len)
        return "the dependency's version is too long";
    strcpy(min, sp + 4);
    return pkg_check_version(min);
}

const char *pkg_check_deps(const struct pkg_manifest *m)
{
    size_t i;
    for (i = 0; i < m->ndeps; i++) {
        if (m->name != NULL && strcmp(m->deps[i].name, m->name) == 0)
            return "a package cannot depend on itself";
        if (i > 0 && strcmp(m->deps[i - 1].name, m->deps[i].name) >= 0)
            return "Depends lines must be sorted by name, each package once";
    }
    return NULL;
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

/* Dotted numbers, then optionally "+" and dotted numbers: the build, such as
 * the date of the nightly a component was taken from (41.7+20260918). */
const char *pkg_check_version(const char *s)
{
    size_t digits = 0;
    int plus = 0;
    if (s == NULL || *s == '\0')
        return "the version is empty";
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            if (++digits > 9u)
                return "a version component is longer than 9 digits";
        } else if (*s == '.' || (*s == '+' && !plus)) {
            if (digits == 0u)
                return "the version has an empty component";
            if (*s == '+') plus = 1;
            digits = 0;
        } else {
            return "the version must be dotted numbers, such as 40.1 or 1.2.3, optionally "
                   "followed by + and a build such as 20260918";
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
        "catalog", "startup", "data", "slave", "sdk", "image"
    };
    size_t i;
    if (s == NULL || *s == '\0')
        return "the kind is empty";
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (strcmp(s, kinds[i]) == 0)
            return NULL;
    return "unknown kind; expected one of application, library, device, boot, "
           "class, font, catalog, startup, data, slave, sdk, image";
}

/* Dotted numbers up to the end or a '+', missing components counting 0. */
static int cmp_dotted(const char **pa, const char **pb)
{
    const char *a = *pa, *b = *pb;
    int r = 0;
    while ((*a && *a != '+') || (*b && *b != '+')) {
        unsigned long x = 0, y = 0;
        while (*a >= '0' && *a <= '9') x = x * 10u + (unsigned long)(*a++ - '0');
        while (*b >= '0' && *b <= '9') y = y * 10u + (unsigned long)(*b++ - '0');
        if (r == 0 && x != y)
            r = x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    *pa = a;
    *pb = b;
    return r;
}

/* The version first; at equal versions the build, none counting lowest, so
 * 41.7 < 41.7+20260917 < 41.7+20260918 < 41.8. */
int pkg_version_cmp(const char *a, const char *b)
{
    int r = cmp_dotted(&a, &b);
    if (r != 0)
        return r;
    if (*a == '+') a++;
    if (*b == '+') b++;
    return cmp_dotted(&a, &b);
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
    for (i = 0; i < m->ndeps; i++) {
        if (m->deps[i].min)
            sb_printf(&b, "Depends: %s >= %s\n", m->deps[i].name, m->deps[i].min);
        else
            sb_printf(&b, "Depends: %s\n", m->deps[i].name);
    }
    for (i = 0; i < m->provides.n; i++)
        sb_printf(&b, "Provides: %s\n", m->provides.v[i]);
    {
        const struct pkg_about *a = &m->about;
        if (a->short_desc) sb_printf(&b, "Short: %s\n", a->short_desc);
        for (i = 0; i < a->description.n; i++) sb_printf(&b, "Description: %s\n", a->description.v[i]);
        if (a->category) sb_printf(&b, "Category: %s\n", a->category);
        if (a->tags.n) {
            sb_printf(&b, "Tags: ");
            for (i = 0; i < a->tags.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", a->tags.v[i]);
            sb_printf(&b, "\n");
        }
        for (i = 0; i < a->authors.n; i++) sb_printf(&b, "Author: %s\n", a->authors.v[i]);
        if (a->homepage) sb_printf(&b, "Homepage: %s\n", a->homepage);
        if (a->repository) sb_printf(&b, "Repository: %s\n", a->repository);
        if (a->license) sb_printf(&b, "License: %s\n", a->license);
        if (a->distribution) sb_printf(&b, "Distribution: %s\n", a->distribution);
        for (i = 0; i < a->changes.n; i++) sb_printf(&b, "Changes: %s\n", a->changes.v[i]);
        if (a->icon) sb_printf(&b, "Icon: %s\n", a->icon);
        for (i = 0; i < a->screenshots.n; i++) sb_printf(&b, "Screenshot: %s\n", a->screenshots.v[i]);
    }
    if (m->payload)
        sb_printf(&b, "Payload: %s\n", m->payload);
    if (m->source)
        sb_printf(&b, "Source: %s\n", m->source);
    if (m->archive_sha)
        sb_printf(&b, "Archive: %s %llu %s\n", m->archive_sha, m->archive_size, m->archive_url);
    for (i = 0; i < m->nfiles; i++)
        sb_printf(&b, "File: %s %llu %s\n", m->files[i].digest,
                  m->files[i].size, m->files[i].path);
    for (i = 0; i < m->ncontent; i++)
        sb_printf(&b, "Content: %s %llu %s\n", m->content[i].digest,
                  m->content[i].size, m->content[i].path);
    /* Amiga attributes, only where they differ from the default: the files
     * of the package, then the files inside its image. */
    {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            const struct pkg_file *v = pass ? m->content : m->files;
            size_t k, n = pass ? m->ncontent : m->nfiles;
            for (k = 0; k < n; k++)
                if (v[k].prot != 0)
                    sb_printf(&b, v[k].prot <= 0xFFFFFFFFull ? "Protect: 0x%08llX %s\n"
                              : "Protect: 0x%016llX %s\n", v[k].prot, v[k].path);
        }
        for (pass = 0; pass < 2; pass++) {
            const struct pkg_file *v = pass ? m->content : m->files;
            size_t k, n = pass ? m->ncontent : m->nfiles;
            for (k = 0; k < n; k++)
                if (v[k].comment != NULL && v[k].comment[0]) {
                    char *esc = (char *)malloc(3 * strlen(v[k].comment) + 1);
                    if (esc == NULL) { b.bad = 1; break; }
                    pkg_ameta_escape((const unsigned char *)v[k].comment, strlen(v[k].comment), esc);
                    sb_printf(&b, "Comment: %s %s\n", esc, v[k].path);
                    free(esc);
                }
        }
    }
    for (i = 0; i < m->nfiles; i++)
        if (m->files[i].config)
            sb_printf(&b, "Config: %s\n", m->files[i].path);
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
        } else if (strcmp(key, "Depends") == 0) {
            char dn[65], dv[64];
            if ((why = pkg_parse_dep(val, dn, sizeof dn, dv, sizeof dv)) != NULL) {
                seterr(err, errlen, line, "%s: \"%s\"", why, val);
                free(val); goto fail;
            }
            if (m->ndeps > 0 && strcmp(m->deps[m->ndeps - 1].name, dn) >= 0) {
                seterr(err, errlen, line, "Depends lines must be sorted by name, each package once: \"%s\"", dn);
                free(val); goto fail;
            }
            if (pkg_manifest_add_dep(m, dn, dv) != 0) {
                seterr(err, errlen, line, "out of memory");
                free(val); goto fail;
            }
            free(val);
        } else if (strcmp(key, "Payload") == 0) {
            if (m->payload != NULL || !is_hex64(val, vl)) {
                seterr(err, errlen, line, "Payload must appear once, as 64 lowercase hex digits");
                free(val); goto fail;
            }
            m->payload = val;
        } else if (strcmp(key, "Source") == 0) {
            const char *bang = strstr(val, "!/");
            if (m->source != NULL || bang == NULL || bang == val || strchr(val, '\\') != NULL) {
                seterr(err, errlen, line, "Source must appear once, as <archive>!/<path inside it>");
                free(val); goto fail;
            }
            m->source = val;
        } else if (strcmp(key, "Short") == 0 || strcmp(key, "Category") == 0
                   || strcmp(key, "Homepage") == 0 || strcmp(key, "Repository") == 0
                   || strcmp(key, "License") == 0 || strcmp(key, "Distribution") == 0
                   || strcmp(key, "Icon") == 0) {
            /* the catalogue fields that appear once */
            struct pkg_about *a = &m->about;
            char **slot = key[0] == 'S' ? &a->short_desc : key[0] == 'C' ? &a->category
                        : key[0] == 'H' ? &a->homepage : key[0] == 'R' ? &a->repository
                        : key[0] == 'L' ? &a->license : key[0] == 'D' ? &a->distribution : &a->icon;
            why = key[0] == 'S' ? (val[0] ? pkg_check_about_text("Short", val, 40) : "Short is empty")
                : key[0] == 'C' ? pkg_check_category(val)
                : key[0] == 'H' ? pkg_check_url("Homepage", val)
                : key[0] == 'R' ? pkg_check_url("Repository", val)
                : key[0] == 'L' ? pkg_check_license(val)
                : key[0] == 'D' ? pkg_check_distribution(val)
                : pkg_check_path(val);
            if (*slot != NULL) why = "appears twice";
            if (why != NULL) {
                seterr(err, errlen, line, "%s: %s", key, why);
                free(val); goto fail;
            }
            *slot = val;
        } else if (strcmp(key, "Description") == 0 || strcmp(key, "Changes") == 0
                   || strcmp(key, "Author") == 0 || strcmp(key, "Screenshot") == 0) {
            /* the ones that repeat; an empty Description or Changes is a paragraph break */
            struct pkg_about *a = &m->about;
            struct pkg_strs *l = key[0] == 'D' ? &a->description : key[0] == 'C' ? &a->changes
                               : key[0] == 'A' ? &a->authors : &a->screenshots;
            why = key[0] == 'S' ? pkg_check_path(val)
                : key[0] == 'A' ? (val[0] ? pkg_check_about_text("Author", val, 80) : "Author is empty")
                : pkg_check_about_text(key, val, 1000);
            if (why == NULL && l->n >= 400) why = "appears more than 400 times";
            if (why != NULL || pkg_strs_add(l, val) != 0) {
                seterr(err, errlen, line, "%s: %s", key, why ? why : "out of memory");
                free(val); goto fail;
            }
            free(val);
        } else if (strcmp(key, "Provides") == 0) {
            why = pkg_check_libname(val);
            if (why == NULL && m->provides.n > 0 && strcmp(m->provides.v[m->provides.n - 1], val) >= 0)
                why = "Provides lines must be sorted, each name once";
            if (why != NULL || pkg_strs_add(&m->provides, val) != 0) {
                seterr(err, errlen, line, "Provides: %s", why ? why : "out of memory");
                free(val); goto fail;
            }
            free(val);
        } else if (strcmp(key, "Tags") == 0) {
            char *t = val, *next;
            for (; t != NULL; t = next) {
                size_t k;
                next = strstr(t, ", ");
                if (next) { *next = '\0'; next += 2; }
                why = pkg_check_tag(t);
                for (k = 0; why == NULL && k < m->about.tags.n; k++)
                    if (strcmp(m->about.tags.v[k], t) == 0) why = "a tag appears twice";
                if (why == NULL && m->about.tags.n >= 16) why = "more than 16 tags";
                if (why != NULL || pkg_strs_add(&m->about.tags, t) != 0) {
                    seterr(err, errlen, line, "Tags: %s", why ? why : "out of memory");
                    free(val); goto fail;
                }
            }
            free(val);
        } else if (strcmp(key, "Archive") == 0) {
            /* "<sha256> <size> <url>": where the Source archive is published */
            char *s1 = strchr(val, ' '), *s2 = s1 ? strchr(s1 + 1, ' ') : NULL, *endnum;
            unsigned long long asz = 0;
            const char *u = s2 ? s2 + 1 : "";
            size_t q;
            int bad = m->archive_sha != NULL || s1 == NULL || s2 == NULL
                      || !is_hex64(val, (size_t)(s1 - val));
            if (!bad) {
                *s2 = '\0';
                asz = strtoull(s1 + 1, &endnum, 10);
                bad = *endnum != '\0' || s1[1] == '-' || s1[1] == '+' || s1 + 1 == endnum
                      || (strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0);
                for (q = 0; !bad && u[q]; q++)
                    bad = (unsigned char)u[q] <= ' ' || u[q] == 0x7F;
            }
            if (bad) {
                seterr(err, errlen, line, "Archive must appear once, as <sha256> <size> <http or "
                       "https URL, no spaces>");
                free(val); goto fail;
            }
            *s1 = '\0';
            m->archive_sha = dupstr(val);
            m->archive_size = asz;
            m->archive_url = dupstr(u);
            free(val);
            if (m->archive_sha == NULL || m->archive_url == NULL) {
                seterr(err, errlen, line, "out of memory");
                goto fail;
            }
        } else if (strcmp(key, "File") == 0 || strcmp(key, "Content") == 0) {
            /* Content: the files inside an image, the same syntax as File. */
            int is_c = key[0] == 'C';
            struct pkg_file *list = is_c ? m->content : m->files;
            size_t count = is_c ? m->ncontent : m->nfiles;
            char *sp1 = strchr(val, ' '), *sp2, *endnum;
            unsigned long long size;
            if (sp1 == NULL || !is_hex64(val, (size_t)(sp1 - val))) {
                seterr(err, errlen, line, "%s must start with a 64-digit lowercase hex digest", key);
                free(val); goto fail;
            }
            sp2 = strchr(sp1 + 1, ' ');
            if (sp2 == NULL || sp2 == sp1 + 1) {
                seterr(err, errlen, line, "%s must be \"<digest> <size> <path>\"", key);
                free(val); goto fail;
            }
            *sp2 = '\0';
            size = strtoull(sp1 + 1, &endnum, 10);
            if (*endnum != '\0' || sp1[1] == '-' || sp1[1] == '+') {
                seterr(err, errlen, line, "the %s size is not a plain decimal number", key);
                free(val); goto fail;
            }
            if ((why = pkg_check_path(sp2 + 1)) != NULL) {
                seterr(err, errlen, line, "%s: \"%s\"", why, sp2 + 1);
                free(val); goto fail;
            }
            if (count > 0 && strcmp(list[count - 1].path, sp2 + 1) >= 0) {
                seterr(err, errlen, line, "%s lines must be sorted by path with no repeat: \"%s\"", key, sp2 + 1);
                free(val); goto fail;
            }
            if ((is_c ? pkg_manifest_add_content(m, sp2 + 1, val, size)
                      : pkg_manifest_add_file(m, sp2 + 1, val, size)) != 0) {
                seterr(err, errlen, line, "out of memory");
                free(val); goto fail;
            }
            free(val);
        } else if (strcmp(key, "Config") == 0) {
            /* a configuration file of the package: a File line above names it */
            size_t k2;
            for (k2 = 0; k2 < m->nfiles && strcmp(m->files[k2].path, val) != 0; k2++)
                ;
            if (k2 == m->nfiles || m->files[k2].config) {
                seterr(err, errlen, line, "Config names \"%s\", which no File line lists, or twice", val);
                free(val); goto fail;
            }
            m->files[k2].config = 1;
            free(val);
        } else if (strcmp(key, "Protect") == 0 || strcmp(key, "Comment") == 0) {
            /* "<value> <path>": an Amiga attribute of a file listed above. */
            char *sp = strchr(val, ' ');
            struct pkg_file *t;
            if (sp == NULL || sp == val) {
                seterr(err, errlen, line, "%s must be \"<value> <path>\"", key);
                free(val); goto fail;
            }
            *sp = '\0';
            t = pkg_manifest_attr_target(m, sp + 1);
            if (t == NULL) {
                seterr(err, errlen, line, "%s names \"%s\", which no File or Content line lists", key, sp + 1);
                free(val); goto fail;
            }
            if (key[0] == 'P') {
                unsigned long long w;
                if (pkg_ameta_parse_prot(val, strlen(val), &w) != NULL || w == 0 || t->prot != 0) {
                    seterr(err, errlen, line, "Protect must be a nonzero 0x word, once per path: \"%s\"", val);
                    free(val); goto fail;
                }
                t->prot = w;
            } else {
                size_t vl = strlen(val);
                long cl;
                char *c = (char *)malloc(vl + 1), latin[PKG_COMMENT_MAX + 1];
                if (c == NULL) { seterr(err, errlen, line, "out of memory"); free(val); goto fail; }
                cl = pkg_ameta_unescape(val, vl, (unsigned char *)c);
                if (cl >= 0) c[cl] = '\0';
                if (cl <= 0 || t->comment != NULL || memchr(c, '\0', (size_t)cl) != NULL
                    || pkg_comment_latin1(c, latin, sizeof latin) < 0) {
                    seterr(err, errlen, line, "Comment must be escaped, at most %d characters of "
                           "Latin-1, once per path", PKG_COMMENT_MAX);
                    free(c); free(val); goto fail;
                }
                t->comment = c;
            }
            free(val);
        } else {
            /* a key for another system: kept in the signed text, never acted on */
            size_t k;
            int shape = (key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= 'a' && key[0] <= 'z');
            for (k = 1; shape && key[k]; k++)
                shape = (key[k] >= 'A' && key[k] <= 'Z') || (key[k] >= 'a' && key[k] <= 'z')
                        || (key[k] >= '0' && key[k] <= '9') || key[k] == '-' || key[k] == '_' || key[k] == '.';
            if (!shape) {
                seterr(err, errlen, line, "\"%s\" is no key: a key is a letter, then letters, "
                       "digits, - _ or .", key);
                free(val); goto fail;
            }
            for (k = 0; k < m->ignored.n && strcmp(m->ignored.v[k], key) != 0; k++)
                ;
            if (k == m->ignored.n && pkg_strs_add(&m->ignored, key) != 0) {
                seterr(err, errlen, line, "out of memory");
                free(val); goto fail;
            }
            free(val);
        }
#undef ONCE
    }

    if (!seen_format)       { seterr(err, errlen, line, "the manifest is empty"); goto fail; }
    if (m->name == NULL)    { seterr(err, errlen, line, "Name is missing"); goto fail; }
    if (m->version == NULL) { seterr(err, errlen, line, "Version is missing"); goto fail; }
    if (m->architecture == NULL) { seterr(err, errlen, line, "Architecture is missing"); goto fail; }
    if (m->kind == NULL)    { seterr(err, errlen, line, "Kind is missing"); goto fail; }
    if ((why = pkg_check_deps(m)) != NULL) { seterr(err, errlen, line, "%s", why); goto fail; }
    {
        /* Icon and Screenshot name files of the package, or of its image */
        const char *pics[401];
        size_t np = 0, k, f;
        if (m->about.icon) pics[np++] = m->about.icon;
        for (k = 0; k < m->about.screenshots.n; k++) pics[np++] = m->about.screenshots.v[k];
        for (k = 0; k < np; k++) {
            int found = 0;
            for (f = 0; f < m->nfiles && !found; f++) found = strcmp(m->files[f].path, pics[k]) == 0;
            for (f = 0; f < m->ncontent && !found; f++) found = strcmp(m->content[f].path, pics[k]) == 0;
            if (!found) {
                seterr(err, errlen, line, "Icon or Screenshot names \"%s\", which is no file of the package", pics[k]);
                goto fail;
            }
        }
    }
    if (m->archive_sha != NULL && m->source == NULL) {
        seterr(err, errlen, line, "Archive says where a Source archive is published, and there is no Source");
        goto fail;
    }
    return 0;

fail:
    pkg_manifest_free(m);
    return -1;
}
