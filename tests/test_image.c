/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The image writer's own rules. Whether the result is a correct FFS volume is
 * judged elsewhere, by readers written apart from this one: amitools in
 * tests/image.sh, and the AROS FFS handler on hosted AROS.
 */

#include "pkg_image.h"
#include "pkg_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static void ok(int c, const char *what) { checks++; if (!c) { failures++; printf("  FAIL %s\n", what); } }

static const unsigned char A[] = "alpha", B[] = "beta";

static int builds(const struct pkg_image_entry *e, size_t n, const char *vol, char *err)
{
    unsigned char *img;
    size_t len;
    int rc = pkg_image_build(e, n, vol, &img, &len, err, 300);
    if (rc == 0) free(img);
    return rc == 0;
}

static void refusals(void)
{
    char err[300];
    struct pkg_image_entry ok1[] = { { "C/Hello", A, 5 } };
    struct pkg_image_entry longname[] = { { "C/abcdefghijklmnopqrstuvwxyz12345", A, 5 } };
    struct pkg_image_entry thirty[] = { { "C/abcdefghijklmnopqrstuvwxyz1234", A, 5 } };
    struct pkg_image_entry casefold[] = { { "C/README", A, 5 }, { "C/readme", B, 4 } };
    struct pkg_image_entry latin1[] = { { "C/\xe9t\xe9", A, 5 }, { "C/\xc9T\xc9", B, 4 } };
    struct pkg_image_entry divide[] = { { "C/\xf7", A, 5 }, { "C/\xd7", B, 4 } };
    struct pkg_image_entry filedir[] = { { "C", A, 5 }, { "C/x", B, 4 } };
    struct pkg_image_entry dircase[] = { { "C/x", A, 5 }, { "c/y", B, 4 } };

    printf("refusals\n");
    ok(builds(ok1, 1, "Vol", err), "a plain drawer builds");
    ok(!builds(longname, 1, "Vol", err) && strstr(err, "does not fit FFS"), "a 33-byte name refused");
    ok(builds(thirty, 1, "Vol", err), "a 30-byte name, the FFS limit, builds");
    ok(!builds(casefold, 2, "Vol", err) && strstr(err, "one name to FFS"), "README and readme refused");
    ok(!builds(latin1, 2, "Vol", err), "Latin-1 names equal under international case refused");
    ok(builds(divide, 2, "Vol", err), "the division sign and the multiplication sign stay distinct");
    ok(!builds(filedir, 2, "Vol", err), "a file and a directory of one name refused");
    ok(!builds(dircase, 2, "Vol", err), "a directory named twice with different case refused");
    ok(!builds(ok1, 1, "", err), "an empty volume name refused");
    ok(!builds(ok1, 1, "Vol:", err), "a volume name with a colon refused");
    ok(!builds(ok1, 1, "abcdefghijklmnopqrstuvwxyz12345", err), "a 31-byte volume name refused");
}

static void shape(void)
{
    struct pkg_image_entry e[] = { { "C/Hello", A, 5 }, { "Libs/b", B, 4 } };
    unsigned char *x, *y;
    size_t lx, ly;
    char err[300], vol[40];
    unsigned long blocks, root;

    printf("shape\n");
    ok(pkg_image_build(e, 2, "Hello", &x, &lx, err, sizeof err) == 0, "builds");
    ok(pkg_image_build(e, 2, "Hello", &y, &ly, err, sizeof err) == 0, "builds again");
    ok(lx == ly && memcmp(x, y, lx) == 0, "the same input gives the same bytes");
    ok(lx % (PKG_IMAGE_TRACK * PKG_IMAGE_BLOCK) == 0, "a whole number of tracks");
    ok(pkg_be32_get(x) == 0x444F5303ul, "DOS\\3 in the boot block");
    blocks = (unsigned long)(lx / PKG_IMAGE_BLOCK);
    root = (blocks + 1) / 2;
    ok(pkg_be32_get(x + root * PKG_IMAGE_BLOCK + 127 * 4) == 1ul, "the root block where FFS computes it");
    ok(pkg_image_check(x, lx, vol, sizeof vol, err, sizeof err) == 0 && strcmp(vol, "Hello") == 0,
       "pkg_image_check accepts it and reads the volume name");
    x[root * PKG_IMAGE_BLOCK + 200] ^= 1;
    ok(pkg_image_check(x, lx, NULL, 0, err, sizeof err) != 0, "one flipped root bit is refused");
    ok(pkg_image_check(y, ly - 512, NULL, 0, err, sizeof err) != 0, "a truncated image is refused");
    free(x);
    free(y);
}

int main(void)
{
    refusals();
    shape();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
