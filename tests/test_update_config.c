/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "pkg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_config(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static void rejected(struct pkg_update *u, const char *path, const char *text, size_t n)
{
    struct pkg_update saved = *u;
    char error[80];
    write_config(path, text, n);
    assert(pkg_update_from_file(u, path, error, sizeof error) == PKG_RC_USAGE);
    assert(*error);
    assert(u->package == saved.package && u->root == saved.root &&
           u->channel == saved.channel && u->_owned == saved._owned);
}

int main(void)
{
    static const char good[] = "# application update\r\nFormat: pkg-update 1\r\nPackage: hello\r\nRoot: root\r\nChannel: channel\r\nFuture: ignored\r\n";
    static const char *const bad[] = {
        "Package: hello\nRoot: root\n",
        "Format: pkg-update 2\nPackage: hello\nRoot: root\n",
        "Format: pkg-update 1\nPackage: hello\n",
        "Format: pkg-update 1\nRoot: root\n",
        "Format: pkg-update 1\nPackage: hello\nRoot:\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nChannel:\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nFormat: pkg-update 1\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nPackage: hello\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nRoot: other\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nChannel: one\nChannel: two\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nMalformed\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\n: value\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: ro\rot\n",
        "Format: pkg-update 1\nPackage: hello\nRoot: root\nFuture: \001\n"
    };
    char dir[] = "/tmp/pkg-update-config-XXXXXX", path[256], expected[256], error[256];
    char nul[] = "Format: pkg-update 1\nPackage: hello\nRoot: root\n\0Channel: hidden\n";
    char *large;
    const char *text;
    struct pkg_update u, saved;
    struct pkg_update_found found;
    size_t i;
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/program.pkgupdate", dir);
    pkg_update_init(&u);
    u.package = "borrowed"; u.root = "SYS:";
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) rejected(&u, path, bad[i], strlen(bad[i]));
    write_config(path, good, sizeof good - 1);
    assert(pkg_update_from_file(&u, path, error, sizeof error) == PKG_RC_OK);
    assert(!*error && !strcmp(u.package, "hello"));
    snprintf(expected, sizeof expected, "%s/root", dir);
    assert(!strcmp(u.root, expected));
    snprintf(expected, sizeof expected, "%s/channel", dir);
    assert(!strcmp(u.channel, expected));
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) rejected(&u, path, bad[i], strlen(bad[i]));
    rejected(&u, path, nul, sizeof nul - 1);
    large = (char *)malloc(70000);
    assert(large);
    memset(large, 'x', 9000);
    rejected(&u, path, large, 9000);
    memset(large, '\n', 70000);
    rejected(&u, path, large, 70000);
    free(large);
    text = "Format: pkg-update 1\nPackage: hello\nRoot: SYS:\nChannel: https://example.invalid/pkg";
    write_config(path, text, strlen(text));
    assert(pkg_update_from_file(&u, path, NULL, 0) == 0);
    assert(!strcmp(u.root, "SYS:") && !strcmp(u.channel, "https://example.invalid/pkg"));
    text = "Format: pkg-update 1\nPackage: hello\nRoot: C:\\Programs\\Hello\nChannel: \\\\server\\channel\n";
    write_config(path, text, strlen(text));
    assert(pkg_update_from_file(&u, path, error, sizeof error) == 0);
    assert(!strcmp(u.root, "C:\\Programs\\Hello") && !strcmp(u.channel, "\\\\server\\channel"));
    text = "Format: pkg-update 1\nPackage: hello\nRoot: /absolute/root\n";
    write_config(path, text, strlen(text));
    assert(pkg_update_from_file(&u, path, error, sizeof error) == 0);
    assert(!strcmp(u.root, "/absolute/root") && !u.channel);
    saved = u;
    assert(unlink(path) == 0);
    assert(pkg_update_from_file(&u, path, error, sizeof error) == PKG_RC_IO);
    assert(u._owned == saved._owned && u.root == saved.root);
    assert(pkg_update_from_file(&u, NULL, error, sizeof error) == PKG_RC_USAGE);
    assert(pkg_update_from_file(NULL, path, error, sizeof error) == PKG_RC_USAGE);
    pkg_update_free(&u);
    assert(!u.package && !u.root && !u.channel && !u._owned);
    pkg_update_free(&u);
    u.package = "borrowed"; u.root = "SYS:"; u.channel = "https://example.invalid";
    pkg_update_free(&u);
    pkg_update_found_init(&found);
    found.installed = strdup("1.0"); found.offered = strdup("2.0");
    found.changes = strdup("Changes"); found.signer = strdup("key");
    found.channel = strdup("channel"); found.homepage = strdup("homepage");
    found.short_desc = strdup("description"); found.state = PKG_UPDATE_AVAILABLE;
    pkg_update_found_free(&found);
    assert(!found.installed && !found.offered && !found.changes && !found.signer &&
           !found.channel && !found.homepage && !found.short_desc && !found.state);
    pkg_update_found_free(&found);
    assert(rmdir(dir) == 0);
    puts("update configuration: ok");
    return 0;
}
