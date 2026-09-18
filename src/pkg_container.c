/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_container.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The stream caps every length at what a big-endian LONG can carry. */
#define PKG_MAX_LEN 0xFFFFFFFFu

const char *pkg_strstatus(enum pkg_status s)
{
    switch (s) {
    case PKG_OK:           return "ok";
    case PKG_E_MAGIC:      return "not a PKG stream: the magic is not 'PKG'";
    case PKG_E_VERSION:    return "unsupported PKG version";
    case PKG_E_SIZE:       return "packageSize disagrees with the stream length";
    case PKG_E_TRUNCATED:  return "entry runs past the end of the stream";
    case PKG_E_PATH_TERM:  return "path is not NUL-terminated at its stated length";
    case PKG_E_PATH_EMPTY: return "path length is zero";
    case PKG_E_OVERFLOW:   return "length overflows";
    case PKG_E_NOMEM:      return "out of memory";
    case PKG_E_STOPPED:    return "stopped by the caller";
    }
    return "unknown status";
}

unsigned long pkg_be32_get(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

void pkg_be32_put(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFFu);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8)  & 0xFFu);
    p[3] = (unsigned char)( v        & 0xFFu);
}

static uint32_t be32_get(const unsigned char *p)
{
    return (uint32_t)pkg_be32_get(p);
}

static void be32_put(unsigned char *p, uint32_t v)
{
    pkg_be32_put(p, (unsigned long)v);
}

/* ---- reading ---------------------------------------------------------- */

enum pkg_status pkg_read(const unsigned char *buf, size_t len,
                         pkg_entry_fn fn, void *ctx, int *stopped_at)
{
    size_t at;

    if (stopped_at)
        *stopped_at = 0;
    if (buf == NULL || len < PKG_HEADER_SIZE)
        return PKG_E_TRUNCATED;
    if (buf[0] != 'P' || buf[1] != 'K' || buf[2] != 'G')
        return PKG_E_MAGIC;
    if (buf[3] != PKG_VERSION)
        return PKG_E_VERSION;
    if (be32_get(buf + 4) != (uint32_t)len || len > PKG_MAX_LEN)
        return PKG_E_SIZE;

    at = PKG_HEADER_SIZE;
    while (at < len) {
        uint32_t path_len, data_len;
        size_t need;
        struct pkg_entry e;

        /* pathLength */
        if (len - at < 4u)
            return PKG_E_TRUNCATED;
        path_len = be32_get(buf + at);
        at += 4u;
        if (path_len == 0u)
            return PKG_E_PATH_EMPTY;
        if (path_len > PKG_MAX_LEN - 1u)
            return PKG_E_OVERFLOW;

        /* path, with its trailing NUL */
        need = (size_t)path_len + 1u;
        if (len - at < need)
            return PKG_E_TRUNCATED;
        if (buf[at + path_len] != 0u)
            return PKG_E_PATH_TERM;
        e.path     = (const char *)(buf + at);
        e.path_len = (size_t)path_len;
        at += need;

        /* dataLength */
        if (len - at < 4u)
            return PKG_E_TRUNCATED;
        data_len = be32_get(buf + at);
        at += 4u;

        /* data */
        if (len - at < (size_t)data_len)
            return PKG_E_TRUNCATED;
        e.data     = data_len ? buf + at : NULL;
        e.data_len = (size_t)data_len;
        at += (size_t)data_len;

        if (fn != NULL) {
            int rc = fn(&e, ctx);
            if (rc != 0) {
                if (stopped_at)
                    *stopped_at = rc;
                return PKG_E_STOPPED;
            }
        }
    }
    return PKG_OK;
}

/* ---- writing ---------------------------------------------------------- */

struct pkg_writer {
    unsigned char *buf;
    size_t         len;
    size_t         cap;
    int            failed;   /* sticky, so a caller may add then check once */
};

static int wr_reserve(struct pkg_writer *w, size_t extra)
{
    size_t want;
    unsigned char *grown;

    if (extra > (size_t)-1 - w->len)
        return 0;
    want = w->len + extra;
    if (want <= w->cap)
        return 1;
    while (w->cap < want) {
        size_t next = w->cap ? w->cap * 2u : 256u;
        if (next < w->cap)
            return 0;
        w->cap = next;
    }
    grown = (unsigned char *)realloc(w->buf, w->cap);
    if (grown == NULL)
        return 0;
    w->buf = grown;
    return 1;
}

static int wr_bytes(struct pkg_writer *w, const void *p, size_t n)
{
    if (n == 0u)
        return 1;
    if (!wr_reserve(w, n))
        return 0;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 1;
}

static int wr_be32(struct pkg_writer *w, uint32_t v)
{
    unsigned char tmp[4];
    be32_put(tmp, v);
    return wr_bytes(w, tmp, sizeof tmp);
}

struct pkg_writer *pkg_writer_new(void)
{
    struct pkg_writer *w = (struct pkg_writer *)calloc(1, sizeof *w);
    unsigned char head[PKG_HEADER_SIZE];

    if (w == NULL)
        return NULL;
    head[0] = 'P'; head[1] = 'K'; head[2] = 'G';
    head[3] = (unsigned char)PKG_VERSION;
    be32_put(head + 4, 0u);            /* patched by pkg_writer_finish */
    if (!wr_bytes(w, head, sizeof head)) {
        pkg_writer_free(w);
        return NULL;
    }
    return w;
}

void pkg_writer_free(struct pkg_writer *w)
{
    if (w == NULL)
        return;
    free(w->buf);
    free(w);
}

enum pkg_status pkg_writer_add(struct pkg_writer *w, const char *path,
                               const unsigned char *data, size_t data_len)
{
    size_t path_len;

    if (w == NULL)
        return PKG_E_NOMEM;
    if (w->failed)
        return (enum pkg_status)w->failed;

    if (path == NULL)
        return (enum pkg_status)(w->failed = PKG_E_PATH_EMPTY);
    path_len = strlen(path);
    if (path_len == 0u)
        return (enum pkg_status)(w->failed = PKG_E_PATH_EMPTY);
    if (path_len > PKG_MAX_LEN - 1u || data_len > PKG_MAX_LEN)
        return (enum pkg_status)(w->failed = PKG_E_OVERFLOW);

    if (!wr_be32(w, (uint32_t)path_len)
     || !wr_bytes(w, path, path_len + 1u)   /* the trailing NUL travels */
     || !wr_be32(w, (uint32_t)data_len)
     || !wr_bytes(w, data, data_len))
        return (enum pkg_status)(w->failed = PKG_E_NOMEM);

    return PKG_OK;
}

enum pkg_status pkg_writer_finish(struct pkg_writer *w,
                                  unsigned char **out, size_t *out_len)
{
    if (w == NULL || out == NULL || out_len == NULL)
        return PKG_E_NOMEM;
    if (w->failed)
        return (enum pkg_status)w->failed;
    if (w->len > PKG_MAX_LEN)
        return PKG_E_OVERFLOW;

    be32_put(w->buf + 4, (uint32_t)w->len);
    *out     = w->buf;
    *out_len = w->len;
    w->buf   = NULL;               /* ownership moves to the caller */
    w->len   = 0u;
    w->cap   = 0u;
    return PKG_OK;
}

