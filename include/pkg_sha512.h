/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * SHA-512, FIPS 180-4. Needed by Ed25519, which is defined over it.
 */

#ifndef PKG_SHA512_H
#define PKG_SHA512_H

#include <stddef.h>
#include <stdint.h>

#define PKG_SHA512_LEN 64u

struct pkg_sha512 {
    uint64_t      h[8];
    uint64_t      total;
    unsigned char block[128];
    size_t        used;
};

void pkg_sha512_init(struct pkg_sha512 *c);
void pkg_sha512_update(struct pkg_sha512 *c, const void *data, size_t len);
void pkg_sha512_final(struct pkg_sha512 *c, unsigned char out[PKG_SHA512_LEN]);
void pkg_sha512(unsigned char out[PKG_SHA512_LEN], const void *data, size_t len);

#endif
