/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* .ameta reader and writer, per docs/features/file-metadata/ameta.md in the
 * planning repository. Portable C99: bytes in, bytes out. */

#include "pkg_ameta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 4096u
#define MAX_FILE (1u << 20)

void pkg_ameta_init(struct pkg_ameta *a)
{
    memset(a, 0, sizeof *a);
    a->usable = 1;
}

static void entry_clear(struct pkg_ameta_entry *e)
{
    size_t i;
    free(e->comment);
    for (i = 0; i < e->nunknown; i++) {
        free(e->unknown[i].key);
        free(e->unknown[i].value);
    }
    free(e->unknown);
    e->comment = NULL;
    e->comment_len = 0;
    e->unknown = NULL;
    e->nunknown = 0;
    e->has_prot = e->has_uid = e->has_gid = 0;
    e->prot = 0;
    e->uid = e->gid = 0;
    e->stale = 0;
}

void pkg_ameta_free(struct pkg_ameta *a)
{
    size_t i;
    for (i = 0; i < a->n; i++) {
        entry_clear(&a->e[i]);
        free(a->e[i].name);
    }
    free(a->e);
    free(a->r);
    pkg_ameta_init(a);
}

static int report(struct pkg_ameta *a, unsigned line, const char *reason)
{
    struct pkg_ameta_report *r = (struct pkg_ameta_report *)realloc(a->r, (a->nr + 1) * sizeof *r);
    if (r == NULL)
        return -1;
    a->r = r;
    r[a->nr].line = line;
    snprintf(r[a->nr].reason, sizeof r[a->nr].reason, "%s", reason);
    a->nr++;
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void pkg_ameta_escape(const unsigned char *in, size_t len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char b = in[i];
        if (b == 0x25 || b <= 0x20 || b >= 0x7F) {
            out[o++] = '%';
            out[o++] = hex[b >> 4];
            out[o++] = hex[b & 15];
        } else {
            out[o++] = (char)b;
        }
    }
    out[o] = '\0';
}

long pkg_ameta_unescape(const char *in, size_t len, unsigned char *out)
{
    size_t i = 0, o = 0;
    while (i < len) {
        if (in[i] == '%') {
            int h, l;
            if (i + 2 >= len || (h = hexval((unsigned char)in[i + 1])) < 0
                || (l = hexval((unsigned char)in[i + 2])) < 0)
                return -1;
            out[o++] = (unsigned char)(h << 4 | l);
            i += 3;
        } else {
            out[o++] = (unsigned char)in[i++];
        }
    }
    return (long)o;
}

const char *pkg_ameta_parse_prot(const char *v, size_t len, unsigned long long *out)
{
    size_t i;
    unsigned long long w = 0;
    if (len < 2 || v[0] != '0' || v[1] != 'x')
        return "bad-number";
    if (len > 18)
        return "prot-too-long";
    if (len == 2)
        return "bad-number";
    for (i = 2; i < len; i++) {
        int d = hexval((unsigned char)v[i]);
        if (d < 0)
            return "bad-number";
        w = w << 4 | (unsigned long long)d;
    }
    *out = w;
    return NULL;
}

static const char *parse_id(const char *v, size_t len, unsigned long *out)
{
    size_t i;
    unsigned long long n = 0;
    if (len == 0)
        return "bad-number";
    for (i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9')
            return "bad-number";
        n = n * 10 + (unsigned long long)(v[i] - '0');
        if (n > 0xFFFFFFFFull)
            return "bad-number";
    }
    *out = (unsigned long)n;
    return NULL;
}

/* Strict UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF. */
static int valid_utf8(const unsigned char *p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t need;
        unsigned long cp;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return 0;
        if (i + need >= len) return 0;
        {
            size_t k;
            for (k = 1; k <= need; k++) {
                if ((p[i + k] & 0xC0) != 0x80) return 0;
                cp = cp << 6 | (p[i + k] & 0x3F);
            }
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

static int bytes_eq(const unsigned char *a, size_t al, const unsigned char *b, size_t bl)
{
    return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

struct pkg_ameta_entry *pkg_ameta_find(struct pkg_ameta *a, const unsigned char *name, size_t len)
{
    size_t i;
    for (i = 0; i < a->n; i++)
        if (!a->e[i].stale && bytes_eq(a->e[i].name, a->e[i].name_len, name, len))
            return &a->e[i];
    return NULL;
}

struct pkg_ameta_entry *pkg_ameta_get(struct pkg_ameta *a, const unsigned char *name, size_t len)
{
    struct pkg_ameta_entry *e = pkg_ameta_find(a, name, len);
    size_t i;
    if (e != NULL)
        return e;
    for (i = 0; i < a->n; i++)          /* a stale entry of that name starts again */
        if (a->e[i].stale && bytes_eq(a->e[i].name, a->e[i].name_len, name, len)) {
            entry_clear(&a->e[i]);
            return &a->e[i];
        }
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        struct pkg_ameta_entry *g = (struct pkg_ameta_entry *)realloc(a->e, nc * sizeof *g);
        if (g == NULL)
            return NULL;
        a->e = g;
        a->cap = nc;
    }
    e = &a->e[a->n];
    memset(e, 0, sizeof *e);
    e->name = (unsigned char *)malloc(len ? len : 1);
    if (e->name == NULL)
        return NULL;
    if (len)
        memcpy(e->name, name, len);
    e->name_len = len;
    a->n++;
    return e;
}

void pkg_ameta_delete(struct pkg_ameta *a, const unsigned char *name, size_t len)
{
    struct pkg_ameta_entry *e = pkg_ameta_find(a, name, len);
    size_t i;
    if (e == NULL)
        return;
    i = (size_t)(e - a->e);
    entry_clear(e);
    free(e->name);
    memmove(&a->e[i], &a->e[i + 1], (a->n - i - 1) * sizeof *e);
    a->n--;
}

int pkg_ameta_rename(struct pkg_ameta *a, const unsigned char *old, size_t ol,
                     const unsigned char *nw, size_t nl)
{
    struct pkg_ameta_entry *e;
    unsigned char *n;
    if (pkg_ameta_find(a, old, ol) == NULL)
        return 0;
    pkg_ameta_delete(a, nw, nl);              /* the new name takes the old entry */
    e = pkg_ameta_find(a, old, ol);
    n = (unsigned char *)malloc(nl ? nl : 1);
    if (n == NULL)
        return -1;
    if (nl)
        memcpy(n, nw, nl);
    free(e->name);
    e->name = n;
    e->name_len = nl;
    return 0;
}

int pkg_ameta_set_comment(struct pkg_ameta_entry *e, const unsigned char *c, size_t len)
{
    unsigned char *p = NULL;
    if (len) {
        p = (unsigned char *)malloc(len);
        if (p == NULL)
            return -1;
        memcpy(p, c, len);
    }
    free(e->comment);
    e->comment = p;
    e->comment_len = len;
    return 0;
}

static char *dupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p != NULL) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

int pkg_ameta_parse(const unsigned char *data, size_t len, struct pkg_ameta *a)
{
    size_t at = 0;
    unsigned line = 0;
    struct pkg_ameta_entry *cur = NULL;
    unsigned char *buf;

    if (len > MAX_FILE) {
        a->usable = 0;
        return report(a, 0, "file-too-large");
    }
    buf = (unsigned char *)malloc(len + 1);
    if (buf == NULL)
        return -1;
    while (at < len || line == 0) {
        const unsigned char *s = data + at;
        const unsigned char *nl = at < len ? (const unsigned char *)memchr(s, '\n', len - at) : NULL;
        size_t ll = nl ? (size_t)(nl - s) : len - at;
        const char *text = (const char *)s, *sp, *why = NULL;
        size_t kl, vl;

        if (at >= len && line == 0)
            ll = 0;
        at = nl ? at + ll + 1 : len;
        line++;
        if (ll > 0 && s[ll - 1] == '\r')
            ll--;
        if (line == 1) {
            if (ll != 7 || memcmp(s, "ameta 1", 7) != 0) {
                a->usable = 0;
                free(buf);
                return report(a, 1, "bad-header");
            }
            continue;
        }
        if (ll > MAX_LINE) { if (report(a, line, "line-too-long")) goto oom; continue; }
        if (!valid_utf8(s, ll)) { if (report(a, line, "not-utf8")) goto oom; continue; }
        sp = (const char *)memchr(text, ' ', ll);
        kl = sp ? (size_t)(sp - text) : ll;
        vl = sp ? ll - kl - 1 : 0;
        sp = sp ? sp + 1 : text + ll;
        if (kl == 4 && memcmp(text, "file", 4) == 0) {
            long n = pkg_ameta_unescape(sp, vl, buf);
            if (n < 0) why = "malformed-escape";
            else if (n == 0 || (n == 1 && buf[0] == '.') || (n == 2 && buf[0] == '.' && buf[1] == '.')
                     || memchr(buf, '/', (size_t)n) != NULL)
                why = "bad-name";
            else {
                struct pkg_ameta_entry *old = pkg_ameta_find(a, buf, (size_t)n);
                if (old != NULL) {
                    if (report(a, line, "duplicate")) goto oom;
                    entry_clear(old);        /* the later section replaces it whole */
                    cur = old;
                } else if ((cur = pkg_ameta_get(a, buf, (size_t)n)) == NULL) {
                    goto oom;
                }
            }
        } else if (cur == NULL) {
            why = "key-before-file";
        } else if (kl == 4 && memcmp(text, "prot", 4) == 0) {
            unsigned long long w;
            why = pkg_ameta_parse_prot(sp, vl, &w);
            if (why == NULL) { cur->prot = w; cur->has_prot = 1; }
        } else if (kl == 7 && memcmp(text, "comment", 7) == 0) {
            long n = pkg_ameta_unescape(sp, vl, buf);
            if (n < 0) why = "malformed-escape";
            else if (pkg_ameta_set_comment(cur, buf, (size_t)n)) goto oom;
        } else if (kl == 3 && (memcmp(text, "uid", 3) == 0 || memcmp(text, "gid", 3) == 0)) {
            unsigned long id;
            why = parse_id(sp, vl, &id);
            if (why == NULL) {
                if (text[0] == 'u') { cur->uid = id; cur->has_uid = 1; }
                else                { cur->gid = id; cur->has_gid = 1; }
            }
        } else {
            struct pkg_ameta_kv *kv = (struct pkg_ameta_kv *)realloc(cur->unknown,
                                           (cur->nunknown + 1) * sizeof *kv);
            if (kv == NULL) goto oom;
            cur->unknown = kv;
            kv[cur->nunknown].key = dupn(text, kl);
            kv[cur->nunknown].value = dupn(sp, vl);
            if (kv[cur->nunknown].key == NULL || kv[cur->nunknown].value == NULL) goto oom;
            cur->nunknown++;
        }
        if (why != NULL && report(a, line, why))
            goto oom;
    }
    free(buf);
    return 0;
oom:
    free(buf);
    return -1;
}

size_t pkg_ameta_mark_stale(struct pkg_ameta *a,
                            int (*present)(const unsigned char *, size_t, void *), void *ctx)
{
    size_t i, n = 0;
    for (i = 0; i < a->n; i++) {
        a->e[i].stale = !present(a->e[i].name, a->e[i].name_len, ctx);
        n += (size_t)a->e[i].stale;
    }
    return n;
}

static int is_default(const struct pkg_ameta_entry *e)
{
    return (e->prot & ~PKG_AMETA_RECORD_MASK) == 0 && e->comment_len == 0
           && !e->has_uid && !e->has_gid;
}

struct sorted { const struct pkg_ameta_entry *e; char *esc; };

static int by_escaped(const void *x, const void *y)
{
    return strcmp(((const struct sorted *)x)->esc, ((const struct sorted *)y)->esc);
}

int pkg_ameta_emit(const struct pkg_ameta *a, char **out, size_t *len)
{
    struct sorted *v = NULL;
    size_t i, n = 0, cap = 64, o = 0;
    char *b = NULL;
    int rc = -1;

    *out = NULL;
    *len = 0;
    if (a->n)
        v = (struct sorted *)calloc(a->n, sizeof *v);
    if (a->n && v == NULL)
        return -1;
    for (i = 0; i < a->n; i++) {
        const struct pkg_ameta_entry *e = &a->e[i];
        if (e->stale || (is_default(e) && e->nunknown == 0))
            continue;
        v[n].e = e;
        v[n].esc = (char *)malloc(3 * e->name_len + 1);
        if (v[n].esc == NULL) goto done;
        pkg_ameta_escape(e->name, e->name_len, v[n].esc);
        cap += strlen(v[n].esc) + 3 * e->comment_len + 64;
        for (o = 0; o < e->nunknown; o++)
            cap += strlen(e->unknown[o].key) + strlen(e->unknown[o].value) + 2;
        n++;
    }
    if (n == 0) { rc = 0; goto done; }
    qsort(v, n, sizeof *v, by_escaped);
    b = (char *)malloc(cap);
    if (b == NULL) goto done;
    o = (size_t)snprintf(b, cap, "ameta 1\n");
    for (i = 0; i < n; i++) {
        const struct pkg_ameta_entry *e = v[i].e;
        size_t k;
        o += (size_t)snprintf(b + o, cap - o, "file %s\n", v[i].esc);
        if (e->prot & ~PKG_AMETA_RECORD_MASK)
            o += (size_t)(e->prot <= 0xFFFFFFFFull ? snprintf(b + o, cap - o, "prot 0x%08llX\n", e->prot)
                                                   : snprintf(b + o, cap - o, "prot 0x%016llX\n", e->prot));
        if (e->comment_len) {
            memcpy(b + o, "comment ", 8);
            o += 8;
            pkg_ameta_escape(e->comment, e->comment_len, b + o);
            o += strlen(b + o);
            b[o++] = '\n';
        }
        if (e->has_uid) o += (size_t)snprintf(b + o, cap - o, "uid %lu\n", e->uid);
        if (e->has_gid) o += (size_t)snprintf(b + o, cap - o, "gid %lu\n", e->gid);
        for (k = 0; k < e->nunknown; k++)
            o += (size_t)snprintf(b + o, cap - o, "%s %s\n", e->unknown[k].key, e->unknown[k].value);
    }
    b[o] = '\0';                        /* a string too, for callers that print it */
    *out = b;
    *len = o;
    b = NULL;
    rc = 0;
done:
    for (i = 0; v != NULL && i < n; i++) free(v[i].esc);
    free(v);
    free(b);
    return rc;
}

unsigned long long pkg_ameta_publish_word(const struct pkg_ameta_entry *e, int host_has_mode,
                                          int host_executable)
{
    unsigned long long w = e != NULL && e->has_prot ? e->prot : 0;
    if (host_has_mode)
        w = (w & ~PKG_AMETA_OWNER_EXECUTE) | (host_executable ? 0 : PKG_AMETA_OWNER_EXECUTE);
    return w;
}
