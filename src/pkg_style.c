/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * See pkg_style.h. The layout, for a terminal with colour:
 *
 *   ✓ installed hello 1.2 into SYS:            result: mark, bold up to ':'
 *     3 files, signed by 26ffb2bc                 detail: dim
 *     adopted   2 files already there             item: word coloured by meaning
 *     → to run it, mount the image ...            hint: arrow, wrapped
 *   ! warning: no executable in the drawer       warning: yellow
 *   ✗ install: nothere is not in the channel     refusal: red, verb named
 *     → check the name; pkg SHOW ...              next step
 *
 *   Package    Version  Kind         Files        table: header bold, columns
 *   hello      1.2      application  3 files             sized to their content
 *
 * Without colour the same lines are plain: no marks, "warning:" and
 * "next:" spelled out, and the columns kept, so a script reads them.
 */

#include "pkg.h"
#include "pkg_style.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pkg_style_caps caps[2];   /* [0] stdout, [1] stderr */
static const char *verb = "pkg";
static int aros;
static int colour256;                   /* the terminal has the 256-colour palette */
static int activity_len;                /* the activity line standing on the screen, in
                                           characters; 0 when none is */
static int activity_err;                /* the stream it was drawn on */

/* ---- what the terminal has ------------------------------------------- */

static int env_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    size_t i;
    if (v == NULL)
        return 0;
    for (i = 0; v[i] && value[i]; i++)
        if (tolower((unsigned char)v[i]) != tolower((unsigned char)value[i]))
            return 0;
    return v[i] == '\0' && value[i] == '\0';
}

static int env_has(const char *name, const char *needle)
{
    const char *v = getenv(name);
    size_t n = strlen(needle), i;
    if (v == NULL)
        return 0;
    for (i = 0; v[i]; i++) {
        size_t j;
        for (j = 0; j < n && v[i + j]; j++)
            if (tolower((unsigned char)v[i + j]) != tolower((unsigned char)needle[j]))
                break;
        if (j == n)
            return 1;
    }
    return 0;
}

static int locale_utf8(void)
{
    static const char *const names[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (v != NULL && *v)
            return env_has(names[i], "utf-8") || env_has(names[i], "utf8");
    }
    return 0;
}

static int columns(void)
{
    const char *c = getenv("COLUMNS");
    int n = c ? atoi(c) : 0;
    if (n >= 40 && n <= 500)
        return n;
    return 80;
}

void pkg_style_init(int out_interactive, int err_interactive, int on_aros)
{
    int i;
    /* The project's purple is a 256-colour number; where the terminal has
     * only the eight, plain magenta stands for it. */
    colour256 = env_has("TERM", "256color") || env_has("TERM", "direct")
                || getenv("COLORTERM") != NULL;
    int force_on = env_is("PKG_COLOR", "always") || env_is("PKG_COLOR", "1");
    int force_off = env_is("PKG_COLOR", "never") || env_is("PKG_COLOR", "0")
                    || getenv("NO_COLOR") != NULL || env_is("TERM", "dumb");
    aros = on_aros;
    for (i = 0; i < 2; i++) {
        int on = i == 0 ? out_interactive : err_interactive;
        if (force_off) on = 0;
        if (force_on) on = 1;
        caps[i].bold = on;
        caps[i].colour = on;      /* on AROS: pens, translated in sgr() */
        caps[i].utf8 = on && !aros && locale_utf8();
        caps[i].width = columns();
    }
}

void pkg_style_verb(const char *v)
{
    verb = v ? v : "pkg";
}

const struct pkg_style_caps *pkg_style_caps(int is_error)
{
    return &caps[is_error ? 1 : 0];
}

/* ---- pieces ---------------------------------------------------------- */

#define ESC "\033["
static const char *sgr(int is_error, const char *code)
{
    /* A ring of buffers, so several calls can stand in one expression. */
    static char ring[8][16];
    static unsigned next;
    char *b = ring[next++ % 8u];
    struct pkg_style_caps *c = &caps[is_error ? 1 : 0];
    if (!c->bold)
        return "";
    if (aros) {
        /* The Amiga console: its colour numbers select the screen's pens,
         * and only pens 0 to 3 differ on the standard palette (grey, black,
         * white, light blue); faint does nothing, bold, italic and inverse
         * do. Checked on hosted AROS, 2026-09-19. So: light blue for what is
         * secondary, bold light blue for marks and good news, inverse video
         * for bad news, italic for what asks a look. */
        if (strcmp(code, "38;5;135") == 0) code = "1;33";   /* no purple: the highlight pen */
        else if (strcmp(code, "2") == 0) code = "33";
        else if (strcmp(code, "32") == 0 || strcmp(code, "36") == 0) code = "1;33";
        else if (strcmp(code, "31") == 0) code = "7";
        else if (strcmp(code, "33") == 0) code = "3";
    } else if (!c->colour && code[0] == '3' && code[1] != '\0') {
        /* Colours only where they mean what they say. */
        return "";
    } else if (!colour256 && strcmp(code, "38;5;135") == 0) {
        code = "35";
    }
    snprintf(b, sizeof ring[0], ESC "%sm", code);
    return b;
}

const char *pkg_style_sgr(int is_error, const char *code)
{
    return sgr(is_error, code);
}

#define BOLD   "1"
#define DIM    "2"
#define ITALIC "3"
#define RESET  "0"
#define RED    "31"
#define PURPLE "38;5;135"   /* the project's logo, where the palette has it */
#define GREEN  "32"
#define YELLOW "33"
#define CYAN   "36"

/* The mark before a line, or "" when the stream is plain. The AROS console
 * is Latin-1, where a thin '+' hardly shows; a guillemet does. */
static const char *mark(int is_error, const char *utf, const char *ascii)
{
    struct pkg_style_caps *c = &caps[is_error ? 1 : 0];
    if (!c->bold)
        return "";
    if (aros && strcmp(ascii, "+ ") == 0)
        return "\xBB ";
    return c->utf8 ? utf : ascii;
}

struct buf { char *p; size_t len, cap; };

static void badd(struct buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1u > b->cap) {
        size_t c = b->cap ? b->cap : 256u;
        char *q;
        while (c < b->len + n + 1u) c *= 2u;
        q = (char *)realloc(b->p, c);
        if (q == NULL) return;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void bfree(struct buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* Text wrapped at the stream's width: the first line after `first`, the
 * others after `rest`, breaking at spaces. Both buffers get the words;
 * only `styled` gets the sequences `open` and `close` around the text. */
static void wrapped(struct buf *styled, struct buf *plain, int is_error,
                    const char *first, const char *rest, const char *open,
                    const char *text)
{
    /* A plain stream is for a script or a log: one line stays one line. */
    int width = caps[is_error ? 1 : 0].bold ? caps[is_error ? 1 : 0].width : 1 << 20;
    int indent = (int)strlen(rest);
    const char *p = text;
    int line = 0;
    const char *close = sgr(is_error, RESET);
    while (*p) {
        int room = width - (line == 0 ? (int)strlen(first) : indent) - 1;
        size_t take = strlen(p);
        if (room < 20) room = 20;
        if ((int)take > room) {
            size_t k = (size_t)room;
            while (k > 0 && p[k] != ' ') k--;
            take = k > 0 ? k : (size_t)room;
        }
        badd(styled, line == 0 ? first : rest);
        badd(plain, line == 0 ? first : rest);
        if (*open) badd(styled, open);
        {
            char *piece = (char *)malloc(take + 1u);
            if (piece == NULL) return;
            memcpy(piece, p, take);
            piece[take] = '\0';
            badd(styled, piece);
            badd(plain, piece);
            free(piece);
        }
        if (*open) badd(styled, close);
        badd(styled, "\n");
        badd(plain, "\n");
        p += take;
        while (*p == ' ') p++;
        line++;
    }
    if (line == 0) {           /* an empty line keeps its place */
        badd(styled, first);  badd(plain, first);
        badd(styled, "\n");   badd(plain, "\n");
    }
}

/* The colour of an item's word, by what it means to the person. */
static const char *word_colour(const char *w)
{
    if (strcmp(w, "missing") == 0 || strcmp(w, "changed") == 0 || strcmp(w, "refused") == 0)
        return RED;
    if (strcmp(w, "kept") == 0 || strcmp(w, "edited") == 0 || strcmp(w, "moved") == 0
        || strcmp(w, "aside") == 0 || strcmp(w, "left out") == 0 || strncmp(w, "skipped", 7) == 0
        || strncmp(w, "not ", 4) == 0)
        return YELLOW;
    if (strncmp(w, "would", 5) == 0)
        return CYAN;
    if (strcmp(w, "adopted") == 0 || strcmp(w, "unchanged") == 0 || strcmp(w, "restored") == 0
        || strcmp(w, "added") == 0 || strcmp(w, "published") == 0 || strncmp(w, "upgraded", 8) == 0)
        return GREEN;
    return BOLD;
}

/* The colour of a table cell that states a condition. */
static const char *cell_colour(const char *cell)
{
    if (strcmp(cell, "ok") == 0 || strcmp(cell, "current") == 0 || strncmp(cell, "intact", 6) == 0)
        return GREEN;
    if (strstr(cell, "missing") != NULL || strstr(cell, "changed") != NULL)
        return RED;
    if (strncmp(cell, "upgradable", 10) == 0)
        return YELLOW;
    if (strncmp(cell, "withdrawn", 9) == 0 || strncmp(cell, "no longer", 9) == 0
        || strncmp(cell, "files edited", 12) == 0 || strncmp(cell, "bad", 3) == 0
        || strcmp(cell, "damaged") == 0 || strstr(cell, "fail") != NULL)
        return RED;
    return NULL;
}

/* ---- tables ----------------------------------------------------------- */

#define MAX_COLS 8

struct tline { int kind; int is_error; char *text; };
static struct tline *table;
static size_t table_n, table_cap;
static int in_table;
static int after_table;   /* the last thing drawn was a table */

static void table_add(int kind, int is_error, const char *text)
{
    struct tline *t;
    if (table_n == table_cap) {
        size_t c = table_cap ? table_cap * 2u : 32u;
        struct tline *q = (struct tline *)realloc(table, c * sizeof *q);
        if (q == NULL) return;
        table = q;
        table_cap = c;
    }
    t = &table[table_n++];
    t->kind = kind;
    t->is_error = is_error;
    t->text = (char *)malloc(strlen(text) + 1u);
    if (t->text) strcpy(t->text, text);
}

static int split(char *text, char **cells)
{
    int n = 0;
    char *p = text;
    while (n < MAX_COLS) {
        char *tab = strchr(p, '\t');
        cells[n++] = p;
        if (tab == NULL) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

/* Display width: bytes, less the continuation bytes of UTF-8. */
static int dwidth(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static void pad(struct buf *b, int n)
{
    while (n-- > 0) badd(b, " ");
}

static void draw_line(pkg_style_writer write, int kind, int is_error, const char *text);

/* The activity line's mark is three character cells wide; in bytes that is
 * three, or four where the middle dot arrives as UTF-8 (0xC2 0xB7). */
static size_t mark_bytes(const char *text)
{
    return text[0] != '\0' && (unsigned char)text[1] == 0xC2u ? 4u : 3u;
}

/* Take the activity line off the screen, leaving the cursor at the left of
 * the line it stood on, so that whatever comes next starts there. */
static void erase_activity(struct buf *b, int e)
{
    if (activity_len <= 0)
        return;
    badd(b, "\r");
    if (caps[e].bold) {
        badd(b, ESC "K");
    } else {
        pad(b, activity_len);
        badd(b, "\r");
    }
    activity_len = 0;
}

static void table_draw(pkg_style_writer write)
{
    int widths[MAX_COLS] = { 0 }, ncols = 0;
    size_t i;
    /* 1. widths, over the header and the rows */
    for (i = 0; i < table_n; i++) {
        char *copy, *cells[MAX_COLS];
        int n, k;
        if (table[i].kind != PKG_LINE_HEAD && table[i].kind != PKG_LINE_ROW) continue;
        copy = (char *)malloc(strlen(table[i].text) + 1u);
        if (copy == NULL) continue;
        strcpy(copy, table[i].text);
        n = split(copy, cells);
        if (n > ncols) ncols = n;
        for (k = 0; k < n; k++) {
            int w = dwidth(cells[k]);
            if (w > widths[k]) widths[k] = w;
        }
        free(copy);
    }
    /* 2. the lines, in order */
    for (i = 0; i < table_n; i++) {
        struct buf styled = { NULL, 0, 0 }, plain = { NULL, 0, 0 };
        char *copy, *cells[MAX_COLS];
        int n, k, e = table[i].is_error;
        if (table[i].kind != PKG_LINE_HEAD && table[i].kind != PKG_LINE_ROW) {
            draw_line(write, table[i].kind, e, table[i].text);
            continue;
        }
        copy = (char *)malloc(strlen(table[i].text) + 1u);
        if (copy == NULL) continue;
        strcpy(copy, table[i].text);
        n = split(copy, cells);
        for (k = 0; k < n; k++) {
            const char *colour = NULL;
            int last = k == n - 1;
            if (table[i].kind == PKG_LINE_HEAD)
                colour = BOLD;
            else if (k > 0)
                colour = cell_colour(cells[k]);
            if (colour) badd(&styled, sgr(e, colour));
            badd(&styled, cells[k]);
            if (colour) badd(&styled, sgr(e, RESET));
            badd(&plain, cells[k]);
            if (!last) {
                pad(&styled, widths[k] - dwidth(cells[k]) + 2);
                pad(&plain, widths[k] - dwidth(cells[k]) + 2);
            }
        }
        badd(&styled, "\n");
        badd(&plain, "\n");
        write(e, styled.p ? styled.p : "\n", plain.p ? plain.p : "\n");
        bfree(&styled);
        bfree(&plain);
        free(copy);
    }
    for (i = 0; i < table_n; i++) free(table[i].text);
    table_n = 0;
    in_table = 0;
    after_table = 1;
}

void pkg_style_flush(pkg_style_writer write)
{
    if (in_table) table_draw(write);
}

/* ---- lines ------------------------------------------------------------ */

static void draw_line(pkg_style_writer write, int kind, int is_error, const char *text)
{
    struct buf styled = { NULL, 0, 0 }, plain = { NULL, 0, 0 };
    int e = is_error;

    switch (kind) {
    case PKG_LINE_RESULT:
    case PKG_LINE_PROBLEM: {
        /* Bold up to the first ": ", dim after it: the deed, then its figures. */
        const char *colon = strstr(text, ": ");
        char head[1024];
        int bad = kind == PKG_LINE_PROBLEM;
        const char *m = bad ? mark(e, "\xE2\x9C\x97 ", "x ")     /* ✗ */
                            : mark(e, "\xE2\x9C\x93 ", "+ ");    /* ✓ */
        if (colon && colon - text < (long)sizeof head) {
            memcpy(head, text, (size_t)(colon - text));
            head[colon - text] = '\0';
        } else {
            snprintf(head, sizeof head, "%s", text);
            colon = NULL;
        }
        if (after_table && caps[e].bold) badd(&styled, "\n");
        badd(&styled, sgr(e, bad ? RED : GREEN)); badd(&styled, m); badd(&styled, sgr(e, RESET));
        badd(&styled, sgr(e, BOLD)); badd(&styled, head); badd(&styled, sgr(e, RESET));
        badd(&styled, "\n");
        if (colon) {
            /* The figures on their own line, wrapped, so the deed stands alone. */
            struct buf p2 = { NULL, 0, 0 };
            if (caps[e].bold)
                wrapped(&styled, &p2, e, "  ", "  ", sgr(e, DIM), colon + 2);
            else {
                styled.len = 0; if (styled.p) styled.p[0] = '\0';
                badd(&styled, text); badd(&styled, "\n");
            }
            bfree(&p2);
        }
        badd(&plain, text); badd(&plain, "\n");
        break;
    }
    case PKG_LINE_DETAIL:
        wrapped(&styled, &plain, e, "  ", "  ", sgr(e, DIM), text);
        break;
    case PKG_LINE_NOTE:
        wrapped(&styled, &plain, e, "  ", "  ", sgr(e, ITALIC), text);
        break;
    case PKG_LINE_ITEM: {
        /* "word\trest": the word padded to a column, coloured by meaning. */
        const char *tab = strchr(text, '\t');
        char word[40];
        const char *rest = tab ? tab + 1 : "";
        char lead[64];
        int wl;
        if (tab == NULL) { snprintf(word, sizeof word, "%s", text); rest = ""; }
        else { wl = (int)(tab - text); if (wl > 39) wl = 39; memcpy(word, text, (size_t)wl); word[wl] = '\0'; }
        snprintf(lead, sizeof lead, "  %-9s%s", word, strlen(word) < 9 ? "" : " ");
        badd(&styled, "  ");
        badd(&styled, sgr(e, word_colour(word)));
        badd(&styled, word);
        badd(&styled, sgr(e, RESET));
        pad(&styled, dwidth(word) < 9 ? 9 - dwidth(word) : 1);
        badd(&styled, rest); badd(&styled, "\n");
        badd(&plain, lead); badd(&plain, rest); badd(&plain, "\n");
        break;
    }
    case PKG_LINE_HINT: {
        char first[32], rest[32];
        const char *m = mark(e, "\xE2\x86\x92 ", "> ");    /* → */
        snprintf(first, sizeof first, "  %s%s%s", sgr(e, CYAN), m, sgr(e, RESET));
        snprintf(rest, sizeof rest, "    ");
        if (!caps[e].bold) {
            wrapped(&styled, &plain, e, "  hint: ", "        ", "", text);
        } else {
            struct buf p2 = { NULL, 0, 0 };
            wrapped(&styled, &p2, e, first, rest, sgr(e, aros ? ITALIC : DIM), text);
            bfree(&p2);
            wrapped(&p2, &plain, e, "  hint: ", "        ", "", text);
            bfree(&p2);
        }
        break;
    }
    case PKG_LINE_WARNING: {
        char first[48];
        const char *m = mark(e, "! ", "! ");
        snprintf(first, sizeof first, "%s%swarning:%s ", sgr(e, YELLOW), m, sgr(e, RESET));
        if (!caps[e].bold) {
            wrapped(&styled, &plain, e, "warning: ", "  ", "", text);
        } else {
            struct buf p2 = { NULL, 0, 0 };
            wrapped(&styled, &p2, e, first, "  ", "", text);
            bfree(&p2);
            wrapped(&p2, &plain, e, "warning: ", "  ", "", text);
            bfree(&p2);
        }
        break;
    }
    case PKG_LINE_REFUSAL: {
        char first[80], plain_first[80];
        const char *m = mark(e, "\xE2\x9C\x97 ", "x ");    /* ✗ */
        snprintf(first, sizeof first, "%s%s%s:%s ", sgr(e, RED), m, verb, sgr(e, RESET));
        snprintf(plain_first, sizeof plain_first, "pkg %s: ", verb);
        if (!caps[e].bold) {
            wrapped(&styled, &plain, e, plain_first, "  ", "", text);
        } else {
            struct buf p2 = { NULL, 0, 0 };
            wrapped(&styled, &p2, e, first, "  ", sgr(e, BOLD), text);
            bfree(&p2);
            wrapped(&p2, &plain, e, plain_first, "  ", "", text);
            bfree(&p2);
        }
        break;
    }
    case PKG_LINE_NEXT: {
        char first[32];
        const char *m = mark(e, "\xE2\x86\x92 ", "> ");
        snprintf(first, sizeof first, "  %s%s%s", sgr(e, CYAN), m, sgr(e, RESET));
        if (!caps[e].bold) {
            wrapped(&styled, &plain, e, "  next: ", "        ", "", text);
        } else {
            struct buf p2 = { NULL, 0, 0 };
            wrapped(&styled, &p2, e, first, "    ", "", text);
            bfree(&p2);
            wrapped(&p2, &plain, e, "  next: ", "        ", "", text);
            bfree(&p2);
        }
        break;
    }
    case PKG_LINE_PROGRESS: {
        /* The activity line: the mark, a verb, the object, a measure, drawn
         * in place and erased when the step ends. Only `styled` is filled,
         * so a LOG file holds none of it. The mark is the project's logo,
         * purple where the palette has it; the rest is dim. A stream with no
         * styling still rewrites in place, with spaces and a carriage return
         * instead of the erase sequence, so that PKG_COLOR=never leaves no
         * escape sequence anywhere. */
        size_t m;
        if (*text == '\0') {
            int had = activity_len;
            erase_activity(&styled, e);
            if (caps[e].bold && had > 0)
                badd(&styled, ESC "[?25h");
            break;
        }
        m = mark_bytes(text);
        /* The cursor sits wherever the line ends and jumps with it: hidden
         * while the line is alive, shown again when it is erased. */
        if (caps[e].bold && activity_len == 0)
            badd(&styled, ESC "[?25l");
        badd(&styled, "\r  ");
        if (caps[e].bold) {
            char head[8];
            snprintf(head, sizeof head, "%.*s", (int)m, text);
            badd(&styled, sgr(e, PURPLE)); badd(&styled, head); badd(&styled, sgr(e, RESET));
            badd(&styled, sgr(e, DIM)); badd(&styled, text + m); badd(&styled, sgr(e, RESET));
            badd(&styled, ESC "K");
        } else {
            int was = activity_len, now = (int)strlen(text);
            badd(&styled, text);
            pad(&styled, was > now ? was - now : 0);
        }
        activity_len = (int)strlen(text) + 2;
        activity_err = e;
        break;
    }
    default:
        badd(&styled, text); badd(&styled, "\n");
        badd(&plain, text); badd(&plain, "\n");
        break;
    }
    write(e, styled.p ? styled.p : "", plain.p ? plain.p : "");
    bfree(&styled);
    bfree(&plain);
    if (kind != PKG_LINE_PROGRESS)
        after_table = 0;
}

void pkg_style_line(pkg_style_writer write, int kind, int is_error, const char *text)
{
    /* Nothing is ever written over the activity line: it goes first, so a
     * result, a refusal or a table starts on a line of its own with no
     * frame or carriage return left behind it. */
    if (kind != PKG_LINE_PROGRESS && activity_len > 0) {
        struct buf clear = { NULL, 0, 0 };
        int e = activity_err;
        erase_activity(&clear, e);
        if (caps[e].bold)
            badd(&clear, ESC "[?25h");          /* the cursor comes back with the text */
        write(e, clear.p != NULL ? clear.p : "", "");
        bfree(&clear);
    }
    if (kind == PKG_LINE_HEAD) {
        if (in_table) table_draw(write);
        in_table = 1;
        table_add(kind, is_error, text);
        return;
    }
    if (in_table) {
        if (kind == PKG_LINE_END) { table_draw(write); return; }
        table_add(kind, is_error, text);
        return;
    }
    if (kind == PKG_LINE_END)
        return;
    draw_line(write, kind, is_error, text);
}
