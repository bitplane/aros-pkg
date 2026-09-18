/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The host filesystem, and the only non-portable file in the tree. Everything
 * above this interface is C99; this implementation is POSIX, which covers
 * macOS and Linux. The AROS implementation of the same interface uses
 * dos.library and comes with the AROS build of the client.
 */

#ifndef PKG_FS_H
#define PKG_FS_H

#include <stddef.h>

/* Return 0, or -1 with errno set. */
int  pkg_fs_read(const char *path, unsigned char **buf, size_t *len);
int  pkg_fs_write_atomic(const char *path, const void *buf, size_t len);
int  pkg_fs_mkdirs(const char *dir);
int  pkg_fs_exists(const char *path);          /* 1 if anything is there */
int  pkg_fs_is_dir(const char *path);
int  pkg_fs_rename(const char *from, const char *to);
int  pkg_fs_unlink(const char *path);
int  pkg_fs_rmtree(const char *path);
void pkg_fs_prune_empty_parents(const char *root, const char *rel);

/* Walk regular files under root, calling fn with a '/'-separated path
 * relative to root. Symlinks and special files are refused, naming the file.
 *
 * Left out, and passed to skip (when not NULL) so the caller can name each:
 * every name starting with '.', file or directory, which covers the hidden
 * files of macOS and Unix (.DS_Store, AppleDouble "._" files, .git,
 * .Trashes) and of the Amiga Workbench (.backdrop); "Icon\r", the custom
 * folder icon of macOS; Thumbs.db and desktop.ini. Amiga icons are Name.info,
 * which never start with '.', and stay. *skipped counts what was left out. */
typedef int (*pkg_fs_walk_fn)(const char *rel, void *ctx);
typedef void (*pkg_fs_skip_fn)(const char *rel, int is_dir, void *ctx);
int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, pkg_fs_skip_fn skip, void *ctx,
                unsigned *skipped, char *err, size_t errlen);

/* Directory entries of dir, sorted, dotfiles excluded. Caller frees each
 * string and the array. */
int pkg_fs_list(const char *dir, char ***names, size_t *count);

/* Caller frees. */
char *pkg_join(const char *a, const char *b);

/* Replace argc/argv with the host's own view of the command line, as UTF-8.
 * On Windows the C runtime's argv is in the ANSI code page and loses names
 * outside it; elsewhere this changes nothing. 0 or -1. */
int pkg_host_args(int *argc, char ***argv);

/* Fill buf from the system's cryptographic random source. 0 or -1. */
int pkg_fs_random(void *buf, size_t len);

/* Like pkg_fs_write_atomic, readable and writable by the owner alone. For a
 * signing key, which must not be world-readable even for the instant between
 * creation and a chmod. */
int pkg_fs_write_private(const char *path, const void *buf, size_t len);

#endif
