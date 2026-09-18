/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The oracle for the writer is the worked example in the AROS tree, at
 * tools/package/FORMAT:
 *
 *   "PKG", 1, 28L, 3L, "foo", 0, 8L, "barbarba"
 *
 * which is 28 bytes: an 8-byte header, then 4 + 4 + 4 + 8 for the one entry.
 * That example was written by someone else, before this code existed, so it
 * checks the layout rather than confirming what the writer happens to do.
 */

#include "pkg_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static void ok_status(enum pkg_status got, enum pkg_status want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s: got %s, want %s\n", what,
               pkg_strstatus(got), pkg_strstatus(want));
    }
}

/* ---------------------------------------------------------------------- */

static const unsigned char format_example[28] = {
    'P', 'K', 'G', 0x01,
    0x00, 0x00, 0x00, 0x1C,              /* packageSize = 28            */
    0x00, 0x00, 0x00, 0x03,              /* pathLength  = 3             */
    'f', 'o', 'o', 0x00,                 /* path + trailing NUL         */
    0x00, 0x00, 0x00, 0x08,              /* dataLength  = 8             */
    'b', 'a', 'r', 'b', 'a', 'r', 'b', 'a'
};

static void writer_matches_the_format_document(void)
{
    struct pkg_writer *w = pkg_writer_new();
    unsigned char *out = NULL;
    size_t out_len = 0;

    printf("writer_matches_the_format_document\n");
    ok(w != NULL, "writer allocated");
    ok_status(pkg_writer_add(w, "foo", (const unsigned char *)"barbarba", 8),
              PKG_OK, "add foo");
    ok_status(pkg_writer_finish(w, &out, &out_len), PKG_OK, "finish");
    ok(out_len == sizeof format_example, "length is 28");
    if (out != NULL && out_len == sizeof format_example)
        ok(memcmp(out, format_example, sizeof format_example) == 0,
           "bytes equal the FORMAT example");
    free(out);
    pkg_writer_free(w);
}

struct collect {
    int    n;
    char   path[8][64];
    char   data[8][64];
    size_t data_len[8];
};

static int collect_cb(const struct pkg_entry *e, void *ctx)
{
    struct collect *c = (struct collect *)ctx;
    if (c->n >= 8)
        return 1;
    snprintf(c->path[c->n], sizeof c->path[0], "%s", e->path);
    c->data_len[c->n] = e->data_len;
    if (e->data_len < sizeof c->data[0]) {
        memcpy(c->data[c->n], e->data, e->data_len);
        c->data[c->n][e->data_len] = '\0';
    }
    c->n++;
    return 0;
}

static void reader_reads_the_format_document(void)
{
    struct collect c;
    memset(&c, 0, sizeof c);

    printf("reader_reads_the_format_document\n");
    ok_status(pkg_read(format_example, sizeof format_example,
                       collect_cb, &c, NULL), PKG_OK, "read");
    ok(c.n == 1, "one entry");
    ok(strcmp(c.path[0], "foo") == 0, "path is foo");
    ok(c.data_len[0] == 8, "data is 8 bytes");
    ok(strcmp(c.data[0], "barbarba") == 0, "data is barbarba");
}

static void round_trips_several_entries(void)
{
    struct pkg_writer *w = pkg_writer_new();
    unsigned char *out = NULL;
    size_t out_len = 0;
    struct collect c;

    printf("round_trips_several_entries\n");
    memset(&c, 0, sizeof c);
    ok_status(pkg_writer_add(w, "C/Pkg", (const unsigned char *)"exe", 3),
              PKG_OK, "add C/Pkg");
    ok_status(pkg_writer_add(w, "Libs/one.library", NULL, 0),
              PKG_OK, "add an empty payload");
    ok_status(pkg_writer_add(w, "S/Startup", (const unsigned char *)"; hi", 4),
              PKG_OK, "add S/Startup");
    ok_status(pkg_writer_finish(w, &out, &out_len), PKG_OK, "finish");
    ok_status(pkg_read(out, out_len, collect_cb, &c, NULL), PKG_OK, "read back");
    ok(c.n == 3, "three entries");
    ok(strcmp(c.path[0], "C/Pkg") == 0, "first path");
    ok(c.data_len[1] == 0, "second payload is empty");
    ok(strcmp(c.path[2], "S/Startup") == 0, "third path");
    free(out);
    pkg_writer_free(w);
}

static void empty_package_reads_no_entries(void)
{
    struct pkg_writer *w = pkg_writer_new();
    unsigned char *out = NULL;
    size_t out_len = 0;
    struct collect c;

    printf("empty_package_reads_no_entries\n");
    memset(&c, 0, sizeof c);
    ok_status(pkg_writer_finish(w, &out, &out_len), PKG_OK, "finish");
    ok(out_len == PKG_HEADER_SIZE, "header only");
    ok_status(pkg_read(out, out_len, collect_cb, &c, NULL), PKG_OK, "read");
    ok(c.n == 0, "no entries");
    free(out);
    pkg_writer_free(w);
}

/* Each refusal gets its own malformed stream, built from the good one. */
static void malformed_streams_are_refused(void)
{
    unsigned char b[sizeof format_example];

    printf("malformed_streams_are_refused\n");

    memcpy(b, format_example, sizeof b);
    b[1] = 'X';
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_MAGIC, "bad magic");

    memcpy(b, format_example, sizeof b);
    b[3] = 0;
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_VERSION, "version 0");

    memcpy(b, format_example, sizeof b);
    b[3] = 2;
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_VERSION, "version 2");

    memcpy(b, format_example, sizeof b);
    b[7] = 0x1D;
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_SIZE,
              "packageSize disagrees with the buffer");

    memcpy(b, format_example, sizeof b);
    b[7] = 0x1B;
    ok_status(pkg_read(b, sizeof b - 1, NULL, NULL, NULL), PKG_E_TRUNCATED,
              "entry truncated");

    memcpy(b, format_example, sizeof b);
    b[15] = 'X';
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_PATH_TERM,
              "path not NUL-terminated");

    memcpy(b, format_example, sizeof b);
    b[11] = 0;
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_PATH_EMPTY,
              "zero path length");

    memcpy(b, format_example, sizeof b);
    b[19] = 0x09;
    ok_status(pkg_read(b, sizeof b, NULL, NULL, NULL), PKG_E_TRUNCATED,
              "dataLength past the end");

    ok_status(pkg_read(format_example, 4, NULL, NULL, NULL), PKG_E_TRUNCATED,
              "shorter than a header");
}

static int stop_at_first(const struct pkg_entry *e, void *ctx)
{
    (void)e; (void)ctx;
    return 7;
}

static void callback_can_stop_the_walk(void)
{
    int stopped = 0;
    printf("callback_can_stop_the_walk\n");
    ok_status(pkg_read(format_example, sizeof format_example,
                       stop_at_first, NULL, &stopped),
              PKG_E_STOPPED, "stopped");
    ok(stopped == 7, "the callback's value reaches the caller");
}

/* The accessors are checked against hand-written byte arrays. Asymmetric
 * values are deliberate: 1 and 0x01000000 are each other's byte-swap, so a
 * swapped implementation passes a test built only from palindromes. */
static void accessors_express_the_stream_not_the_host(void)
{
    static const struct { unsigned char b[4]; unsigned long v; } vec[] = {
        { { 0x00, 0x00, 0x00, 0x00 }, 0x00000000uL },
        { { 0x00, 0x00, 0x00, 0x01 }, 0x00000001uL },
        { { 0x01, 0x00, 0x00, 0x00 }, 0x01000000uL },
        { { 0x01, 0x02, 0x03, 0x04 }, 0x01020304uL },
        { { 0xDE, 0xAD, 0xBE, 0xEF }, 0xDEADBEEFuL },
        { { 0xFF, 0xFF, 0xFF, 0xFF }, 0xFFFFFFFFuL }
    };
    size_t i;

    printf("accessors_express_the_stream_not_the_host\n");
    for (i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        unsigned char out[4];
        ok(pkg_be32_get(vec[i].b) == vec[i].v, "get matches the vector");
        pkg_be32_put(out, vec[i].v);
        ok(memcmp(out, vec[i].b, 4) == 0, "put matches the vector");
    }
}

/* A 68000 raises an address error on an unaligned 32-bit access, so reading
 * from an odd offset has to work. Under -fsanitize=alignment this also fails
 * loudly for any implementation that casts a struct over the buffer. */
static void reads_from_a_misaligned_buffer(void)
{
    size_t offset;

    printf("reads_from_a_misaligned_buffer\n");
    for (offset = 1; offset <= 3; offset++) {
        unsigned char *raw = (unsigned char *)malloc(sizeof format_example + 4);
        struct collect c;
        memset(&c, 0, sizeof c);
        memcpy(raw + offset, format_example, sizeof format_example);
        ok_status(pkg_read(raw + offset, sizeof format_example,
                           collect_cb, &c, NULL), PKG_OK, "read at an odd offset");
        ok(c.n == 1 && strcmp(c.path[0], "foo") == 0, "entry read correctly");
        free(raw);
    }
}

int main(void)
{
    accessors_express_the_stream_not_the_host();
    reads_from_a_misaligned_buffer();
    writer_matches_the_format_document();
    reader_reads_the_format_document();
    round_trips_several_entries();
    empty_package_reads_no_entries();
    malformed_streams_are_refused();
    callback_can_stop_the_walk();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
