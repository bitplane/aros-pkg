/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The image writer. Block layout follows the FFS on-disk format as the AROS
 * afs handler reads it (rom/filesys/afs/afsblocks.h), with 512-byte blocks,
 * so 128 longs, of which a header block uses:
 *
 *   0 type (2, or 16 for a file's extension block)   1 own block
 *   2 data blocks listed here   4 first data block   5 checksum
 *   6..77 hash table, or data blocks listed last-first from 77
 *   78 root: bitmap valid flag   79..103 root: bitmap blocks
 *   81 file: byte size   108.. name, a length byte and up to 30 bytes
 *   124 next in hash chain   125 parent   126 file: first extension block
 *   127 secondary type: 1 root, 2 directory, -3 file
 *
 * Every long is written with pkg_be32_put; nothing here depends on the host's
 * byte order.
 */

#include "pkg_image.h"
#include "pkg_container.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LONGS        (PKG_IMAGE_BLOCK / 4)
#define TABLE        72u                     /* LONGS - 56 */
#define T_SHORT      2ul
#define T_LIST       16ul
#define ST_ROOT      1ul
#define ST_USERDIR   2ul
#define ST_FILE      0xFFFFFFFDul            /* -3 */
#define DOS3         0x444F5303ul            /* 'DOS' 3: FFS, international */
#define MAP_BITS     ((LONGS - 1) * 32u)     /* blocks one bitmap block covers */
#define MAP_PAGES    25u                     /* bitmap blocks the root lists */

#define L_TYPE       0
#define L_OWN        1
#define L_COUNT      2
#define L_FIRST      4
#define L_CHECKSUM   5
#define L_TABLE      6
#define L_TABLE_END  77
#define L_BM_FLAG    78
#define L_BM_PAGES   79
#define L_PROTECT    80
#define L_SIZE       81
#define L_COMMENT    82                      /* a length byte, then up to 79 bytes */
#define L_NAME       108
#define L_CHAIN      124
#define L_PARENT     125
#define L_EXT        126
#define L_SECTYPE    127

struct node {
    char          name[PKG_IMAGE_NAMEMAX + 1];
    size_t        parent;
    int           dir;
    size_t        entry;        /* files: index into the entries */
    unsigned long block;
};

static void fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* FFS international capitalisation, as capitalch() with DOS\2 and above. */
static unsigned char cap(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 224 && c <= 254 && c != 247))
        return (unsigned char)(c - 32);
    return c;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (cap((unsigned char)*a) != cap((unsigned char)*b))
            return 0;
    return *a == *b;
}

static unsigned hash(const char *name)
{
    unsigned long h = strlen(name);
    for (; *name; name++)
        h = (h * 13 + cap((unsigned char)*name)) & 0x7FF;
    return (unsigned)(h % TABLE);
}

static unsigned char *blk(unsigned char *img, unsigned long b) { return img + (size_t)b * PKG_IMAGE_BLOCK; }
static unsigned long get(const unsigned char *b, unsigned i) { return pkg_be32_get(b + 4 * i); }
static void put(unsigned char *b, unsigned i, unsigned long v) { pkg_be32_put(b + 4 * i, v & 0xFFFFFFFFul); }

static void put_name(unsigned char *b, const char *name)
{
    size_t n = strlen(name);
    b[4 * L_NAME] = (unsigned char)n;
    memcpy(b + 4 * L_NAME + 1, name, n);
}

/* The long at `at` becomes whatever makes the block sum to zero. */
static void seal(unsigned char *b, unsigned at)
{
    unsigned long sum = 0;
    unsigned i;
    put(b, at, 0);
    for (i = 0; i < LONGS; i++)
        sum = (sum + get(b, i)) & 0xFFFFFFFFul;
    put(b, at, (0x100000000ull - sum) & 0xFFFFFFFFul);
}

static unsigned long data_blocks(size_t len)
{
    return (unsigned long)((len + PKG_IMAGE_BLOCK - 1) / PKG_IMAGE_BLOCK);
}

static unsigned long ext_blocks(unsigned long nd)
{
    return nd > TABLE ? (nd - TABLE + TABLE - 1) / TABLE : 0;
}

/* Hang `child` in `dir`'s hash table, at the end of its chain. */
static void link_into(unsigned char *img, unsigned long dir, unsigned long child, const char *name)
{
    unsigned slot = L_TABLE + hash(name);
    unsigned char *d = blk(img, dir);
    unsigned long at = get(d, slot);
    if (at == 0) {
        put(d, slot, child);
        return;
    }
    while (get(blk(img, at), L_CHAIN) != 0)
        at = get(blk(img, at), L_CHAIN);
    put(blk(img, at), L_CHAIN, child);
}

/* Find or add `name` under `parent`. Refuses a name FFS cannot tell from an
 * existing one, and a file where a directory is, or the reverse. */
static long child(struct node *nodes, size_t *count, size_t parent, const char *name,
                  size_t len, int dir, size_t entry, char *err, size_t errlen)
{
    char buf[PKG_IMAGE_NAMEMAX + 1];
    size_t i;

    if (len == 0 || len > PKG_IMAGE_NAMEMAX) {
        fail(err, errlen, "the name \"%.*s\" does not fit FFS, which allows 1 to %u bytes",
             (int)len, name, PKG_IMAGE_NAMEMAX);
        return -1;
    }
    memcpy(buf, name, len);
    buf[len] = '\0';
    for (i = 1; i < *count; i++) {
        if (nodes[i].parent != parent || !same_name(nodes[i].name, buf))
            continue;
        if (dir && nodes[i].dir && strcmp(nodes[i].name, buf) == 0)
            return (long)i;
        fail(err, errlen, "\"%s\" and \"%s\" are one name to FFS, which ignores case, "
             "or a file and a directory of one name", nodes[i].name, buf);
        return -1;
    }
    strcpy(nodes[*count].name, buf);
    nodes[*count].parent = parent;
    nodes[*count].dir = dir;
    nodes[*count].entry = entry;
    nodes[*count].block = 0;
    return (long)(*count)++;
}

int pkg_image_build(const struct pkg_image_entry *e, size_t n, const char *volume,
                    unsigned char **out, size_t *out_len, char *err, size_t errlen)
{
    struct node *nodes;
    size_t count = 1, max = 1, i;
    unsigned long need, blocks, pages, root, next, b;
    unsigned char *img;

    *out = NULL;
    *out_len = 0;
    if (volume == NULL || strlen(volume) == 0 || strlen(volume) > PKG_IMAGE_NAMEMAX
        || strpbrk(volume, ":/") != NULL) {
        fail(err, errlen, "the volume name must be 1 to %u bytes, without ':' or '/'",
             PKG_IMAGE_NAMEMAX);
        return -1;
    }
    for (i = 0; i < n; i++) {
        const char *p;
        for (p = e[i].path; *p; p++)
            if (*p == '/') max++;
        max++;
    }
    nodes = calloc(max, sizeof *nodes);
    if (nodes == NULL) {
        fail(err, errlen, "out of memory");
        return -1;
    }
    strcpy(nodes[0].name, volume);
    nodes[0].dir = 1;

    /* The tree, in path order. */
    for (i = 0; i < n; i++) {
        const char *p = e[i].path, *slash;
        size_t at = 0;
        long got;
        while ((slash = strchr(p, '/')) != NULL) {
            got = child(nodes, &count, at, p, (size_t)(slash - p), 1, 0, err, errlen);
            if (got < 0) { free(nodes); return -1; }
            at = (size_t)got;
            p = slash + 1;
        }
        if (child(nodes, &count, at, p, strlen(p), 0, i, err, errlen) < 0) {
            free(nodes);
            return -1;
        }
    }

    /* Size: boot blocks, root, bitmap, and each node's own blocks, rounded up
     * to a whole track. The bitmap's size depends on the total, so iterate. */
    need = 3;
    for (i = 1; i < count; i++) {
        if (nodes[i].dir) {
            need++;
        } else {
            unsigned long nd = data_blocks(e[nodes[i].entry].len);
            need += 1 + nd + ext_blocks(nd);
        }
    }
    pages = 1;
    for (;;) {
        blocks = (need + pages + PKG_IMAGE_TRACK - 1) / PKG_IMAGE_TRACK * PKG_IMAGE_TRACK;
        if ((blocks - 2 + MAP_BITS - 1) / MAP_BITS == pages)
            break;
        pages = (blocks - 2 + MAP_BITS - 1) / MAP_BITS;
    }
    if (pages > MAP_PAGES) {
        fail(err, errlen, "the image would need %lu blocks, beyond the %u bitmap blocks "
             "a root lists; bitmap extension blocks are not written", blocks, MAP_PAGES);
        free(nodes);
        return -1;
    }
    img = calloc(blocks, PKG_IMAGE_BLOCK);
    if (img == NULL) {
        fail(err, errlen, "out of memory for %lu blocks", blocks);
        free(nodes);
        return -1;
    }
    root = (blocks + 1) / 2;

    /* Boot block, root block, bitmap blocks right after the root. */
    put(blk(img, 0), 0, DOS3);
    put(blk(img, 0), 2, root);
    {
        unsigned char *r = blk(img, root);
        put(r, L_TYPE, T_SHORT);
        put(r, 3, TABLE);
        put(r, L_BM_FLAG, 0xFFFFFFFFul);
        for (i = 0; i < pages; i++)
            put(r, L_BM_PAGES + (unsigned)i, root + 1 + i);
        put_name(r, volume);
        put(r, L_SECTYPE, ST_ROOT);
    }
    nodes[0].block = root;

    /* Everything else, in node order, from block 2, stepping over the root and
     * its bitmap. A file is its header, its data, then its extension blocks. */
    next = 2;
#define TAKE() (next == root ? (next = root + 1 + pages, next++) : next++)
    for (i = 1; i < count; i++) {
        struct node *nd = &nodes[i];
        unsigned char *h;
        nd->block = TAKE();
        h = blk(img, nd->block);
        put(h, L_TYPE, T_SHORT);
        put(h, L_OWN, nd->block);
        put_name(h, nd->name);
        put(h, L_PARENT, nodes[nd->parent].block);
        if (nd->dir) {
            put(h, L_SECTYPE, ST_USERDIR);
        } else {
            const struct pkg_image_entry *f = &e[nd->entry];
            unsigned long ndata = data_blocks(f->len), k, first = 0, prev_ext = 0;
            unsigned long *list = malloc((ndata ? ndata : 1) * sizeof *list);
            unsigned char *cur = h;
            if (list == NULL) {
                fail(err, errlen, "out of memory");
                free(img);
                free(nodes);
                return -1;
            }
            put(h, L_SECTYPE, ST_FILE);
            put(h, L_SIZE, (unsigned long)f->len);
            put(h, L_PROTECT, f->protect & 0xFFFFFFFFul);
            if (f->comment != NULL && f->comment[0]) {
                size_t cl = strlen(f->comment);
                if (cl > 79) cl = 79;           /* the caller checked; the block has room for 79 */
                h[4 * L_COMMENT] = (unsigned char)cl;
                memcpy(h + 4 * L_COMMENT + 1, f->comment, cl);
            }
            for (k = 0; k < ndata; k++) {
                size_t off = (size_t)k * PKG_IMAGE_BLOCK;
                size_t len = f->len - off < PKG_IMAGE_BLOCK ? f->len - off : PKG_IMAGE_BLOCK;
                list[k] = TAKE();
                memcpy(blk(img, list[k]), f->data + off, len);
            }
            if (ndata) first = list[0];
            put(h, L_FIRST, first);
            for (k = 0; k < ndata; k++) {
                if (k > 0 && k % TABLE == 0) {
                    unsigned long x = TAKE();
                    unsigned char *xb = blk(img, x);
                    put(cur, L_COUNT, TABLE);
                    if (cur != h) seal(cur, L_CHECKSUM);
                    put(prev_ext ? blk(img, prev_ext) : h, L_EXT, x);
                    if (prev_ext) seal(blk(img, prev_ext), L_CHECKSUM);
                    put(xb, L_TYPE, T_LIST);
                    put(xb, L_OWN, x);
                    put(xb, L_PARENT, nd->block);
                    put(xb, L_SECTYPE, ST_FILE);
                    cur = xb;
                    prev_ext = x;
                }
                put(cur, L_TABLE_END - (unsigned)(k % TABLE), list[k]);
            }
            if (ndata) put(cur, L_COUNT, (ndata - 1) % TABLE + 1);
            if (cur != h) seal(cur, L_CHECKSUM);
            free(list);
        }
        link_into(img, nodes[nd->parent].block, nd->block, nd->name);
    }
#undef TAKE

    /* Checksums last: linking changes chains after a block is written. */
    for (i = 0; i < count; i++)
        seal(blk(img, nodes[i].block), L_CHECKSUM);

    /* Bitmap: a set bit is a free block. Blocks 2 to next-1, the root and the
     * bitmap are in use; the rounding slack is free; bits past the end stay 0. */
    for (b = 2; b < blocks; b++) {
        int used = b < next || (b >= root && b <= root + pages);
        unsigned long idx = b - 2;
        unsigned char *m = blk(img, root + 1 + idx / MAP_BITS);
        unsigned word = 1 + (unsigned)(idx % MAP_BITS / 32);
        if (!used)
            put(m, word, get(m, word) | (1ul << (idx % 32)));
    }
    for (i = 0; i < pages; i++)
        seal(blk(img, root + 1 + i), 0);

    free(nodes);
    *out = img;
    *out_len = (size_t)blocks * PKG_IMAGE_BLOCK;
    return 0;
}

int pkg_image_check(const unsigned char *img, size_t len, char *volume, size_t volume_len,
                    char *err, size_t errlen)
{
    unsigned long blocks, root, sum = 0;
    const unsigned char *r;
    unsigned i;

    if (len == 0 || len % (PKG_IMAGE_TRACK * PKG_IMAGE_BLOCK) != 0) {
        fail(err, errlen, "an image is a whole number of %u-byte tracks; this is %lu bytes",
             PKG_IMAGE_TRACK * PKG_IMAGE_BLOCK, (unsigned long)len);
        return -1;
    }
    if (pkg_be32_get(img) != DOS3) {
        fail(err, errlen, "the boot block does not say DOS\\3");
        return -1;
    }
    blocks = (unsigned long)(len / PKG_IMAGE_BLOCK);
    root = (blocks + 1) / 2;
    r = img + (size_t)root * PKG_IMAGE_BLOCK;
    for (i = 0; i < LONGS; i++)
        sum = (sum + get(r, i)) & 0xFFFFFFFFul;
    if (get(r, L_TYPE) != T_SHORT || get(r, L_SECTYPE) != ST_ROOT || sum != 0) {
        fail(err, errlen, "block %lu is not a valid root block", root);
        return -1;
    }
    if (get(r, L_BM_FLAG) == 0) {
        fail(err, errlen, "the root says its bitmap is not valid");
        return -1;
    }
    if (r[4 * L_NAME] == 0 || r[4 * L_NAME] > PKG_IMAGE_NAMEMAX) {
        fail(err, errlen, "the root block has no usable volume name");
        return -1;
    }
    if (volume != NULL && volume_len > 0) {
        size_t n = r[4 * L_NAME] < volume_len - 1 ? r[4 * L_NAME] : volume_len - 1;
        memcpy(volume, r + 4 * L_NAME + 1, n);
        volume[n] = '\0';
    }
    return 0;
}
