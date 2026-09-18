/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * SHA-256, FIPS 180-4. Byte-wise, host-order free, like the container.
 */

#ifndef PKG_SHA256_H
#define PKG_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define PKG_SHA256_LEN     32u
#define PKG_SHA256_HEXLEN  64u

struct pkg_sha256 {
    uint32_t      h[8];
    uint64_t      total;     /* bytes hashed so far */
    unsigned char block[64];
    size_t        used;      /* bytes waiting in block */
};

void pkg_sha256_init(struct pkg_sha256 *c);
void pkg_sha256_update(struct pkg_sha256 *c, const void *data, size_t len);
void pkg_sha256_final(struct pkg_sha256 *c, unsigned char out[PKG_SHA256_LEN]);

/* One call, lowercase hex, NUL-terminated: hex needs 65 bytes. */
void pkg_sha256_hex(const void *data, size_t len, char hex[PKG_SHA256_HEXLEN + 1]);
void pkg_sha256_tohex(const unsigned char d[PKG_SHA256_LEN],
                      char hex[PKG_SHA256_HEXLEN + 1]);

#endif
