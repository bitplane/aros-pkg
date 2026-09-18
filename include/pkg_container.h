/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The AROS .pkg container: reader and writer.
 *
 * The byte layout is the one AROS documents in tools/package/FORMAT:
 *
 *   package     = header, file*
 *   file        = pathLength, path, dataLength, data
 *   header      = 'P', 'K', 'G', version, packageSize
 *   version     = UBYTE            ; must be 1
 *   packageSize = LONG             ; big endian, the whole stream
 *   pathLength  = LONG             ; big endian
 *   path        = BYTE[pathLength+1]   ; trailing NUL
 *   dataLength  = LONG             ; big endian
 *   data        = BYTE[dataLength]
 *
 * This is the RAW stream. AROS's workbench/c/Unpack reads it through bzip2,
 * while arch/riscv64-opensbi/kernel/kernel_elf.c reads it uncompressed from
 * memory. Compression is therefore a layer above this one and is not handled
 * here.
 *
 * The reader is strict where AROS's own is not: it requires version 1, and it
 * requires packageSize to agree with the buffer it was handed. Unpack accepts
 * any version byte and ignores packageSize.
 */

#ifndef PKG_CONTAINER_H
#define PKG_CONTAINER_H

#include <stddef.h>

#define PKG_HEADER_SIZE  8u
#define PKG_VERSION      1u

enum pkg_status {
    PKG_OK = 0,
    PKG_E_MAGIC,        /* the first three bytes are not 'P','K','G' */
    PKG_E_VERSION,      /* version byte is not 1                     */
    PKG_E_SIZE,         /* packageSize disagrees with the buffer     */
    PKG_E_TRUNCATED,    /* an entry runs past the end of the buffer   */
    PKG_E_PATH_TERM,    /* the byte at path[pathLength] is not NUL    */
    PKG_E_PATH_EMPTY,   /* pathLength is zero                        */
    PKG_E_OVERFLOW,     /* a length would overflow size_t            */
    PKG_E_NOMEM,
    PKG_E_STOPPED       /* the callback asked to stop                */
};

/* Human-readable form of a status, for a refusal that names its reason. */
const char *pkg_strstatus(enum pkg_status s);

/* ---- byte order ------------------------------------------------------- *
 *
 * These two functions are the ONLY place in this codebase where a byte order
 * is expressed, and they express the STREAM's order, never the host's. They
 * read and write one byte at a time with explicit shifts, so they compile to
 * the same behaviour on a little-endian and a big-endian host, and they place
 * no alignment requirement on the pointer.
 *
 * That last property is load-bearing rather than tidy: on a 68000 an unaligned
 * 32-bit access raises an address error, so any design that maps a packed
 * struct onto a byte stream is broken on the oldest target this program
 * serves. `make check-portability` refuses host-order conversion macros,
 * endianness conditionals and byte-swap builtins anywhere in the tree. It
 * names no forbidden token itself, so that it cannot trip over its own
 * documentation.
 */
unsigned long pkg_be32_get(const unsigned char *p);
void          pkg_be32_put(unsigned char *p, unsigned long v);

/* ---- reading ---------------------------------------------------------- */

struct pkg_entry {
    const char          *path;   /* NUL-terminated, inside the input buffer */
    size_t               path_len;
    const unsigned char *data;   /* inside the input buffer, may be NULL    */
    size_t               data_len;
};

/* Return 0 to continue, non-zero to stop the walk (pkg_read returns
 * PKG_E_STOPPED and leaves *stopped_at set to the callback's value). */
typedef int (*pkg_entry_fn)(const struct pkg_entry *e, void *ctx);

enum pkg_status pkg_read(const unsigned char *buf, size_t len,
                         pkg_entry_fn fn, void *ctx, int *stopped_at);

/* ---- writing ---------------------------------------------------------- */

struct pkg_writer;

struct pkg_writer *pkg_writer_new(void);
void               pkg_writer_free(struct pkg_writer *w);

enum pkg_status pkg_writer_add(struct pkg_writer *w, const char *path,
                               const unsigned char *data, size_t data_len);

/* On PKG_OK the caller owns *out and frees it with free(). */
enum pkg_status pkg_writer_finish(struct pkg_writer *w,
                                  unsigned char **out, size_t *out_len);

#endif /* PKG_CONTAINER_H */
