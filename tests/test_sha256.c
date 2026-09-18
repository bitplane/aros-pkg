/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The oracle is the FIPS 180-2 / NIST example set, written long before this
 * code. The million-'a' vector runs through update() in uneven chunks, so the
 * block boundary handling is exercised as well as the arithmetic.
 */

#include "pkg_sha256.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check_hex(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %s\n    got  %s\n    want %s\n", what, got, want);
    }
}

int main(void)
{
    char hex[PKG_SHA256_HEXLEN + 1];
    struct pkg_sha256 c;
    unsigned char d[PKG_SHA256_LEN];
    static unsigned char buf[1000];
    size_t done, step;

    printf("nist_vectors\n");
    pkg_sha256_hex("", 0, hex);
    check_hex(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    pkg_sha256_hex("abc", 3, hex);
    check_hex(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    pkg_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    check_hex(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");

    printf("million_a_in_uneven_chunks\n");
    memset(buf, 'a', sizeof buf);
    pkg_sha256_init(&c);
    for (done = 0, step = 1; done < 1000000u; done += step, step = step % 997u + 1u) {
        if (step > 1000000u - done)
            step = 1000000u - done;
        pkg_sha256_update(&c, buf, step);
    }
    pkg_sha256_final(&c, d);
    pkg_sha256_tohex(d, hex);
    check_hex(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "1e6 x 'a'");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
