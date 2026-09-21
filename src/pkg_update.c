/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_LINE_MAX 8192
#define UPDATE_FILE_MAX 65536

struct update_owned { char *value[4]; };

void pkg_update_init(struct pkg_update *u)
{
    if (u) memset(u, 0, sizeof *u);
}

void pkg_update_free(struct pkg_update *u)
{
    struct update_owned *owned;
    int i;
    if (!u) return;
    owned = (struct update_owned *)u->_owned;
    if (owned) {
        for (i = 0; i < 4; ++i) free(owned->value[i]);
        free(owned);
    }
    pkg_update_init(u);
}

void pkg_update_found_init(struct pkg_update_found *f)
{
    if (f) memset(f, 0, sizeof *f);
}

void pkg_update_found_free(struct pkg_update_found *f)
{
    if (!f) return;
    free(f->installed); free(f->offered); free(f->changes);
    free(f->signer); free(f->channel); free(f->homepage); free(f->short_desc);
    pkg_update_found_init(f);
}

static char *copy_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = 0;
    return s;
}

static int absolute_path(const char *s)
{
    if (*s == '/' || *s == '\\') return 1;
    /* AROS volumes, Windows drives and URL schemes contain a colon. */
    for (; *s && *s != '/' && *s != '\\'; ++s)
        if (*s == ':') return 1;
    return 0;
}

static char *beside_file(const char *file, const char *value)
{
    const char *p;
    size_t prefix = 0, n;
    char *joined;
    if (absolute_path(value)) return copy_text(value);
    for (p = file; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':') prefix = (size_t)(p - file) + 1;
    if (!prefix) return copy_text(value);
    n = strlen(value) + 1;
    joined = (char *)malloc(prefix + n);
    if (joined) { memcpy(joined, file, prefix); memcpy(joined + prefix, value, n); }
    return joined;
}

int pkg_update_from_file(struct pkg_update *u, const char *path,
                         char *err, unsigned long errlen)
{
    static const char *const keys[] = { "Format", "Package", "Root", "Channel" };
    struct pkg_update parsed;
    struct update_owned *owned;
    FILE *file;
    char line[UPDATE_LINE_MAX], *key, *value, *colon, *resolved;
    const char *why = "invalid update configuration";
    size_t used = 0, total = 0;
    int c, i, rc = PKG_RC_USAGE;
    if (err && errlen) err[0] = 0;
    if (!u || !path || !*path) {
        if (err && errlen) snprintf(err, (size_t)errlen, "update configuration needs an object and a file path");
        return PKG_RC_USAGE;
    }
    file = fopen(path, "rb");
    if (!file) {
        if (err && errlen) snprintf(err, (size_t)errlen, "cannot open update configuration: %s", path);
        return PKG_RC_IO;
    }
    pkg_update_init(&parsed);
    owned = (struct update_owned *)calloc(1, sizeof *owned);
    if (!owned) { fclose(file); why = "out of memory"; rc = PKG_RC_IO; goto report; }
    parsed._owned = owned;
    for (;;) {
        c = fgetc(file);
        if (c != EOF) {
            if (++total > UPDATE_FILE_MAX) { why = "update configuration exceeds 65536 bytes"; goto fail; }
            if (c == 0 || c == 127 || (c < 32 && c != '\t' && c != '\r' && c != '\n')) {
                why = "control character in update configuration"; goto fail;
            }
        }
        if (c != EOF && c != '\n') {
            if (used + 1 >= sizeof line) { why = "update configuration line exceeds 8191 bytes"; goto fail; }
            line[used++] = (char)c;
            continue;
        }
        if (c == EOF && ferror(file)) { why = "cannot read update configuration"; rc = PKG_RC_IO; goto fail; }
        line[used] = 0;
        key = trim(line);
        if (*key && *key != '#') {
            colon = strchr(key, ':');
            if (!colon) { why = "expected Key: value in update configuration"; goto fail; }
            *colon = 0;
            key = trim(key); value = trim(colon + 1);
            if (!*key || strchr(key, '\r') || strchr(value, '\r')) goto fail;
            for (i = 0; i < 4; ++i) if (!strcmp(key, keys[i])) break;
            if (i < 4) {
                if (owned->value[i]) { why = "duplicate update configuration field"; goto fail; }
                if (!*value) { why = "empty update configuration field"; goto fail; }
                owned->value[i] = copy_text(value);
                if (!owned->value[i]) { rc = PKG_RC_IO; why = "out of memory"; goto fail; }
            }
        }
        used = 0;
        if (c == EOF) break;
    }
    if (!owned->value[0] || strcmp(owned->value[0], "pkg-update 1") ||
        !owned->value[1] || !owned->value[2]) {
        why = "expected Format: pkg-update 1, Package and Root"; goto fail;
    }
    for (i = 2; i < 4; ++i) {
        if (!owned->value[i]) continue;
        resolved = beside_file(path, owned->value[i]);
        if (!resolved) { rc = PKG_RC_IO; why = "out of memory"; goto fail; }
        free(owned->value[i]); owned->value[i] = resolved;
    }
    if (fclose(file)) { file = NULL; rc = PKG_RC_IO; why = "cannot close update configuration"; goto fail; }
    parsed.package = owned->value[1];
    parsed.root = owned->value[2];
    parsed.channel = owned->value[3];
    pkg_update_free(u);
    *u = parsed;
    return PKG_RC_OK;
fail:
    if (file) fclose(file);
    pkg_update_free(&parsed);
report:
    if (err && errlen) snprintf(err, (size_t)errlen, "%s", why);
    return rc;
}
