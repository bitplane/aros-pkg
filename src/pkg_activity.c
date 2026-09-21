/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * See pkg_activity.h. Three things are decided here and nowhere else:
 *
 * WHEN a line appears. A step that ends quickly must print nothing at all,
 * so the first frame waits until the step has lasted longer than a person
 * waits without an answer: half a second, or PKG_PROGRESS_AFTER
 * milliseconds, which a test sets to 0.
 *
 * HOW FAST the mark moves. Every draw is an advance of the work, never of a
 * clock: pkg has one thread and gains none here. A fast loop would then spin
 * the mark far too fast to read, so a draw is refused when the last one was
 * less than an eighth of a second ago. What this cannot do, and must not
 * pretend to do, is animate a blocking call: a connect or a spawned curl
 * says "waiting for <host>" once and the mark stands still, which is the
 * truth about what pkg is doing.
 *
 * WHAT the measure says. A percentage, a rate and the time left are stated
 * only where pkg knows the whole: a download's Content-Length, the size of
 * the archive being read, i of n for a counter. Without one, the verb and
 * the object stand alone rather than a figure nobody can trust. A
 * percentage is never more than 100 and never falls back within a step.
 */

#include "pkg_activity.h"
#include "pkg_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES    6
#define MIN_GAP   125            /* milliseconds between draws: eight a second */
#define DEF_AFTER 500            /* milliseconds before the first frame */

static pkg_activity_show_fn show_fn;
static pkg_activity_say_fn  say_fn;
static void               *show_user;
static int  said;                        /* the step has been announced */
static int  measured;                    /* its size is known: it shows a figure */
static int  depth;                       /* steps inside steps */
static char step_size[48];               /* "637 MB", "208 versions", or empty */
static char last_line[400];              /* what this step last drew */
static int                 charset;
static int                 after = -1;

static char  step_verb[48], step_object[72];
static char  verb[48], object[72];
static int   running, shown, still, frame;
static long long began, drawn_at;

/* the smoothed rate, and the last percentage, both of one step */
static long long rate_at, rate_done;
static double    rate;           /* bytes a second, 0 until two samples */
static int       last_pct;

void pkg_activity_to(pkg_activity_show_fn show, pkg_activity_say_fn say, void *user)
{
    show_fn = show;
    say_fn = say;
    show_user = user;
    running = shown = still = 0;
}

void pkg_activity_charset(int cs)
{
    charset = cs;
}

/* PKG_PROGRESS_AFTER, read once: the milliseconds a step may last before it
 * says anything. 0 shows the line at once, which is what a test wants. */
static int after_ms(void)
{
    if (after < 0) {
        const char *v = getenv("PKG_PROGRESS_AFTER");
        after = DEF_AFTER;
        if (v != NULL && *v != '\0') {
            long n = strtol(v, NULL, 10);
            if (n >= 0 && n <= 600000) after = (int)n;
        }
    }
    return after;
}

/* ---- the mark --------------------------------------------------------- */

/* The owner's frames: a drop, a ring that expands, and the drop again. All
 * six exist in Latin-1, so the AROS console shows the same pulse; where the
 * terminal cannot be trusted with the middle dot, a full stop stands for it
 * rather than a stray byte. */
static const char *frame_text(int i)
{
    static const char *const fixed[FRAMES] = { NULL, " o ", " O ", "(O)", "( )", NULL };
    static char dot[8];
    if (fixed[i] != NULL)
        return fixed[i];
    snprintf(dot, sizeof dot, " %s ",
             charset == PKG_ACTIVITY_LATIN1 ? "\xB7"
             : charset == PKG_ACTIVITY_UTF8 ? "\xC2\xB7" : ".");
    return dot;
}

/* ---- the measure ------------------------------------------------------ */

/* A figure a person reads at a glance: whole above ten, one decimal below,
 * so "637 MB" and "0.4 MB" both say what they are worth. */
static void figure(char *out, size_t ol, double v)
{
    if (v >= 10.0 || v <= -10.0)
        snprintf(out, ol, "%.0f", v);
    else
        snprintf(out, ol, "%.1f", v);
}

/* "212 of 637 MB", or "212 MB" when the whole is not known. The unit
 * follows the larger of the two, so it does not change as the file
 * arrives. */
static void bytes_text(char *out, size_t ol, long long done, long long total)
{
    long long ref = total > 0 ? total : done;
    const char *unit = "bytes";
    double div = 1.0;
    char a[24], b[24];
    if (ref >= 1048576) { unit = "MB"; div = 1048576.0; }
    else if (ref >= 1024) { unit = "KB"; div = 1024.0; }
    figure(a, sizeof a, (double)done / div);
    if (total > 0) {
        figure(b, sizeof b, (double)total / div);
        snprintf(out, ol, "%s/%s %s", a, b, unit);
    } else {
        snprintf(out, ol, "%s %s", a, unit);
    }
}

/* "  6 MB/s", from the smoothed rate; nothing while it is still unknown. */
static void rate_text(char *out, size_t ol)
{
    const char *unit = "bytes";
    double div = 1.0, v = rate;
    char a[24];
    out[0] = '\0';
    if (v <= 0.0)
        return;
    if (v >= 1048576.0) { unit = "MB"; div = 1048576.0; }
    else if (v >= 1024.0) { unit = "KB"; div = 1024.0; }
    figure(a, sizeof a, v / div);
    snprintf(out, ol, "  %s %s/s", a, unit);
}

/* "  1:10 left", from the rate and what is left of the whole. */
static void left_text(char *out, size_t ol, long long done, long long total)
{
    long long s;
    out[0] = '\0';
    if (rate <= 0.0 || total <= 0 || done >= total)
        return;
    s = (long long)((double)(total - done) / rate) + 1;
    if (s > 99 * 3600)
        return;
    if (s >= 3600)
        snprintf(out, ol, "  %lld:%02lld:%02lld left", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, ol, "  %lld:%02lld left", s / 60, s % 60);
}

/* ---- drawing ---------------------------------------------------------- */

/* Two shapes, and the mark belongs to one of them. A step that can say how
 * far it has got says that: the figure moves, so nothing else need move. A
 * step that cannot carries the pulsing mark instead, which is there to say
 * that pkg is alive when there is nothing else to say. */
static void put(const char *measure)
{
    char line[400];
    if (measure[0] != '\0')
        snprintf(line, sizeof line, "  %s%s%s, %s", verb,
                 object[0] ? " " : "", object, measure + 2);
    else
        snprintf(line, sizeof line, "%s %s%s%s", frame_text(frame), verb,
                 object[0] ? " " : "", object);
    shown = 1;
    snprintf(last_line, sizeof last_line, "%s", line);
    show_fn(show_user, line);
    if (measure[0] == '\0')
        frame = (frame + 1) % FRAMES;
}

static void announce(void);

/* Whether this advance may be drawn now, and the moment it is drawn at:
 * after the delay, and no sooner than an eighth of a second after the last
 * one. A wait that is standing still is replaced straight away. */
static int may_draw(long long *now)
{
    if (show_fn == NULL || !running)
        return 0;
    *now = pkg_fs_now_ms();
    if (still) {
        still = 0;               /* work has come back: the step speaks again */
        snprintf(verb, sizeof verb, "%s", step_verb);
        snprintf(object, sizeof object, "%s", step_object);
        frame = 0;
    } else if (!shown) {
        /* A step that knows its size knew it was long before it started, and
         * shows its line at once, from the first byte. A step that does not
         * waits, so that what turns out to be quick says nothing at all. */
        if (!measured && *now - began < after_ms())
            return 0;
    } else if (*now - drawn_at < MIN_GAP) {
        return 0;
    }
    drawn_at = *now;
    return 1;
}

/* ---- a step ----------------------------------------------------------- */

/* An object is a file name or a channel: long ones are cut short, on a
 * character and not in the middle of one, so the line keeps its shape. */
static void set_object(char *out, size_t ol, const char *in)
{
    size_t n;
    out[0] = '\0';
    if (in == NULL || *in == '\0')
        return;
    snprintf(out, ol, "%s", in);
    n = strlen(out);
    if (n + 1 == ol)
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80)
            out[n--] = '\0';
}

/* The sentence that says what is about to take time: the verb, what it acts
 * on, and how much of it there is. Printed once per step, and never twice. */
static void announce(void)
{
    char line[200];
    if (said || say_fn == NULL)
        return;
    said = 1;
    snprintf(line, sizeof line, "%s%s%s%s%s", step_verb,
             step_object[0] ? " " : "", step_object,
             step_size[0] ? ", " : "", step_size);
    say_fn(show_user, line);
}

/* Work worth announcing before it starts rather than after half a second of
 * silence: enough bytes that a person would wonder, or enough things to
 * count. Below this the sentence waits with the line, and a step that ends
 * quickly says nothing at all. */
#define BIG_BYTES  (4ll << 20)
#define BIG_THINGS 200ll

/* What a step is, kept while a step inside it runs. */
struct held {
    char verb[48], object[72], size[48], line[400];
    int said, measured, last_pct, shown;
    long long began;
};
static struct held stack[4];

void pkg_activity_step(const char *v, const char *o, long long whole, int kind, const char *word)
{
    if (show_fn == NULL)
        return;
    if (running && depth < (int)(sizeof stack / sizeof stack[0])) {
        struct held *h = &stack[depth++];
        snprintf(h->verb, sizeof h->verb, "%s", step_verb);
        snprintf(h->object, sizeof h->object, "%s", step_object);
        snprintf(h->size, sizeof h->size, "%s", step_size);
        h->said = said; h->measured = measured; h->last_pct = last_pct; h->began = began;
        h->shown = shown;
        snprintf(h->line, sizeof h->line, "%s", last_line);
    }
    /* What is on the screen stays there until this step draws over it: a step
     * that begins inside another must not blank what that one was saying. */
    snprintf(step_verb, sizeof step_verb, "%s", v != NULL ? v : "working");
    set_object(step_object, sizeof step_object, o);
    snprintf(verb, sizeof verb, "%s", step_verb);
    snprintf(object, sizeof object, "%s", step_object);
    step_size[0] = '\0';
    running = 1;
    measured = kind != PKG_ACTIVITY_NOTHING && whole > 0;
    said = shown = still = 0;
    frame = 0;
    began = drawn_at = pkg_fs_now_ms();
    rate_at = rate_done = 0;
    rate = 0.0;
    last_pct = 0;
    (void)word;
    (void)announce;
}

void pkg_activity_bytes(long long done, long long total)
{
    char measure[240], m[80], r[48], l[72];
    long long now;
    if (!may_draw(&now))
        return;
    /* the rate over the last half second at least, smoothed so it does not
     * jump about while a person reads it */
    if (rate_at == 0) {
        rate_at = now;
        rate_done = done;
    } else if (now - rate_at >= 500 && done >= rate_done) {
        double r2 = (double)(done - rate_done) * 1000.0 / (double)(now - rate_at);
        rate = rate > 0.0 ? rate * 0.7 + r2 * 0.3 : r2;
        rate_at = now;
        rate_done = done;
    }
    bytes_text(m, sizeof m, done, total);
    if (total > 0) {
        rate_text(r, sizeof r);
        left_text(l, sizeof l, done, total);
    } else {
        r[0] = l[0] = '\0';      /* a rate or a time left needs the whole */
    }
    snprintf(measure, sizeof measure, "  %s%s%s", m, r, l);
    put(measure);
}

void pkg_activity_percent(long long done, long long total)
{
    char measure[32];
    long long now;
    int pct;
    if (!may_draw(&now))
        return;
    if (total <= 0) {
        put("");
        return;
    }
    pct = (int)(done * 100 / total);
    if (pct > 100) pct = 100;
    if (pct < last_pct) pct = last_pct;      /* a share never falls back */
    last_pct = pct;
    snprintf(measure, sizeof measure, "  %d%%", pct);
    put(measure);
}

void pkg_activity_count(unsigned long long i, unsigned long long n, const char *unit)
{
    char measure[80];
    long long now;
    if (!may_draw(&now))
        return;
    if (n == 0) {
        put("");
        return;
    }
    if (i > n) i = n;
    snprintf(measure, sizeof measure, "  %llu/%llu%s%s", i, n,
             unit != NULL ? " " : "", unit != NULL ? unit : "");
    put(measure);
}

void pkg_activity_waiting(const char *host)
{
    long long now;
    if (show_fn == NULL || !running)
        return;
    if (host == NULL) {                  /* the wait is over; the step goes on */
        if (still) {
            still = 0;
            snprintf(verb, sizeof verb, "%s", step_verb);
            snprintf(object, sizeof object, "%s", step_object);
            frame = 0;
        }
        return;
    }
    if (!still || strcmp(object, host) != 0) {
        snprintf(verb, sizeof verb, "waiting for");
        set_object(object, sizeof object, host);
        still = 1;
        frame = 0;
        drawn_at = 0;
    }
    now = pkg_fs_now_ms();
    if (!shown && now - began < after_ms())
        return;                          /* a wait that is over in a moment says nothing */
    if (shown && now - drawn_at < MIN_GAP)
        return;
    drawn_at = now;
    put("");                             /* the mark moves: that is the whole of it */
}

void pkg_activity_done(void)
{
    if (show_fn == NULL) {
        running = shown = still = 0;
        return;
    }
    still = 0;
    if (depth > 0) {
        struct held *h = &stack[--depth];
        snprintf(step_verb, sizeof step_verb, "%s", h->verb);
        snprintf(step_object, sizeof step_object, "%s", h->object);
        snprintf(step_size, sizeof step_size, "%s", h->size);
        snprintf(verb, sizeof verb, "%s", h->verb);
        snprintf(object, sizeof object, "%s", h->object);
        said = h->said;
        measured = h->measured;
        last_pct = h->last_pct;
        began = h->began;
        drawn_at = 0;                 /* its figure may be drawn again at once */
        running = 1;
        shown = h->shown;
        snprintf(last_line, sizeof last_line, "%s", h->line);
        if (shown && last_line[0] != '\0')
            show_fn(show_user, last_line);   /* what pkg is still doing, back in its place */
        return;
    }
    if (shown)
        show_fn(show_user, "");       /* nothing is left running: the line goes */
    running = shown = 0;
    last_line[0] = '\0';
}
