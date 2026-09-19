/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- capture ---------------------------------------------------------- */

struct cap { char *p; size_t len, cap; };
static int capturing;
static struct cap cap_out, cap_err;

static void cap_add(struct cap *c, const char *buf, size_t len)
{
    if (c->len + len + 1u > c->cap) {
        size_t n = c->cap ? c->cap : 256u;
        char *q;
        while (n < c->len + len + 1u) n *= 2u;
        q = (char *)realloc(c->p, n);
        if (q == NULL) return;
        c->p = q;
        c->cap = n;
    }
    memcpy(c->p + c->len, buf, len);
    c->len += len;
    c->p[c->len] = '\0';
}

static void cap_vadd(struct cap *c, const char *fmt, va_list ap)
{
    char small[1024];
    va_list cp;
    int n;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0) return;
    if ((size_t)n < sizeof small) {
        cap_add(c, small, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1u);
        if (big == NULL) return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
        cap_add(c, big, (size_t)n);
        free(big);
    }
}

void pkg_capture_begin(void)
{
    memset(&cap_out, 0, sizeof cap_out);
    memset(&cap_err, 0, sizeof cap_err);
    capturing = 1;
}

void pkg_capture_end(char **out, size_t *out_len, char **err, size_t *err_len)
{
    capturing = 0;
    *out = cap_out.p; *out_len = cap_out.len;
    *err = cap_err.p; *err_len = cap_err.len;
    memset(&cap_out, 0, sizeof cap_out);
    memset(&cap_err, 0, sizeof cap_err);
}

#ifdef __AROS__
#include <proto/dos.h>

static void emit(const char *buf, size_t len)
{
    BPTR out = Output();
    if (out != BNULL && len > 0)
        Write(out, (APTR)buf, (LONG)len);
}

int pkg_out_interactive(int is_error)
{
    BPTR out = Output();
    (void)is_error;                  /* both streams are Output() here */
    return out != BNULL && IsInteractive(out);
}

static void vemit(const char *fmt, va_list ap)
{
    char small[1024];
    va_list cp;
    int n;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0)
        return;
    if ((size_t)n < sizeof small) {
        emit(small, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1u);
        if (big == NULL)
            return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
        emit(big, (size_t)n);
        free(big);
    }
}

void pkg_out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (capturing) cap_vadd(&cap_out, fmt, ap); else vemit(fmt, ap);
    va_end(ap);
}

void pkg_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (capturing) cap_vadd(&cap_err, fmt, ap); else vemit(fmt, ap);
    va_end(ap);
}

void pkg_verr(const char *fmt, va_list ap)
{
    if (capturing) cap_vadd(&cap_err, fmt, ap); else vemit(fmt, ap);
}

void pkg_outraw(const char *buf, size_t len)
{
    if (capturing) cap_add(&cap_out, buf, len); else emit(buf, len);
}

#else

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define isatty _isatty
#else
#include <unistd.h>
#endif

int pkg_out_interactive(int is_error)
{
    return isatty(is_error ? 2 : 1);
}

#ifdef _WIN32
/* Binary mode, so "\n" stays one byte: the machine contract is the same
 * bytes on every host, and text mode would turn each newline into CR LF. */
static void binary_once(void)
{
    static int done;
    if (!done) {
        done = 1;
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
    }
}
#else
static void binary_once(void) { }
#endif

void pkg_out(const char *fmt, ...)
{
    va_list ap;
    binary_once();
    va_start(ap, fmt);
    if (capturing) cap_vadd(&cap_out, fmt, ap); else vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void pkg_err(const char *fmt, ...)
{
    va_list ap;
    binary_once();
    va_start(ap, fmt);
    if (capturing) cap_vadd(&cap_err, fmt, ap); else vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void pkg_verr(const char *fmt, va_list ap)
{
    binary_once();
    if (capturing) cap_vadd(&cap_err, fmt, ap); else vfprintf(stderr, fmt, ap);
}

void pkg_outraw(const char *buf, size_t len)
{
    binary_once();
    if (capturing) cap_add(&cap_out, buf, len); else fwrite(buf, 1, len, stdout);
}

#endif
