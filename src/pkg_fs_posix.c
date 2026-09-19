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
#include <time.h>
#include <unistd.h>
#if !defined(__AROS__)
#include <sys/file.h>
#endif
#ifdef __AROS__
#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/execbase.h>
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

#ifdef __AROS__
int pkg_fs_loaded(const char *name, int device, unsigned *version, unsigned *revision,
                  unsigned *opencnt)
{
    struct Library *lib;
    int found = 0;
    Forbid();
    lib = (struct Library *)FindName(device ? &SysBase->DeviceList : &SysBase->LibList, (CONST_STRPTR)name);
    if (lib != NULL) {
        *version = lib->lib_Version;
        *revision = lib->lib_Revision;
        *opencnt = lib->lib_OpenCnt;
        found = 1;
    }
    Permit();
    return found;
}

int pkg_fs_fullpath(const char *path, char *out, size_t ol)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    int ok;
    if (l == BNULL) return 0;
    ok = NameFromLock(l, (STRPTR)out, (LONG)ol) != 0;
    UnLock(l);
    return ok;
}
#else
int pkg_fs_loaded(const char *name, int device, unsigned *version, unsigned *revision,
                  unsigned *opencnt)
{
    (void)name; (void)device; (void)version; (void)revision; (void)opencnt;
    return -1;
}

int pkg_fs_fullpath(const char *path, char *out, size_t ol)
{
    (void)path; (void)out; (void)ol;
    return 0;
}
#endif

int pkg_fs_interactive(void)
{
    return isatty(1);
}

int pkg_fs_write_new(const char *path, const void *buf, size_t len)
{
    const unsigned char *b = (const unsigned char *)buf;
    int fd;
    if (mkparents(path) != 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    while (len > 0) {
        ssize_t w = write(fd, b, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(path); return -1;
        }
        b += w;
        len -= (size_t)w;
    }
    if (close(fd) != 0) { unlink(path); return -1; }
    return 0;
}

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

/* A temporary name beside `path`, short whatever the file's name: FFS takes
 * names of 30 characters at most, and a file's own name may already be that
 * long. "<dir>/.pkg" and eight hex digits, created exclusively. */
static int open_tmp_beside(const char *path, char *tmp, size_t tl, int mode)
{
    static unsigned long counter;
    const char *slash = strrchr(path, '/');
    int dl = slash ? (int)(slash - path) + 1 : 0, fd = -1, tries;
    for (tries = 0; tries < 16 && fd < 0; tries++) {
        unsigned char r[4];
        unsigned long v;
        if (pkg_fs_random(r, sizeof r) == 0)
            v = (unsigned long)r[0] << 24 | (unsigned long)r[1] << 16 | (unsigned long)r[2] << 8 | r[3];
        else    /* no /dev/urandom: the process and a counter */
            v = ((unsigned long)getpid() * 2654435761ul + ++counter * 40503ul + (unsigned long)time(NULL))
                & 0xFFFFFFFFul;
        snprintf(tmp, tl, "%.*s.pkg%08lx", dl, path, v);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    return fd;
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
    fd = open_tmp_beside(path, tmp, lp + 16u, mode);
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

/* ---- the network ------------------------------------------------------ */

#if defined(__AROS__)
int pkg_net_send(const char *method, const char *url, const char *body_file,
                 const char *header_file, const char *out_file, int *code,
                 char *err, size_t errlen)
{
    (void)method; (void)url; (void)body_file; (void)header_file; (void)out_file; (void)code;
    snprintf(err, errlen, "PUSH runs on the machine that publishes, not on AROS yet");
    return -1;
}

int pkg_net_get(const char *url, const char *dest, char *err, size_t errlen)
{
    (void)url; (void)dest;
    snprintf(err, errlen, "network channels are not built for AROS yet; copy the channel to a "
             "volume this machine reads, and name that directory");
    return -1;
}

char *pkg_cache_dir(void)
{
    const char *e = getenv("PKG_CACHE");
    char *p = (char *)malloc(64 + (e ? strlen(e) : 0));
    if (p) strcpy(p, e ? e : "T:pkg-cache");
    return p;
}
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;

char *pkg_cache_dir(void)
{
    const char *e = getenv("PKG_CACHE"), *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    size_t n = 32 + (e ? strlen(e) : 0) + (x ? strlen(x) : 0) + (h ? strlen(h) : 0);
    char *p = (char *)malloc(n);
    if (p == NULL) return NULL;
    if (e && *e) snprintf(p, n, "%s", e);
    else if (x && *x) snprintf(p, n, "%s/pkg", x);
    else if (h && *h) snprintf(p, n, "%s/.cache/pkg", h);
    else snprintf(p, n, "/tmp/pkg-cache");
    return p;
}

int pkg_net_send(const char *method, const char *url, const char *body_file,
                 const char *header_file, const char *out_file, int *code,
                 char *err, size_t errlen)
{
    char data[1100], hdr[1100], codebuf[32];
    char *argv[20];
    int n = 0, st, fd, saved;
    pid_t pid;
    posix_spawn_file_actions_t fa;
    char codefile[] = "/tmp/pkg-code.XXXXXX";

    argv[n++] = "curl"; argv[n++] = "-sS"; argv[n++] = "-X"; argv[n++] = (char *)method;
    if (body_file) {
        /* streamed from the file, never held whole: an archive is large */
        snprintf(data, sizeof data, "%s", body_file);
        argv[n++] = "-T"; argv[n++] = data;
        argv[n++] = "-H"; argv[n++] = "Content-Type: application/octet-stream";
        argv[n++] = "-H"; argv[n++] = "Expect:";
    }
    if (header_file) {
        snprintf(hdr, sizeof hdr, "@%s", header_file);
        argv[n++] = "-H"; argv[n++] = hdr;
    }
    argv[n++] = "-o"; argv[n++] = (char *)out_file;
    argv[n++] = "-w"; argv[n++] = "%{http_code}";
    argv[n++] = (char *)url;
    argv[n] = NULL;
    fd = mkstemp(codefile);
    if (fd < 0) { snprintf(err, errlen, "cannot make a temporary file"); return -1; }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 1);
    saved = posix_spawnp(&pid, "curl", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd);
    if (saved != 0) { unlink(codefile); snprintf(err, errlen, "PUSH needs curl, which is not on this machine's PATH"); return -1; }
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) { unlink(codefile); snprintf(err, errlen, "curl did not finish"); return -1; }
    fd = open(codefile, O_RDONLY);
    n = fd >= 0 ? (int)read(fd, codebuf, sizeof codebuf - 1) : 0;
    if (fd >= 0) close(fd);
    unlink(codefile);
    codebuf[n > 0 ? n : 0] = '\0';
    *code = atoi(codebuf);
    if (*code == 0) {
        snprintf(err, errlen, "no answer from %s (curl exit %d)", url, WEXITSTATUS(st));
        return -1;
    }
    return 0;
}

/* https: the system's curl, with no shell in between. */
static int get_with_curl(const char *url, const char *tmp, char *err, size_t errlen)
{
    /* -s alone: a 404 for a file that may not exist (a withdrawal) is no
     * error, and the exit code says the rest */
    char *argv[] = { "curl", "-s", "-f", "-L", "--max-redirs", "5", "-o", (char *)tmp, (char *)url, NULL };
    pid_t pid;
    int st;
    if (posix_spawnp(&pid, "curl", NULL, NULL, argv, environ) != 0) {
        snprintf(err, errlen, "https needs curl, which is not on this machine's PATH");
        return -1;
    }
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        snprintf(err, errlen, "curl did not finish");
        return -1;
    }
    if (WEXITSTATUS(st) == 22) return 1;        /* -f: an HTTP error; Pkg asks for files that may not exist */
    if (WEXITSTATUS(st) != 0) {
        snprintf(err, errlen, "curl failed with exit code %d fetching %s", WEXITSTATUS(st), url);
        return -1;
    }
    return 0;
}

static int http_get_once(const char *url, int fd, char *location, size_t ll, char *err, size_t errlen)
{
    char host[256], port[8] = "80", req[2300], head[8192];
    const char *p = url + 7, *slash = strchr(p, '/'), *colon;
    const char *path = slash ? slash : "/";
    size_t hl = slash ? (size_t)(slash - p) : strlen(p), hlen = 0;
    struct addrinfo hints, *ai = NULL, *a;
    int s = -1, code = 0, chunked = 0;
    long long clen = -1;
    char *body;
    ssize_t n;

    colon = memchr(p, ':', hl);
    if (hl == 0 || hl >= sizeof host) { snprintf(err, errlen, "no host in %s", url); return -1; }
    if (colon) {
        snprintf(port, sizeof port, "%.*s", (int)(hl - (size_t)(colon - p) - 1), colon + 1);
        hl = (size_t)(colon - p);
    }
    snprintf(host, sizeof host, "%.*s", (int)hl, p);
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) {
        snprintf(err, errlen, "cannot find the host %s", host);
        return -1;
    }
    for (a = ai; a && s < 0; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s >= 0 && connect(s, a->ai_addr, a->ai_addrlen) != 0) { close(s); s = -1; }
    }
    freeaddrinfo(ai);
    if (s < 0) { snprintf(err, errlen, "cannot connect to %s:%s", host, port); return -1; }
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Pkg\r\nConnection: close\r\n\r\n",
             path, host);
    if (write(s, req, strlen(req)) != (ssize_t)strlen(req)) {
        close(s); snprintf(err, errlen, "cannot send to %s", host); return -1;
    }
    /* the head, up to the blank line */
    for (;;) {
        if (hlen + 1 >= sizeof head) { close(s); snprintf(err, errlen, "an oversized reply from %s", host); return -1; }
        n = read(s, head + hlen, sizeof head - 1 - hlen);
        if (n <= 0) { close(s); snprintf(err, errlen, "%s closed the connection early", host); return -1; }
        hlen += (size_t)n;
        head[hlen] = '\0';
        if ((body = strstr(head, "\r\n\r\n")) != NULL) { body += 4; break; }
    }
    if (sscanf(head, "HTTP/%*s %d", &code) != 1) { close(s); snprintf(err, errlen, "%s did not answer HTTP", host); return -1; }
    {
        char *line = strstr(head, "\r\n");
        while (line && line + 2 < body - 2) {
            char *eol = strstr(line + 2, "\r\n");
            size_t k = eol ? (size_t)(eol - (line + 2)) : 0;
            if (k > 15 && strncasecmp(line + 2, "Content-Length:", 15) == 0) clen = atoll(line + 17);
            if (k > 18 && strncasecmp(line + 2, "Transfer-Encoding:", 18) == 0
                && strstr(line + 2, "chunked") && (size_t)(strstr(line + 2, "chunked") - (line + 2)) < k)
                chunked = 1;
            if (k > 9 && strncasecmp(line + 2, "Location:", 9) == 0 && location) {
                const char *v = line + 11;
                while (*v == ' ') v++;
                snprintf(location, ll, "%.*s", (int)(eol - v), v);
            }
            line = eol;
        }
    }
    if (code == 404 || code == 410) { close(s); return 1; }
    if (code >= 300 && code < 400) { close(s); return 3; }
    if (code != 200) { close(s); snprintf(err, errlen, "%s answered HTTP %d for %s", host, code, path); return -1; }
    {
        /* the body: what followed the head, then the rest of the stream */
        size_t have = hlen - (size_t)(body - head);
        char buf[65536];
        long long got = 0;
        if (!chunked) {
            if (have && write(fd, body, have) != (ssize_t)have) goto werr;
            got = (long long)have;
            while (clen < 0 || got < clen) {
                n = read(s, buf, sizeof buf);
                if (n <= 0) break;
                if (write(fd, buf, (size_t)n) != n) goto werr;
                got += n;
            }
            close(s);
            if (clen >= 0 && got != clen) { snprintf(err, errlen, "%s sent %lld of %lld bytes", host, got, clen); return -1; }
            return 0;
        } else {
            /* chunked: collect everything, then decode */
            size_t cap = have + 65536, len = have, at = 0;
            char *all = (char *)malloc(cap + 1);
            if (all == NULL) { close(s); snprintf(err, errlen, "out of memory"); return -1; }
            memcpy(all, body, have);
            while ((n = read(s, buf, sizeof buf)) > 0) {
                if (len + (size_t)n + 1 > cap) {
                    char *g;
                    cap = (len + (size_t)n) * 2;
                    g = (char *)realloc(all, cap + 1);
                    if (g == NULL) { free(all); close(s); snprintf(err, errlen, "out of memory"); return -1; }
                    all = g;
                }
                memcpy(all + len, buf, (size_t)n);
                len += (size_t)n;
            }
            close(s);
            all[len] = '\0';
            for (;;) {
                unsigned long size = strtoul(all + at, NULL, 16);
                char *eol = strstr(all + at, "\r\n");
                if (eol == NULL) break;
                at = (size_t)(eol - all) + 2;
                if (size == 0) { free(all); return 0; }
                if (at + size > len || write(fd, all + at, size) != (ssize_t)size) {
                    free(all); snprintf(err, errlen, "%s sent a broken chunked reply", host); return -1;
                }
                at += size + 2;
            }
            free(all);
            snprintf(err, errlen, "%s ended a chunked reply early", host);
            return -1;
        }
    }
werr:
    close(s);
    snprintf(err, errlen, "cannot write the download: %s", strerror(errno));
    return -1;
}

int pkg_net_get(const char *url, const char *dest, char *err, size_t errlen)
{
    size_t dl = strlen(dest);
    char *tmp = (char *)malloc(dl + 8), cur[2100], loc[2100];
    int hops, rc = -1, fd;

    if (tmp == NULL) { snprintf(err, errlen, "out of memory"); return -1; }
    snprintf(tmp, dl + 8, "%s.part", dest);
    snprintf(cur, sizeof cur, "%s", url);
    for (hops = 0; hops < 6; hops++) {
        if (strncmp(cur, "https://", 8) == 0) {
            rc = get_with_curl(cur, tmp, err, errlen);
            break;
        }
        if (strncmp(cur, "http://", 7) != 0) {
            snprintf(err, errlen, "cannot fetch %s: only http:// and https:// are read", cur);
            rc = -1;
            break;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno)); rc = -1; break; }
        loc[0] = '\0';
        rc = http_get_once(cur, fd, loc, sizeof loc, err, errlen);
        close(fd);
        if (rc != 3) break;
        if (loc[0] == '\0') { snprintf(err, errlen, "a redirect with no Location"); rc = -1; break; }
        if (loc[0] == '/') {                     /* same host */
            const char *h = strchr(cur + 8, '/');
            snprintf(cur, sizeof cur, "%.*s%s", h ? (int)(h - cur) : (int)strlen(cur), cur, loc);
        } else {
            snprintf(cur, sizeof cur, "%s", loc);
        }
        rc = -1;
        snprintf(err, errlen, "too many redirects");
    }
    if (rc == 0 && rename(tmp, dest) != 0) { snprintf(err, errlen, "cannot keep %s: %s", dest, strerror(errno)); rc = -1; }
    if (rc != 0) unlink(tmp);
    free(tmp);
    return rc;
}
#endif
