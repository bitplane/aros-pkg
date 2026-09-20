/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

/* TLS for AROS. Every other system Pkg runs on has a program or a library
 * that speaks https already (curl, WinHTTP); AROS has neither, so the AROS
 * builds carry Mbed TLS and this is the seam between it and Pkg's own HTTP
 * client. A session belongs to one socket and is handed back as a handle, so
 * the client can hold a connection open for the next request and hold one to
 * a second host beside it. What the sessions share, the authorities and the
 * random source, is read and seeded once per process. */

#ifndef PKG_TLS_H
#define PKG_TLS_H

#include <stddef.h>
#include <sys/types.h>

struct pkg_tls;

/* Reads the authorities and seeds the random source, once per process; the
 * first handshake would do it anyway, and this lets the caller see what that
 * one-time work costs. 0, or -1 with a sentence in `err`. */
int pkg_tls_prepare(char *err, size_t errlen);

/* The handshake on an open socket. `host` is the name from the address: it
 * is sent as the server name and the certificate is checked against it, on
 * this connection and no other. The authorities are PKG_CAFILE, or the
 * bundle built into the program. NULL and a sentence in `err` on a refusal. */
struct pkg_tls *pkg_tls_open(int sock, const char *host, char *err, size_t errlen);

ssize_t pkg_tls_read(struct pkg_tls *t, void *buf, size_t n);
ssize_t pkg_tls_write(struct pkg_tls *t, const void *buf, size_t n);

/* Ends the session and frees the handle; the socket itself is the caller's
 * to close. */
void pkg_tls_close(struct pkg_tls *t);

#endif
