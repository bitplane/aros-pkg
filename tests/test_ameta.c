/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* Runs every case of the .ameta reference corpus (tests/ameta-corpus, or the
 * directory given as the argument): read, publish word, operation, write. */

#define _POSIX_C_SOURCE 200809L
#include "pkg_ameta.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed, cases;

struct buf { char *p; size_t n, cap; };

static void put(struct buf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        b->cap = (b->n + l + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static unsigned char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    unsigned char *p;
    long n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) { *len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (unsigned char *)malloc((size_t)n + 1);
    *len = fread(p, 1, (size_t)n, f);
    p[*len] = '\0';
    fclose(f);
    return p;
}

/* The names of a list file, one escaped name per line, unescaped. */
struct names { unsigned char *v[64]; size_t len[64]; size_t n; int given; };

static void load_names(const char *dir, const char *file, struct names *ns)
{
    size_t len;
    char *t = (char *)slurp(dir, file, &len), *line, *save = NULL;
    ns->n = 0;
    ns->given = t != NULL;
    if (t == NULL) return;
    for (line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        ns->v[ns->n] = (unsigned char *)malloc(strlen(line) + 1);
        ns->len[ns->n] = (size_t)pkg_ameta_unescape(line, strlen(line), ns->v[ns->n]);
        ns->n++;
    }
    free(t);
}

static int in_names(const unsigned char *name, size_t len, void *ctx)
{
    const struct names *ns = (const struct names *)ctx;
    size_t i;
    for (i = 0; i < ns->n; i++)
        if (ns->len[i] == len && memcmp(ns->v[i], name, len) == 0)
            return 1;
    return 0;
}

struct row { char esc[800]; const struct pkg_ameta_entry *e; };

static int by_esc(const void *a, const void *b)
{
    return strcmp(((const struct row *)a)->esc, ((const struct row *)b)->esc);
}

static void fmt_prot(char *out, unsigned long long w)
{
    if (w <= 0xFFFFFFFFull) sprintf(out, "0x%08llX", w);
    else sprintf(out, "0x%016llX", w);
}

static void describe(struct pkg_ameta *a, struct buf *b)
{
    struct row rows[64];
    size_t i, n = 0, pass;
    char tmp[1200];
    if (!a->usable) {
        for (i = 0; i < a->nr; i++) {
            snprintf(tmp, sizeof tmp, "ignored-file %s\n", a->r[i].reason);
            put(b, tmp);
        }
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        n = 0;
        for (i = 0; i < a->n; i++)
            if (a->e[i].stale == (int)pass) {
                pkg_ameta_escape(a->e[i].name, a->e[i].name_len, rows[n].esc);
                rows[n++].e = &a->e[i];
            }
        qsort(rows, n, sizeof rows[0], by_esc);
        for (i = 0; i < n; i++) {
            const struct pkg_ameta_entry *e = rows[i].e;
            if (pass == 1) {
                snprintf(tmp, sizeof tmp, "stale %s\n", rows[i].esc);
                put(b, tmp);
                continue;
            }
            put(b, "entry ");
            put(b, rows[i].esc);
            if (e->has_prot) { char w[24]; fmt_prot(w, e->prot); put(b, " prot "); put(b, w); }
            if (e->comment_len) {
                char c[800];
                pkg_ameta_escape(e->comment, e->comment_len, c);
                put(b, " comment ");
                put(b, c);
            }
            if (e->has_uid) { snprintf(tmp, sizeof tmp, " uid %lu", e->uid); put(b, tmp); }
            if (e->has_gid) { snprintf(tmp, sizeof tmp, " gid %lu", e->gid); put(b, tmp); }
            put(b, "\n");
        }
    }
    for (i = 0; i < a->nr; i++) {
        snprintf(tmp, sizeof tmp, "report %u %s\n", a->r[i].line, a->r[i].reason);
        put(b, tmp);
    }
}

static void apply(struct pkg_ameta *a, char *ops)
{
    char *line, *save = NULL;
    unsigned char n1[512], n2[512], v[1024];
    for (line = strtok_r(ops, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char w0[16], w1[512], w2[512];
        int got = sscanf(line, "%15s %511s %511s", w0, w1, w2);
        long l1 = got >= 2 ? pkg_ameta_unescape(w1, strlen(w1), n1) : 0;
        if (strcmp(w0, "set") == 0 && got == 3) {
            struct pkg_ameta_entry *e = pkg_ameta_get(a, n1, (size_t)l1);
            const char *val = strstr(line, w2);
            val = strchr(val, ' ');
            val = val ? val + 1 : "";
            if (strcmp(w2, "prot") == 0) {
                pkg_ameta_parse_prot(val, strlen(val), &e->prot);
                e->has_prot = 1;
            } else if (strcmp(w2, "comment") == 0) {
                long cl = pkg_ameta_unescape(val, strlen(val), v);
                pkg_ameta_set_comment(e, v, (size_t)cl);
            } else if (strcmp(w2, "uid") == 0) {
                e->uid = strtoul(val, NULL, 10); e->has_uid = 1;
            } else {
                e->gid = strtoul(val, NULL, 10); e->has_gid = 1;
            }
        } else if (strcmp(w0, "delete") == 0) {
            pkg_ameta_delete(a, n1, (size_t)l1);
        } else if (strcmp(w0, "rename") == 0 && got == 3) {
            long l2 = pkg_ameta_unescape(w2, strlen(w2), n2);
            pkg_ameta_rename(a, n1, (size_t)l1, n2, (size_t)l2);
        }
    }
}

static int same(const char *what, const char *expected, const char *got, const char *cs)
{
    if (strcmp(expected ? expected : "", got ? got : "") == 0)
        return 1;
    printf("  FAIL %s: %s\n--- expected\n%s--- got\n%s", cs, what, expected ? expected : "",
           got ? got : "");
    return 0;
}

static void run(const char *dir, const char *name)
{
    char cs[1024];
    size_t len;
    unsigned char *in;
    char *ops, *expected_read, *expected_pub, *expected_ameta, *outcome;
    struct pkg_ameta a;
    struct names present, exec;
    struct buf got = { NULL, 0, 0 };
    int ok = 1;

    snprintf(cs, sizeof cs, "%s/%s", dir, name);
    cases++;
    in = slurp(cs, "input.ameta", &len);
    load_names(cs, "entries", &present);
    pkg_ameta_init(&a);
    if (in != NULL && len > 0) {
        pkg_ameta_parse(in, len, &a);
        if (a.usable) pkg_ameta_mark_stale(&a, in_names, &present);
    }
    put(&got, "");
    describe(&a, &got);
    expected_read = (char *)slurp(cs, "read", &len);
    ok &= same("read", expected_read, got.p, name);

    expected_pub = (char *)slurp(cs, "publish", &len);
    if (expected_pub != NULL) {
        struct buf pb = { NULL, 0, 0 };
        struct row rows[64];
        size_t i;
        load_names(cs, "host-executable", &exec);
        for (i = 0; i < present.n; i++) {
            pkg_ameta_escape(present.v[i], present.len[i], rows[i].esc);
            rows[i].e = a.usable ? pkg_ameta_find(&a, present.v[i], present.len[i]) : NULL;
        }
        qsort(rows, present.n, sizeof rows[0], by_esc);
        put(&pb, "");
        for (i = 0; i < present.n; i++) {
            unsigned char raw[512];
            long rl = pkg_ameta_unescape(rows[i].esc, strlen(rows[i].esc), raw);
            char w[24], line[900];
            fmt_prot(w, pkg_ameta_publish_word(rows[i].e, exec.given,
                                               in_names(raw, (size_t)rl, &exec)));
            snprintf(line, sizeof line, "word %s %s\n", rows[i].esc, w);
            put(&pb, line);
        }
        ok &= same("publish", expected_pub, pb.p, name);
        free(pb.p);
        for (i = 0; i < exec.n; i++) free(exec.v[i]);
    }

    ops = (char *)slurp(cs, "operation", &len);
    if (ops != NULL) {
        char *text = NULL;
        size_t tl;
        const char *result;
        expected_ameta = (char *)slurp(cs, "expected.ameta", &len);
        outcome = (char *)slurp(cs, "outcome", &len);
        if (!a.usable) {
            result = "refuse\n";
        } else {
            apply(&a, ops);
            pkg_ameta_emit(&a, &text, &tl);
            result = text ? "write\n" : "delete\n";
        }
        ok &= same("outcome", outcome ? outcome : "write\n", result, name);
        if (text != NULL)
            ok &= same("written file", expected_ameta, text, name);
        free(text);
        free(expected_ameta);
        free(outcome);
        free(ops);
    }
    printf("%s  %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
    {
        size_t i;
        for (i = 0; i < present.n; i++) free(present.v[i]);
    }
    free(in); free(expected_read); free(expected_pub); free(got.p);
    pkg_ameta_free(&a);
}

static int by_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/ameta-corpus/cases";
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[128];
    size_t n = 0, i;
    if (d == NULL) { printf("no corpus at %s\n", dir); return 1; }
    while ((e = readdir(d)) != NULL && n < 128)
        if (e->d_name[0] != '.')
            names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, n, sizeof names[0], by_str);
    for (i = 0; i < n; i++) { run(dir, names[i]); free(names[i]); }
    printf("%d cases, %d failures\n", cases, failed);
    return failed != 0 || cases == 0;
}
