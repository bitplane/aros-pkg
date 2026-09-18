/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 John Knipper */

/* libbzip2 1.0.8, unmodified in third_party/bzip2, compiled as one unit so
 * every build of Pkg (host, Windows, AROS, the sanitizers) gets it from the
 * same source list. Only the three warnings its code raises under this tree's
 * flags are silenced, here and nowhere else. */

#define BZ_NO_STDIO 1

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"          /* an older GCC knows fewer of these */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#include "../third_party/bzip2/crctable.c"
#include "../third_party/bzip2/randtable.c"
#include "../third_party/bzip2/huffman.c"
#include "../third_party/bzip2/blocksort.c"
#include "../third_party/bzip2/compress.c"
#include "../third_party/bzip2/decompress.c"
#include "../third_party/bzip2/bzlib.c"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
