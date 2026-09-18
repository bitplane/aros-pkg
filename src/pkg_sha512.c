/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_sha512.h"

#include <string.h>

static const uint64_t K[80] = {
    0x428a2f98d728ae22u, 0x7137449123ef65cdu, 0xb5c0fbcfec4d3b2fu, 0xe9b5dba58189dbbcu,
    0x3956c25bf348b538u, 0x59f111f1b605d019u, 0x923f82a4af194f9bu, 0xab1c5ed5da6d8118u,
    0xd807aa98a3030242u, 0x12835b0145706fbeu, 0x243185be4ee4b28cu, 0x550c7dc3d5ffb4e2u,
    0x72be5d74f27b896fu, 0x80deb1fe3b1696b1u, 0x9bdc06a725c71235u, 0xc19bf174cf692694u,
    0xe49b69c19ef14ad2u, 0xefbe4786384f25e3u, 0x0fc19dc68b8cd5b5u, 0x240ca1cc77ac9c65u,
    0x2de92c6f592b0275u, 0x4a7484aa6ea6e483u, 0x5cb0a9dcbd41fbd4u, 0x76f988da831153b5u,
    0x983e5152ee66dfabu, 0xa831c66d2db43210u, 0xb00327c898fb213fu, 0xbf597fc7beef0ee4u,
    0xc6e00bf33da88fc2u, 0xd5a79147930aa725u, 0x06ca6351e003826fu, 0x142929670a0e6e70u,
    0x27b70a8546d22ffcu, 0x2e1b21385c26c926u, 0x4d2c6dfc5ac42aedu, 0x53380d139d95b3dfu,
    0x650a73548baf63deu, 0x766a0abb3c77b2a8u, 0x81c2c92e47edaee6u, 0x92722c851482353bu,
    0xa2bfe8a14cf10364u, 0xa81a664bbc423001u, 0xc24b8b70d0f89791u, 0xc76c51a30654be30u,
    0xd192e819d6ef5218u, 0xd69906245565a910u, 0xf40e35855771202au, 0x106aa07032bbd1b8u,
    0x19a4c116b8d2d0c8u, 0x1e376c085141ab53u, 0x2748774cdf8eeb99u, 0x34b0bcb5e19b48a8u,
    0x391c0cb3c5c95a63u, 0x4ed8aa4ae3418acbu, 0x5b9cca4f7763e373u, 0x682e6ff3d6b2b8a3u,
    0x748f82ee5defb2fcu, 0x78a5636f43172f60u, 0x84c87814a1f0ab72u, 0x8cc702081a6439ecu,
    0x90befffa23631e28u, 0xa4506cebde82bde9u, 0xbef9a3f7b2c67915u, 0xc67178f2e372532bu,
    0xca273eceea26619cu, 0xd186b8c721c0c207u, 0xeada7dd6cde0eb1eu, 0xf57d4f7fee6ed178u,
    0x06f067aa72176fbau, 0x0a637dc5a2c898a6u, 0x113f9804bef90daeu, 0x1b710b35131c471bu,
    0x28db77f523047d84u, 0x32caab7b40c72493u, 0x3c9ebe0a15c9bebcu, 0x431d67c49c100d4cu,
    0x4cc5d4becb3e42b6u, 0x597f299cfc657e2au, 0x5fcb6fab3ad6faecu, 0x6c44198c4a475817u
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void compress(struct pkg_sha512 *c, const unsigned char *p)
{
    uint64_t w[80], a, b, cc, d, e, f, g, h;
    int i, j;

    for (i = 0; i < 16; i++) {
        w[i] = 0;
        for (j = 0; j < 8; j++)
            w[i] = (w[i] << 8) | (uint64_t)p[8 * i + j];
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 80; i++) {
        uint64_t S1 = ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void pkg_sha512_init(struct pkg_sha512 *c)
{
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908u, 0xbb67ae8584caa73bu, 0x3c6ef372fe94f82bu,
        0xa54ff53a5f1d36f1u, 0x510e527fade682d1u, 0x9b05688c2b3e6c1fu,
        0x1f83d9abfb41bd6bu, 0x5be0cd19137e2179u
    };
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->used = 0;
}

void pkg_sha512_update(struct pkg_sha512 *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->total += (uint64_t)len;
    while (len > 0) {
        size_t take = 128u - c->used;
        if (take > len)
            take = len;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used == 128u) {
            compress(c, c->block);
            c->used = 0;
        }
    }
}

void pkg_sha512_final(struct pkg_sha512 *c, unsigned char out[PKG_SHA512_LEN])
{
    uint64_t bits = c->total * 8u;
    unsigned char pad = 0x80, zero = 0, lenb[16];
    int i, j;

    pkg_sha512_update(c, &pad, 1);
    while (c->used != 112u)
        pkg_sha512_update(c, &zero, 1);
    memset(lenb, 0, 8);            /* the high 64 bits of the 128-bit length */
    for (i = 0; i < 8; i++)
        lenb[8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    pkg_sha512_update(c, lenb, 16);
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            out[8 * i + j] = (unsigned char)(c->h[i] >> (56 - 8 * j));
}

void pkg_sha512(unsigned char out[PKG_SHA512_LEN], const void *data, size_t len)
{
    struct pkg_sha512 c;
    pkg_sha512_init(&c);
    pkg_sha512_update(&c, data, len);
    pkg_sha512_final(&c, out);
}
