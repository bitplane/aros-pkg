/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The activity line: what pkg is doing while a step takes time, and that the
 * step is advancing. One line, rewritten in place, always the same shape:
 * the mark, a verb, the object, a measure.
 *
 *     (O) downloading AROS-20260919-contrib.tar.bz2  212 of 637 MB  6 MB/s  1:10 left
 *      O  reading the archive  38%
 *      o  checking contrib-nightly  57 of 208
 *      .  waiting for aros-pkg.azurewebsites.net
 *
 * The mark is the project's logo pulsing in place: a drop, a ring that
 * expands and fades, the drop again. It advances when the WORK advances and
 * never on a clock of its own, because pkg has no threads; a mark that has
 * stopped moving therefore means pkg is really stuck, which is what a person
 * wants to be able to see. A step that ends quickly shows nothing at all,
 * and the line is erased when the step ends, so the result sentence prints
 * where it stood.
 *
 * The line is never drawn in machine output, never written to a LOG file and
 * never shown when nothing is watching: the caller decides that by giving,
 * or not giving, a show function.
 */

#ifndef PKG_ACTIVITY_H
#define PKG_ACTIVITY_H

/* Where the line goes. `text` is the whole line, mark included; an empty
 * text means the step ended and the line must be erased. NULL, the default,
 * turns the activity line off and makes every call below cost nothing. */
typedef void (*pkg_activity_show_fn)(void *user, const char *text);
/* Where the sentence that announces a step goes: an ordinary line, printed
 * once, which stays on the screen after the step has ended. */
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
 * What pkg is about to do is said at once, in a line of its own, when the
 * work is known to be long: a person reads it before the wait, not after.
 * When the size is unknown, that sentence waits with the line, and both
 * appear once the step has lasted longer than a person waits without an
 * answer (about half a second; PKG_PROGRESS_AFTER in milliseconds overrides
 * it, 0 for a test). A step that ends before then says nothing at all. */
void pkg_activity_step(const char *verb, const char *object,
                       long long whole, int kind, const char *word);

/* Work advanced. Each of these draws at most eight times a second, so a
 * fast loop does not spin the mark madly, and draws nothing before the
 * delay above has passed.
 *   bytes:   a transfer; with a total, a rate and the time left as well
 *   percent: a share of a whole, as "38%"
 *   count:   "57 of 208", or "3 of 9 parts" when a unit names them */
void pkg_activity_bytes(long long done, long long total);
void pkg_activity_percent(long long done, long long total);
void pkg_activity_count(unsigned long long i, unsigned long long n, const char *unit);

/* A blocking wait with nothing to call back: a connect, a TLS handshake, a
 * spawned curl. The text is printed once, straight away, and the mark does
 * not move, because pretending it advances would say the opposite of the
 * truth. NULL ends the wait without ending the step. */
void pkg_activity_waiting(const char *host);

/* The step ended: the line is erased. Safe when no step is running. */
void pkg_activity_done(void);

#endif
