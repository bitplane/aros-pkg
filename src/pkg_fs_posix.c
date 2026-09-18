/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char *pkg_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int slash = la > 0 && a[la - 1] != '/';
    char *p = (char *)malloc(la + (size_t)slash + lb + 1u);
    if (p == NULL)
        return NULL;
    memcpy(p, a, la);
    if (slash)
        p[la] = '/';
    memcpy(p + la + (size_t)slash, b, lb + 1u);
    return p;
}

int pkg_fs_read(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p = NULL;
    size_t cap = 0, n = 0;

    if (f == NULL)
        return -1;
    for (;;) {
        size_t got;
        if (n == cap) {
            size_t ncap = cap ? cap * 2u : 65536u;
            unsigned char *q = (unsigned char *)realloc(p, ncap);
            if (q == NULL) { free(p); fclose(f); errno = ENOMEM; return -1; }
            p = q;
            cap = ncap;
        }
        got = fread(p + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) { free(p); fclose(f); errno = EIO; return -1; }
            break;
        }
    }
    fclose(f);
    *buf = p;
    *len = n;
    return 0;
}

int pkg_fs_mkdirs(const char *dir)
{
    char *p = strdup(dir), *s;
    if (p == NULL)
        return -1;
    for (s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST) { free(p); return -1; }
            *s = '/';
        }
    }
    if (mkdir(p, 0755) != 0 && errno != EEXIST) { free(p); return -1; }
    free(p);
    if (!pkg_fs_is_dir(dir)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int mkparents(const char *path)
{
    char *p = strdup(path), *slash;
    int rc = 0;
    if (p == NULL)
        return -1;
    slash = strrchr(p, '/');
    if (slash != NULL && slash != p) {
        *slash = '\0';
        rc = pkg_fs_mkdirs(p);
    }
    free(p);
    return rc;
}

int pkg_fs_write_atomic(const char *path, const void *buf, size_t len)
{
    size_t lp = strlen(path);
    char *tmp = (char *)malloc(lp + 16u);
    int fd;
    const unsigned char *b = (const unsigned char *)buf;

    if (tmp == NULL)
        return -1;
    if (mkparents(path) != 0) { free(tmp); return -1; }
    snprintf(tmp, lp + 16u, "%s.tmp%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(tmp); return -1; }
    while (len > 0) {
        ssize_t w = write(fd, b, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp); free(tmp); return -1;
        }
        b += w;
        len -= (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int pkg_fs_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int pkg_fs_is_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int pkg_fs_rename(const char *from, const char *to)
{
    if (mkparents(to) != 0)
        return -1;
    return rename(from, to);
}

int pkg_fs_unlink(const char *path)
{
    return unlink(path);
}

int pkg_fs_rmtree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    d = opendir(path);
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char *c;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        c = pkg_join(path, e->d_name);
        if (c == NULL || pkg_fs_rmtree(c) != 0) { free(c); closedir(d); return -1; }
        free(c);
    }
    closedir(d);
    return rmdir(path);
}

void pkg_fs_prune_empty_parents(const char *root, const char *rel)
{
    char *r = strdup(rel), *slash;
    if (r == NULL)
        return;
    while ((slash = strrchr(r, '/')) != NULL) {
        char *full;
        *slash = '\0';
        full = pkg_join(root, r);
        if (full == NULL || rmdir(full) != 0) { free(full); break; }
        free(full);
    }
    free(r);
}

static int skip_host_metadata(const char *name)
{
    return strcmp(name, ".DS_Store") == 0 || strncmp(name, "._", 2) == 0;
}

static int walk(const char *root, const char *rel, pkg_fs_walk_fn fn, void *ctx,
                unsigned *skipped, char *err, size_t errlen)
{
    char *dir = rel[0] ? pkg_join(root, rel) : strdup(root);
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (dir == NULL)
        return -1;
    d = opendir(dir);
    if (d == NULL) {
        snprintf(err, errlen, "cannot open \"%s\": %s", dir, strerror(errno));
        free(dir);
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char *child_rel, *child;
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (skip_host_metadata(e->d_name)) {
            if (skipped) (*skipped)++;
            continue;
        }
        child_rel = rel[0] ? pkg_join(rel, e->d_name) : strdup(e->d_name);
        child = child_rel ? pkg_join(root, child_rel) : NULL;
        if (child == NULL || lstat(child, &st) != 0) {
            snprintf(err, errlen, "cannot read \"%s\"", child ? child : e->d_name);
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = walk(root, child_rel, fn, ctx, skipped, err, errlen);
        } else if (S_ISREG(st.st_mode)) {
            rc = fn(child_rel, ctx);
        } else {
            snprintf(err, errlen, "\"%s\" is a symlink or special file; a package holds regular files only",
                     child_rel);
            rc = -1;
        }
        free(child_rel);
        free(child);
    }
    closedir(d);
    free(dir);
    return rc;
}

int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, void *ctx,
                unsigned *skipped, char *err, size_t errlen)
{
    if (skipped)
        *skipped = 0;
    if (errlen)
        err[0] = '\0';
    return walk(root, "", fn, ctx, skipped, err, errlen);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int pkg_fs_list(const char *dir, char ***names, size_t *count)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **v = NULL;
    size_t n = 0, cap = 0;

    *names = NULL;
    *count = 0;
    if (d == NULL)
        return errno == ENOENT ? 0 : -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2u : 16u;
            char **w = (char **)realloc(v, ncap * sizeof *w);
            if (w == NULL) break;
            v = w;
            cap = ncap;
        }
        v[n] = strdup(e->d_name);
        if (v[n] == NULL) break;
        n++;
    }
    closedir(d);
    if (n > 1u)
        qsort(v, n, sizeof *v, cmp_str);
    *names = v;
    *count = n;
    return 0;
}
