/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

/* TLS for AROS. Every other system Pkg runs on has a program or a library
 * that speaks https already (curl, WinHTTP); AROS has neither, so the AROS
 * builds carry Mbed TLS and this is the seam between it and Pkg's own HTTP
 * client. One session at a time: Pkg opens a connection for one transfer and
 * closes it after. */

#ifndef PKG_TLS_H
#define PKG_TLS_H

#include <stddef.h>
#include <sys/types.h>

/* The handshake on an open socket. `host` is the name from the address: it
 * is sent as the server name and the certificate is checked against it. The
 * authorities are PKG_CAFILE, or the bundle built into the program. */
int pkg_tls_open(int sock, const char *host, char *err, size_t errlen);

ssize_t pkg_tls_read(void *buf, size_t n);
ssize_t pkg_tls_write(const void *buf, size_t n);

/* Ends the session; the socket itself is the caller's to close. */
void pkg_tls_close(void);

/* Non-zero between pkg_tls_open and pkg_tls_close. */
int pkg_tls_active(void);

#endif
