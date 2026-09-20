/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Reading a .pkginfo (pkg_pkginfo.h). The parser is the manifest's in
 * miniature: one "Key: value" per line, strict about what it knows, silent
 * about what it does not.
 */

#include "pkg_pkginfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKGINFO_FORMAT 1

static char *dupstr(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1u);
    if (p != NULL) strcpy(p, s);
    return p;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

void pkg_pkginfo_free(struct pkg_pkginfo *pi)
{
    struct pkg_about *a = &pi->about;
    free(pi->name); free(pi->version); free(pi->kind); free(pi->depends);
    free(pi->files_line); free(pi->config_line);
    free(a->short_desc); free(a->category); free(a->homepage); free(a->repository);
    free(a->license); free(a->distribution); free(a->icon);
    pkg_strs_free(&a->description);
    pkg_strs_free(&a->changes);
    pkg_strs_free(&a->tags);
    pkg_strs_free(&a->authors);
    pkg_strs_free(&a->screenshots);
    pkg_strs_free(&pi->files);
    pkg_strs_free(&pi->config);
    memset(pi, 0, sizeof *pi);
}

/* The comma-separated form FILES and CONFIG take. */
static char *join(const struct pkg_strs *l)
{
    size_t i, n = 1;
    char *out;
    for (i = 0; i < l->n; i++) n += strlen(l->v[i]) + 2u;
    out = (char *)malloc(n);
    if (out == NULL) return NULL;
    out[0] = '\0';
    for (i = 0; i < l->n; i++) {
        if (i) strcat(out, ",");
        strcat(out, l->v[i]);
    }
    return out;
}

/* A value that may appear once. */
static int once(char **slot, const char *v, const char *key, char *err, size_t errlen)
{
    if (*slot != NULL) {
        snprintf(err, errlen, "%s is given twice; one .pkginfo says one thing", key);
        return -1;
    }
    *slot = dupstr(v);
    return *slot == NULL ? -1 : 0;
}

static int lower_tags(const char *v, struct pkg_strs *out, char *err, size_t errlen)
{
    const char *p = v;
    while (*p) {
        char one[128], *c;
        size_t n;
        const char *why;
        while (*p == ' ' || *p == ',') p++;
        n = strcspn(p, ",");
        while (n > 0 && p[n - 1u] == ' ') n--;
        if (n == 0) break;
        if (n >= sizeof one) {
            snprintf(err, errlen, "Tags holds a word longer than %u characters",
                     (unsigned)sizeof one - 1u);
            return -1;
        }
        memcpy(one, p, n);
        one[n] = '\0';
        for (c = one; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        if ((why = pkg_check_tag(one)) != NULL) {
            snprintf(err, errlen, "Tags: \"%s\": %s", one, why);
            return -1;
        }
        if (pkg_strs_add(out, one) != 0) return -1;
        p += strcspn(p, ",");
    }
    if (out->n > 16) {
        snprintf(err, errlen, "Tags holds %lu tags; 16 at most", (unsigned long)out->n);
        return -1;
    }
    return 0;
}

/* Files and Config: an installed path, relative to the system root. */
static int add_path(struct pkg_strs *out, const char *v, const char *key,
                    char *err, size_t errlen)
{
    const char *why;
    if (v[0] == '\0') {
        snprintf(err, errlen, "%s names no path", key);
        return -1;
    }
    if (strchr(v, ',') != NULL) {
        snprintf(err, errlen, "%s: \"%s\" holds a comma, which separates paths in Pkg's "
                 "FILES list; write one path per %s line", key, v, key);
        return -1;
    }
    if ((why = pkg_check_path(v)) != NULL) {
        snprintf(err, errlen, "%s: \"%s\": %s", key, v, why);
        return -1;
    }
    return pkg_strs_add(out, v);
}

int pkg_pkginfo_parse(const char *text, size_t len, struct pkg_pkginfo *pi,
                      char *err, size_t errlen, int *line)
{
    struct pkg_about *ab = &pi->about;
    size_t i = 0;
    int ln = 0, seen_format = 0;

    memset(pi, 0, sizeof *pi);
    err[0] = '\0';
    *line = 0;
    while (i < len) {
        char key[64], val[1200];
        const char *why = NULL;
        size_t e = i, k, vs, ve, ks = i;
        while (e < len && text[e] != '\n') e++;
        while (ks < e && (text[ks] == ' ' || text[ks] == '\t')) ks++;
        ve = e;
        if (ve > i && text[ve - 1u] == '\r') ve--;
        ln++;
        if (ks >= ve) {                     /* an empty line, or one of spaces */
            i = e + 1u;
            continue;
        }
        i = ks;
        for (k = i; k < ve && text[k] != ':' && k - i < sizeof key - 1u; k++)
            key[k - i] = text[k];
        key[k - i] = '\0';
        vs = k < ve && text[k] == ':' ? k + 1u : ve;
        while (vs < ve && (text[vs] == ' ' || text[vs] == '\t')) vs++;
        while (ve > vs && (text[ve - 1u] == ' ' || text[ve - 1u] == '\t')) ve--;
        if (ve - vs >= sizeof val) ve = vs + sizeof val - 1u;
        memcpy(val, text + vs, ve - vs);
        val[ve - vs] = '\0';
        i = e + 1u;
        *line = ln;
        if (!seen_format) {
            int n = 0;
            if (!ieq(key, "Format") || sscanf(val, "pkginfo %d", &n) != 1) {
                snprintf(err, errlen, "the first line is not \"Format: pkginfo %d\"; this is not "
                         "a .pkginfo file", PKGINFO_FORMAT);
                return -1;
            }
            if (n != PKGINFO_FORMAT) {
                snprintf(err, errlen, "it is pkginfo %d and this Pkg reads pkginfo %d: Pkg is "
                         "older than the file. Update Pkg", n, PKGINFO_FORMAT);
                return -1;
            }
            seen_format = 1;
            continue;
        }
        if (key[0] == '#')
            continue;                       /* a remark */
        if (k >= ve || text[k] != ':') {
            snprintf(err, errlen, "\"%.40s\" is not a \"Key: value\" line", key);
            return -1;
        }
        if (ieq(key, "Name")) {
            if (once(&pi->name, val, "Name", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_name(pi->name);
        } else if (ieq(key, "Version")) {
            if (once(&pi->version, val, "Version", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_version(pi->version);
        } else if (ieq(key, "Kind")) {
            if (once(&pi->kind, val, "Kind", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_kind(pi->kind);
        } else if (ieq(key, "Short")) {
            if (once(&ab->short_desc, val, "Short", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_about_text("Short", ab->short_desc, 40);
        } else if (ieq(key, "Category")) {
            if (once(&ab->category, val, "Category", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_category(ab->category);
        } else if (ieq(key, "Homepage")) {
            if (once(&ab->homepage, val, "Homepage", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_url("Homepage", ab->homepage);
        } else if (ieq(key, "Repository")) {
            if (once(&ab->repository, val, "Repository", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_url("Repository", ab->repository);
        } else if (ieq(key, "License")) {
            if (once(&ab->license, val, "License", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_license(ab->license);
        } else if (ieq(key, "Distribution")) {
            if (once(&ab->distribution, val, "Distribution", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_distribution(ab->distribution);
        } else if (ieq(key, "Icon")) {
            if (once(&ab->icon, val, "Icon", err, errlen) != 0) goto oom_or_said;
            why = pkg_check_path(ab->icon);
        } else if (ieq(key, "Tags")) {
            if (ab->tags.n != 0) {
                snprintf(err, errlen, "Tags is given twice; one .pkginfo says one thing");
                return -1;
            }
            if (lower_tags(val, &ab->tags, err, errlen) != 0) goto oom_or_said;
        } else if (ieq(key, "Author")) {
            if ((why = pkg_check_about_text("Author", val, 80)) == NULL
                && pkg_strs_add(&ab->authors, val) != 0) goto oom_or_said;
        } else if (ieq(key, "Screenshot")) {
            if ((why = pkg_check_path(val)) == NULL
                && pkg_strs_add(&ab->screenshots, val) != 0) goto oom_or_said;
        } else if (ieq(key, "Description") || ieq(key, "Changes")) {
            struct pkg_strs *l = ieq(key, "Description") ? &ab->description : &ab->changes;
            /* an empty value is the paragraph break between two of them */
            if (val[0] != '\0') why = pkg_check_about_text(key, val, 1000);
            if (why == NULL && pkg_strs_add(l, val) != 0) goto oom_or_said;
        } else if (ieq(key, "Depends")) {
            const char *p = val;
            if (once(&pi->depends, val, "Depends", err, errlen) != 0) goto oom_or_said;
            while (*p) {
                char dn[65], dv[64];
                char one[160];
                size_t n;
                while (*p == ' ' || *p == ',') p++;
                n = strcspn(p, ",");
                while (n > 0 && p[n - 1u] == ' ') n--;
                if (n == 0) break;
                if (n >= sizeof one) {
                    snprintf(err, errlen, "Depends names a package in more than %u characters",
                             (unsigned)sizeof one - 1u);
                    return -1;
                }
                memcpy(one, p, n);
                one[n] = '\0';
                if ((why = pkg_parse_dep(one, dn, sizeof dn, dv, sizeof dv)) != NULL) {
                    snprintf(err, errlen, "Depends: \"%s\": %s", one, why);
                    return -1;
                }
                p += strcspn(p, ",");
            }
        } else if (ieq(key, "Files")) {
            if (add_path(&pi->files, val, "Files", err, errlen) != 0) goto oom_or_said;
        } else if (ieq(key, "Config")) {
            if (add_path(&pi->config, val, "Config", err, errlen) != 0) goto oom_or_said;
        }
        /* any other key belongs to the port's own tools: ignored, as in a
         * manifest (Upstream-Archive, Port, Port-Maintainer) */
        if (why != NULL) {
            snprintf(err, errlen, "%s", why);
            return -1;
        }
    }
    *line = 0;
    pi->files_line = join(&pi->files);
    pi->config_line = join(&pi->config);
    if (pi->files_line == NULL || pi->config_line == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    return 0;

oom_or_said:
    if (err[0] == '\0') snprintf(err, errlen, "out of memory");
    return -1;
}
