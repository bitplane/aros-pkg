/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * How the command line lays out what the library says. Each line arrives
 * with its role (pkg_sink.line); this module draws it: a mark and a colour
 * where the terminal has them, columns for a table, wrapping for a long
 * hint, plain text everywhere else. It writes nothing itself; it hands the
 * caller the styled text and the plain text of every line, and the caller
 * puts the first on the screen and the second in the LOG file.
 *
 * Colour and marks are for a person at a terminal. Piped, redirected or
 * captured output is plain and stable, so a script can read it. PKG_COLOR
 * is `always` or `never` to override; NO_COLOR and TERM=dumb turn colour
 * off. Marks are UTF-8 under a UTF-8 locale and ASCII otherwise. On AROS
 * the console has bold and italic but its colour numbers select palette
 * pens, so only bold and italic are used there.
 */

#ifndef PKG_STYLE_H
#define PKG_STYLE_H

#include <stddef.h>

/* What the front end can draw on each stream. */
struct pkg_style_caps {
    int colour;   /* SGR colours */
    int bold;     /* SGR bold, dim and italic */
    int utf8;     /* the marks may be UTF-8 */
    int width;    /* columns, 80 when unknown */
};

/* `out` and `err` say whether each stream is a terminal a person watches;
 * `on_aros` selects the console rules above. Reads PKG_COLOR, NO_COLOR,
 * TERM, COLUMNS and the locale variables. */
void pkg_style_init(int out_interactive, int err_interactive, int on_aros);

/* The operation running, named in a refusal: "install". */
void pkg_style_verb(const char *verb);

/* The writer: `styled` is for the screen, `plain` for a log. Either text
 * may hold several lines; both end with a newline unless the line is a
 * progress counter. */
typedef void (*pkg_style_writer)(int is_error, const char *styled, const char *plain);

/* One line with its role (enum pkg_line). Tables are kept until their end
 * and then drawn with their columns sized; pkg_style_flush draws one that
 * was never ended. */
void pkg_style_line(pkg_style_writer write, int kind, int is_error, const char *text);
void pkg_style_flush(pkg_style_writer write);

/* The capabilities in force, for a caller that draws something itself
 * (the usage text). `is_error` picks the stream. */
const struct pkg_style_caps *pkg_style_caps(int is_error);

/* SGR sequences, empty when the stream has no styling: pkg_style_sgr(1,
 * "1") is bold on stderr, pkg_style_sgr(0, "") is reset on stdout. */
const char *pkg_style_sgr(int is_error, const char *code);

#endif
