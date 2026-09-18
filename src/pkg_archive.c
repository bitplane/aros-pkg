/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* A tar reader over a byte source that is either the file itself or its
 * bzip2 decompression (third_party/bzip2, BZ_NO_STDIO). Handles ustar names
 * with their prefix, GNU long names ('L') and pax path records ('x'), and
 * streams concatenated bzip2 streams as the parallel compressors write
 * them. Portable C99. */

#include "pkg_archive.h"
#include "bzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libbzip2 without stdio asks the program what to do on an internal error. */
void bz_internal_error(int errcode)
{
    (void)errcode;
    abort();
}

#define INBUF  (1u << 16)

struct src {
    FILE          *f;
    int            bz;          /* 1: bzip2 */
    bz_stream      s;
    int            open;        /* a bzip2 stream is initialised */
    int            eof;
    unsigned char  in[INBUF];
    char          *err;
    size_t         errlen;
};

static int fail(struct src *r, const char *msg)
{
    if (r->errlen) snprintf(r->err, r->errlen, "%s", msg);
    return -1;
}

/* Fill exactly n bytes; 1 on success, 0 at a clean end before any byte, -1
 * on damage or a short end. */
static int get(struct src *r, unsigned char *out, size_t n)
{
    size_t got = 0;
    if (!r->bz) {
        got = fread(out, 1, n, r->f);
        if (got == n) return 1;
        return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
    }
    while (got < n) {
        int rc;
        if (r->s.avail_in == 0 && !r->eof) {
            size_t k = fread(r->in, 1, INBUF, r->f);
            if (k == 0) r->eof = 1;
            r->s.next_in = (char *)r->in;
            r->s.avail_in = (unsigned)k;
        }
        if (!r->open) {
            if (r->s.avail_in == 0 && r->eof)
                return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
            if (BZ2_bzDecompressInit(&r->s, 0, 0) != BZ_OK)
                return fail(r, "cannot start bzip2 decompression");
            r->open = 1;
        }
        r->s.next_out = (char *)out + got;
        r->s.avail_out = (unsigned)(n - got);
        rc = BZ2_bzDecompress(&r->s);
        got = n - r->s.avail_out;
        if (rc == BZ_STREAM_END) {
            /* another stream may follow: parallel compressors write several */
            BZ2_bzDecompressEnd(&r->s);
            r->open = 0;
            if (r->s.avail_in == 0 && r->eof && got < n)
                return got == 0 ? 0 : fail(r, "the archive ends in the middle of a record");
        } else if (rc != BZ_OK) {
            return fail(r, "the bzip2 data is damaged");
        } else if (r->s.avail_in == 0 && r->eof && r->s.avail_out > 0) {
            return fail(r, "the bzip2 data ends before its stream does");
        }
    }
    return 1;
}

static unsigned long long octal(const unsigned char *p, size_t n)
{
    unsigned long long v = 0;
    size_t i = 0;
    if (p[0] & 0x80) {                   /* GNU base-256 for large sizes */
        for (i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned)(p[i] - '0');
    return v;
}

static int skip(struct src *r, unsigned long long n)
{
    unsigned char buf[4096];
    while (n > 0) {
        size_t k = n > sizeof buf ? sizeof buf : (size_t)n;
        if (get(r, buf, k) != 1) return r->err[0] ? -1 : fail(r, "the archive is truncated");
        n -= k;
    }
    return 0;
}

/* The value of the "path" record of a pax header block, if any. */
static void pax_path(const unsigned char *b, size_t n, char *out, size_t ol)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && b[j] >= '0' && b[j] <= '9') len = len * 10 + (size_t)(b[j++] - '0');
        if (len == 0 || i + len > n || j >= n || b[j] != ' ') return;
        j++;
        if (i + len - j > 5 && memcmp(b + j, "path=", 5) == 0) {
            size_t vl = i + len - (j + 5) - 1;   /* without the newline */
            if (vl >= ol) vl = ol - 1;
            memcpy(out, b + j + 5, vl);
            out[vl] = '\0';
        }
        i += len;
    }
}

int pkg_archive_walk(const char *file, pkg_archive_want_fn want, pkg_archive_data_fn data,
                     void *ctx, char *err, size_t errlen)
{
    struct src *r = (struct src *)calloc(1, sizeof *r);
    unsigned char h[512], magic[3];
    char name[4096], longname[4096];
    int rc = -1, zeros = 0;

    if (errlen) err[0] = '\0';
    if (r == NULL) { if (errlen) snprintf(err, errlen, "out of memory"); return -1; }
    r->err = err;
    r->errlen = errlen;
    r->f = fopen(file, "rb");
    if (r->f == NULL) { fail(r, "cannot open the archive"); free(r); return -1; }
    if (fread(magic, 1, 3, r->f) == 3 && magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h')
        r->bz = 1;
    rewind(r->f);
    longname[0] = '\0';
    for (;;) {
        struct pkg_archive_entry e;
        unsigned long long size, pad;
        int g = get(r, h, sizeof h), w;
        char type;
        if (g == 0) { rc = 0; break; }                 /* no end blocks: accept */
        if (g < 0) break;
        if (h[0] == '\0') {                              /* two zero blocks end it */
            if (++zeros == 2) { rc = 0; break; }
            continue;
        }
        zeros = 0;
        {
            unsigned long long sum = 0, want_sum = octal(h + 148, 8);
            int k;
            for (k = 0; k < 512; k++) sum += (k >= 148 && k < 156) ? 32u : h[k];
            if (sum != want_sum) { fail(r, "a tar header has a bad checksum"); break; }
        }
        type = (char)h[156];
        size = octal(h + 124, 12);
        pad = (512 - size % 512) % 512;
        if (type == 'L' || type == 'x' || type == 'g') {
            unsigned char *b;
            if (size > 65536) { fail(r, "an extended tar header is too long"); break; }
            b = (unsigned char *)malloc((size_t)size + 1);
            if (b == NULL || get(r, b, (size_t)size) != 1 || skip(r, pad) != 0) {
                free(b);
                if (!err[0]) fail(r, "the archive is truncated");
                break;
            }
            b[size] = '\0';
            if (type == 'L') snprintf(longname, sizeof longname, "%s", (const char *)b);
            else if (type == 'x') pax_path(b, (size_t)size, longname, sizeof longname);
            free(b);
            continue;
        }
        if (longname[0]) {
            snprintf(name, sizeof name, "%s", longname);
            longname[0] = '\0';
        } else if (memcmp(h + 257, "ustar", 5) == 0 && h[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        } else {
            snprintf(name, sizeof name, "%.100s", (const char *)h);
        }
        e.path = name;
        e.size = size;
        e.mode = (unsigned)octal(h + 100, 8);
        e.is_dir = type == '5';
        if (type != '0' && type != '\0' && type != '5' && type != '7') {
            /* links, devices and the like: named, never delivered */
            if (skip(r, size + pad) != 0) break;
            continue;
        }
        if (e.is_dir) { size_t l = strlen(name); if (l && name[l - 1] == '/') name[l - 1] = '\0'; }
        w = want ? want(&e, ctx) : 0;
        if (w < 0) { if (errlen) err[0] = '\0'; break; }
        if (w == 1 && !e.is_dir) {
            unsigned char buf[16384];
            unsigned long long left = size;
            int stop = 0;
            while (left > 0) {
                size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
                if (get(r, buf, k) != 1) { if (!err[0]) fail(r, "the archive is truncated"); stop = 1; break; }
                if (data && data(&e, buf, k, ctx) != 0) { stop = 2; break; }
                left -= k;
            }
            if (stop == 1) break;
            if (stop == 2) { if (errlen) err[0] = '\0'; break; }
            if (data && data(&e, buf, 0, ctx) != 0) { if (errlen) err[0] = '\0'; break; }
            if (skip(r, pad) != 0) break;
        } else if (skip(r, size + pad) != 0) {
            break;
        }
    }
    if (r->open) BZ2_bzDecompressEnd(&r->s);
    fclose(r->f);
    free(r);
    return rc;
}

int pkg_archive_split(const char *s, char *archive, size_t al, char *inner, size_t il)
{
    const char *bang = strstr(s, "!/");
    size_t n;
    if (bang == NULL)
        return 0;
    n = (size_t)(bang - s);
    if (n == 0 || n >= al) return 0;
    memcpy(archive, s, n);
    archive[n] = '\0';
    snprintf(inner, il, "%s", bang + 2);
    n = strlen(inner);
    while (n > 0 && inner[n - 1] == '/') inner[--n] = '\0';
    return 1;
}
