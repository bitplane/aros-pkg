/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static void ok(int c, const char *what) { checks++; if (!c) { failures++; printf("  FAIL %s\n", what); } }

#define D1 "1111111111111111111111111111111111111111111111111111111111111111"
#define D2 "2222222222222222222222222222222222222222222222222222222222222222"

static int parses(const char *text, char *err, size_t errlen)
{
    struct pkg_manifest m;
    int rc = pkg_manifest_parse(text, strlen(text), &m, err, errlen);
    if (rc == 0)
        pkg_manifest_free(&m);
    return rc == 0;
}

static void round_trip(void)
{
    struct pkg_manifest m, back;
    char *out; size_t len; char err[200];

    printf("round_trip\n");
    pkg_manifest_init(&m);
    pkg_manifest_set(&m.name, "hello");
    pkg_manifest_set(&m.version, "1.2");
    pkg_manifest_set(&m.architecture, "generic");
    pkg_manifest_set(&m.kind, "application");
    pkg_manifest_set(&m.payload, D2);
    pkg_manifest_add_file(&m, "Libs/my data.txt", D1, 12);
    pkg_manifest_add_file(&m, "C/Hello", D2, 3);
    pkg_manifest_sort(&m);
    ok(strcmp(m.files[0].path, "C/Hello") == 0, "sorted by path");
    ok(pkg_manifest_emit(&m, &out, &len) == 0, "emit");
    ok(pkg_manifest_parse(out, len, &back, err, sizeof err) == 0, "parse back");
    ok(back.nfiles == 2 && strcmp(back.files[1].path, "Libs/my data.txt") == 0,
       "a path with a space survives");
    ok(back.files[1].size == 12, "size survives");
    ok(strcmp(back.payload, D2) == 0, "payload survives");
    free(out);
    pkg_manifest_free(&m);
    pkg_manifest_free(&back);
}

static void unsafe_paths_are_refused(void)
{
    static const char *const bad[] = {
        "", "/abs", "a/../b", "..", "./a", "a//b", "a/", "SYS:C/x",
        "a\\b", ".pkg/db/x", "a\tb"
    };
    size_t i;
    printf("unsafe_paths_are_refused\n");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ok(pkg_check_path(bad[i]) != NULL, bad[i]);
    ok(pkg_check_path("C/Hello") == NULL, "C/Hello accepted");
    ok(pkg_check_path("Work/My Drawer/x.info") == NULL, "spaces accepted");
    ok(pkg_check_path("a/.pkg") == NULL, ".pkg is reserved at the top only");
}

static void strict_parsing(void)
{
    char err[200];
    const char *head = "Format: pkg-manifest 1\nName: a\nVersion: 1\nArchitecture: generic\nKind: data\n";
    char buf[1024];

    printf("strict_parsing\n");
    ok(parses(head, err, sizeof err), "minimal manifest");

    snprintf(buf, sizeof buf, "%sColour: blue\n", head);
    ok(!parses(buf, err, sizeof err), "unknown key refused");
    snprintf(buf, sizeof buf, "%sName: b\n", head);
    ok(!parses(buf, err, sizeof err), "duplicate key refused");
    ok(!parses("Name: a\nFormat: pkg-manifest 1\n", err, sizeof err), "Format not first refused");
    ok(!parses("Format: pkg-manifest 2\n", err, sizeof err), "unknown format version refused");
    ok(!parses("Format: pkg-manifest 1\nName: a\nVersion: 1\nArchitecture: generic\nKind: data", err, sizeof err),
       "missing final newline refused");
    ok(!parses("Format: pkg-manifest 1\nName: a\nArchitecture: generic\nKind: data\n", err, sizeof err),
       "missing Version refused");
    snprintf(buf, sizeof buf, "%sFile: %s 3 b\nFile: %s 3 a\n", head, D1, D1);
    ok(!parses(buf, err, sizeof err), "unsorted File lines refused");
    snprintf(buf, sizeof buf, "%sFile: %s 3 ../x\n", head, D1);
    ok(!parses(buf, err, sizeof err), "traversal in a File line refused");
    ok(strstr(err, "..") != NULL, "the refusal quotes the path");
    snprintf(buf, sizeof buf, "%sFile: %s -3 x\n", head, D1);
    ok(!parses(buf, err, sizeof err), "negative size refused");
    snprintf(buf, sizeof buf, "%sFile: ABCD 3 x\n", head);
    ok(!parses(buf, err, sizeof err), "short digest refused");
    snprintf(buf, sizeof buf, "%sKind: gadget\n", "Format: pkg-manifest 1\nName: a\nVersion: 1\nArchitecture: generic\n");
    ok(!parses(buf, err, sizeof err), "unknown kind refused");
}

static void versions(void)
{
    printf("versions\n");
    ok(pkg_version_cmp("1.2", "1.10") < 0, "1.2 < 1.10, numerically");
    ok(pkg_version_cmp("40.1", "40.1") == 0, "equal");
    ok(pkg_version_cmp("1.2", "1.2.0") == 0, "1.2 equals 1.2.0");
    ok(pkg_version_cmp("2", "1.99") > 0, "2 > 1.99");
    ok(pkg_check_version("1.2rc1") != NULL, "1.2rc1 refused");
    ok(pkg_check_version("1..2") != NULL, "1..2 refused");
    ok(pkg_check_version("1.") != NULL, "1. refused");
    ok(pkg_check_version("1234567890") != NULL, "ten-digit component refused");
    ok(pkg_check_name("Hello") != NULL, "uppercase name refused");
    ok(pkg_check_name("hello-world_2.0") == NULL, "hello-world_2.0 accepted");
}

int main(void)
{
    round_trip();
    unsafe_paths_are_refused();
    strict_parsing();
    versions();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
