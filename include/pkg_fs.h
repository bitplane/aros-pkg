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
 * Host metadata the Amiga side has no use for (.DS_Store, AppleDouble "._"
 * files) is skipped and counted in *skipped. */
typedef int (*pkg_fs_walk_fn)(const char *rel, void *ctx);
int pkg_fs_walk(const char *root, pkg_fs_walk_fn fn, void *ctx,
                unsigned *skipped, char *err, size_t errlen);

/* Directory entries of dir, sorted, dotfiles excluded. Caller frees each
 * string and the array. */
int pkg_fs_list(const char *dir, char ***names, size_t *count);

/* Caller frees. */
char *pkg_join(const char *a, const char *b);

#endif
