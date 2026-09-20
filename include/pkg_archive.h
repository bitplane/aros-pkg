/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* Reading an archive someone else made: a tar file, plain or compressed with
 * bzip2, as the AROS nightly builds publish them. Read once, front to back;
 * nothing is extracted unless the caller asks for a file's bytes.
 *
 * A bzip2 archive can also be read in the middle. Its stream is a chain of
 * independent blocks, so one full pass can write down where every member
 * begins and which block holds that point, and a later read of a few members
 * decompresses only from those blocks. That note is the block map. */

#ifndef PKG_ARCHIVE_H
#define PKG_ARCHIVE_H

#include <stddef.h>

struct pkg_archive_entry {
    const char        *path;    /* as the archive names it, '/'-separated */
    unsigned long long size;
    unsigned           mode;    /* the tar mode bits, 0 when absent */
    int                is_dir;
};

/* Called for each entry, directories included. Return 1 to receive the
 * file's bytes through `data`, 0 to skip them, -1 to stop the walk. */
typedef int (*pkg_archive_want_fn)(const struct pkg_archive_entry *e, void *ctx);
/* The bytes of a wanted file, in order, then once more with len 0 at its
 * end. Return 0, or -1 to stop the walk. */
typedef int (*pkg_archive_data_fn)(const struct pkg_archive_entry *e, const unsigned char *buf,
                                   size_t len, void *ctx);

/* Walk the archive at `file`. 0 when it was read to its end; -1 with a
 * reason in err when it is not an archive this reads, is truncated or
 * damaged, or a callback stopped it (err then empty). */
int pkg_archive_walk(const char *file, pkg_archive_want_fn want, pkg_archive_data_fn data,
                     void *ctx, char *err, size_t errlen);

/* The same walk, and with it the block map of a bzip2 archive: *map is set
 * to the map's text, which the caller frees, or to NULL when the archive is
 * not one this can map. The text carries no header of its own; whoever keeps
 * it ties it to the archive it was made from. */
int pkg_archive_walk_map(const char *file, pkg_archive_want_fn want, pkg_archive_data_fn data,
                         void *ctx, char **map, char *err, size_t errlen);

/* Read members out of `file` using the block map `map`, decompressing only
 * from the block each one lies in. `want` and `data` are called as the walk
 * calls them, in the archive's order. 0 when every wanted member was
 * delivered; 1 when the map cannot serve this read and the caller should
 * walk the archive instead; -1 on an error named in err. */
int pkg_archive_read_mapped(const char *file, const char *map, pkg_archive_want_fn want,
                            pkg_archive_data_fn data, void *ctx, char *err, size_t errlen);

/* "archive!/inner/path": split at the first "!/". 1 when s has that form,
 * with the two parts copied out; 0 otherwise. */
int pkg_archive_split(const char *s, char *archive, size_t al, char *inner, size_t il);

#endif
