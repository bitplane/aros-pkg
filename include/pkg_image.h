/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * A read-only application image: a Fast File System volume (DOS\3, FFS with
 * international names) written once, in memory, with no partition table. It
 * is the file format UAE calls a hardfile and AmigaOS, AROS and MorphOS all
 * mount with the FFS handler they ship. Why FFS and not AFS+: see the README,
 * "The image route".
 *
 * Geometry is fixed so that the file size alone gives the mount entry: 512-byte
 * blocks, one surface, 32 blocks per track, two reserved blocks. An image of
 * N blocks is mounted with LowCyl 0, HighCyl N/32 - 1, and its root block is
 * at (N + 1) / 2, the place every FFS handler computes.
 *
 * The same input always gives the same bytes: entries are laid out in path
 * order, every date is zero, and free space is none beyond rounding.
 */

#ifndef PKG_IMAGE_H
#define PKG_IMAGE_H

#include <stddef.h>

#define PKG_IMAGE_BLOCK     512u
#define PKG_IMAGE_TRACK     32u     /* blocks per track, and the size unit */
#define PKG_IMAGE_NAMEMAX   30u     /* FFS name length */

struct pkg_image_entry {
    const char          *path;      /* '/'-separated, relative, checked by the caller */
    const unsigned char *data;
    size_t               len;
    unsigned long        protect;   /* the file header's protection long; 0 is rwed */
    const char          *comment;   /* Latin-1, at most 79 bytes; NULL or "" for none */
};

/* Build an image holding the files, their directories implied by the paths.
 * Entries must be sorted by path. Caller frees *out. Returns 0, or -1 with a
 * reason in err: a name longer than 30 bytes, two names equal to FFS (which
 * ignores case), a file and a directory of one name, or an image beyond one
 * bitmap extension's reach (about 49 MB). */
int pkg_image_build(const struct pkg_image_entry *e, size_t n, const char *volume,
                    unsigned char **out, size_t *out_len, char *err, size_t errlen);

/* Check that a buffer is an image this module would accept to publish: a
 * whole number of tracks, DOS\3 in the boot block, a root block of the right
 * type with a valid checksum and a valid bitmap flag. Returns 0, or -1 with a
 * reason. The volume name is copied into volume, if not NULL. */
int pkg_image_check(const unsigned char *img, size_t len, char *volume, size_t volume_len,
                    char *err, size_t errlen);

#endif
