/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Ed25519, RFC 8032, pure variant. Written in the compact form of TweetNaCl
 * (public domain) so that one maintainer can read the whole of it, with two
 * changes: no left shift of a negative value anywhere, since that is undefined
 * in C99 and the test suite runs under UBSan; and verification refuses a
 * signature whose S is not below the group order L, which RFC 8032 requires
 * and TweetNaCl omits, so a signature cannot be altered into a second valid
 * one.
 *
 * Not constant-time against a local attacker measuring cache behaviour; the
 * signing key lives on the developer's machine and signing runs there.
 */

#ifndef PKG_ED25519_H
#define PKG_ED25519_H

#include <stddef.h>

#define PKG_ED25519_SEED   32u
#define PKG_ED25519_PUBLIC 32u
#define PKG_ED25519_SECRET 64u   /* seed followed by the public key */
#define PKG_ED25519_SIG    64u

void pkg_ed25519_keypair(unsigned char pk[PKG_ED25519_PUBLIC],
                         unsigned char sk[PKG_ED25519_SECRET],
                         const unsigned char seed[PKG_ED25519_SEED]);

void pkg_ed25519_sign(unsigned char sig[PKG_ED25519_SIG],
                      const unsigned char *msg, size_t len,
                      const unsigned char sk[PKG_ED25519_SECRET]);

/* 0 when the signature is valid, -1 otherwise. */
int pkg_ed25519_verify(const unsigned char sig[PKG_ED25519_SIG],
                       const unsigned char *msg, size_t len,
                       const unsigned char pk[PKG_ED25519_PUBLIC]);

#endif
