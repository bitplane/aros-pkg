/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * A command's words read against its template, the way AmigaDOS's ReadArgs
 * reads them, so that what a verb accepts is written in one place and every
 * word it does not accept is refused by name instead of ignored.
 *
 * A template is a comma-separated list of items, each a word with modifiers:
 *
 *   ROOT/K     a keyword and its value: ROOT <dir>, or ROOT=<dir>
 *   DRYRUN/S   a switch: the word alone
 *   PACKAGE    a word taken by its place on the line; the name is only for
 *              usage and refusals, never matched: a drawer may be called
 *              "drawer" and must not swallow the word after it
 *   PACKAGE/M  every remaining word taken by place (one /M per template)
 *   /A         required
 *   /W         taken by place even when it is spelled like a keyword: the
 *              channel after CHANNEL ADD may be a directory called "root"
 *   /R         shown as required, but the caller may fill it from elsewhere
 *              (a root from the environment): not checked here
 *   /G         taken on every verb (MACHINE, TRACE, LOG): left out of the
 *              verb's own usage and of the words a refusal lists
 *   =<text>    how the value is shown in usage: KEY/K=<public key>
 *
 * Keywords are matched in any case. Words are never modified. Everything
 * here is portable C99, with no allocation: values point into argv.
 */

#ifndef PKG_ARGS_H
#define PKG_ARGS_H

#include <stddef.h>

#define PKG_ARGS_ITEMS 48    /* items in one template */
#define PKG_ARGS_WORDS 64    /* words one /M item takes */

enum {
    PKG_ARG_KEY    = 1,      /* /K */
    PKG_ARG_SWITCH = 2,      /* /S */
    PKG_ARG_NEEDED = 4,      /* /A */
    PKG_ARG_MULTI  = 8,      /* /M */
    PKG_ARG_WORD   = 16,     /* /W */
    PKG_ARG_ROOTED = 32,     /* /R */
    PKG_ARG_GLOBAL = 64      /* /G: taken on every verb, left out of its usage */
};

struct pkg_arg {
    char        name[24];    /* as written in the template, upper case */
    char        shown[32];   /* the value as usage shows it, "" for the default */
    unsigned    flags;
    int         set;         /* given on the line */
    const char *value;       /* /K and positional: the value; /S: NULL */
    const char *values[PKG_ARGS_WORDS];   /* /M: every word, in order */
    size_t      nvalues;
};

struct pkg_args {
    struct pkg_arg item[PKG_ARGS_ITEMS];
    size_t         n;
};

/* Reads a template. 0, or -1 for a template that does not parse, which is
 * a mistake in pkg itself: the words say which. */
int pkg_args_template(const char *tmpl, struct pkg_args *out, char *err, size_t errlen);

/* Reads argv[first..argc-1] against a template read by pkg_args_template.
 * `verb` names the command in refusals ("INSTALL", "CHANNEL ADD").
 * `elsewhere`, when given, says whether a word is a keyword of another verb:
 * such a word typed in capitals is refused as a keyword this verb does not
 * take, where a word in any other case is taken by place, since a package
 * may well be called "file". 0, or -1 with the refusal in err. */
int pkg_args_read(struct pkg_args *a, const char *verb, int first, int argc, char **argv,
                  int (*elsewhere)(const char *word), char *err, size_t errlen);

/* The item of that name, or NULL. */
struct pkg_arg *pkg_args_item(struct pkg_args *a, const char *name);

/* The template as a person reads it: "<name>... ROOT <dir> [AT <dir>]",
 * each keyword's value shown by `shown` unless the template says. */
void pkg_args_syntax(const struct pkg_args *a, const char *(*shown)(const char *name),
                     char *out, size_t len);

#endif
