/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#define _POSIX_C_SOURCE 200809L
/* flock, and the nanosecond modification time .ameta writers compare. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#include "pkg_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if !defined(__AROS__)
#include <sys/file.h>
#endif
#ifdef __AROS__
#include <proto/dos.h>
#include <dos/dos.h>
#endif

int pkg_host_args(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

char *pkg_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    /* No separator after "RAM:" either: on AmigaDOS a leading '/' in the rest
     * of a path means the parent directory, so "RAM:/C" is not "RAM:C". */
    int slash = la > 0 && a[la - 1] != '/' && a[la - 1] != ':';
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

/* Existence is tested before each mkdir, never inferred from errno after it.
 * AROS's posixc does not report EEXIST for a directory that is already there,
 * which the first hosted run showed: the first file of a package staged, the
 * second failed on the parents the first had just created. */
static int mkdir_one(const char *p)
{
    if (pkg_fs_is_dir(p))
        return 0;
    if (mkdir(p, 0755) == 0)
        return 0;
    return pkg_fs_is_dir(p) ? 0 : -1;
}

int pkg_fs_mkdirs(const char *dir)
{
    char *p = strdup(dir), *s;
    if (p == NULL)
        return -1;
    for (s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir_one(p) != 0) { free(p); return -1; }
            *s = '/';
        }
    }
    if (mkdir_one(p) != 0) { free(p); return -1; }
    free(p);
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

static int write_atomic_mode(const char *path, const void *buf, size_t len, int mode);

int pkg_fs_write_atomic(const char *path, const void *buf, size_t len)
{
    return write_atomic_mode(path, buf, len, 0644);
}

int pkg_fs_write_private(const char *path, const void *buf, size_t len)
{
    return write_atomic_mode(path, buf, len, 0600);
}

int pkg_fs_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (f == NULL)
        return -1;
    got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

static int write_atomic_mode(const char *path, const void *buf, size_t len, int mode)
{
    size_t lp = strlen(path);
    char *tmp = (char *)malloc(lp + 16u);
    int fd;
    const unsigned char *b = (const unsigned char *)buf;

    if (tmp == NULL)
        return -1;
    if (mkparents(path) != 0) { free(tmp); return -1; }
    snprintf(tmp, lp + 16u, "%s.tmp%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
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

/* stat, following links: a directory reached through a symlink is still a
 * directory to create files under. On macOS /var is exactly that, a link to
 * /private/var, and every temporary directory lives beneath it. The walk that
 * builds a package keeps its own lstat, since there a link must be refused. */
int pkg_fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
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

    if (!pkg_fs_exists(path))
        return 0;
    if (lstat(path, &st) != 0)
        return -1;
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

/* What pkg_fs.h says a walk leaves out: the same rule on every host, since
 * drawers travel between them. */
static int skip_host_metadata(const char *name)
{
    return name[0] == '.' || strcmp(name, "Icon\r") == 0
        || strcasecmp(name, "Thumbs.db") == 0 || strcasecmp(name, "desktop.ini") == 0;
}
static int walk(const char *root, const char *rel, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip,
                void *ctx, unsigned *skipped, char *err, size_t errlen)
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
        child_rel = rel[0] ? pkg_join(rel, e->d_name) : strdup(e->d_name);
        child = child_rel ? pkg_join(root, child_rel) : NULL;
        if (child != NULL && skip_host_metadata(e->d_name)) {
            if (skipped) (*skipped)++;
            if (skip) skip(child_rel, lstat(child, &st) == 0 && S_ISDIR(st.st_mode), ctx);
        } else if (child == NULL || lstat(child, &st) != 0) {
            snprintf(err, errlen, "cannot read \"%s\"", child ? child : e->d_name);
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = walk(root, child_rel, fn, skip, ctx, skipped, err, errlen);
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

int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip, void *ctx,
                unsigned *skipped, char *err, size_t errlen)
{
    if (skipped)
        *skipped = 0;
    if (errlen)
        err[0] = '\0';
    return walk(root, "", fn, skip, ctx, skipped, err, errlen);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int pkg_fs_list(const char *dir, char ***names, size_t *count)
{
    DIR *d;
    struct dirent *e;
    char **v = NULL;
    size_t n = 0, cap = 0;

    *names = NULL;
    *count = 0;
    if (!pkg_fs_is_dir(dir))
        return 0;                 /* absent: an empty list, whatever errno says */
    d = opendir(dir);
    if (d == NULL)
        return -1;
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

/* ---- Amiga attributes -------------------------------------------------- */

int pkg_fs_owner_exec(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (st.st_mode & S_IXUSR) != 0;
}

int pkg_fs_set_owner_exec(const char *path, int exec)
{
    struct stat st;
    mode_t m;
    if (stat(path, &st) != 0)
        return -1;
    m = exec ? (st.st_mode | S_IXUSR) : (st.st_mode & ~(mode_t)S_IXUSR);
    return m == st.st_mode ? 0 : chmod(path, m & 07777);
}

#ifdef __AROS__
int pkg_fs_amiga_get(const char *path, unsigned long long *prot, char *comment, size_t cl)
{
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int rc = -1;
    if (lock == BNULL)
        return -1;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib != NULL && Examine(lock, fib)) {
        *prot = (unsigned long long)(ULONG)fib->fib_Protection;
        snprintf(comment, cl, "%s", (const char *)fib->fib_Comment);
        rc = 1;
    }
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

int pkg_fs_amiga_set(const char *path, unsigned long long prot, const char *comment_latin1)
{
    if (!SetProtection((CONST_STRPTR)path, (ULONG)(prot & 0xFFFFFFFFull)))
        return -1;
    if (!SetComment((CONST_STRPTR)path, (CONST_STRPTR)(comment_latin1 ? comment_latin1 : "")))
        return -1;
    return 1;
}
#else
int pkg_fs_amiga_get(const char *path, unsigned long long *prot, char *comment, size_t cl)
{
    (void)path; (void)prot; (void)comment; (void)cl;
    return 0;
}

int pkg_fs_amiga_set(const char *path, unsigned long long prot, const char *comment_latin1)
{
    (void)path; (void)prot; (void)comment_latin1;
    return 0;
}
#endif

void pkg_fs_unprotect(const char *path)
{
#ifdef __AROS__
    SetProtection((CONST_STRPTR)path, 0);
#else
    (void)path;
#endif
}

int pkg_fs_identity(const char *path, struct pkg_fs_id *id)
{
    struct stat st;
    memset(id, 0, sizeof *id);
    if (stat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    id->exists = 1;
    id->dev = (unsigned long long)st.st_dev;
    id->ino = (unsigned long long)st.st_ino;
    id->size = (unsigned long long)st.st_size;
#if defined(__APPLE__)
    id->mtime_s = (long long)st.st_mtimespec.tv_sec;
    id->mtime_ns = (long long)st.st_mtimespec.tv_nsec;
#elif defined(__AROS__)
    id->mtime_s = (long long)st.st_mtime;
#else
    id->mtime_s = (long long)st.st_mtim.tv_sec;
    id->mtime_ns = (long long)st.st_mtim.tv_nsec;
#endif
    return 0;
}

static int same_id(const struct pkg_fs_id *a, const struct pkg_fs_id *b)
{
    return a->exists == b->exists && (!a->exists
        || (a->dev == b->dev && a->ino == b->ino && a->size == b->size
            && a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns));
}

int pkg_fs_replace_if_same(const char *path, const struct pkg_fs_id *before,
                           const void *buf, size_t len)
{
    struct pkg_fs_id now;
    char *tmp = NULL;
    if (buf != NULL) {
        size_t pl = strlen(path);
        int fd = -1, tries;
        tmp = (char *)malloc(pl + 16);
        if (tmp == NULL)
            return -1;
        /* ".ameta." and a random suffix, created exclusively: what mkstemp
         * does, written out because AROS's C library has no mkstemp. */
        for (tries = 0; tries < 8 && fd < 0; tries++) {
            unsigned char r[4];
            if (pkg_fs_random(r, sizeof r) != 0) break;
            snprintf(tmp, pl + 16, "%s.%02x%02x%02x%02x", path, r[0], r[1], r[2], r[3]);
            fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
        }
        if (fd < 0) { free(tmp); return -1; }
        if (write(fd, buf, len) != (ssize_t)len || fsync(fd) != 0) {
            close(fd); unlink(tmp); free(tmp);
            return -1;
        }
        close(fd);
        chmod(tmp, 0644);
    }
    if (pkg_fs_identity(path, &now) != 0 || !same_id(&now, before)) {
        if (tmp) { unlink(tmp); free(tmp); }
        return 1;
    }
    if (tmp != NULL) {
        int rc = rename(tmp, path);
        if (rc != 0) unlink(tmp);
        free(tmp);
        return rc == 0 ? 0 : -1;
    }
    return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
}

void *pkg_fs_lock_dir(const char *dir)
{
#if defined(__AROS__)
    (void)dir;
    return NULL;
#else
    int *h, fd = open(dir, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (flock(fd, LOCK_EX) != 0 || (h = (int *)malloc(sizeof *h)) == NULL) {
        close(fd);
        return NULL;
    }
    *h = fd;
    return h;
#endif
}

void pkg_fs_unlock_dir(void *lock)
{
#if !defined(__AROS__)
    if (lock != NULL) {
        flock(*(int *)lock, LOCK_UN);
        close(*(int *)lock);
        free(lock);
    }
#else
    (void)lock;
#endif
}
