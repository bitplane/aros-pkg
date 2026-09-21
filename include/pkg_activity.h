/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * One activity line: measured bytes or counts from zero, and a timed pulse
 * for work whose total is unknown. See docs/activity.md.
 */

#ifndef PKG_ACTIVITY_H
#define PKG_ACTIVITY_H

/* Where the line goes. `text` is the whole line, mark included; an empty
 * text means the step ended and the line must be erased. NULL, the default,
 * turns the activity line off and makes every call below cost nothing. */
typedef void (*pkg_activity_show_fn)(void *user, const char *text);
/* Retained for source compatibility. The announcement and measure share
 * the activity line; the say callback is unused. */
typedef void (*pkg_activity_say_fn)(void *user, const char *text);
void pkg_activity_to(pkg_activity_show_fn show, pkg_activity_say_fn say, void *user);

/* How the mark's middle dot may be written, since the frames must be
 * readable on the terminal they land on: ASCII "." is the default and is
 * always safe, LATIN1 is the single byte 0xB7 (the AROS console), UTF8 is
 * the two bytes of U+00B7. */
enum {
    PKG_ACTIVITY_ASCII = 0,
    PKG_ACTIVITY_LATIN1,
    PKG_ACTIVITY_UTF8
};
void pkg_activity_charset(int cs);

/* What the step's whole is counted in, for the sentence that announces it:
 * bytes read "637 MB", a count reads "208 versions" with the word given. */
enum {
    PKG_ACTIVITY_NOTHING = 0,
    PKG_ACTIVITY_BYTES,
    PKG_ACTIVITY_THINGS
};

/* A step begins: a verb in the tool's voice, what it acts on (may be NULL),
 * and how much there is of it (0 when that is not known yet), counted in
 * one of the kinds above with `word` naming them for a count.
 *
 * A measured step draws its name and zero immediately, before returning
 * to the caller. An unknown step appears on a tick after half a second
 * (PKG_PROGRESS_AFTER overrides the delay in milliseconds).
 * The word parameter is retained for source compatibility; count updates
 * can supply a unit explicitly. */
void pkg_activity_step(const char *verb, const char *object,
                       long long whole, int kind, const char *word);

/* Work advanced. Redraws are limited to eight per second.
 * Bytes with a known total include the rate and estimated time left.
 * Unknown totals pulse after the delay. Counts use "57/208". */
void pkg_activity_bytes(long long done, long long total);
void pkg_activity_percent(long long done, long long total);
void pkg_activity_count(unsigned long long i, unsigned long long n, const char *unit);

/* Name a network wait. NULL restores the enclosing operation's name.
 * Call tick from polling loops while the operation waits. */
void pkg_activity_tick(void);
void pkg_activity_waiting(const char *host);

/* The step ended: the line is erased. Safe when no step is running. */
void pkg_activity_done(void);

#endif
