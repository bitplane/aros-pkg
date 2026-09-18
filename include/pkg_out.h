/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Where the tool's words go. Under POSIX, results on stdout and refusals on
 * stderr. Under AmigaDOS both go to Output(), through dos.library, because
 * that is the stream a shell redirection (`>file`) captures and the stream
 * every AmigaDOS command reports on: the first hosted run showed the C
 * library's stdout arriving nowhere a redirection could see it.
 */

#ifndef PKG_OUT_H
#define PKG_OUT_H

#include <stdarg.h>
#include <stddef.h>

void pkg_out(const char *fmt, ...);
void pkg_err(const char *fmt, ...);
void pkg_verr(const char *fmt, va_list ap);
void pkg_outraw(const char *buf, size_t len);

/* Capture, for the ARexx port: between begin and end, results and refusals are
 * kept in memory instead of written, so a command's output can become RESULT
 * and its refusal the text LASTERROR returns. end hands both buffers to the
 * caller, who frees them; either may be NULL when nothing was written. */
void pkg_capture_begin(void);
void pkg_capture_end(char **out, size_t *out_len, char **err, size_t *err_len);

#endif
