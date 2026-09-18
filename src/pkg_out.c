/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_out.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __AROS__
#include <proto/dos.h>

static void emit(const char *buf, size_t len)
{
    BPTR out = Output();
    if (out != BNULL && len > 0)
        Write(out, (APTR)buf, (LONG)len);
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
    vemit(fmt, ap);
    va_end(ap);
}

void pkg_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vemit(fmt, ap);
    va_end(ap);
}

void pkg_verr(const char *fmt, va_list ap)
{
    vemit(fmt, ap);
}

void pkg_outraw(const char *buf, size_t len)
{
    emit(buf, len);
}

#else

void pkg_out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void pkg_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void pkg_verr(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
}

void pkg_outraw(const char *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
}

#endif
