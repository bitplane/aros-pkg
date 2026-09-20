/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * libpkg: every operation Pkg performs, behind the interface in pkg.h. The
 * command line (pkg_main.c) and anything else that links this reach the same
 * code, and so the same checks.
 *
 * DEPENDS takes a comma-separated list, each "name" or "name >= version". An
 * install resolves the whole graph, fetching and verifying every package,
 * before it places a single file; dependencies go in first and are marked as
 * such in `.pkg/auto`, and removing orphans takes out the ones nothing needs.
 *
 * KIND image turns the drawer into a read-only FFS volume, pkg_image.h, and
 * the package holds that one file, <name>.hdf.
 *
 * A channel is a directory: `index` holds one "name version digest" line per
 * published version, `objects/` holds each payload, its manifest and its
 * signature under the payload's SHA-256.
 *
 * Every package is signed; there is no development mode. The signature covers
 * the manifest, and the manifest names the payload digest, so one signature
 * covers every byte installed. A root pins the key that signed each package the
 * first time it was installed, in `.pkg/keys`, and refuses a different key
 * later unless acceptkey names the new key in full.
 *
 * Selection: with no version, the highest published version (COMPATIBLE with
 * any). With a version, exactly that one (EXACT) or a refusal.
 */

#include "pkg.h"
#include "pkg_container.h"
#include "pkg_ed25519.h"
#include "pkg_sha512.h"
#include "pkg_fs.h"
#include "pkg_image.h"
#include "pkg_manifest.h"
#include "pkg_ameta.h"
#include "pkg_archive.h"
#include "pkg_sha256.h"
#include "pkg_pkginfo.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- exit codes and failure classes ----------------------------------- *
 *
 * One table on every host. A refusal ends with the code of its class, and the
 * code is the same number on AROS, macOS and Windows, so a script written on
 * one reads the same on the others. Every refusal code is at least 10 because
 * AmigaDOS tests for failure with `If ERROR`, which is true from 10 upwards; a
 * usage error is 20, RETURN_FAIL. On POSIX and Windows any non-zero code is a
 * failure, so the AmigaDOS constraint costs them nothing. The ARexx port
 * returns the same number as RC.
 *
 * The first refusal a command meets decides its class. */
enum {
    PKGRC_OK         = PKG_RC_OK,
    PKGRC_REFUSED    = PKG_RC_REFUSED,
    PKGRC_NOTFOUND   = PKG_RC_NOTFOUND,
    PKGRC_INTEGRITY  = PKG_RC_INTEGRITY,
    PKGRC_SIGNATURE  = PKG_RC_SIGNATURE,
    PKGRC_KEY        = PKG_RC_KEY,
    PKGRC_CONFLICT   = PKG_RC_CONFLICT,
    PKGRC_DEPENDENCY = PKG_RC_DEPENDENCY,
    PKGRC_IO         = PKG_RC_IO,
    PKGRC_POLICY     = PKG_RC_POLICY,
    PKGRC_USAGE      = PKG_RC_USAGE
};
#define class_name pkg_class_name

const char *pkg_class_name(int c)
{
    switch (c) {
    case PKGRC_OK:         return "ok";
    case PKGRC_NOTFOUND:   return "not-found";
    case PKGRC_INTEGRITY:  return "integrity";
    case PKGRC_SIGNATURE:  return "signature";
    case PKGRC_KEY:        return "key";
    case PKGRC_CONFLICT:   return "conflict";
    case PKGRC_DEPENDENCY: return "dependency";
    case PKGRC_IO:         return "io";
    case PKGRC_POLICY:     return "policy";
    case PKGRC_USAGE:      return "usage";
    default:             return "refused";
    }
}

/* ---- output ----------------------------------------------------------- *
 *
 * Everything an operation says goes to the caller's sink: fields to `record`
 * in the structured form, sentences to `text` in the other. The flag the
 * code below tests is `machine`, the structured form, a name kept from the
 * command line, where it is the MACHINE keyword. */

static const struct pkg_sink *sink;
static const char *verb_name = "pkg";
static int refused_class;
static int machine;              /* the structured form */
static int dryrun;               /* every check, no write */

/* Text leaves through one of two doors. A sink with `line` gets each line
 * with its role and no framing; a sink with `text` alone gets the text as
 * the command line always printed it, framing and newlines included, so
 * the ARexx port and every other embedder see no change. `plain` is that
 * framed text; `kind` and `bare` the role and the unframed line for `line`,
 * split at its line breaks. */
static void emit_line(int kind, int is_error, const char *bare)
{
    char *copy, *p, *next;
    if (sink->line == NULL)
        return;
    copy = (char *)malloc(strlen(bare) + 1u);
    if (copy == NULL)
        return;
    strcpy(copy, bare);
    for (p = copy; p != NULL; p = next) {
        next = strchr(p, '\n');
        if (next != NULL)
            *next++ = '\0';
        if (next == NULL && *p == '\0' && p != copy)
            break;                       /* the newline that ended the text */
        sink->line(sink->user, kind, is_error, p);
    }
    free(copy);
}

static void emit(int is_error, const char *fmt, va_list ap)
{
    char small[1024], *big = NULL;
    va_list cp;
    int n;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof small) {
        big = (char *)malloc((size_t)n + 1u);
        if (big == NULL)
            return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
    }
    if (sink->line != NULL)
        emit_line(PKG_LINE_TEXT, is_error, big ? big : small);
    else
        sink->text(sink->user, is_error, big ? big : small);
    free(big);
}

/* A line with a role: `fmt` gives the bare line, and `frame` how the text
 * form wraps it ("%s" for none, "  hint: %s\n" for a hint). */
static void emit_kind(int kind, int is_error, const char *frame, const char *fmt, va_list ap)
{
    char small[1024], *big = NULL, *bare;
    va_list cp;
    int n;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    va_copy(cp, ap);
    n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof small) {
        big = (char *)malloc((size_t)n + 1u);
        if (big == NULL)
            return;
        vsnprintf(big, (size_t)n + 1u, fmt, ap);
    }
    bare = big ? big : small;
    if (sink->line != NULL) {
        emit_line(kind, is_error, bare);
    } else {
        size_t need = strlen(bare) + strlen(frame) + 1u;
        char *framed = (char *)malloc(need);
        if (framed != NULL) {
            snprintf(framed, need, frame, bare);
            sink->text(sink->user, is_error, framed);
            free(framed);
        }
    }
    free(big);
}

static void say_kind(int kind, const char *frame, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_kind(kind, 0, frame, fmt, ap);
    va_end(ap);
}

/* What the operation did, one sentence, no newline. */
#define say_result(...) say_kind(PKG_LINE_RESULT, "%s\n", __VA_ARGS__)
/* A result that is bad news, drawn as such; still the operation's answer. */
#define say_problem(...) say_kind(PKG_LINE_PROBLEM, "%s\n", __VA_ARGS__)
/* "1 changed", "2 missing" or "1 changed, 2 missing". */
static const char *damage(size_t changed, size_t missing)
{
    static char text[80];
    if (changed && missing)
        snprintf(text, sizeof text, "%lu changed, %lu missing", (unsigned long)changed,
                 (unsigned long)missing);
    else if (changed)
        snprintf(text, sizeof text, "%lu changed", (unsigned long)changed);
    else
        snprintf(text, sizeof text, "%lu missing", (unsigned long)missing);
    return text;
}
/* A line under a result: a count, a source, a key. */
#define say_detail(...) say_kind(PKG_LINE_DETAIL, "  %s\n", __VA_ARGS__)
/* A file or a package under a result, "kept\tC/Hello (edited)": a word,
 * a tab, the rest. The text form pads the word to eight columns, as the
 * command line always did. */
static void say_item(const char *word, const char *fmt, ...)
{
    char rest[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rest, sizeof rest, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_ITEM, "%s", "%s\t%s", word, rest);
    else
        say_kind(PKG_LINE_ITEM, "  %s\n", "%-8s %s", word, rest);
}
/* A package's line in a batch: its name, then what became of it. */
static void say_pkgline(const char *name, const char *fmt, ...)
{
    char rest[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rest, sizeof rest, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_ITEM, "%s", "%s\t%s", name, rest);
    else
        say_kind(PKG_LINE_ITEM, "%s\n", "%-24s %s", name, rest);
}
/* A remark under a result. */
#define say_note(...) say_kind(PKG_LINE_NOTE, "  note: %s\n", __VA_ARGS__)
/* A table: its header, its rows, cells apart by tabs, and its end. The text
 * form prints the rows with the column widths the command line always
 * used, `widths`, and no header. */
static const int *table_widths;
static void tbl_head(const int *widths, const char *cells)
{
    table_widths = widths;
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_HEAD, "%s", "%s", cells);
}
static void tbl_row(const char *fmt, ...)
{
    char cells[4096], line[4600], *p, *tab;
    int col = 0, at = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cells, sizeof cells, fmt, ap);
    va_end(ap);
    if (sink != NULL && sink->line != NULL) {
        say_kind(PKG_LINE_ROW, "%s", "%s", cells);
        return;
    }
    for (p = cells; p != NULL; p = tab, col++) {
        int w = table_widths ? table_widths[col] : 0;
        tab = strchr(p, '\t');
        if (tab != NULL)
            *tab++ = '\0';
        if (tab != NULL)
            at += snprintf(line + at, sizeof line - (size_t)at, "%-*s ", w, p);
        else
            at += snprintf(line + at, sizeof line - (size_t)at, "%s", p);
        if ((size_t)at >= sizeof line - 1u)
            break;
    }
    say_kind(PKG_LINE_ROW, "%s\n", "%s", line);
}
static void tbl_end(void)
{
    table_widths = NULL;
    if (sink != NULL && sink->line != NULL)
        say_kind(PKG_LINE_END, "%s", "%s", "");
}

/* The operation's account of itself, for the sink's trace, if it has one. */
static void tr(const char *fmt, ...)
{
    char line[1400];
    int n;
    va_list ap;
    if (sink == NULL || sink->trace == NULL)
        return;
    n = snprintf(line, sizeof line, "%s: ", verb_name);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    sink->trace(sink->user, line);
}

/* Text for a person: the answer, or a refusal or warning. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(0, fmt, ap);
    va_end(ap);
}

/* A download a person is watching: "downloading 12.4 of 640.0 MB", rewritten
 * in place like the counter below, and cleared when the file is there. */
static int transfer_shown;
static void transfer_progress(long long done, long long total)
{
    char text[80];
    if (total > 0)
        snprintf(text, sizeof text, "downloading %.1f of %.1f MB", done / 1048576.0, total / 1048576.0);
    else
        snprintf(text, sizeof text, "downloading %.1f MB", done / 1048576.0);
    transfer_shown = 1;
    if (sink->line != NULL)
        sink->line(sink->user, PKG_LINE_PROGRESS, 0, text);
    else
        say("\r  %s", text);
}
static int net_get_watched(const char *url, const char *dest, char *err, size_t errlen)
{
    int rc;
    pkg_fs_on_transfer = (!machine && sink != NULL && sink->progress) ? transfer_progress : NULL;
    transfer_shown = 0;
    rc = pkg_net_get(url, dest, err, errlen);
    pkg_fs_on_transfer = NULL;
    if (transfer_shown) {
        if (sink->line != NULL)
            sink->line(sink->user, PKG_LINE_PROGRESS, 0, "");
        else
            say("\r%*s\r", 40, "");
    }
    return rc;
}

/* A counter for a person watching a long step: "checking 800/1500",
 * rewritten in place, cleared when the step ends. */
static void progress(const char *what, size_t i, size_t n)
{
    static char last[80];
    if (machine || sink == NULL || !sink->progress || n < 40)
        return;
    if (i < n && i % 20 != 0)
        return;
    if (i >= n) {
        if (last[0]) {
            if (sink->line != NULL)
                sink->line(sink->user, PKG_LINE_PROGRESS, 0, "");
            else
                say("\r%*s\r", (int)strlen(last), "");
        }
        last[0] = '\0';
        return;
    }
    snprintf(last, sizeof last, "%s %lu/%lu", what, (unsigned long)i, (unsigned long)n);
    if (sink->line != NULL)
        sink->line(sink->user, PKG_LINE_PROGRESS, 0, last);
    else
        say("\r  %s", last);
}

static void say_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(1, fmt, ap);
    va_end(ap);
}

static void say_raw(const char *buf, size_t len)
{
    char *s;
    if (sink == NULL || (sink->text == NULL && sink->line == NULL))
        return;
    s = (char *)malloc(len + 1u);
    if (s == NULL)
        return;
    memcpy(s, buf, len);
    s[len] = '\0';
    if (sink->line != NULL)
        emit_line(PKG_LINE_TEXT, 0, s);
    else
        sink->text(sink->user, 0, s);
    free(s);
}

/* A result word, or its conditional under a dry run: "installed" becomes
 * "would-install", so a dry run can never be read as the real thing. */
static const char *res(const char *done, const char *would)
{
    return dryrun ? would : done;
}

/* A result field, in the structured form only: "key: value", the
 * manifest's syntax. */
/* A record is one line whatever went into it: a reason with line breaks, a
 * name or comment a person typed with a tab or a stray control character.
 * Every value leaves through here or rec_item, so none can break the
 * key: value form a reader relies on. */
static void one_line(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F)
            *s = ' ';
}

static void kv(const char *key, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    if (!machine || sink == NULL || sink->record == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    one_line(buf);
    sink->record(sink->user, key, buf);
}

/* The closing sentence: a summary record for a program, the sentence
 * itself for a person. */
static void summary_line(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kv("summary", "%s", buf);
    if (!machine)
        say_result("%s", buf);
}

/* A record with several fields: the joined value goes to `record`, as the
 * command line prints it, and the fields one by one to `item`, for a program
 * that should not have to split strings. Key/value pairs, NULL-terminated. */
static void rec_item(const char *kind, const char *joined, ...)
{
    const char *keys[12], *vals[12];
    char *clean[12], line[4096];
    int n = 0, i;
    va_list ap;
    if (!machine || sink == NULL)
        return;
    snprintf(line, sizeof line, "%s", joined);
    one_line(line);
    if (sink->record != NULL)
        sink->record(sink->user, kind, line);
    if (sink->item == NULL)
        return;
    va_start(ap, joined);
    while (n < 12) {
        const char *k = va_arg(ap, const char *);
        const char *v;
        if (k == NULL)
            break;
        v = va_arg(ap, const char *);
        keys[n] = k;
        clean[n] = (char *)malloc(strlen(v ? v : "") + 1);
        if (clean[n] != NULL) { strcpy(clean[n], v ? v : ""); one_line(clean[n]); }
        vals[n] = clean[n] ? clean[n] : "";
        n++;
    }
    va_end(ap);
    sink->item(sink->user, kind, n, keys, vals);
    for (i = 0; i < n; i++) free(clean[i]);
}

/* The caller's cancel callback, asked between steps. */
static int cancelled(const char *before);

/* What an agent, or a person, should do after a refusal. Every refusal says
 * it, as `next` in the structured form and as a last line in the text one,
 * so the answer to "what now?" never has to be guessed from the reason's
 * wording. The reasons themselves never hand over a ready-made command that
 * overrides a safeguard (ACCEPTKEY, DOWNGRADE, removing something): those
 * steps belong to whoever requested the operation, a person or the agent
 * that launched this one, and `ask-requester` says so. */
static const char *next_default(int cls)
{
    switch (cls) {
    case PKGRC_NOTFOUND:   return "check-name";
    case PKGRC_INTEGRITY:  return "stop";
    case PKGRC_SIGNATURE:  return "stop";
    case PKGRC_KEY:        return "ask-requester";
    case PKGRC_CONFLICT:   return "ask-requester";
    case PKGRC_DEPENDENCY: return "ask-requester";
    case PKGRC_POLICY:     return "ask-requester";
    case PKGRC_USAGE:      return "fix-command";
    default:               return "report";
    }
}

/* The command line's sentence for a `next` value, naming its commands. */
static const char *next_cli_words(const char *next)
{
    if (next == NULL)
        return "report this to whoever requested it";
    if (strcmp(next, "stop") == 0)
        return "stop here: the bytes or signatures are not what was published, and "
               "no keyword or other channel makes that safe";
    if (strcmp(next, "ask-requester") == 0)
        return "ask whoever requested this (the person, or the agent that launched you); "
               "it is their decision, not a step to take for them";
    if (strcmp(next, "fix-command") == 0)
        return "fix the command; pkg HELP lists the verbs and keywords";
    if (strcmp(next, "check-name") == 0)
        return "check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, "
               "pkg LIST ROOT <dir> what a root holds";
    if (strcmp(next, "use-upgrade") == 0)
        return "to move to that version, UPGRADE instead of INSTALL";
    if (strcmp(next, "use-install") == 0)
        return "it is not installed there; INSTALL it instead";
    return "report this to whoever requested it";
}
#define next_words next_cli_words

/* The same, in words for any front end: no command is named. */
const char *pkg_next_words(const char *next)
{
    if (next == NULL || strcmp(next, "report") == 0)
        return "Nothing more can be done from here.";
    if (strcmp(next, "stop") == 0)
        return "Do not go on: this is not what its publisher published, and no other copy "
               "or option makes it safe.";
    if (strcmp(next, "ask-requester") == 0)
        return "This is your decision. Read the reason; if you agree, confirm and it will "
               "be done that way.";
    if (strcmp(next, "fix-command") == 0)
        return "Something in the request was wrong; correct it and try again.";
    if (strcmp(next, "check-name") == 0)
        return "That name is not there. Choose from what is offered.";
    if (strcmp(next, "use-upgrade") == 0)
        return "Another version is installed; upgrade it to this one instead.";
    if (strcmp(next, "use-install") == 0)
        return "It is not installed yet; install it instead.";
    return "Nothing more can be done from here.";
}

static const char *pending_next;   /* set by refuse_n for the next refusal */
static const char *refused_next;
static int quiet;                  /* SHOW checks entries without reporting refusals */
static char quiet_reason[2048];

static int refuse_c(int cls, const char *fmt, ...)
{
    char buf[2048], *q;
    va_list ap;
    const char *next = pending_next ? pending_next : next_default(cls);
    pending_next = NULL;
    if (refused_class == 0) {
        refused_class = cls;
        refused_next = next;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tr("refused, %s (%d), next %s: %s", class_name(cls), cls, next, buf);
    if (quiet) {
        snprintf(quiet_reason, sizeof quiet_reason, "%s", buf);
        return 1;
    }
    if (machine) {
        char code[8];
        one_line(buf);
        (void)q;
        snprintf(code, sizeof code, "%d", refused_class);
        if (sink && sink->record) {
            sink->record(sink->user, "result", "refused");
            sink->record(sink->user, "class", class_name(refused_class));
            sink->record(sink->user, "code", code);
            sink->record(sink->user, "reason", buf);
            sink->record(sink->user, "next", refused_next);
        }
        return 1;
    }
    if (sink && sink->line) {
        emit_line(PKG_LINE_REFUSAL, 1, buf);
        emit_line(PKG_LINE_NEXT, 1, next_words(refused_next));
    } else if (sink && sink->text) {
        char *line;
        size_t n = strlen(verb_name) + strlen(buf) + strlen(next_words(refused_next)) + 32;
        line = (char *)malloc(n);
        if (line != NULL) {
            snprintf(line, n, "pkg %s: %s\n  next: %s\n", verb_name, buf, next_words(refused_next));
            sink->text(sink->user, 1, line);
            free(line);
        }
    }
    return 1;
}

/* A refusal whose next step differs from its class's usual one. */
#define refuse_n(cls, nx, ...) (pending_next = (nx), refuse_c((cls), __VA_ARGS__))
#define refuse(...) refuse_c(PKGRC_REFUSED, __VA_ARGS__)

/* A warning is not a failure and leaves the operation's class alone. */
static void warn(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("warning", "%s", buf);
    else if (sink != NULL && sink->line != NULL)
        emit_line(PKG_LINE_WARNING, 1, buf);
    else
        say_result("pkg %s: warning: %s", verb_name, buf);
}

/* What usually comes next after a success, or what is worth telling the
 * person: never a command that overrides a safeguard. */
static void hint(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("hint", "%s", buf);
    else
        say_kind(PKG_LINE_HINT, "  hint: %s\n", "%s", buf);
}

static void short12(const char *hex, char out[13])
{
    memcpy(out, hex, 12);
    out[12] = '\0';
}

static void tohex(const unsigned char *b, size_t n, char *hex)
{
    static const char dg[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        hex[2 * i] = dg[b[i] >> 4];
        hex[2 * i + 1] = dg[b[i] & 15];
    }
    hex[2 * n] = '\0';
}

static int fromhex(unsigned char *b, size_t n, const char *hex)
{
    size_t i;
    if (hex == NULL || strlen(hex) != 2 * n)
        return -1;
    for (i = 0; i < 2 * n; i++)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
            return -1;
    for (i = 0; i < n; i++) {
        int hi = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        int lo = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        b[i] = (unsigned char)(hi * 16 + lo);
    }
    return 0;
}

/* ---- the filesystem, traced ------------------------------------------ *
 *
 * Every file operation goes through these, so the trace shows each one. */

static int t_read(const char *path, unsigned char **buf, size_t *len)
{
    int rc = pkg_fs_read(path, buf, len);
    if (rc == 0) tr("read %s, %lu bytes", path, (unsigned long)*len);
    else tr("read %s: absent or unreadable", path);
    return rc;
}

static int t_write(const char *path, const void *buf, size_t len)
{
    int rc = pkg_fs_write_atomic(path, buf, len);
    tr("write %s, %lu bytes%s", path, (unsigned long)len, rc == 0 ? "" : ": FAILED");
    return rc;
}

static int t_write_private(const char *path, const void *buf, size_t len)
{
    int rc = pkg_fs_write_private(path, buf, len);
    tr("write %s, %lu bytes, owner only%s", path, (unsigned long)len, rc == 0 ? "" : ": FAILED");
    return rc;
}

static int t_rename(const char *from, const char *to)
{
    int rc = pkg_fs_rename(from, to);
    tr("move %s -> %s%s", from, to, rc == 0 ? "" : ": FAILED");
    return rc;
}

static int t_unlink(const char *path)
{
    int rc = pkg_fs_unlink(path);
    tr("delete %s%s", path, rc == 0 ? "" : ": FAILED");
    return rc;
}

static int t_rmtree(const char *path)
{
    int rc = pkg_fs_rmtree(path);
    tr("clear %s%s", path, rc == 0 ? "" : ": FAILED");
    return rc;
}

#define pkg_fs_read          t_read
#define pkg_fs_write_atomic  t_write
#define pkg_fs_write_private t_write_private
#define pkg_fs_rename        t_rename
#define pkg_fs_unlink        t_unlink
#define pkg_fs_rmtree        t_rmtree

/* ---- keys ------------------------------------------------------------- */

struct key {
    unsigned char pk[PKG_ED25519_PUBLIC];
    unsigned char sk[PKG_ED25519_SECRET];
    char          pkhex[2 * PKG_ED25519_PUBLIC + 1];
};

static int cmd_keygen(const struct pkg_options *a)
{
    unsigned char seed[PKG_ED25519_SEED];
    struct key k;
    char seedhex[2 * PKG_ED25519_SEED + 1], text[256];
    int n;

    if (a->file == NULL)
        return refuse_c(20, "name the key file with FILE <path>");
    if (pkg_fs_exists(a->file))
        return refuse_c(15, "\"%s\" already exists; a key is never overwritten", a->file);
    if (pkg_fs_random(seed, sizeof seed) != 0) {
        int typed = machine ? -2 : pkg_fs_random_typed(seed, sizeof seed);
        if (typed == -2)
            return refuse_c(17, "this system has no random source, and a key made without one could be "
                            "guessed: run KEYGEN in a Shell window, where pkg makes it from the moments "
                            "you press keys, or make the key on a Mac or a PC and bring the file here");
        if (typed != 0)
            return refuse_c(17, "the key was not made: typing stopped before there was enough of it");
    }
    pkg_ed25519_keypair(k.pk, k.sk, seed);
    tohex(seed, sizeof seed, seedhex);
    tohex(k.pk, sizeof k.pk, k.pkhex);
    n = snprintf(text, sizeof text, "Pkg-Secret-Key: 1\nSeed: %s\nPublic: %s\n",
                 seedhex, k.pkhex);
    if (pkg_fs_write_private(a->file, text, (size_t)n) != 0)
        return refuse_c(17, "cannot write \"%s\": %s", a->file, strerror(errno));
    memset(seed, 0, sizeof seed);
    memset(seedhex, 0, sizeof seedhex);
    memset(text, 0, sizeof text);
    kv("result", "created");
    kv("file", "%s", a->file);
    kv("public", "%s", k.pkhex);
    if (!machine) {
        say_result("key written to %s, readable by you alone", a->file);
        say_detail("public key %s", k.pkhex);
    }
    hint("every later version of what this key publishes must be signed with it: keep the "
         "file with the person's secrets, outside any channel or repository, and back it up. "
         "Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared");
    return 0;
}

static int load_key(const char *path, struct key *k)
{
    unsigned char *buf, seed[PKG_ED25519_SEED], pk[PKG_ED25519_PUBLIC];
    size_t len;
    char seedhex[65], pubhex[65];

    if (path == NULL)
        return refuse_c(14, "no signing key: give SIGN <keyfile>, or set PKG_SIGNKEY. "
                        "A publisher who has published before must sign with the same key, or "
                        "every machine that installed their packages refuses the new ones: ask "
                        "whoever requested this where theirs is before creating one with KEYGEN. "
                        "DRYRUN needs no key");
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(17, "cannot read the key \"%s\": %s", path, strerror(errno));
    if (len > 512u
        || sscanf((const char *)buf, "Pkg-Secret-Key: 1\nSeed: %64s\nPublic: %64s",
                  seedhex, pubhex) != 2
        || fromhex(seed, sizeof seed, seedhex) != 0
        || fromhex(pk, sizeof pk, pubhex) != 0) {
        free(buf);
        return refuse_c(12, "\"%s\" is not a pkg key file", path);
    }
    memset(buf, 0, len);
    free(buf);
    pkg_ed25519_keypair(k->pk, k->sk, seed);
    memset(seed, 0, sizeof seed);
    if (memcmp(k->pk, pk, sizeof pk) != 0)
        return refuse_c(12, "\"%s\" is damaged: its public key does not match its seed", path);
    tohex(k->pk, sizeof k->pk, k->pkhex);
    return 0;
}

static int write_sig(const char *path, const struct key *k,
                     const unsigned char *msg, size_t len)
{
    unsigned char sig[PKG_ED25519_SIG];
    char sighex[2 * PKG_ED25519_SIG + 1], text[256];
    int n;
    pkg_ed25519_sign(sig, msg, len, k->sk);
    tohex(sig, sizeof sig, sighex);
    n = snprintf(text, sizeof text, "Signer: %s\nSignature: %s\n", k->pkhex, sighex);
    return pkg_fs_write_atomic(path, text, (size_t)n);
}

/* Verify a detached signature file over msg. On success the signer's public
 * key is copied to signer (65 bytes). */
static int check_sig(const char *sigpath, const unsigned char *msg, size_t len,
                     char signer[65], const char *what)
{
    unsigned char *buf, pk[PKG_ED25519_PUBLIC], sig[PKG_ED25519_SIG];
    size_t blen;
    char pkhex[65], sighex[129];

    if (pkg_fs_read(sigpath, &buf, &blen) != 0)
        return refuse_c(13, "%s is not signed. Every package is signed; nothing was installed", what);
    if (blen > 512u
        || sscanf((const char *)buf, "Signer: %64s\nSignature: %128s", pkhex, sighex) != 2
        || fromhex(pk, sizeof pk, pkhex) != 0 || fromhex(sig, sizeof sig, sighex) != 0) {
        free(buf);
        return refuse_c(13, "the signature of %s is malformed; nothing was installed", what);
    }
    free(buf);
    if (pkg_ed25519_verify(sig, msg, len, pk) != 0)
        return refuse_c(13, "the signature of %s does not verify against its manifest, so the "
                      "manifest or the signature was altered after signing; nothing was installed",
                      what);
    memcpy(signer, pkhex, 65);
    return 0;
}

/* ---- OpenSSH's formats, for machines that have ssh-keygen and no Pkg ---- *
 * The same Ed25519 key, written as OpenSSH writes it: the public key as an
 * "ssh-ed25519" line, and a signature in the SSHSIG format (PROTOCOL.sshsig
 * in OpenSSH's sources) that `ssh-keygen -Y verify` checks. */

struct sshbuf { unsigned char b[512]; size_t n; };

static void ssh_put(struct sshbuf *s, const void *p, size_t len)   /* a string: length, bytes */
{
    s->b[s->n++] = (unsigned char)(len >> 24);
    s->b[s->n++] = (unsigned char)(len >> 16);
    s->b[s->n++] = (unsigned char)(len >> 8);
    s->b[s->n++] = (unsigned char)len;
    memcpy(s->b + s->n, p, len);
    s->n += len;
}

static void ssh_put_str(struct sshbuf *s, const char *str) { ssh_put(s, str, strlen(str)); }

static void ssh_pubkey_blob(struct sshbuf *s, const unsigned char pk[PKG_ED25519_PUBLIC])
{
    s->n = 0;
    ssh_put_str(s, "ssh-ed25519");
    ssh_put(s, pk, PKG_ED25519_PUBLIC);
}

/* Base64 of len bytes into out, a line break every wrap characters when wrap
 * is not 0; out needs 4 * len / 3 + len / wrap + 8 bytes. */
static void base64(const unsigned char *in, size_t len, char *out, size_t wrap)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, col = 0;
    for (i = 0; i < len; i += 3) {
        unsigned long v = (unsigned long)in[i] << 16
                        | (i + 1 < len ? (unsigned long)in[i + 1] << 8 : 0)
                        | (i + 2 < len ? in[i + 2] : 0);
        char q[4];
        int k;
        q[0] = t[v >> 18 & 63];
        q[1] = t[v >> 12 & 63];
        q[2] = i + 1 < len ? t[v >> 6 & 63] : '=';
        q[3] = i + 2 < len ? t[v & 63] : '=';
        for (k = 0; k < 4; k++) {
            if (wrap && col == wrap) { *out++ = '\n'; col = 0; }
            *out++ = q[k];
            col++;
        }
    }
    *out = '\0';
}

/* "ssh-ed25519 AAAA... <comment>", the comment the key file's name without
 * its directory and extension: jkn for ~/.config/aros-pkg/jkn.key. */
static void ssh_pubkey_line(const struct key *k, const char *path, char *out, size_t outsz)
{
    struct sshbuf s;
    char b64[128], comment[64];
    const char *base = path, *p;
    size_t cl;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            base = p + 1;
    p = strrchr(base, '.');
    cl = p != NULL && p != base ? (size_t)(p - base) : strlen(base);
    if (cl >= sizeof comment) cl = sizeof comment - 1;
    memcpy(comment, base, cl);
    comment[cl] = '\0';
    for (p = comment; *p; p++)
        if (*p == ' ' || (unsigned char)*p < 0x21)
            comment[p - comment] = '-';
    ssh_pubkey_blob(&s, k->pk);
    base64(s.b, s.n, b64, 0);
    if (snprintf(out, outsz, "ssh-ed25519 %s %s", b64, comment) >= (int)outsz)
        out[outsz - 1] = '\0';   /* a long comment is cut; the key is whole */
}

/* An SSHSIG signature over msg for namespace ns, armored, as ssh-keygen -Y
 * sign writes it: Ed25519 over "SSHSIG", the namespace, an empty reserved
 * string, "sha512" and the SHA-512 of the message. */
static int ssh_sign(const char *path, const struct key *k, const char *ns,
                    const unsigned char *msg, size_t len)
{
    struct sshbuf tosign, out, pub, sigb;
    unsigned char h[PKG_SHA512_LEN], sig[PKG_ED25519_SIG];
    char text[1024], b64[700];
    int n;

    pkg_sha512(h, msg, len);
    tosign.n = 0;
    memcpy(tosign.b, "SSHSIG", 6);
    tosign.n = 6;
    ssh_put_str(&tosign, ns);
    ssh_put_str(&tosign, "");
    ssh_put_str(&tosign, "sha512");
    ssh_put(&tosign, h, sizeof h);
    pkg_ed25519_sign(sig, tosign.b, tosign.n, k->sk);

    ssh_pubkey_blob(&pub, k->pk);
    sigb.n = 0;
    ssh_put_str(&sigb, "ssh-ed25519");
    ssh_put(&sigb, sig, sizeof sig);
    memcpy(out.b, "SSHSIG", 6);
    out.n = 6;
    out.b[out.n++] = 0; out.b[out.n++] = 0; out.b[out.n++] = 0; out.b[out.n++] = 1;
    ssh_put(&out, pub.b, pub.n);
    ssh_put_str(&out, ns);
    ssh_put_str(&out, "");
    ssh_put_str(&out, "sha512");
    ssh_put(&out, sigb.b, sigb.n);
    base64(out.b, out.n, b64, 70);
    n = snprintf(text, sizeof text,
                 "-----BEGIN SSH SIGNATURE-----\n%s\n-----END SSH SIGNATURE-----\n", b64);
    return pkg_fs_write_atomic(path, text, (size_t)n);
}

/* Which public key a key file holds, without showing its secret. */
static int cmd_keyinfo(const struct pkg_options *a)
{
    struct key k;
    char line[200];
    const char *path = a->file ? a->file : a->target ? a->target : a->sign;
    if (path == NULL)
        return refuse_c(20, "name the key file with FILE <keyfile>");
    if (load_key(path, &k) != 0)
        return 1;
    kv("result", "shown");
    kv("file", "%s", path);
    kv("public", "%s", k.pkhex);
    if (a->ssh) {
        ssh_pubkey_line(&k, path, line, sizeof line);
        kv("ssh", "%s", line);
        if (!machine)
            printf("%s\n", line);     /* alone on its line, for an allowed_signers file */
    } else if (!machine)
        say_result("%s holds the public key %s", path, k.pkhex);
    memset(&k, 0, sizeof k);
    return 0;
}

static int cmd_sign(const struct pkg_options *a)
{
    struct key k;
    unsigned char *buf;
    size_t len;
    if (a->target == NULL || a->key == NULL || a->out == NULL)
        return refuse_c(20, "usage: SIGN <file> KEY <keyfile> OUT <sigfile> [SSH NAMESPACE <ns>]");
    if (a->ssh && (a->nspace == NULL || a->nspace[0] == '\0' || strlen(a->nspace) > 64))
        return refuse_c(20, "SSH signs for a namespace, the word the verifier names with -n: "
                        "give NAMESPACE <ns>, at most 64 characters");
    if (!a->ssh && a->nspace != NULL)
        return refuse_c(20, "NAMESPACE belongs to an SSH signature: add SSH, or leave NAMESPACE out");
    if (load_key(a->key, &k) != 0)
        return 1;
    if (pkg_fs_read(a->target, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->target);
    if ((a->ssh ? ssh_sign(a->out, &k, a->nspace, buf, len)
                : write_sig(a->out, &k, buf, len)) != 0) {
        free(buf);
        return refuse_c(17, "cannot write \"%s\"", a->out);
    }
    free(buf);
    kv("result", "signed");
    kv("file", "%s", a->target);
    kv("signer", "%s", k.pkhex);
    if (!machine)
        say_result("signed %s with %.16s", a->target, k.pkhex);
    return 0;
}

static int ascii_casecmp(const char *x, const char *y);

/* CHECKSIG <file> FILE <sigfile> [KEY <public key>]: the counterpart of SIGN
 * for a machine without ssh-keygen, and what a portal runs to check a signed
 * push request. */
static int cmd_checksig(const struct pkg_options *a)
{
    unsigned char *buf;
    size_t len;
    char signer[65];
    if (a->target == NULL || a->file == NULL)
        return refuse_c(20, "usage: CHECKSIG <file> FILE <sigfile> [KEY <the signer's public key>]");
    if (pkg_fs_read(a->target, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->target);
    {
        unsigned char *sb, pk[PKG_ED25519_PUBLIC], sig[PKG_ED25519_SIG];
        size_t sl;
        char sighex[129];
        if (pkg_fs_read(a->file, &sb, &sl) != 0) {
            free(buf);
            return refuse_c(17, "cannot read the signature \"%s\"", a->file);
        }
        if (sl > 512u || sscanf((const char *)sb, "Signer: %64s\nSignature: %128s", signer, sighex) != 2
            || fromhex(pk, sizeof pk, signer) != 0 || fromhex(sig, sizeof sig, sighex) != 0) {
            free(sb); free(buf);
            return refuse_c(13, "\"%s\" is not a signature SIGN wrote: a Signer: line and a Signature: line", a->file);
        }
        free(sb);
        if (pkg_ed25519_verify(sig, buf, len, pk) != 0) {
            free(buf);
            return refuse_c(13, "the signature does not check: %s or %s was changed after signing, "
                            "or the signature is for another file", a->target, a->file);
        }
    }
    free(buf);
    if (a->key != NULL && ascii_casecmp(a->key, signer) != 0)
        return refuse_c(14, "%s is signed, by %s and not by the key given, %s", a->target, signer, a->key);
    kv("result", "good");
    kv("file", "%s", a->target);
    kv("signer", "%s", signer);
    if (!machine)
        say_result("%s is signed by %s%s", a->target, signer, a->key ? ", the key given" : "");
    return 0;
}

/* ---- building a package from a drawer --------------------------------- */

struct loaded {
    char          *rel;
    unsigned char *data;
    size_t         len;
    unsigned long long prot;    /* the AROS protection word it is published with */
    char          *comment;     /* its comment, UTF-8; NULL for none */
    int            host_exec;   /* from an archive's mode: 1, 0; -2 ask the file system */
    /* From an archive's metadata index, with no data loaded: */
    int            pre;
    char           pre_digest[PKG_SHA256_HEXLEN + 1];
    const char    *pre_arch;    /* NULL: no executable header */
    char           pre_cookie[140];  /* "name version", or "" */
};

struct drawer {
    const char    *root;
    const char    *files;       /* FILES: the paths of the tree that make the package */
    struct loaded *v;
    size_t         n, cap;
    char         **left_out;        /* host files not packaged, dirs with '/' */
    size_t         nleft;
    char           err[512];
};

static void leave_out(const char *rel, int is_dir, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (!is_dir && (strcmp(base, ".ameta") == 0 || strncmp(base, ".ameta.", 7) == 0))
        return;                     /* read for the attributes, never packaged */
    char **g = (char **)realloc(d->left_out, (d->nleft + 1) * sizeof *g);
    size_t n = strlen(rel) + 2;
    char *s = (char *)malloc(n);
    if (g == NULL || s == NULL) { free(s); if (g) d->left_out = g; return; }
    d->left_out = g;
    snprintf(s, n, "%s%s", rel, is_dir ? "/" : "");
    d->left_out[d->nleft++] = s;
}

static int files_match(const char *files, const char *rel);

static int load_one(const char *rel, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    char *full;
    const char *why = pkg_check_path(rel);

    if (d->files != NULL && !files_match(d->files, rel))
        return 0;                   /* FILES: only the paths it names, of a larger tree */
    if (why != NULL) {
        snprintf(d->err, sizeof d->err, "\"%s\": %s", rel, why);
        return -1;
    }
    if (d->n == d->cap) {
        size_t ncap = d->cap ? d->cap * 2u : 32u;
        struct loaded *w = (struct loaded *)realloc(d->v, ncap * sizeof *w);
        if (w == NULL) return -1;
        d->v = w;
        d->cap = ncap;
    }
    memset(&d->v[d->n], 0, sizeof d->v[d->n]);     /* every field, the new ones too */
    full = pkg_join(d->root, rel);
    d->v[d->n].rel = full ? pkg_join("", rel) : NULL;
    if (full == NULL || d->v[d->n].rel == NULL
        || pkg_fs_read(full, &d->v[d->n].data, &d->v[d->n].len) != 0) {
        snprintf(d->err, sizeof d->err, "cannot read \"%s\"", rel);
        free(full);
        return -1;
    }
    free(full);
    d->v[d->n].prot = 0;
    d->v[d->n].comment = NULL;
    d->v[d->n].host_exec = -2;
    d->n++;
    return 0;
}

/* A drawer that is a part of an archive: "archive!/prefix", optionally only
 * the paths FILES names under it. Paths are relative to the prefix, as they
 * install; owner Execute comes from the archive's own mode bits.
 *
 * Publishing needs each file's digest, size, mode, CPU and $VER, never its
 * bytes: the first read of an archive writes them to <archive>.pkgidx, keyed
 * to the archive's size and time, and every later publish from it reads that
 * index instead of the archive. Installers never use the index: they read
 * the archive and check each file against the signed manifest. */
static const char *file_arch(const unsigned char *p, size_t len);
static int cookie(const struct loaded *f, char *name, size_t nl, char *ver, size_t vl);

static int files_match(const char *files, const char *rel)
{
    const char *p = files;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n > 0 && p[n - 1] == '/') n--;
        if (n > 0 && strncmp(rel, p, n) == 0 && (rel[n] == '\0' || rel[n] == '/'))
            return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static const char *arch_intern(const char *a)
{
    static const char *const known[] = { "i386", "m68k", "ppc", "ppc64", "arm", "x86_64", "aarch64" };
    size_t i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strcmp(a, known[i]) == 0) return known[i];
    return NULL;
}

#define IDX_HEAD 65536u      /* enough of a file's start to read its CPU */
#define IDX_WIN  8192u       /* the window $VER cookies are looked for in */
#define IDX_KEEP 256u        /* what a cookie may need after "$VER: " */

/* Each file is read once, in pieces, and never held whole: a digest built
 * as it goes, its first bytes for the CPU, and a sliding window for its
 * $VER. An archive claiming a file of any size costs the same memory. */
struct idx_build {
    char              *out;         /* the index text being written */
    size_t             len, cap;
    struct pkg_sha256  sha;
    unsigned char      head[IDX_HEAD];
    size_t             hlen;
    unsigned long long total;
    unsigned char      win[IDX_WIN];
    size_t             wlen;
    int                found, started;
    char               cn[65], cv[64];
    int                oom;
};

/* Look for a cookie starting in win[0 .. limit). */
static void ib_scan(struct idx_build *ib, size_t limit)
{
    size_t j;
    for (j = 0; !ib->found && j < limit && j + 6 <= ib->wlen; j++) {
        struct loaded f;
        if (ib->win[j] != '$' || memcmp(ib->win + j, "$VER: ", 6) != 0)
            continue;
        memset(&f, 0, sizeof f);
        f.data = ib->win + j;
        f.len = ib->wlen - j;
        if (cookie(&f, ib->cn, sizeof ib->cn, ib->cv, sizeof ib->cv))
            ib->found = 1;
    }
}

static void ib_put(struct idx_build *ib, const char *s)
{
    size_t n = strlen(s);
    if (ib->oom) return;
    if (ib->len + n + 1 > ib->cap) {
        size_t nc = ib->cap ? ib->cap * 2 : 65536;
        char *g;
        while (nc < ib->len + n + 1) nc *= 2;
        g = (char *)realloc(ib->out, nc);
        if (g == NULL) { ib->oom = 1; return; }
        ib->out = g;
        ib->cap = nc;
    }
    memcpy(ib->out + ib->len, s, n + 1);
    ib->len += n;
}

static int ib_want(const struct pkg_archive_entry *e, void *ctx)
{
    (void)ctx;
    return e->is_dir ? 0 : 1;
}

static int ib_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct idx_build *ib = (struct idx_build *)ctx;
    if (!ib->started) {
        pkg_sha256_init(&ib->sha);
        ib->hlen = ib->wlen = 0;
        ib->total = 0;
        ib->found = 0;
        ib->started = 1;
    }
    if (len > 0) {
        size_t at = 0;
        pkg_sha256_update(&ib->sha, buf, len);
        ib->total += len;
        if (ib->hlen < IDX_HEAD) {
            size_t k = IDX_HEAD - ib->hlen < len ? IDX_HEAD - ib->hlen : len;
            memcpy(ib->head + ib->hlen, buf, k);
            ib->hlen += k;
        }
        while (!ib->found && at < len) {
            size_t k = IDX_WIN - ib->wlen < len - at ? IDX_WIN - ib->wlen : len - at;
            memcpy(ib->win + ib->wlen, buf + at, k);
            ib->wlen += k;
            at += k;
            if (ib->wlen == IDX_WIN) {
                /* every start with IDX_KEEP bytes after it, then slide */
                ib_scan(ib, IDX_WIN - IDX_KEEP);
                memmove(ib->win, ib->win + IDX_WIN - IDX_KEEP, IDX_KEEP);
                ib->wlen = IDX_KEEP;
            }
        }
        return 0;
    }
    {
        /* The end of the file: what publishing needs of it, nothing of its bytes. */
        unsigned char dg[PKG_SHA256_LEN];
        char hex[PKG_SHA256_HEXLEN + 1], line[5200];
        const char *arch;
        size_t k;
        if (!ib->found)
            ib_scan(ib, ib->wlen);
        pkg_sha256_final(&ib->sha, dg);
        for (k = 0; k < PKG_SHA256_LEN; k++)
            snprintf(hex + 2 * k, 3, "%02x", dg[k]);
        arch = file_arch(ib->head, ib->hlen);
        snprintf(line, sizeof line, "%s %llu %o %s %s %s %s\n", hex, ib->total,
                 e->mode, arch ? arch : "-", ib->found ? ib->cn : "-", ib->found ? ib->cv : "-", e->path);
        ib_put(ib, line);
        ib->started = 0;
    }
    return ib->oom ? -1 : 0;
}

/* The archive's index, built when missing or stale; the caller frees it. */
static char *archive_index(const char *archive, char *err, size_t errlen)
{
    struct pkg_fs_id id;
    char *ip = NULL, head[128], *text = NULL;
    unsigned char *buf = NULL;
    size_t len = 0;
    struct idx_build *ib;
    char *text_out;

    if (pkg_fs_identity(archive, &id) != 0 || !id.exists) {
        snprintf(err, errlen, "cannot read %s", archive);
        return NULL;
    }
    /* The version changes whenever what it records is computed differently. */
    snprintf(head, sizeof head, "pkgidx 4 %llu %lld\n", id.size, id.mtime_s);
    ip = (char *)malloc(strlen(archive) + 8);
    if (ip == NULL) { snprintf(err, errlen, "out of memory"); return NULL; }
    snprintf(ip, strlen(archive) + 8, "%s.pkgidx", archive);
    if (pkg_fs_read(ip, &buf, &len) == 0 && len > strlen(head)
        && memcmp(buf, head, strlen(head)) == 0) {
        text = (char *)realloc(buf, len + 1);
        if (text != NULL) text[len] = '\0';
        tr("archive index %s is current", ip);
        free(ip);
        return text;
    }
    free(buf);
    tr("indexing %s: every file read once, for its digest, mode, CPU and $VER", archive);
    ib = (struct idx_build *)calloc(1, sizeof *ib);
    if (ib == NULL) { snprintf(err, errlen, "out of memory"); free(ip); return NULL; }
    ib_put(ib, head);
    if (pkg_archive_walk(archive, ib_want, ib_data, ib, err, errlen) != 0 || ib->oom) {
        if (ib->oom && !err[0]) snprintf(err, errlen, "out of memory");
        free(ib->out); free(ib); free(ip);
        return NULL;
    }
    if (pkg_fs_write_atomic(ip, ib->out, ib->len) != 0)
        tr("the archive index could not be kept at %s; it is used for this run only", ip);
    free(ip);
    text_out = ib->out;
    free(ib);
    return text_out;
}

static int load_archive(struct drawer *d, const char *archive, const char *prefix, const char *files)
{
    char err[300], *idx, *line, *next;
    size_t pl = strlen(prefix);
    err[0] = '\0';
    idx = archive_index(archive, err, sizeof err);
    if (idx == NULL) {
        snprintf(d->err, sizeof d->err, "%.200s: %.280s", archive, err[0] ? err : "cannot read it");
        return -1;
    }
    next = strchr(idx, '\n');                      /* past the header */
    for (line = next ? next + 1 : NULL; line && *line; line = next) {
        char hex[PKG_SHA256_HEXLEN + 1], arch[16], cn[65], cv[64];
        unsigned long size;
        unsigned mode;
        int off = 0;
        const char *path, *rel, *c;
        struct loaded *f;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (sscanf(line, "%64s %lu %o %15s %64s %63s %n", hex, &size, &mode, arch, cn, cv, &off) != 6
            || off == 0)
            continue;
        path = line + off;
        rel = path;
        if (pl) {
            if (strncmp(path, prefix, pl) != 0 || path[pl] != '/') continue;
            rel = path + pl + 1;
        }
        if (files && !files_match(files, rel)) continue;
        for (c = rel; c; c = strchr(c, '/') ? strchr(c, '/') + 1 : NULL)
            if (*c == '.') break;
        if (c) { leave_out(rel, 0, d); continue; }     /* hidden, as a drawer's */
        if (pkg_check_path(rel) != NULL) {
            snprintf(d->err, sizeof d->err, "\"%.200s\": %s", rel, pkg_check_path(rel));
            free(idx);
            return -1;
        }
        if (d->n == d->cap) {
            size_t ncap = d->cap ? d->cap * 2u : 32u;
            struct loaded *w = (struct loaded *)realloc(d->v, ncap * sizeof *w);
            if (w == NULL) { free(idx); return -1; }
            d->v = w;
            d->cap = ncap;
        }
        f = &d->v[d->n];
        memset(f, 0, sizeof *f);
        f->rel = pkg_join("", rel);
        if (f->rel == NULL) { free(idx); return -1; }
        f->len = size;
        f->host_exec = (mode & 0100) != 0;
        f->pre = 1;
        snprintf(f->pre_digest, sizeof f->pre_digest, "%s", hex);
        f->pre_arch = strcmp(arch, "-") ? arch_intern(arch) : NULL;
        if (strcmp(cn, "-")) snprintf(f->pre_cookie, sizeof f->pre_cookie, "%s %s", cn, cv);
        d->n++;
    }
    free(idx);
    return 0;
}

static int by_rel(const void *a, const void *b)
{
    return strcmp(((const struct loaded *)a)->rel, ((const struct loaded *)b)->rel);
}

static void drawer_free(struct drawer *d)
{
    size_t i;
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); free(d->v[i].comment); }
    free(d->v);
    for (i = 0; i < d->nleft; i++) free(d->left_out[i]);
    free(d->left_out);
}

/* The first "$VER: name version" cookie of one file, lower-cased. */
static int cookie(const struct loaded *f, char *name, size_t nl, char *ver, size_t vl)
{
    const unsigned char *p = f->data;
    size_t j;
    if (f->pre) {
        const char *sp = strchr(f->pre_cookie, ' ');
        if (sp == NULL) return 0;
        snprintf(name, nl, "%.*s", (int)(sp - f->pre_cookie), f->pre_cookie);
        snprintf(ver, vl, "%s", sp + 1);
        return 1;
    }
    for (j = 0; j + 6 < f->len; j++) {
        size_t k = j + 6, a = 0, b = 0;
        if (memcmp(p + j, "$VER: ", 6) != 0)
            continue;
        while (k < f->len && p[k] > ' ' && p[k] < 0x7F && a + 1 < nl)
            name[a++] = (char)tolower(p[k++]);
        name[a] = '\0';
        while (k < f->len && p[k] == ' ') k++;
        /* digits and dots, then one +build if the program carries one:
         * "$VER: pkg 1.7.0+20260920" is published as that whole version, not
         * as 1.7.0, so two builds of one day and the next are two versions. */
        while (k < f->len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.' || p[k] == '+') && b + 1 < vl)
            ver[b++] = (char)p[k++];
        while (b > 0 && (ver[b - 1] == '.' || ver[b - 1] == '+')) b--;
        ver[b] = '\0';
        if (a > 0 && b > 0 && pkg_check_name(name) == NULL && pkg_check_version(ver) == NULL)
            return 1;
    }
    return 0;
}

/* Name and version from the drawer's $VER cookies. With `want` (NAME given),
 * the cookie of that name gives the version. Without it, the cookies must all
 * agree: two programs in one drawer, each with its own cookie, would otherwise
 * make the package whichever sorts first, silently. 1 found, 0 none, -1 the
 * cookies disagree, listed in `seen`. */
static int find_ver(const struct drawer *d, const char *want, char *name, size_t nl,
                    char *ver, size_t vl, const char **from, char *seen, size_t sl)
{
    size_t i, at = 0, count = 0;
    int found = 0, clash = 0;
    char n[65], v[64], only_v[64];
    const char *only_from = NULL;

    seen[0] = '\0';
    for (i = 0; i < d->n; i++) {
        if (!cookie(&d->v[i], n, sizeof n, v, sizeof v))
            continue;
        if (at + 100 < sl)
            at += (size_t)snprintf(seen + at, sl - at, "%s%s (%s %s)", at ? ", " : "",
                                   d->v[i].rel, n, v);
        if (count++ == 0) {
            snprintf(only_v, sizeof only_v, "%s", v);
            only_from = d->v[i].rel;
        }
        if (want != NULL) {
            if (!found && strcmp(n, want) == 0) {
                snprintf(name, nl, "%s", n);
                snprintf(ver, vl, "%s", v);
                *from = d->v[i].rel;
                found = 1;
            }
        } else if (!found) {
            snprintf(name, nl, "%s", n);
            snprintf(ver, vl, "%s", v);
            *from = d->v[i].rel;
            found = 1;
        } else if (strcmp(n, name) != 0 || pkg_version_cmp(v, ver) != 0) {
            clash = 1;
        }
    }
    if (want != NULL && !found && count == 1) {
        /* One program in the drawer, packaged under another name than its
         * cookie gives ("afs.handler" as afs-handler): its version stands. */
        snprintf(name, nl, "%s", want);
        snprintf(ver, vl, "%s", only_v);
        *from = only_from;
        return 1;
    }
    if (want != NULL && !found && count > 1)
        return -1;
    return clash ? -1 : found;
}

/* The CPU a file is built for, from its own header: ELF's e_machine, or the
 * hunk format every 68k Amiga executable has. NULL for data and scripts. */
static const char *file_arch(const unsigned char *p, size_t len)
{
    if (len >= 20 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') {
        unsigned m = p[5] == 2 ? (unsigned)p[18] << 8 | p[19] : (unsigned)p[19] << 8 | p[18];
        switch (m) {
        case 3:   return "i386";
        case 4:   return "m68k";
        case 20:  return "ppc";
        case 21:  return "ppc64";
        case 40:  return "arm";
        case 62:  return "x86_64";
        case 183: return "aarch64";
        default:  return NULL;
        }
    }
    if (len >= 24 && pkg_be32_get(p) == 0x000003F3ul) {
        /* HUNK_HEADER: resident library names (a zero-terminated list of
         * counted strings), the table size, first and last hunk, one size
         * per hunk, then the first hunk's type. A program starts with
         * HUNK_CODE; a hunk file of data only, such as deficons.prefs, is
         * not tied to a CPU. */
        size_t at = 4, n;
        unsigned long first, last, type;
        while (at + 4 <= len && (n = pkg_be32_get(p + at)) != 0) {
            at += 4 + 4 * (size_t)n;
            if (n > len) return NULL;
        }
        if (at + 16 > len) return NULL;
        first = pkg_be32_get(p + at + 8);
        last = pkg_be32_get(p + at + 12);
        if (last < first || last - first > 4096) return NULL;
        at += 16 + 4 * (size_t)(last - first + 1);
        if (at + 4 > len) return NULL;
        type = pkg_be32_get(p + at) & 0x3FFFFFFFul;
        if (type != 0x3E9ul)                        /* HUNK_CODE */
            return NULL;
        /* A classic font starts its code with moveq #n,d0 and rts: a stub
         * that returns at once, before the data diskfont.library reads. */
        if (at + 12 <= len && p[at + 8] == 0x70 && p[at + 10] == 0x4E && p[at + 11] == 0x75)
            return NULL;
        return "m68k";
    }
    return NULL;
}

/* The CPU a file of the drawer is built for, NULL for none. A hunk file in
 * a Keymaps drawer is a keymap, data keymap.library loads on any CPU. */
static const char *entry_arch(const struct loaded *f)
{
    const char *a = f->pre ? f->pre_arch : file_arch(f->data, f->len);
    if (a != NULL && strcmp(a, "m68k") == 0
        && (strncmp(f->rel, "Keymaps/", 8) == 0 || strstr(f->rel, "/Keymaps/") != NULL))
        return NULL;
    return a;
}

/* The drawer's architecture: the one its executables share, "generic" when
 * it has none. 1 found, 0 none, -1 more than one: `seen` then says how many
 * files each CPU has, with one of them. */
static int drawer_arch(const struct drawer *d, const char **arch, const char **from,
                       char *seen, size_t sl)
{
    const char *cpu[8], *ex[8];
    size_t cnt[8], nc = 0, i, k, at = 0;
    *arch = NULL;
    seen[0] = '\0';
    for (i = 0; i < d->n; i++) {
        const char *a = entry_arch(&d->v[i]);
        if (a == NULL)
            continue;
        if (*arch == NULL) {
            *arch = a;
            *from = d->v[i].rel;
        }
        for (k = 0; k < nc && strcmp(cpu[k], a) != 0; k++) ;
        if (k == nc && nc < 8) { cpu[nc] = a; ex[nc] = d->v[i].rel; cnt[nc++] = 0; }
        if (k < nc) cnt[k]++;
    }
    for (k = 0; nc > 1 && k < nc && at + 1 < sl; k++)
        at += (size_t)snprintf(seen + at, sl - at, "%s%s in %lu file%s, such as %.200s", k ? "; " : "",
                               cpu[k], (unsigned long)cnt[k], cnt[k] == 1 ? "" : "s", ex[k]);
    return nc > 1 ? -1 : *arch != NULL;
}

/* DEPENDS "a >= 1.0, b": each item a name, alone or with ">=" and the lowest
 * version that will do, spaces optional around ">=". */
static int add_depends(struct pkg_manifest *m, const char *list)
{
    const char *p = list, *why;
    if (list != NULL && strcmp(list, "none") == 0)
        return 0;                   /* DEPENDS none: a version that needs nothing */
    while (p != NULL && *p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[160], norm[170], dn[65], dv[64], *ge;
        size_t a = 0, b;
        if (len >= sizeof item)
            return refuse_c(20, "a DEPENDS item is too long");
        memcpy(item, p, len);
        item[len] = '\0';
        p = end ? end + 1 : NULL;
        while (item[a] == ' ') a++;
        b = strlen(item);
        while (b > a && item[b - 1] == ' ') item[--b] = '\0';
        if (b == a)
            continue;
        ge = strstr(item + a, ">=");
        if (ge != NULL) {
            char *l = ge, *r = ge + 2;
            while (l > item + a && l[-1] == ' ') l--;
            while (*r == ' ') r++;
            *l = '\0';
            snprintf(norm, sizeof norm, "%s >= %s", item + a, r);
        } else {
            snprintf(norm, sizeof norm, "%s", item + a);
        }
        if ((why = pkg_parse_dep(norm, dn, sizeof dn, dv, sizeof dv)) != NULL)
            return refuse_c(20, "DEPENDS \"%s\": %s", norm, why);
        if (pkg_manifest_add_dep(m, dn, dv) != 0)
            return refuse("out of memory");
    }
    pkg_manifest_sort(m);
    if ((why = pkg_check_deps(m)) != NULL)
        return refuse_c(20, "DEPENDS: %s", why);
    return 0;
}

/* Replace the drawer's files by one: the drawer as an FFS volume named after
 * the package, <name>.hdf. */
static int to_image(struct drawer *d, const char *name)
{
    struct pkg_image_entry *e = calloc(d->n ? d->n : 1, sizeof *e);
    unsigned char *img;
    size_t len, i;
    char err[300], rel[80];

    if (e == NULL) return refuse("out of memory");
    char (*latin)[PKG_COMMENT_MAX + 1] = calloc(d->n ? d->n : 1, sizeof *latin);
    if (latin == NULL) { free(e); return refuse("out of memory"); }
    for (i = 0; i < d->n; i++) {
        e[i].path = d->v[i].rel;
        e[i].data = d->v[i].data;
        e[i].len = d->v[i].len;
        e[i].protect = (unsigned long)(d->v[i].prot & 0xFFFFFFFFull);
        if (d->v[i].comment != NULL)
            pkg_comment_latin1(d->v[i].comment, latin[i], sizeof latin[i]);
        e[i].comment = latin[i];
    }
    if (pkg_image_build(e, d->n, name, &img, &len, err, sizeof err) != 0) {
        free(e);
        free(latin);
        return refuse_c(20, "the drawer cannot become an image: %s", err);
    }
    free(e);
    free(latin);
    snprintf(rel, sizeof rel, "%s.hdf", name);
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); free(d->v[i].comment); }
    d->v[0].rel = malloc(strlen(rel) + 1);
    if (d->v[0].rel == NULL) { free(img); d->n = 0; return refuse("out of memory"); }
    strcpy(d->v[0].rel, rel);
    d->v[0].data = img;
    d->v[0].len = len;
    d->v[0].prot = 0;
    d->v[0].comment = NULL;
    d->n = 1;
    return 0;
}

/* What PUBLISH lets a new version inherit: the channel it publishes into.
 * NULL for MANIFEST and IMAGE, which know no channel. */
struct index;
static const struct index *inherit_ix;
static const char *inherit_channel;
static int inherited(const char *name, const char *arch, char *kind, size_t kl,
                     char *deps, size_t dl, char *from, size_t fl, char *conf, size_t cl);

struct built {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    char          *text;
    size_t         text_len;
    char           ver_from[512];   /* the version was read from this file's $VER */
    char           name_from[512];  /* the name was */
    char           cookie_ver[64];  /* what that $VER says, whatever was used */
    char           cookie_file[512];
    char           arch_from[512];
    char           kind_from[160];  /* KIND taken from this published version */
    char           deps_from[160];  /* DEPENDS too */
    char           config_from[160]; /* CONFIG too */
    char           about_from[1200];/* the catalogue fields too, or the .pkginfo */
    char           info_from[1200]; /* INFO: the .pkginfo the fields were read from */
    char           info_fields[200];/*   and which of them it gave */
    size_t         nconfig;         /* configuration files */
    unsigned       skipped;
    char         **left_out;
    size_t         nleft;
};

/* INFO: which fields this .pkginfo gave, for the line under the result. */
static void say_info_from(const struct built *b)
{
    char what[300];
    int cat = b->about_from[0] && strcmp(b->about_from, b->info_from) == 0;
    if (b->info_from[0] == '\0') return;
    snprintf(what, sizeof what, "%s%s%s", b->info_fields,
             b->info_fields[0] && cat ? " and " : "", cat ? "the catalogue fields" : "");
    if (what[0] != '\0')
        say_detail("%s from %s", what, b->info_from);
}

static void built_free(struct built *b)
{
    size_t i;
    pkg_manifest_free(&b->m);
    free(b->pkg);
    free(b->text);
    for (i = 0; i < b->nleft; i++) free(b->left_out[i]);
    free(b->left_out);
    b->left_out = NULL;
    b->nleft = 0;
}

/* Letters and digits only, lower case: "afs.handler" and "afs-handler" agree. */
static void squash(const char *in, char *out, size_t ol)
{
    size_t o = 0;
    for (; *in && o + 1 < ol; in++)
        if ((*in >= 'a' && *in <= 'z') || (*in >= '0' && *in <= '9'))
            out[o++] = *in;
        else if (*in >= 'A' && *in <= 'Z')
            out[o++] = (char)(*in + 32);
    out[o] = '\0';
}

/* An executable whose $VER names another program than its file name is
 * usually the wrong file copied into the drawer. Scripts and data carry the
 * cookies of whatever they belong to, so only executables are checked. */
static void check_cookie_names(const struct drawer *d)
{
    size_t i;
    char n[65], v[64], sn[65], sf[80];
    for (i = 0; i < d->n; i++) {
        const char *base = strrchr(d->v[i].rel, '/');
        base = base ? base + 1 : d->v[i].rel;
        if (entry_arch(&d->v[i]) == NULL
            || !cookie(&d->v[i], n, sizeof n, v, sizeof v))
            continue;
        squash(n, sn, sizeof sn);
        squash(base, sf, sizeof sf);
        if (sn[0] && sf[0] && strstr(sf, sn) == NULL && strstr(sn, sf) == NULL)
            warn("%s carries the $VER cookie of %s %s, another program than its file name "
                 "says: is it the file you meant to publish?", d->v[i].rel, n, v);
    }
}

struct present_ctx { const char *dir; };

static int present_in(const unsigned char *name, size_t len, void *ctx)
{
    const struct present_ctx *pc = (const struct present_ctx *)ctx;
    char n[512], *p;
    int there;
    if (len >= sizeof n) return 1;
    memcpy(n, name, len);
    n[len] = '\0';
    p = pkg_join(pc->dir, n);
    there = p != NULL && pkg_fs_exists(p);
    free(p);
    return there;
}

static char *pkg_strdup(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    if (p != NULL) strcpy(p, s);
    return p;
}

static void latin1_to_utf8(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 3 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = '\0';
}

/* The protection word and comment each file of the drawer is published
 * with. On AROS, the file system's own. Elsewhere, the publishing profile of
 * ameta.md: owner Execute from the host mode, where there is one, every
 * other bit and the comment from the directory's .ameta, AROS defaults
 * otherwise. A signed package does not drop metadata silently: a malformed
 * or stale .ameta line, or a comment AROS cannot store, is refused. */
static int drawer_attrs(struct drawer *d)
{
    char done[64][512];
    size_t ndone = 0, i;
    int warned_owner = 0;
    for (i = 0; i < d->n; i++) {
        struct loaded *f = &d->v[i];
        char *full = d->root ? pkg_join(d->root, f->rel) : NULL, latin[PKG_COMMENT_MAX + 1], cbuf[4 * PKG_COMMENT_MAX + 1];
        const char *slash = strrchr(f->rel, '/'), *base = slash ? slash + 1 : f->rel;
        char dirrel[512], *dir, *ap;
        unsigned char *buf = NULL;
        size_t len = 0, k;
        struct pkg_ameta a;
        struct pkg_ameta_entry *e;
        int native, ex, seen = 0;
        if (d->root == NULL) {          /* from an archive: its mode, AROS defaults */
            f->prot = pkg_ameta_publish_word(NULL, 1, f->host_exec == 1);
            free(full);
            continue;
        }
        if (full == NULL) return refuse("out of memory");
        native = pkg_fs_amiga_get(full, &f->prot, latin, sizeof latin);
        if (native < 0) { free(full); return refuse_c(17, "cannot read the attributes of \"%s\"", f->rel); }
        if (native == 1) {
            if (latin[0]) {
                latin1_to_utf8(latin, cbuf, sizeof cbuf);
                f->comment = pkg_strdup(cbuf);
            }
            free(full);
            continue;
        }
        ex = pkg_fs_owner_exec(full);
        free(full);
        snprintf(dirrel, sizeof dirrel, "%.*s", slash ? (int)(slash - f->rel) : 0, f->rel);
        dir = pkg_join(d->root, dirrel);
        ap = dir ? pkg_join(dir, ".ameta") : NULL;
        if (ap == NULL) { free(dir); return refuse("out of memory"); }
        pkg_ameta_init(&a);
        if (pkg_fs_exists(ap)) {
            struct present_ctx pc;
            if (pkg_fs_read(ap, &buf, &len) != 0 || pkg_ameta_parse(buf, len, &a) != 0) {
                free(buf); free(ap); free(dir); pkg_ameta_free(&a);
                return refuse_c(17, "cannot read %s/.ameta", dirrel[0] ? dirrel : ".");
            }
            free(buf);
            for (k = 0; k < ndone; k++)
                if (strcmp(done[k], dirrel) == 0) seen = 1;
            if (!a.usable || a.nr > 0) {
                refuse_c(20, "%s/.ameta, line %u: %s. A published package must not drop metadata; "
                         "correct the line", dirrel[0] ? dirrel : ".", a.nr ? a.r[0].line : 1,
                         a.nr ? a.r[0].reason : "unreadable");
                free(ap); free(dir); pkg_ameta_free(&a);
                return 1;
            }
            pc.dir = dir;
            pkg_ameta_mark_stale(&a, present_in, &pc);
            for (k = 0; !seen && k < a.n; k++) {
                char esc[1600];
                pkg_ameta_escape(a.e[k].name, a.e[k].name_len, esc);
                if (a.e[k].stale) {
                    refuse_c(20, "%s/.ameta names %s, which is not in the drawer (renamed or deleted "
                             "on the host?); correct or remove that section", dirrel[0] ? dirrel : ".", esc);
                    free(ap); free(dir); pkg_ameta_free(&a);
                    return 1;
                }
                if ((a.e[k].has_uid || a.e[k].has_gid) && !warned_owner) {
                    warn("%s/.ameta gives an owner, which a package does not carry: installed files "
                         "belong to whoever installs them", dirrel[0] ? dirrel : ".");
                    warned_owner = 1;
                }
                {
                    char raw[512], *sub;
                    snprintf(raw, sizeof raw, "%.*s", (int)a.e[k].name_len, (const char *)a.e[k].name);
                    sub = pkg_join(dir, raw);
                    /* a directory's own attributes: no manifest line holds them */
                    if (sub != NULL && pkg_fs_is_dir(sub)
                        && ((a.e[k].prot & ~PKG_AMETA_RECORD_MASK) || a.e[k].comment_len))
                        warn("%s/.ameta gives attributes to the directory %s, which a package does "
                             "not carry", dirrel[0] ? dirrel : ".", esc);
                    free(sub);
                }
            }
            if (!seen && ndone < 64)
                snprintf(done[ndone++], sizeof done[0], "%s", dirrel);
        }
        e = pkg_ameta_find(&a, (const unsigned char *)base, strlen(base));
        f->prot = pkg_ameta_publish_word(e, ex >= 0, ex == 1);
        if (e != NULL && e->comment_len) {
            char *c = (char *)malloc(e->comment_len + 1);
            if (c == NULL) { free(ap); free(dir); pkg_ameta_free(&a); return refuse("out of memory"); }
            memcpy(c, e->comment, e->comment_len);
            c[e->comment_len] = '\0';
            if (memchr(c, '\0', e->comment_len) != NULL
                || pkg_comment_latin1(c, latin, sizeof latin) < 0) {
                refuse_c(20, "the comment of %s in %s/.ameta is longer than %d characters or holds "
                         "a character outside Latin-1: AROS would cut or change it at install",
                         f->rel, dirrel[0] ? dirrel : ".", PKG_COMMENT_MAX);
                free(c); free(ap); free(dir); pkg_ameta_free(&a);
                return 1;
            }
            f->comment = c;
        }
        tr("attributes of %s: prot 0x%08llx%s%s", f->rel, f->prot, f->comment ? ", comment " : "",
           f->comment ? f->comment : "");
        pkg_ameta_free(&a);
        free(ap);
        free(dir);
    }
    return 0;
}

static int ascii_casecmp_n(const char *x, const char *y, size_t n)
{
    for (; n > 0; n--, x++, y++) {
        int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
        if (cx != cy || cx == 0) return cx - cy;
    }
    return 0;
}

static int ascii_casecmp(const char *x, const char *y)
{
    for (; *x && *y; x++, y++) {
        int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
        if (cx != cy) return cx - cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* Marks the configuration files: each comma-separated entry of `list` is a
 * file of the package or a folder holding some. A name that matches nothing
 * is refused when the command gave it (`strict`), and let go when it came
 * from the last version, whose file may be gone. */
static int mark_config(struct pkg_manifest *m, const char *list, int strict)
{
    const char *p = list;
    while (*p) {
        size_t n, i, hits = 0;
        char pat[1024];
        while (*p == ' ' || *p == ',') p++;
        n = strcspn(p, ",");
        while (n > 0 && p[n - 1] == ' ') n--;
        if (n == 0) break;
        if (n >= sizeof pat)
            return refuse_c(20, "CONFIG holds a name longer than %u characters", (unsigned)sizeof pat - 1);
        memcpy(pat, p, n);
        pat[n] = '\0';
        while (n > 1 && pat[n - 1] == '/') pat[--n] = '\0';
        for (i = 0; i < m->nfiles; i++) {
            const char *f = m->files[i].path;
            if (strcmp(f, pat) == 0 || (strncmp(f, pat, n) == 0 && f[n] == '/')) {
                m->files[i].config = 1;
                hits++;
            }
        }
        if (hits == 0 && strict)
            return refuse_c(20, "CONFIG names \"%s\", which is no file or folder of this package; "
                            "give paths as the package installs them, such as S/Startup-Sequence "
                            "or Prefs/Env-Archive", pat);
        tr("config %s: %lu file%s", pat, (unsigned long)hits, hits == 1 ? "" : "s");
        p += strcspn(p, ",");
    }
    return 0;
}

static int file_digest(const char *path, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size);

/* ---- libraries a program opens ------------------------------------------ */

#include "pkg_syslibs.h"

static int is_system_lib(const char *n)
{
    size_t i;
    for (i = 0; pkg_system_libs[i]; i++)
        if (ascii_casecmp(pkg_system_libs[i], n) == 0) return 1;
    return 0;
}

static int strs_has_nocase(const struct pkg_strs *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (ascii_casecmp(l->v[i], s) == 0) return 1;
    return 0;
}

/* The library and device names a file holds as C strings, "SDL2.library"
 * followed by its NUL: what a program passes to OpenLibrary or OpenDevice. */
static void lib_names(const unsigned char *p, size_t len, struct pkg_strs *out)
{
    static const char *const suffix[] = { ".library", ".device" };
    size_t j, k;
    for (j = 0; j < len; j++) {
        for (k = 0; k < 2; k++) {
            size_t sl = strlen(suffix[k]), a;
            char name[80];
            if (j + sl >= len || memcmp(p + j, suffix[k], sl) != 0 || p[j + sl] != '\0')
                continue;
            a = j;
            while (a > 0 && j - a < 60 && (isalnum(p[a - 1]) || p[a - 1] == '_' || p[a - 1] == '-'
                                         || p[a - 1] == '.' || p[a - 1] == '+'))
                a--;
            if (a == j || (a > 0 && p[a - 1] > ' ' && p[a - 1] < 0x7F && p[a - 1] != ':' && p[a - 1] != '/'))
                continue;           /* no name before it, or the tail of a longer word */
            snprintf(name, sizeof name, "%.*s%s", (int)(j - a), (const char *)p + a, suffix[k]);
            if (pkg_check_libname(name) == NULL && !strs_has_nocase(out, name))
                pkg_strs_add(out, name);
        }
    }
}

static int strcmp_p(const void *x, const void *y)
{
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

static int last_published(const char *name, const char *arch, struct pkg_manifest *em);

/* ---- the catalogue fields at publish ------------------------------------ */

static int valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        int k = p[i] < 0x80 ? 0 : (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2
              : (p[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + (size_t)k >= n + (k ? 0 : 1)) return 0;
        for (i++; k > 0; k--, i++)
            if (i >= n || (p[i] & 0xC0) != 0x80) return 0;
    }
    return 1;
}

/* The lines of a text a person wrote, as the manifest carries them: UTF-8
 * (Latin-1, which Amiga text often is, converted), tabs as spaces, no
 * trailing space, one empty line at most between paragraphs, none at
 * either end. From `start` on, when the text has a header before it. */
static int text_lines(const unsigned char *buf, size_t len, size_t start, struct pkg_strs *out)
{
    char line[1024];
    size_t i = start, o = 0;
    int latin = !valid_utf8(buf, len), blank = 0;
    for (; i <= len; i++) {
        unsigned c = i < len ? buf[i] : '\n';
        if (c == '\r') continue;
        if (c == '\n') {
            while (o > 0 && line[o - 1] == ' ') o--;
            line[o] = '\0';
            if (o == 0) {
                blank = out->n > 0;
            } else {
                if (blank && pkg_strs_add(out, "") != 0) return -1;
                blank = 0;
                if (pkg_strs_add(out, line) != 0) return -1;
            }
            o = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7F) continue;
        if (o == 0 && c == ' ') continue;          /* no leading space either */
        if (latin && c >= 0x80) {
            if (o + 2 < sizeof line) { line[o++] = (char)(0xC0 | (c >> 6)); line[o++] = (char)(0x80 | (c & 0x3F)); }
        } else if (o + 1 < sizeof line) {
            line[o++] = (char)c;
        }
    }
    return 0;
}

static int text_file(const char *path, struct pkg_strs *out, const char *kw)
{
    unsigned char *buf;
    size_t len;
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(11, "%s names %s, which cannot be read", kw, path);
    if (text_lines(buf, len, 0, out) != 0) { free(buf); return refuse("out of memory"); }
    free(buf);
    return 0;
}

/* "a, b, c": each item trimmed, empty ones dropped. */
static int list_items(const char *list, struct pkg_strs *out)
{
    const char *p = list;
    while (*p) {
        char item[512];
        size_t n = strcspn(p, ",");
        size_t a = 0, b = n;
        while (a < b && p[a] == ' ') a++;
        while (b > a && p[b - 1] == ' ') b--;
        if (b - a >= sizeof item) return refuse_c(20, "a list item is longer than %u characters", (unsigned)sizeof item - 1);
        if (b > a) {
            memcpy(item, p + a, b - a);
            item[b - a] = '\0';
            if (pkg_strs_add(out, item) != 0) return refuse("out of memory");
        }
        p += n;
        if (*p == ',') p++;
    }
    return 0;
}

static int strs_copy(struct pkg_strs *dst, const struct pkg_strs *src)
{
    size_t i;
    for (i = 0; i < src->n; i++)
        if (pkg_strs_add(dst, src->v[i]) != 0) return -1;
    return 0;
}

/* An Aminet .readme: "Short:", "Author:", "Type:" and the others in the
 * header, then after the first empty line the text. */
static int aminet_readme(const char *path, struct pkg_about *a)
{
    unsigned char *buf;
    size_t len, i = 0, body = 0;
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(11, "README names %s, which cannot be read", path);
    while (i < len) {
        size_t e = i, k;
        char key[16], val[512];
        while (e < len && buf[e] != '\n') e++;
        if (e == i || (e == i + 1 && buf[i] == '\r')) { body = e + 1; break; }
        for (k = i; k < e && buf[k] != ':' && k - i < sizeof key - 1; k++) key[k - i] = (char)buf[k];
        key[k - i] = '\0';
        if (k < e && buf[k] == ':') {
            struct pkg_strs one = { NULL, 0 };
            size_t vs = k + 1, ve = e;
            while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
            if (ve - vs >= sizeof val) ve = vs + sizeof val - 1;
            if (text_lines(buf + vs, ve - vs, 0, &one) == 0 && one.n > 0) {
                snprintf(val, sizeof val, "%s", one.v[0]);
                if (ascii_casecmp(key, "Short") == 0 && a->short_desc == NULL)
                    a->short_desc = pkg_strdup(val);
                else if (ascii_casecmp(key, "Author") == 0 && a->authors.n == 0)
                    list_items(val, &a->authors);
                else if (ascii_casecmp(key, "Type") == 0 && a->category == NULL) {
                    char *sp = strchr(val, ' ');
                    if (sp) *sp = '\0';        /* "util/arc  (more)" */
                    if (pkg_check_category(val) == NULL) a->category = pkg_strdup(val);
                }
            }
            pkg_strs_free(&one);
        }
        i = e + 1;
    }
    if (body > 0 && body < len && a->description.n == 0) {
        /* the text, less the readme's own heading over it: "DESCRIPTION",
         * "Description" and its line of "=" */
        size_t k = 0, i2;
        text_lines(buf, len, body, &a->description);
        while (k < a->description.n) {
            const char *l = a->description.v[k];
            int rule = l[0] != '\0';
            for (i2 = 0; l[i2] && rule; i2++) rule = l[i2] == '=' || l[i2] == '-' || l[i2] == '*';
            if (!(ascii_casecmp(l, "description") == 0 || ascii_casecmp(l, "description:") == 0
                  || rule || l[0] == '\0'))
                break;
            k++;
        }
        if (k > 0) {
            for (i2 = 0; i2 < k; i2++) free(a->description.v[i2]);
            memmove(a->description.v, a->description.v + k, (a->description.n - k) * sizeof *a->description.v);
            a->description.n -= k;
        }
    }
    free(buf);
    return 0;
}

/* ---- INFO: a .pkginfo the port carries --------------------------------- */

/* What INFO named, parsed, for the length of this build; freed when the next
 * build reads one, since compose_about borrows its strings. */
static struct pkg_pkginfo cur_info;
static int have_info;
static char info_shown[1200];

/* One file's bytes out of an archive, for INFO "!/<path in archive>". */
struct grab {
    const char    *want;
    unsigned char *data;
    size_t         len, cap;
    int            found, oom;
};

static int grab_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct grab *g = (struct grab *)ctx;
    return !e->is_dir && strcmp(e->path, g->want) == 0;
}

static int grab_data(const struct pkg_archive_entry *e, const unsigned char *buf,
                     size_t len, void *ctx)
{
    struct grab *g = (struct grab *)ctx;
    (void)e;
    if (len == 0) { g->found = 1; return -1; }      /* it is whole: stop the walk */
    if (g->len + len > g->cap) {
        size_t ncap = g->cap ? g->cap : 4096;
        unsigned char *w;
        while (ncap < g->len + len) ncap *= 2u;
        w = (unsigned char *)realloc(g->data, ncap);
        if (w == NULL) { g->oom = 1; return -1; }
        g->data = w;
        g->cap = ncap;
    }
    memcpy(g->data + g->len, buf, len);
    g->len += len;
    return 0;
}

/* Read and parse the file INFO names: a path of the file system, or
 * "!/<path>" inside the archive the drawer is a prefix of. */
static int read_info(const struct pkg_options *a)
{
    char err[400], arch_file[1024], arch_prefix[1024];
    unsigned char *buf = NULL;
    size_t len = 0;
    int line = 0, rc;

    if (have_info) { pkg_pkginfo_free(&cur_info); have_info = 0; }
    if (a->info[0] == '!' && a->info[1] == '/') {
        struct grab g;
        memset(&g, 0, sizeof g);
        if (!pkg_archive_split(a->target ? a->target : "", arch_file, sizeof arch_file,
                               arch_prefix, sizeof arch_prefix))
            return refuse_c(20, "INFO \"%s\" reads the file out of the archive the package's "
                            "files come from, and this is no archive: publish from "
                            "\"<archive>!/<top dir>\", or give INFO a path of your file system",
                            a->info);
        g.want = a->info + 2;
        snprintf(info_shown, sizeof info_shown, "%s!/%s", arch_file, g.want);
        tr("reading %s out of %s", g.want, arch_file);
        err[0] = '\0';
        if (pkg_archive_walk(arch_file, grab_want, grab_data, &g, err, sizeof err) != 0
            && !g.found) {
            free(g.data);
            if (g.oom) return refuse("out of memory");
            return refuse_c(err[0] ? 12 : 11, "%s", err[0] ? err : "no file at that path in the archive");
        }
        if (!g.found) {
            free(g.data);
            return refuse_c(11, "the archive %s holds no \"%s\"; INFO names the path as the "
                            "archive names it", arch_file, g.want);
        }
        buf = g.data;
        len = g.len;
    } else {
        snprintf(info_shown, sizeof info_shown, "%s", a->info);
        if (!pkg_fs_exists(a->info))
            return refuse_c(11, "no .pkginfo at \"%s\"", a->info);
        if (pkg_fs_read(a->info, &buf, &len) != 0)
            return refuse_c(17, "cannot read \"%s\"", a->info);
    }
    rc = pkg_pkginfo_parse((const char *)buf, len, &cur_info, err, sizeof err, &line);
    free(buf);
    if (rc != 0) {
        pkg_pkginfo_free(&cur_info);
        /* the publisher's own file, like a keyword's value: a mistake to correct */
        if (line > 0)
            return refuse_c(20, "%s, line %d: %s. Correct the file", info_shown, line, err);
        return refuse_c(20, "%s: %s. Correct the file", info_shown, err);
    }
    have_info = 1;
    tr("INFO %s: name %s, version %s, kind %s, %lu file%s", info_shown,
       cur_info.name ? cur_info.name : "-", cur_info.version ? cur_info.version : "-",
       cur_info.kind ? cur_info.kind : "-", (unsigned long)cur_info.files.n,
       cur_info.files.n == 1 ? "" : "s");
    return 0;
}

static int set_once(char **slot, const char *v)
{
    free(*slot);
    *slot = NULL;
    if (v == NULL || ascii_casecmp(v, "none") == 0)
        return 0;
    return (*slot = pkg_strdup(v)) == NULL ? -1 : 0;
}

/* The catalogue fields of the version being published: the keywords, then
 * the Aminet readme, then the last version published. */
static int compose_about(const struct pkg_options *a, const char *name, const char *arch,
                         struct pkg_about *out, char *from, size_t fl)
{
    struct pkg_manifest em;
    struct pkg_about rd;
    const char *why = NULL;
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&rd, 0, sizeof rd);
    from[0] = '\0';
    tr("catalogue fields: name %s, arch %s, channel index %s", name ? name : "-", arch ? arch : "-", inherit_ix ? "read" : "none");
    if (name != NULL && last_published(name, arch, &em)) {
        struct pkg_about *p = &em.about;
        int any = p->short_desc || p->description.n || p->category || p->tags.n || p->authors.n
                  || p->homepage || p->repository || p->license || p->distribution
                  || p->icon || p->screenshots.n;
        if (set_once(&out->short_desc, p->short_desc) || strs_copy(&out->description, &p->description)
            || set_once(&out->category, p->category) || strs_copy(&out->tags, &p->tags)
            || strs_copy(&out->authors, &p->authors) || set_once(&out->homepage, p->homepage)
            || set_once(&out->repository, p->repository) || set_once(&out->license, p->license)
            || set_once(&out->distribution, p->distribution) || set_once(&out->icon, p->icon)
            || strs_copy(&out->screenshots, &p->screenshots)) {
            pkg_manifest_free(&em);
            return refuse("out of memory");
        }
        if (any) snprintf(from, fl, "%s %s", em.name, em.version);
        pkg_manifest_free(&em);
    }
    if (a->readme != NULL) {
        if (aminet_readme(a->readme, &rd) != 0) return 1;
        if (rd.short_desc) set_once(&out->short_desc, rd.short_desc);
        if (rd.category) set_once(&out->category, rd.category);
        if (rd.authors.n) { pkg_strs_free(&out->authors); strs_copy(&out->authors, &rd.authors); }
        if (rd.description.n) { pkg_strs_free(&out->description); strs_copy(&out->description, &rd.description); }
        free(rd.short_desc); free(rd.category);
        pkg_strs_free(&rd.authors); pkg_strs_free(&rd.description);
        from[0] = '\0';
    }
    if (have_info) {
        /* what the port says about itself: over the readme and over the last
         * version, under the keywords below */
        const struct pkg_about *pi = &cur_info.about;
        int any = pi->short_desc || pi->description.n || pi->category || pi->tags.n
                  || pi->authors.n || pi->homepage || pi->repository || pi->license
                  || pi->distribution || pi->changes.n || pi->icon || pi->screenshots.n;
        if (pi->short_desc) set_once(&out->short_desc, pi->short_desc);
        if (pi->category) set_once(&out->category, pi->category);
        if (pi->homepage) set_once(&out->homepage, pi->homepage);
        if (pi->repository) set_once(&out->repository, pi->repository);
        if (pi->license) set_once(&out->license, pi->license);
        if (pi->distribution) set_once(&out->distribution, pi->distribution);
        if (pi->icon) set_once(&out->icon, pi->icon);
        if (pi->description.n) { pkg_strs_free(&out->description); strs_copy(&out->description, &pi->description); }
        if (pi->changes.n) { pkg_strs_free(&out->changes); strs_copy(&out->changes, &pi->changes); }
        if (pi->tags.n) { pkg_strs_free(&out->tags); strs_copy(&out->tags, &pi->tags); }
        if (pi->authors.n) { pkg_strs_free(&out->authors); strs_copy(&out->authors, &pi->authors); }
        if (pi->screenshots.n) { pkg_strs_free(&out->screenshots); strs_copy(&out->screenshots, &pi->screenshots); }
        if (any) snprintf(from, fl, "%s", info_shown);
        else from[0] = '\0';
    }
    if ((a->short_desc && set_once(&out->short_desc, a->short_desc))
        || (a->category && set_once(&out->category, a->category))
        || (a->homepage && set_once(&out->homepage, a->homepage))
        || (a->repository && set_once(&out->repository, a->repository))
        || (a->license && set_once(&out->license, a->license))
        || (a->distribution && set_once(&out->distribution, a->distribution))
        || (a->icon && set_once(&out->icon, a->icon)))
        return refuse("out of memory");
    if (a->description) {
        pkg_strs_free(&out->description);
        if (ascii_casecmp(a->description, "none") != 0 && text_file(a->description, &out->description, "DESCRIPTION") != 0)
            return 1;
    }
    if (a->changes && text_file(a->changes, &out->changes, "CHANGES") != 0)
        return 1;
    if (a->tags) {
        struct pkg_strs t = { NULL, 0 };
        pkg_strs_free(&out->tags);
        if (ascii_casecmp(a->tags, "none") != 0) {
            if (list_items(a->tags, &t) != 0) return 1;
            for (i = 0; i < t.n; i++) {
                char *c;
                for (c = t.v[i]; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
                if (pkg_strs_add(&out->tags, t.v[i]) != 0) { pkg_strs_free(&t); return refuse("out of memory"); }
            }
            pkg_strs_free(&t);
        }
    }
    if (a->author) {
        pkg_strs_free(&out->authors);
        if (ascii_casecmp(a->author, "none") != 0 && list_items(a->author, &out->authors) != 0) return 1;
    }
    if (a->screenshot) {
        pkg_strs_free(&out->screenshots);
        if (ascii_casecmp(a->screenshot, "none") != 0 && list_items(a->screenshot, &out->screenshots) != 0) return 1;
    }
    /* the rules the manifest parser applies, said here with the keyword */
    if (out->short_desc && (why = pkg_check_about_text("SHORT", out->short_desc, 40)) != NULL) return refuse_c(20, "%s", why);
    if (out->category && (why = pkg_check_category(out->category)) != NULL) return refuse_c(20, "CATEGORY: %s", why);
    if (out->homepage && (why = pkg_check_url("HOMEPAGE", out->homepage)) != NULL) return refuse_c(20, "%s", why);
    if (out->repository && (why = pkg_check_url("REPOSITORY", out->repository)) != NULL) return refuse_c(20, "%s", why);
    if (out->license && (why = pkg_check_license(out->license)) != NULL) return refuse_c(20, "LICENSE: %s", why);
    if (out->distribution && (why = pkg_check_distribution(out->distribution)) != NULL) return refuse_c(20, "DISTRIBUTION: %s", why);
    if (out->tags.n > 16) return refuse_c(20, "TAGS holds %lu tags; 16 at most", (unsigned long)out->tags.n);
    for (i = 0; i < out->tags.n; i++) {
        size_t k;
        if ((why = pkg_check_tag(out->tags.v[i])) != NULL) return refuse_c(20, "TAGS: \"%s\": %s", out->tags.v[i], why);
        for (k = 0; k < i; k++)
            if (strcmp(out->tags.v[k], out->tags.v[i]) == 0) return refuse_c(20, "TAGS names \"%s\" twice", out->tags.v[i]);
    }
    for (i = 0; i < out->authors.n; i++)
        if ((why = pkg_check_about_text("AUTHOR", out->authors.v[i], 80)) != NULL) return refuse_c(20, "%s", why);
    for (i = 0; i < out->description.n; i++)
        if ((why = pkg_check_about_text("a DESCRIPTION line", out->description.v[i], 1000)) != NULL) return refuse_c(20, "%s", why);
    for (i = 0; i < out->changes.n; i++)
        if ((why = pkg_check_about_text("a CHANGES line", out->changes.v[i], 1000)) != NULL) return refuse_c(20, "%s", why);
    return 0;
}

/* An archive's SHA-256 and size, kept in <archive>.sha256 beside it
 * ("sha256 <hex> <size> <mtime>") so that a hundred packages published from
 * one nightly read it once. Recomputed when the archive changes. */
static int archive_digest(const char *archive, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size)
{
    struct pkg_fs_id id;
    char *sp, line[200], want[120];
    unsigned char *buf = NULL;
    size_t len = 0, k;
    int n;

    if (pkg_fs_identity(archive, &id) != 0 || !id.exists) return -1;
    sp = malloc(strlen(archive) + 8);
    if (sp == NULL) return -1;
    snprintf(sp, strlen(archive) + 8, "%s.sha256", archive);
    n = snprintf(want, sizeof want, " %llu %lld\n", id.size, id.mtime_s);
    if (pkg_fs_read(sp, &buf, &len) == 0 && len == 7 + PKG_SHA256_HEXLEN + (size_t)n
        && memcmp(buf, "sha256 ", 7) == 0 && memcmp(buf + 7 + PKG_SHA256_HEXLEN, want, (size_t)n) == 0) {
        for (k = 0; k < PKG_SHA256_HEXLEN; k++) hex[k] = (char)buf[7 + k];
        hex[PKG_SHA256_HEXLEN] = '\0';
        *size = id.size;
        free(buf);
        free(sp);
        return 0;
    }
    free(buf);
    tr("hashing %s", archive);
    if (file_digest(archive, hex, size) != 0) { free(sp); return -1; }
    n = snprintf(line, sizeof line, "sha256 %s%s", hex, want);
    if (pkg_fs_write_atomic(sp, line, (size_t)n) != 0)
        warn("the archive's digest could not be kept in %s; it is computed again next time", sp);
    free(sp);
    return 0;
}

static int build(const struct pkg_options *a, struct built *out)
{
    static struct pkg_options eff;   /* the options with what INFO gave filled in */
    int info_files = 0;
    struct drawer d;
    struct pkg_writer *w;
    char vname[65], vver[64];
    const char *from = NULL, *name, *version, *arch, *kind, *why;
    const char *kind_src, *deps_src;
    char arch_file[1024], arch_prefix[1024];
    char ikind[32], ideps[1024], ifrom[160], iconf[4096];
    const char *conf_src;
    char payload[PKG_SHA256_HEXLEN + 1];
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&d, 0, sizeof d);
    pkg_manifest_init(&out->m);
    if (a->target == NULL)
        return refuse_c(20, "name the drawer to package");
    if (have_info) { pkg_pkginfo_free(&cur_info); have_info = 0; }
    if (a->info != NULL) {
        /* A keyword on the line wins over the file; the file wins over the
         * readme and over the last version published. */
        if (read_info(a) != 0)
            return 1;
        eff = *a;
        if (eff.name == NULL && cur_info.name) eff.name = cur_info.name;
        if (eff.version == NULL && cur_info.version) eff.version = cur_info.version;
        if (eff.kind == NULL && cur_info.kind) eff.kind = cur_info.kind;
        if (eff.depends == NULL && cur_info.depends && cur_info.depends[0]) eff.depends = cur_info.depends;
        if (eff.files == NULL && cur_info.files.n) eff.files = cur_info.files_line;
        if (eff.config == NULL && cur_info.config.n) eff.config = cur_info.config_line;
        info_files = eff.files == cur_info.files_line && cur_info.files.n > 0;
        snprintf(out->info_from, sizeof out->info_from, "%s", info_shown);
        {
            /* what of it was used, for PUBLISH to say where the fields came from */
            const struct { int given; const char *word; } gave[] = {
                { eff.name == cur_info.name && cur_info.name != NULL, "name" },
                { eff.version == cur_info.version && cur_info.version != NULL, "version" },
                { eff.kind == cur_info.kind && cur_info.kind != NULL, "kind" },
                { eff.depends == cur_info.depends && cur_info.depends != NULL, "dependencies" },
                { info_files, "files" },
                { eff.config == cur_info.config_line && cur_info.config.n > 0, "config" }
            };
            size_t g, at = 0;
            for (g = 0; g < sizeof gave / sizeof gave[0]; g++)
                if (gave[g].given && at < sizeof out->info_fields)
                    at += (size_t)snprintf(out->info_fields + at, sizeof out->info_fields - at,
                                           "%s%s", at ? ", " : "", gave[g].word);
        }
        a = &eff;
    }
    kind_src = a->kind;
    deps_src = a->depends;
    conf_src = a->config;
    if (a->upstream != NULL && !pkg_archive_split(a->target, arch_file, sizeof arch_file, arch_prefix,
                                                  sizeof arch_prefix))
        return refuse_c(20, "UPSTREAM says where the archive a package's files stay in is published; "
                        "a drawer has no archive. Publish from \"<archive>!/<top dir>\"");
    if (pkg_archive_split(a->target, arch_file, sizeof arch_file, arch_prefix, sizeof arch_prefix)) {
        const char *base = strrchr(arch_file, '/');
        char src[2200];
        base = base ? base + 1 : arch_file;
        if (!pkg_fs_exists(arch_file))
            return refuse_c(20, "no archive at \"%s\"", arch_file);
        d.root = NULL;                  /* no file system behind these files */
        if (load_archive(&d, arch_file, arch_prefix, a->files) != 0) {
            refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the archive");
            drawer_free(&d);
            return 1;
        }
        snprintf(src, sizeof src, "%s!/%s", base, arch_prefix);
        pkg_manifest_set(&out->m.source, src);
        if (a->upstream != NULL) {
            char hex[PKG_SHA256_HEXLEN + 1];
            unsigned long long asz;
            size_t q;
            const char *u = a->upstream;
            if (strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0) {
                drawer_free(&d);
                return refuse_c(20, "UPSTREAM must be the http or https URL the archive is downloaded from");
            }
            for (q = 0; u[q]; q++)
                if ((unsigned char)u[q] <= ' ' || u[q] == 0x7F) {
                    drawer_free(&d);
                    return refuse_c(20, "UPSTREAM holds a space or a control character; write spaces in a URL as %%20");
                }
            if (archive_digest(arch_file, hex, &asz) != 0) {
                drawer_free(&d);
                return refuse_c(17, "cannot read %s to take its SHA-256", arch_file);
            }
            out->m.archive_sha = pkg_strdup(hex);
            out->m.archive_url = pkg_strdup(u);
            out->m.archive_size = asz;
            if (out->m.archive_sha == NULL || out->m.archive_url == NULL) {
                drawer_free(&d);
                return refuse("out of memory");
            }
        }
        tr("drawer from %s: %lu files under %s", arch_file, (unsigned long)d.n,
           arch_prefix[0] ? arch_prefix : "its top");
    } else {
        if (!pkg_fs_is_dir(a->target))
            return refuse_c(20, "\"%s\" is not a directory", a->target);
        d.root = a->target;
        d.files = a->files;
        if (pkg_fs_walk(a->target, load_one, leave_out, &d, &out->skipped, d.err, sizeof d.err) != 0) {
            refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
            drawer_free(&d);
            return 1;
        }
    }
    if (info_files) {
        /* Every path the .pkginfo claims must be there: a port that moved a
         * file would otherwise publish a package quietly missing it. */
        size_t f;
        for (f = 0; f < cur_info.files.n; f++) {
            const char *want = cur_info.files.v[f];
            size_t wl = strlen(want), k;
            int found = 0;
            for (k = 0; k < d.n && !found; k++)
                found = strcmp(d.v[k].rel, want) == 0
                        || (strncmp(d.v[k].rel, want, wl) == 0 && d.v[k].rel[wl] == '/');
            if (!found) {
                refuse_c(11, "%s names \"%s\" in Files, and \"%s\" holds no such file or "
                         "drawer; correct the .pkginfo, or publish what it describes",
                         info_shown, want, a->target);
                drawer_free(&d);
                return 1;
            }
        }
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->target);
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);
    if (drawer_attrs(&d) != 0) { drawer_free(&d); return 1; }
    {
        size_t t;
        tr("drawer %s: %lu files, %lu left out", a->target, (unsigned long)d.n,
           (unsigned long)d.nleft);
        for (t = 0; t < d.nleft; t++)
            tr("left out %s, host metadata", d.left_out[t]);
    }
    out->left_out = d.left_out;          /* reported by PUBLISH, freed with out */
    out->nleft = d.nleft;
    d.left_out = NULL;
    d.nleft = 0;

    name = a->name;
    version = a->version;
    check_cookie_names(&d);
    {
        char seen[600];
        int got = find_ver(&d, name, vname, sizeof vname, vver, sizeof vver, &from,
                           seen, sizeof seen);
        int needed = name == NULL || version == NULL;
        if (!needed && got < 0)
            got = 0;
        if (got < 0 && a->build != NULL) {
            got = 0;        /* a nightly component whose cookies name other programs: 0+build */
            tr("no $VER cookie of %s among %s: its version is 0+%s", name ? name : "it", seen, a->build);
        }
        if (got < 0 && name != NULL) {
            drawer_free(&d);
            return refuse_c(20, "none of the drawer's $VER cookies is %s: %s. Add VERSION",
                            name, seen);
        }
        if (got < 0) {
            drawer_free(&d);
            return refuse_c(20, "the $VER cookies in the drawer name different programs or "
                            "versions: %s. Say which this package is with NAME and VERSION", seen);
        }
        if (got > 0) {
            char vmain[64];
            /* the build a nightly adds (+20260918) is not the program's own */
            snprintf(vmain, sizeof vmain, "%.*s", version ? (int)strcspn(version, "+") : 0,
                     version ? version : "");
            if (version != NULL && pkg_version_cmp(vmain, vver) != 0)
                warn("VERSION %s, but the $VER cookie in %s says %s: is this the build "
                     "you meant to ship?", version, from, vver);
            if (version == NULL)
                snprintf(out->ver_from, sizeof out->ver_from, "%s", from);
            if (name == NULL)
                snprintf(out->name_from, sizeof out->name_from, "%s", from);
            snprintf(out->cookie_ver, sizeof out->cookie_ver, "%s", vver);
            snprintf(out->cookie_file, sizeof out->cookie_file, "%s", from);
            if (name == NULL) name = vname;
            if (version == NULL) version = vver;
        }
    }
    {
        char seen[1200];
        const char *found_arch = NULL, *afrom = NULL;
        int got = drawer_arch(&d, &found_arch, &afrom, seen, sizeof seen);
        /* A boot package carries the loader stages the firmware runs (GRUB's
         * i386-pc for an x86_64 system), an SDK the libraries of each target
         * it compiles for: their CPU is the machine ARCH names. */
        const char *k0 = a->kind;
        int several_ok;
        if (k0 == NULL && a->arch != NULL && inherit_ix != NULL && name != NULL
            && inherited(name, a->arch, ikind, sizeof ikind, ideps, sizeof ideps, ifrom, sizeof ifrom,
                         iconf, sizeof iconf))
            k0 = ikind;
        several_ok = k0 != NULL && a->arch != NULL && (strcmp(k0, "boot") == 0 || strcmp(k0, "sdk") == 0);
        if (got < 0 && several_ok) {
            tr("executables for several CPUs (%s), accepted for KIND %s, ARCH %s", seen, k0, a->arch);
            got = 0;
        } else if (got < 0) {
            drawer_free(&d);
            return refuse_c(20, "the drawer holds executables for different CPUs: %s. One "
                            "package is one architecture: publish each separately (FILES picks "
                            "the paths). A boot or sdk package may hold several, when KIND and "
                            "ARCH name the kind and the machine it is for", seen);
        }
        if (a->arch != NULL && got > 0 && strcmp(a->arch, found_arch) != 0 && !several_ok) {
            /* afrom points into the drawer: refuse before freeing it. */
            refuse_c(20, "ARCH %s, but %s is built for %s", a->arch, afrom, found_arch);
            drawer_free(&d);
            return 1;
        }
        arch = a->arch ? a->arch : got > 0 ? found_arch : "generic";
        tr("architecture %s: %s", arch, a->arch ? "given with ARCH"
           : got > 0 ? afrom : "no executable header found");
        if (a->arch == NULL && got > 0)
            snprintf(out->arch_from, sizeof out->arch_from, "%s", afrom);
    }
    if (inherit_ix != NULL && name != NULL && (a->kind == NULL || a->depends == NULL || a->config == NULL)
        && inherited(name, arch, ikind, sizeof ikind, ideps, sizeof ideps, ifrom, sizeof ifrom,
                     iconf, sizeof iconf)) {
        /* A new version is the same package: what it is and what it needs
         * come from the last one published, unless the command says. */
        if (a->kind == NULL) {
            kind_src = ikind;
            snprintf(out->kind_from, sizeof out->kind_from, "%s", ifrom);
        }
        if (a->depends == NULL) {
            deps_src = ideps;
            snprintf(out->deps_from, sizeof out->deps_from, "%s", ifrom);
        }
        if (a->config == NULL && iconf[0]) {
            conf_src = iconf;
            snprintf(out->config_from, sizeof out->config_from, "%s", ifrom);
        }
    }
    if (kind_src == NULL && strcmp(verb_name, "publish") == 0) {
        drawer_free(&d);
        return refuse_c(20, "no KIND given, and no version of %s is published in this channel to "
                        "take it from; pkg does not guess what a package is. KIND image for "
                        "a program people run, installed as one volume to mount; application "
                        "for a program installed as loose files; library, device (handlers "
                        "too), class, font or catalog for what other programs use, in Libs, "
                        "Devs or L, Classes, Fonts, Locale; startup or boot for what the system "
                        "runs as it starts; data, sdk or slave", name ? name : "this package");
    }
    kind = kind_src ? kind_src : "application";
    if (out->m.source != NULL && strcmp(kind, "image") == 0) {
        drawer_free(&d);
        return refuse_c(20, "files taken from an archive install at the paths the archive gives "
                        "them; an image is made from a drawer. Use another kind, or unpack the "
                        "drawer and publish it as an image");
    }
    if (name == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no NAME given and no $VER: cookie found; add NAME <name>");
    }
    if (a->build != NULL) {
        /* A nightly: the component's own version, and the build it came from. */
        static char with_build[160];
        size_t k;
        for (k = 0; a->build[k] && ((a->build[k] >= '0' && a->build[k] <= '9') || a->build[k] == '.'); k++) ;
        if (a->build[0] == '\0' || a->build[k] != '\0') {
            drawer_free(&d);
            return refuse_c(20, "BUILD must be dotted numbers, such as the date of the nightly: 20260918");
        }
        snprintf(with_build, sizeof with_build, "%.*s+%s", version ? (int)strcspn(version, "+") : 1,
                 version ? version : "0", a->build);
        version = with_build;
    }
    if (version == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no VERSION given and no $VER: cookie found; add VERSION <version>");
    }
    if ((why = pkg_check_name(name)) || (why = pkg_check_version(version))
        || (why = pkg_check_arch(arch))) {
        drawer_free(&d);
        return refuse_c(20, "%s", why);
    }
    if ((why = pkg_check_kind(kind)) != NULL) {
        static const char *const alias[][2] = {
            { "handler", "device" }, { "filesystem", "device" }, { "driver", "device" },
            { "lib", "library" }, { "libs", "library" }, { "mui", "class" },
            { "datatype", "class" }, { "gadget", "class" }, { "locale", "catalog" },
            { "program", "image" }, { "tool", "image" }, { "app", "image" }, { "game", "image" },
            { "headers", "sdk" }, { "docs", "data" }
        };
        size_t al;
        for (al = 0; al < sizeof alias / sizeof alias[0]; al++)
            if (ascii_casecmp(kind, alias[al][0]) == 0) {
                drawer_free(&d);
                return refuse_c(20, "no kind \"%s\"; the kind for that is %s (a program people "
                                "run is image, or application to install it as loose files)",
                                kind, alias[al][1]);
            }
        drawer_free(&d);
        return refuse_c(20, "%s", why);
    }

    pkg_manifest_set(&out->m.name, name);
    pkg_manifest_set(&out->m.version, version);
    pkg_manifest_set(&out->m.architecture, arch);
    pkg_manifest_set(&out->m.kind, kind);
    if (add_depends(&out->m, deps_src) != 0) {
        drawer_free(&d);
        return 1;
    }
    if (a->config != NULL && strcmp(kind, "image") == 0 && ascii_casecmp(a->config, "none") != 0) {
        drawer_free(&d);
        return refuse_c(20, "CONFIG is for files installed loose, which a person may edit; an "
                        "image is mounted read-only and never edited in place");
    }
    if (strcmp(kind, "image") == 0) {
        /* The manifest names what is inside the image too, so that a new
         * version can be compared with the last one file by file. */
        for (i = 0; i < d.n; i++) {
            char hex[PKG_SHA256_HEXLEN + 1];
            pkg_sha256_hex(d.v[i].data, d.v[i].len, hex);
            if (pkg_manifest_add_content(&out->m, d.v[i].rel, hex,
                                         (unsigned long long)d.v[i].len) != 0) {
                drawer_free(&d);
                return refuse("out of memory");
            }
            out->m.content[out->m.ncontent - 1].prot = d.v[i].prot;
            if (d.v[i].comment != NULL)
                out->m.content[out->m.ncontent - 1].comment = pkg_strdup(d.v[i].comment);
        }
    }
    if (strcmp(kind, "image") == 0 && to_image(&d, name) != 0) {
        drawer_free(&d);
        return 1;
    }

    w = pkg_writer_new();
    if (w == NULL) { drawer_free(&d); return refuse("out of memory"); }
    for (i = 0; i < d.n; i++) {
        char hex[PKG_SHA256_HEXLEN + 1];
        if (d.v[i].pre) snprintf(hex, sizeof hex, "%s", d.v[i].pre_digest);
        else pkg_sha256_hex(d.v[i].data, d.v[i].len, hex);
        if (pkg_manifest_add_file(&out->m, d.v[i].rel, hex, (unsigned long long)d.v[i].len) != 0
            || (!d.v[i].pre && pkg_writer_add(w, d.v[i].rel, d.v[i].data, d.v[i].len) != PKG_OK)) {
            pkg_writer_free(w); drawer_free(&d);
            return refuse("out of memory");
        }
        out->m.files[out->m.nfiles - 1].prot = d.v[i].prot;
        if (d.v[i].comment != NULL)
            out->m.files[out->m.nfiles - 1].comment = pkg_strdup(d.v[i].comment);
    }
    {
        /* Provides: the libraries and devices it ships where LIBS: and DEVS:
         * look; then what its programs open that nothing here provides */
        struct pkg_strs opened = { NULL, 0 };
        for (i = 0; i < d.n; i++) {
            const char *rel = d.v[i].rel, *slash = strchr(rel, '/'), *bn = slash ? slash + 1 : "";
            size_t bl = strlen(bn);
            if (slash && !strchr(bn, '/')
                && ((((size_t)(slash - rel) == 4 && ascii_casecmp_n(rel, "Libs", 4) == 0) && bl > 8
                     && ascii_casecmp(bn + bl - 8, ".library") == 0)
                    || (((size_t)(slash - rel) == 4 && ascii_casecmp_n(rel, "Devs", 4) == 0) && bl > 7
                        && ascii_casecmp(bn + bl - 7, ".device") == 0))
                && pkg_check_libname(bn) == NULL && !strs_has_nocase(&out->m.provides, bn))
                pkg_strs_add(&out->m.provides, bn);
        }
        if (out->m.provides.n > 1)
            qsort(out->m.provides.v, out->m.provides.n, sizeof *out->m.provides.v, strcmp_p);
        for (i = 0; i < d.n; i++) {
            struct pkg_strs mine = { NULL, 0 }, missing = { NULL, 0 };
            size_t k, q;
            if (d.v[i].pre || file_arch(d.v[i].data, d.v[i].len) == NULL)
                continue;           /* only executables; an archive's files are not read here */
            lib_names(d.v[i].data, d.v[i].len, &mine);
            for (k = 0; k < mine.n; k++) {
                const char *ln = mine.v[k];
                int given = strs_has_nocase(&out->m.provides, ln) || is_system_lib(ln);
                for (q = 0; !given && q < out->m.ndeps; q++) {
                    struct pkg_manifest dm;
                    if (last_published(out->m.deps[q].name, arch, &dm)) {
                        given = strs_has_nocase(&dm.provides, ln);
                        pkg_manifest_free(&dm);
                    }
                }
                if (!given) pkg_strs_add(&missing, ln);
            }
            if (missing.n) {
                char list[600];
                size_t at = 0;
                for (k = 0; k < missing.n && at < sizeof list; k++)
                    at += (size_t)snprintf(list + at, sizeof list - at, "%s%s",
                                           k == 0 ? "" : k + 1 == missing.n ? " and " : ", ", missing.v[k]);
                warn("%s opens %s and no dependency provides %s: DEPENDS on the package that "
                     "ships %s, or ship %s in Libs", d.v[i].rel, list, missing.n == 1 ? "it" : "them",
                     missing.n == 1 ? "it" : "them", missing.n == 1 ? "it" : "them");
            }
            pkg_strs_free(&mine);
            pkg_strs_free(&missing);
        }
        pkg_strs_free(&opened);
    }
    drawer_free(&d);
    if (conf_src != NULL && strcmp(kind, "image") != 0 && ascii_casecmp(conf_src, "none") != 0
        && mark_config(&out->m, conf_src, out->config_from[0] == '\0') != 0) {
        pkg_writer_free(w);
        return 1;
    }
    for (i = 0; i < out->m.nfiles; i++)
        out->nconfig += out->m.files[i].config ? 1 : 0;
    if (compose_about(a, name, arch, &out->m.about, out->about_from, sizeof out->about_from) != 0) {
        pkg_writer_free(w);
        return 1;
    }
    {
        /* ICON and SCREENSHOT name files of the package, or of its image */
        const struct pkg_about *ab = &out->m.about;
        size_t k, f, np = (ab->icon ? 1 : 0) + ab->screenshots.n;
        for (k = 0; k < np; k++) {
            const char *pic = (ab->icon && k == 0) ? ab->icon : ab->screenshots.v[k - (ab->icon ? 1 : 0)];
            int found = 0;
            for (f = 0; f < out->m.nfiles && !found; f++) found = strcmp(out->m.files[f].path, pic) == 0;
            for (f = 0; f < out->m.ncontent && !found; f++) found = strcmp(out->m.content[f].path, pic) == 0;
            if (!found) {
                pkg_writer_free(w);
                return refuse_c(20, "%s names \"%s\", which is no file of this package; give the "
                                "path as the package installs it", (ab->icon && k == 0) ? "ICON" : "SCREENSHOT", pic);
            }
        }
    }
    if (pkg_writer_finish(w, &out->pkg, &out->pkg_len) != PKG_OK) {
        pkg_writer_free(w);
        return refuse_c(17, "cannot assemble the container");
    }
    pkg_writer_free(w);
    pkg_sha256_hex(out->pkg, out->pkg_len, payload);
    if (out->m.source == NULL)          /* an archive's files are fetched from it */
        pkg_manifest_set(&out->m.payload, payload);
    if (pkg_manifest_emit(&out->m, &out->text, &out->text_len) != 0)
        return refuse("out of memory");
    return 0;
}

/* ---- channel ---------------------------------------------------------- */

struct entry {
    char name[65];
    char version[64];
    char arch[32];      /* one version may be published for several CPUs */
    char digest[PKG_SHA256_HEXLEN + 1];
    int  withdrawn;     /* its publisher signed a withdrawal */
    unsigned char ch;   /* the channel it was read from: chan_of() names it */
};

struct index {
    struct entry *e;
    size_t        n;
};

static void mark_withdrawn(const char *channel, struct index *ix);

/* ---- the channels of one operation ------------------------------------ *
 *
 * An index may hold the entries of several channels at once (the list a
 * root keeps, see below), so every entry carries the channel it came from
 * and nothing downstream has to be told which one to fetch an object from.
 * The channels of the running operation are kept here, in the order they
 * were read; `ch` is a place in this table. */
#define PKG_MAX_CHANNELS 24
static char *chans[PKG_MAX_CHANNELS];
static size_t nchans;

static int chan_id(const char *url)
{
    size_t i;
    for (i = 0; i < nchans; i++)
        if (strcmp(chans[i], url) == 0)
            return (int)i;
    if (nchans >= PKG_MAX_CHANNELS)
        return -1;
    chans[nchans] = pkg_strdup(url);
    if (chans[nchans] == NULL)
        return -1;
    return (int)nchans++;
}

static const char *chan_of(const struct entry *e)
{
    return e->ch < nchans ? chans[e->ch] : "";
}

static void chans_clear(void)
{
    while (nchans > 0)
        free(chans[--nchans]);
}

/* The channels being read, for a sentence: one name, or all of them. */
static const char *chans_text(void)
{
    static char text[900];
    size_t i, at = 0;
    text[0] = '\0';
    for (i = 0; i < nchans && at + 80 < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", chans[i]);
    return text;
}

/* ---- channels over the network ---------------------------------------- *
 *
 * A channel may be a URL: http://host/pkg reads exactly like a directory
 * channel, file for file. Its files are fetched into a cache (pkg_cache_dir,
 * one directory per channel URL) the first time they are needed, and every
 * check applies to them as to local ones: the manifest against the index,
 * the signature, every file against the manifest. Files named by a digest
 * never change, so they are fetched once; the index and withdrawals can,
 * so they are fetched once per run. */
static int is_url(const char *ch)
{
    return ch != NULL && (strncmp(ch, "http://", 7) == 0 || strncmp(ch, "https://", 8) == 0);
}

static char net_err[400];           /* why the last fetch failed, for the refusal */
static char fetched_once[16][128];  /* mutable files already fetched in this run */
static size_t nfetched_once;

static char *chan_file(const char *channel, const char *rel)
{
    char *cache, sub[PKG_SHA256_HEXLEN + 1], url[2300], *dir, *local;
    size_t k, rl = strlen(rel);
    int mutable_file, again = 1, rc;

    if (!is_url(channel))
        return pkg_join(channel, rel);
    cache = pkg_cache_dir();
    if (cache == NULL) return NULL;
    pkg_sha256_hex((const unsigned char *)channel, strlen(channel), sub);
    sub[16] = '\0';                 /* one directory per channel URL */
    dir = pkg_join(cache, sub);
    free(cache);
    local = dir ? pkg_join(dir, rel) : NULL;
    free(dir);
    if (local == NULL) return NULL;
    mutable_file = strcmp(rel, "index") == 0
                   || (rl > 10 && strcmp(rel + rl - 10, ".withdrawn") == 0)
                   || (rl > 14 && strcmp(rel + rl - 14, ".withdrawn.sig") == 0);
    if (mutable_file) {
        for (k = 0; k < nfetched_once; k++)
            if (strcmp(fetched_once[k], rel) == 0) again = 0;
    } else if (pkg_fs_exists(local)) {
        again = 0;                  /* named by what it holds: a cached copy is the file */
    }
    if (!again)
        return local;
    snprintf(url, sizeof url, "%s%s%s", channel, channel[strlen(channel) - 1] == '/' ? "" : "/", rel);
    {
        /* the cache directory for this file */
        char *slash = strrchr(local, '/');
        if (slash) { *slash = '\0'; pkg_fs_mkdirs(local); *slash = '/'; }
    }
    rc = net_get_watched(url, local, net_err, sizeof net_err);
    tr("fetched %s: %s", url, rc == 0 ? "ok" : rc == 1 ? "not there" : net_err);
    if (rc == 1)
        pkg_fs_unlink(local);       /* not published there: no stale copy either */
    if (rc >= 0 && mutable_file && nfetched_once < 16)
        snprintf(fetched_once[nfetched_once++], sizeof fetched_once[0], "%s", rel);
    if (rc < 0 && !mutable_file)
        pkg_fs_unlink(local);
    return local;
}

static int read_index(const char *channel, struct index *ix)
{
    char *path;
    unsigned char *buf = NULL;
    size_t len = 0, at = 0;
    unsigned line = 0;
    int id = chan_id(channel);

    ix->e = NULL;
    ix->n = 0;
    net_err[0] = '\0';
    if (id < 0)
        return refuse_c(20, "no more than %d channels can be read at once",
                        PKG_MAX_CHANNELS);
    path = chan_file(channel, "index");
    if (path == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(path)) {
        free(path);
        if (is_url(channel) && net_err[0])
            return refuse_c(17, "cannot reach the channel %s: %s", channel, net_err);
        if (is_url(channel))
            return refuse_n(11, "report", "there is no channel at %s: it has no index", channel);
        return 0;                 /* a channel with nothing published yet */
    }
    if (pkg_fs_read(path, &buf, &len) != 0) {
        free(path);
        return refuse_c(17, "cannot read the channel index");
    }
    free(path);
    while (at < len) {
        const char *ls = (const char *)buf + at;
        const char *nl = (const char *)memchr(ls, '\n', len - at);
        size_t ll = nl ? (size_t)(nl - ls) : len - at;
        struct entry en;
        char tmp[256];
        struct entry *w;

        at += ll + 1u;
        line++;
        if (ll == 0u)
            continue;
        /* A hand-edited index may lack its last newline, or end its lines
         * with CR: both are read; what is written back is always clean. */
        if (ll >= sizeof tmp) {
            free(buf); free(ix->e);
            return refuse_c(12, "the channel index is malformed at line %u", line);
        }
        memcpy(tmp, ls, ll);
        tmp[ll] = '\0';
        if (sscanf(tmp, "%64s %63s %31s %64s", en.name, en.version, en.arch, en.digest) != 4
            || pkg_check_name(en.name) || pkg_check_version(en.version)
            || pkg_check_arch(en.arch) || strlen(en.digest) != PKG_SHA256_HEXLEN) {
            free(buf); free(ix->e);
            return refuse_c(12, "the channel index is malformed at line %u", line);
        }
        w = (struct entry *)realloc(ix->e, (ix->n + 1u) * sizeof *w);
        if (w == NULL) { free(buf); return refuse("out of memory"); }
        ix->e = w;
        en.withdrawn = 0;
        en.ch = (unsigned char)id;
        ix->e[ix->n++] = en;
    }
    free(buf);
    mark_withdrawn(channel, ix);
    return 0;
}

static int by_entry(const void *a, const void *b)
{
    const struct entry *x = (const struct entry *)a, *y = (const struct entry *)b;
    int c = strcmp(x->name, y->name);
    if (c == 0) c = pkg_version_cmp(x->version, y->version);
    return c ? c : strcmp(x->arch, y->arch);
}

static int write_index(const char *channel, struct index *ix)
{
    char *path = pkg_join(channel, "index");
    size_t i, cap = ix->n * 200u + 1u, len = 0;
    char *buf = (char *)malloc(cap);
    int rc;

    if (path == NULL || buf == NULL) { free(path); free(buf); return -1; }
    qsort(ix->e, ix->n, sizeof ix->e[0], by_entry);
    for (i = 0; i < ix->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s %s %s %s\n",
                                ix->e[i].name, ix->e[i].version, ix->e[i].arch, ix->e[i].digest);
    rc = pkg_fs_write_atomic(path, buf, len);
    free(path);
    free(buf);
    return rc;
}

/* ---- the channels a root keeps ---------------------------------------- *
 *
 * A machine that installs from the same places every day should not have to
 * type CHANNEL on every command. A root keeps its own list in
 * .pkg/channels, one channel per line in the order they were added, beside
 * .pkg/arch and .pkg/keys: the list belongs to the root, so an AROS system
 * keeps its own in SYS:.pkg and a test root keeps another. CHANNEL on the
 * line still means that channel and no other. */

struct chanlist {
    char  *v[PKG_MAX_CHANNELS];
    size_t n;
};

static void chanlist_free(struct chanlist *c)
{
    while (c->n > 0)
        free(c->v[--c->n]);
}

/* Read the list; a root without one has an empty list, which is not a
 * refusal. */
static int chanlist_read(const char *root, struct chanlist *c)
{
    char *p = pkg_join(root, ".pkg/channels");
    unsigned char *buf;
    size_t len, at = 0;

    c->n = 0;
    if (p == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(p)) { free(p); return 0; }
    if (pkg_fs_read(p, &buf, &len) != 0) {
        refuse_c(17, "cannot read the channel list %s", p);
        free(p);
        return 1;
    }
    free(p);
    while (at < len) {
        const char *ls = (const char *)buf + at;
        const char *nl = (const char *)memchr(ls, '\n', len - at);
        size_t ll = nl ? (size_t)(nl - ls) : len - at;
        char line[1200];
        at += ll + 1u;
        while (ll > 0 && (ls[ll - 1] == '\r' || ls[ll - 1] == ' '))
            ll--;
        if (ll == 0u)
            continue;
        if (ll >= sizeof line || c->n >= PKG_MAX_CHANNELS) {
            free(buf);
            chanlist_free(c);
            return refuse_c(12, "the channel list in %s is malformed, or holds more than %d "
                            "channels; edit %s/.pkg/channels, or remove it and add the channels "
                            "again", root, PKG_MAX_CHANNELS, root);
        }
        memcpy(line, ls, ll);
        line[ll] = '\0';
        c->v[c->n] = pkg_strdup(line);
        if (c->v[c->n] == NULL) { free(buf); chanlist_free(c); return refuse("out of memory"); }
        c->n++;
    }
    free(buf);
    return 0;
}

/* The channels of a list, for a sentence. */
static const char *chanlist_text(const struct chanlist *c)
{
    static char text[900];
    size_t i, at = 0;
    text[0] = '\0';
    for (i = 0; i < c->n && at + 80 < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s", at ? ", " : "", c->v[i]);
    return text;
}

static int chanlist_write(const char *root, const struct chanlist *c)
{
    char *dir = pkg_join(root, ".pkg"), *p = pkg_join(root, ".pkg/channels");
    char *buf;
    size_t i, cap = 8, len = 0;
    int rc;

    for (i = 0; i < c->n; i++)
        cap += strlen(c->v[i]) + 1u;
    buf = (char *)malloc(cap);
    if (dir == NULL || p == NULL || buf == NULL) { free(dir); free(p); free(buf); return -1; }
    pkg_fs_mkdirs(dir);
    free(dir);
    for (i = 0; i < c->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s\n", c->v[i]);
    rc = pkg_fs_write_atomic(p, buf, len);
    free(p);
    free(buf);
    return rc;
}

/* The root whose pinned keys govern a choice among several channels, set
 * when the channels are opened. */
static const char *pick_root;

/* The channels an operation reads from: CHANNEL when it was given, and then
 * that channel alone; else the root's list, in its order. The index that
 * comes back holds every channel's entries, each remembering where it came
 * from. */
static int open_channels(const struct pkg_options *a, struct index *ix)
{
    struct chanlist cl;
    size_t i;

    ix->e = NULL;
    ix->n = 0;
    pick_root = a->root;
    if (a->channel != NULL)
        return read_index(a->channel, ix);
    if (a->root == NULL)
        return refuse_c(20, "name the channel with CHANNEL <dir|url>");
    if (chanlist_read(a->root, &cl) != 0)
        return 1;
    if (cl.n == 0) {
        chanlist_free(&cl);
        return refuse_c(20, "name the channel with CHANNEL <dir|url>, or add it to this root "
                        "once and leave CHANNEL out from then on: "
                        "CHANNEL ADD <dir|url> ROOT %s", a->root);
    }
    for (i = 0; i < cl.n; i++) {
        struct index one;
        struct entry *w;
        if (!is_url(cl.v[i]) && !pkg_fs_is_dir(cl.v[i])) {
            free(ix->e);
            ix->e = NULL;
            ix->n = 0;
            chanlist_free(&cl);
            return refuse_c(11, "%s lists the channel %s, and there is no channel there: not "
                            "mounted, or moved. Nothing was checked or changed; "
                            "CHANNEL REMOVE %s ROOT %s takes it off the list",
                            a->root, cl.v[i], cl.v[i], a->root);
        }
        if (read_index(cl.v[i], &one) != 0) {
            free(ix->e);
            ix->e = NULL;
            ix->n = 0;
            chanlist_free(&cl);
            return 1;
        }
        if (one.n == 0) { free(one.e); continue; }
        w = (struct entry *)realloc(ix->e, (ix->n + one.n) * sizeof *w);
        if (w == NULL) {
            free(one.e); free(ix->e); ix->e = NULL; ix->n = 0;
            chanlist_free(&cl);
            return refuse("out of memory");
        }
        ix->e = w;
        memcpy(ix->e + ix->n, one.e, one.n * sizeof *one.e);
        ix->n += one.n;
        free(one.e);
    }
    tr("reading %lu channel%s listed in %s: %s", (unsigned long)cl.n, cl.n == 1 ? "" : "s",
       a->root, chans_text());
    chanlist_free(&cl);
    return 0;
}

static char *object_path(const char *channel, const char *digest, const char *ext)
{
    char rel[128];
    snprintf(rel, sizeof rel, "objects/%s.%s", digest, ext);
    return chan_file(channel, rel);
}

/* Where installers find the archive a Source line names: archives/<name>
 * in the channel. */
static char *archive_path(const char *channel, const char *name)
{
    char rel[1100];
    if (strchr(name, '/') != NULL || name[0] == '.')
        return NULL;
    snprintf(rel, sizeof rel, "archives/%s", name);
    return chan_file(channel, rel);
}

/* The kind and dependencies of the highest version of a package published in
 * the channel, for the CPU given when it has one there. */
/* The manifest of the highest version of a package published in the
 * channel PUBLISH writes to, for the CPU given when it has one there. 1 and
 * `em` filled, or 0. */
static int last_published(const char *name, const char *arch, struct pkg_manifest *em)
{
    const struct entry *best = NULL;
    size_t o;
    int pass;
    char *mp, err[200];
    unsigned char *buf;
    size_t len;

    if (inherit_ix == NULL)
        return 0;
    for (pass = 0; pass < 2 && best == NULL; pass++)
        for (o = 0; o < inherit_ix->n; o++) {
            const struct entry *e = &inherit_ix->e[o];
            if (strcmp(e->name, name) != 0 || (pass == 0 && strcmp(e->arch, arch) != 0))
                continue;
            if (best == NULL || pkg_version_cmp(e->version, best->version) > 0)
                best = e;
        }
    if (best == NULL)
        return 0;
    mp = object_path(inherit_channel, best->digest, "manifest");
    if (mp == NULL || pkg_fs_read(mp, &buf, &len) != 0) { free(mp); return 0; }
    free(mp);
    if (pkg_manifest_parse((const char *)buf, len, em, err, sizeof err) != 0) { free(buf); return 0; }
    free(buf);
    return 1;
}

static int inherited(const char *name, const char *arch, char *kind, size_t kl,
                     char *deps, size_t dl, char *from, size_t fl, char *conf, size_t cl)
{
    size_t d, at = 0;
    struct pkg_manifest em;

    if (!last_published(name, arch, &em))
        return 0;
    snprintf(kind, kl, "%s", em.kind);
    deps[0] = '\0';
    for (d = 0; d < em.ndeps && at < dl; d++)
        at += (size_t)snprintf(deps + at, dl - at, "%s%s%s%s", d ? ", " : "", em.deps[d].name,
                               em.deps[d].min ? " >= " : "", em.deps[d].min ? em.deps[d].min : "");
    if (em.ndeps == 0)
        snprintf(deps, dl, "none");
    snprintf(from, fl, "%s %s", em.name, em.version);
    conf[0] = '\0';
    for (d = 0, at = 0; d < em.nfiles; d++)
        if (em.files[d].config && at + strlen(em.files[d].path) + 2 < cl)
            at += (size_t)snprintf(conf + at, cl - at, "%s%s", at ? "," : "", em.files[d].path);
    tr("inherited from %s %s: kind %s, depends %s, config %s", em.name, em.version, kind, deps,
       conf[0] ? conf : "none");
    pkg_manifest_free(&em);
    return 1;
}

/* ---- the machine a root is for ------------------------------------------ *
 *
 * A channel may hold one version of a program for several CPUs. The
 * operation picks among the entries built for the root's machine, and
 * `generic` ones. That machine is ARCH when given; else what the root
 * recorded at its first install of a CPU-specific package; else, when Pkg
 * runs on AROS, its own CPU; else unknown, and then a package offered for
 * more than one CPU is refused until ARCH says which. */

static const char *target_arch;    /* NULL: any */
static char root_arch[32];

static const char *native_arch(void)
{
#if defined(__AROS__)
# if defined(__x86_64__)
    return "x86_64";
# elif defined(__aarch64__)
    return "aarch64";
# elif defined(__i386__)
    return "i386";
# elif defined(__arm__)
    return "arm";
# elif defined(__powerpc__) || defined(__PPC__)
    return "ppc";
# elif defined(__mc68000__) || defined(__m68k__)
    return "m68k";
# endif
#endif
    return NULL;
}

static int arch_matches(const struct entry *e)
{
    return target_arch == NULL || strcmp(e->arch, target_arch) == 0
        || strcmp(e->arch, "generic") == 0;
}

static int resolve_arch(const struct pkg_options *a)
{
    char *p = pkg_join(a->root, ".pkg/arch");
    unsigned char *buf;
    size_t len;
    root_arch[0] = '\0';
    target_arch = NULL;
    if (p != NULL && pkg_fs_exists(p) && pkg_fs_read(p, &buf, &len) == 0) {
        sscanf((const char *)buf, "%31s", root_arch);
        free(buf);
    }
    free(p);
    if (a->arch != NULL) {
        const char *why = pkg_check_arch(a->arch);
        if (why != NULL)
            return refuse_c(20, "ARCH \"%s\": %s", a->arch, why);
        if (root_arch[0] && strcmp(root_arch, a->arch) != 0)
            return refuse_c(20, "%s is a root for %s machines; ARCH %s names another", a->root,
                            root_arch, a->arch);
        target_arch = a->arch;
        tr("for %s machines: ARCH says so", target_arch);
    } else if (root_arch[0]) {
        target_arch = root_arch;
        tr("for %s machines: the root recorded it", target_arch);
    } else {
        target_arch = native_arch();
        tr("for %s", target_arch ? "this machine's CPU" : "any CPU: none is known yet");
    }
    return 0;
}

/* Record the root's machine once a CPU-specific package is installed. */
static void record_arch(const char *root, const char *arch)
{
    char *p;
    if (root_arch[0] || strcmp(arch, "generic") == 0)
        return;
    p = pkg_join(root, ".pkg/arch");
    if (p != NULL) {
        char line[40];
        int n = snprintf(line, sizeof line, "%s\n", arch);
        if (pkg_fs_write_atomic(p, line, (size_t)n) == 0)
            tr("%s is now a root for %s machines", root, arch);
        free(p);
    }
}

/* With no machine known, a package offered for more than one CPU cannot be
 * chosen for. 1 when that is the case, the CPUs listed. */
static int arch_ambiguous(const struct index *ix, const char *name, char *list, size_t len)
{
    size_t i, j, at = 0;
    int n = 0;
    list[0] = '\0';
    if (target_arch != NULL)
        return 0;
    for (i = 0; i < ix->n; i++) {
        int seen = 0;
        if (strcmp(ix->e[i].name, name) != 0 || strcmp(ix->e[i].arch, "generic") == 0)
            continue;
        for (j = 0; j < i; j++)
            if (strcmp(ix->e[j].name, name) == 0 && strcmp(ix->e[j].arch, ix->e[i].arch) == 0)
                seen = 1;
        if (seen)
            continue;
        n++;
        if (at + 40 < len)
            at += (size_t)snprintf(list + at, len - at, "%s%s", at ? ", " : "", ix->e[i].arch);
    }
    return n > 1;
}

static char *root_path(const char *root, const char *dir, const char *name);
static int claimed_signer(const char *channel, const char *digest, char out[65]);

/* The key this root pinned for a package, if any. */
static int pinned_key(const char *root, const char *name, char out[65])
{
    char *p = root ? root_path(root, "keys", name) : NULL;
    unsigned char *buf;
    size_t len;
    int got = 0;
    if (p != NULL && pkg_fs_read(p, &buf, &len) == 0) {
        if (len >= 64u) {
            memcpy(out, buf, 64);
            out[64] = '\0';
            got = 1;
        }
        free(buf);
    }
    free(p);
    return got;
}

/* Pick the entry for name: EXACT when version is given, else the highest.
 *
 * With several channels listed, each is asked in turn and the answers are
 * weighed together. Two channels offering the same package under different
 * keys is the case adding a channel must never settle by itself: it is
 * refused, naming both, unless this root has already pinned a key for that
 * name, in which case only the channels whose chosen version carries that
 * key count. Among what is left the newest version wins, and list order
 * breaks a tie. */
static int pick_quiet;     /* the version was already chosen and traced */
static int pick_refused;   /* pick refused; the caller must not say "not found" */
static char pick_why[2400];/* the refusal's reason, for a caller that reports per package */

static const struct entry *pick(const struct index *ix, const char *name, const char *version)
{
    const struct entry *best[PKG_MAX_CHANNELS];
    char signer[PKG_MAX_CHANNELS][65];
    const struct entry *p = NULL;
    size_t i, c, ncand = 0;
    char pin[65];
    int have_pin;

    pick_refused = 0;
    pick_why[0] = '\0';
    for (c = 0; c < PKG_MAX_CHANNELS; c++) {
        best[c] = NULL;
        signer[c][0] = '\0';
    }
    for (i = 0; i < ix->n; i++) {
        const struct entry *e = &ix->e[i];
        const struct entry **b;
        if (strcmp(e->name, name) != 0 || !arch_matches(e) || e->ch >= PKG_MAX_CHANNELS)
            continue;
        b = &best[e->ch];
        if (version) {
            if (pkg_version_cmp(e->version, version) == 0)
                *b = e;
        } else if (e->withdrawn) {
            tr("skipping %s %s: withdrawn by its publisher", e->name, e->version);
        } else if (*b == NULL || pkg_version_cmp(e->version, (*b)->version) > 0) {
            *b = e;
        }
    }
    for (c = 0; c < PKG_MAX_CHANNELS; c++)
        if (best[c] != NULL)
            ncand++;
    if (ncand > 1) {
        /* Only then is a signature file read: one channel is the usual case
         * and must cost nothing. */
        have_pin = pinned_key(pick_root, name, pin);
        for (c = 0; c < PKG_MAX_CHANNELS; c++)
            if (best[c] != NULL)
                claimed_signer(chans[c], best[c]->digest, signer[c]);
        if (have_pin) {
            for (c = 0; c < PKG_MAX_CHANNELS; c++)
                if (best[c] != NULL && strcmp(signer[c], pin) != 0) {
                    tr("%s %s in %s is signed by %s, not by the key this root pinned: not counted",
                       name, best[c]->version, chans[c], signer[c][0] ? signer[c] : "no one");
                    best[c] = NULL;
                }
        } else {
            size_t first = PKG_MAX_CHANNELS;
            for (c = 0; c < PKG_MAX_CHANNELS; c++) {
                if (best[c] == NULL)
                    continue;
                if (first == PKG_MAX_CHANNELS) { first = c; continue; }
                if (strcmp(signer[first], signer[c]) != 0) {
                    pick_refused = 1;
                    snprintf(pick_why, sizeof pick_why,
                             "%s is offered by two channels under different keys, and this root "
                             "has pinned no key for it yet.\n"
                             "  %s %s in %s, signed by %s\n"
                             "  %s %s in %s, signed by %s\n"
                             "Nothing was changed. Taking either one would decide which of them "
                             "is the publisher, and adding a channel is never a way to replace "
                             "someone's package: only whoever requested this can say which key "
                             "is right, by asking the publisher by another route than these "
                             "channels. Install from that channel alone once, with CHANNEL, and "
                             "the root pins its key from then on",
                             name,
                             name, best[first]->version, chans[first],
                             signer[first][0] ? signer[first] : "no one",
                             name, best[c]->version, chans[c],
                             signer[c][0] ? signer[c] : "no one");
                    refuse_n(14, "ask-requester", "%s", pick_why);
                    return NULL;
                }
            }
        }
    }
    for (c = 0; c < PKG_MAX_CHANNELS; c++) {
        if (best[c] == NULL)
            continue;
        if (p == NULL || pkg_version_cmp(best[c]->version, p->version) > 0)
            p = best[c];        /* newest wins; list order breaks a tie */
    }
    if (pick_quiet)
        ;
    else if (p != NULL)
        tr("picked %s %s from %s: %s", p->name, p->version, chan_of(p),
           version ? "the version asked for" : "the highest offered");
    else
        tr("no channel offers %s%s%s", name, version ? " " : "", version ? version : "");
    return p;
}

/* Names in the channel close to `name`: one contains the other, or they
 * agree up to the first '.', '-' or '_' ("identify" and "identify.library"). */
/* Edits (insertions, deletions, substitutions) between two short names. */
static size_t edits(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i, j, row[66], diag, up;
    if (la > 64 || lb > 64)
        return 99;
    for (j = 0; j <= lb; j++) row[j] = j;
    for (i = 1; i <= la; i++) {
        diag = row[0];
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t best;
            up = row[j];
            best = diag + (a[i - 1] != b[j - 1]);
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diag = up;
            row[j] = best;
        }
    }
    return row[lb];
}

static int close_name(const char *a, const char *b)
{
    size_t la = strcspn(a, ".-_"), lb = strcspn(b, ".-_");
    size_t shorter = strlen(a) < strlen(b) ? strlen(a) : strlen(b);
    if (strstr(a, b) != NULL || strstr(b, a) != NULL)
        return 1;
    /* Two edits in a short name is another name, not a typo: "hap" is two
     * edits from "hcat" and one from "happ". */
    if (edits(a, b) <= (shorter <= 4 ? 1 : 2))
        return 1;
    return la == lb && la >= 3 && strncmp(a, b, la) == 0;
}

static void say_not_found(const struct index *ix, const char *name, const char *version,
                          const char *channel)
{
    size_t i, j, at = 0;
    char offered[512], other[200];
    size_t ot = 0;
    offered[0] = other[0] = '\0';
    for (i = 0; i < ix->n; i++) {
        if (strcmp(ix->e[i].name, name) != 0)
            continue;
        if (arch_matches(&ix->e[i])) {
            if (at + 72 < sizeof offered)
                at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                       at ? ", " : "; versions offered: ", ix->e[i].version);
        } else if (version != NULL && pkg_version_cmp(ix->e[i].version, version) == 0
                   && ot + 40 < sizeof other) {
            ot += (size_t)snprintf(other + ot, sizeof other - ot, "%s%s", ot ? ", " : "",
                                   ix->e[i].arch);
        }
    }
    if (ot > 0) {
        refuse_n(PKGRC_NOTFOUND, "report", "%s %s is published for %s only, not for %s machines%s",
                 name, version, other, target_arch, offered);
        return;
    }
    if (at == 0) {
        for (i = 0; i < ix->n && at + 72 < sizeof offered; i++) {
            int seen = 0;
            if (!close_name(ix->e[i].name, name))
                continue;
            for (j = 0; j < i; j++)
                if (strcmp(ix->e[j].name, ix->e[i].name) == 0) seen = 1;
            if (!seen) {
                at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                       at ? ", " : "; did you mean ", ix->e[i].name);
                rec_item("suggest", ix->e[i].name, "name", ix->e[i].name, NULL);
            }
        }
    }
    refuse_c(PKGRC_NOTFOUND, "%s%s%s is not in the channel %s%s", name,
             version ? " " : "", version ? version : "", channel, offered);
}

/* A fetched, verified package.
 *
 * The index names a version by the digest of its MANIFEST, never of its
 * payload. The manifest is the signed object and it names the payload, so the
 * chain runs index -> manifest -> payload, each link checked. Payloads stay
 * content-addressed and may be shared: two versions with identical bytes, or
 * two packages shipping the same files, keep separate manifests and signatures
 * over one payload object. Keying manifests by the payload, as the first layout
 * did, let the second of two such publishes overwrite the first's manifest and
 * signature; a diagnostic run with two identical handler builds found it. */
struct fetched {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    unsigned char *mtext;
    size_t         mlen;
    char           signer[65];
    char           digest[PKG_SHA256_HEXLEN + 1];   /* the manifest's */
};

static void fetched_free(struct fetched *f)
{
    pkg_manifest_free(&f->m);
    free(f->pkg);
    free(f->mtext);
}

/* Where the files of a Source package are read from, and where that is
 * kept: said once a command, with the path in full, since a person looking
 * for a 600 MB download or for a drawer to delete needs to know where it is. */
static const char *opt_unpacked;   /* UNPACKED <dir>, for this call */
static const char *from_kind;      /* where locate_archive found the archive */
static int told_source;

static void from_note(const char *kind, const char *path)
{
    char *cache;
    if (told_source)
        return;
    told_source = 1;
    cache = pkg_cache_dir();
    if (machine) {
        kv("archive-from", "%s %s", kind, path);
        if (cache != NULL) kv("cache", "%s", cache);
    } else {
        say_kind(PKG_LINE_NOTE, "%s\n", "%s %s",
                 strcmp(kind, "map") == 0 ? "reading only the blocks its files lie in, out of" :
                 strcmp(kind, "cache") == 0 ? "reading the archive already in the cache at" :
                 strcmp(kind, "unpacked") == 0 ? "reading the unpacked archive at" :
                 strcmp(kind, "download") == 0 ? "the archive is kept at" :
                 "reading the archive of the channel at", path);
        if (cache != NULL)
            say_kind(PKG_LINE_NOTE, "%s\n", "the cache is %s; PKG_CACHE names another place for it",
                     cache);
    }
    free(cache);
}

struct arch_fetch {
    const struct pkg_manifest *m;
    const char    *prefix;
    unsigned char **data;           /* per manifest file */
    size_t        *len, *cap;
    long           cur;
    long           oversize;        /* a file longer than its manifest says, or -1 */
};

static int af_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct arch_fetch *af = (struct arch_fetch *)ctx;
    size_t pl = strlen(af->prefix), i;
    const char *rel = e->path;
    if (e->is_dir) return 0;
    if (pl) {
        if (strncmp(e->path, af->prefix, pl) != 0 || e->path[pl] != '/') return 0;
        rel = e->path + pl + 1;
    }
    for (i = 0; i < af->m->nfiles; i++)
        if (af->data[i] == NULL && strcmp(af->m->files[i].path, rel) == 0) {
            af->cur = (long)i;
            return 1;
        }
    return 0;
}

static int af_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct arch_fetch *af = (struct arch_fetch *)ctx;
    size_t i = (size_t)af->cur;
    (void)e;
    /* Never hold more than the signed manifest says the file is: an archive
     * claiming more is refused as soon as it passes that size. */
    if (af->len[i] + len > af->m->files[i].size) {
        af->oversize = (long)i;
        return -1;
    }
    if (af->len[i] + len + 1 > af->cap[i]) {
        size_t nc = af->cap[i] ? af->cap[i] : 4096;
        unsigned char *g;
        while (nc < af->len[i] + len + 1) nc *= 2;
        g = (unsigned char *)realloc(af->data[i], nc);
        if (g == NULL) return -1;
        af->data[i] = g;
        af->cap[i] = nc;
    }
    if (len) memcpy(af->data[i] + af->len[i], buf, len);
    af->len[i] += len;
    return 0;
}

/* A package whose files are in someone else's archive: take them out of it
 * and assemble the container the rest of the install reads, so every file
 * is checked against the signed manifest the same way. */
/* Where to read a Source archive. A local channel's own copy first, as its
 * publisher left it. Then, when the manifest says where the archive is
 * published upstream: a copy downloaded before, in the cache under its
 * SHA-256; else a download from that URL, kept only when its size and
 * SHA-256 are the ones signed. Last, the channel's copy over the network.
 * Each file is checked against its own digest when it is read, whatever the
 * archive's origin. NULL with the reason given. */
static char *locate_archive(const char *channel, const struct pkg_manifest *m, const char *an,
                            const char *what)
{
    char *ap, *cache, *dir, *dest, hex[PKG_SHA256_HEXLEN + 1], rel[1100];
    unsigned long long size = 0;
    int rc;

    if (!is_url(channel) || m->archive_sha == NULL) {
        ap = archive_path(channel, an);
        if (ap != NULL && pkg_fs_exists(ap)) {
            from_kind = "channel";
            return ap;
        }
        if (m->archive_sha == NULL) {
            refuse_c(11, "%s comes from the archive %s, which the channel does not have (expected "
                     "at %s)", what, an, ap ? ap : "archives/");
            free(ap);
            return NULL;
        }
        free(ap);
    }
    cache = pkg_cache_dir();
    if (cache == NULL) { refuse("out of memory"); return NULL; }
    snprintf(rel, sizeof rel, "upstream/%s", m->archive_sha);
    dir = pkg_join(cache, rel);
    free(cache);
    dest = dir ? pkg_join(dir, an) : NULL;
    if (dest == NULL) { free(dir); refuse("out of memory"); return NULL; }
    if (pkg_fs_exists(dest)) {
        tr("%s: the archive %s is in the cache", what, an);
        from_kind = "cache";
        free(dir);
        return dest;
    }
    pkg_fs_mkdirs(dir);
    free(dir);
    if (!machine)
        say_kind(PKG_LINE_NOTE, "%s\n", "downloading %s (%llu MB) from %s, once for every package it holds", an,
            (m->archive_size + 524288ull) / 1048576ull, m->archive_url);
    rc = net_get_watched(m->archive_url, dest, net_err, sizeof net_err);
    if (rc == 0 && file_digest(dest, hex, &size) == 0
        && size == m->archive_size && strcmp(hex, m->archive_sha) == 0) {
        tr("%s: downloaded %s, %llu bytes, SHA-256 as signed", what, m->archive_url, size);
        from_kind = "download";
        return dest;
    }
    if (rc == 0) {
        pkg_fs_unlink(dest);
        refuse_c(12, "the archive downloaded from %s is not the one %s was signed with: %llu bytes "
                 "and SHA-256 %s, where the manifest says %llu and %s. It was deleted; nothing was "
                 "installed", m->archive_url, what, size, hex, m->archive_size, m->archive_sha);
        free(dest);
        return NULL;
    }
    pkg_fs_unlink(dest);
    tr("%s: %s: %s", what, m->archive_url, rc == 1 ? "not there" : net_err);
    if (is_url(channel)) {
        /* the channel may carry a copy of its own */
        ap = archive_path(channel, an);
        if (ap != NULL && pkg_fs_exists(ap)) { free(dest); from_kind = "channel"; return ap; }
        free(ap);
    }
    refuse_c(rc == 1 ? 11 : 17, "%s comes from the archive %s, which could not be downloaded from %s: %s",
             what, an, m->archive_url, rc == 1 ? "it is not there any more" : net_err);
    free(dest);
    return NULL;
}

/* The block map of an archive, kept beside it as <archive>.pkgmap. Its
 * header ties it to the archive it was made from: the size and time on this
 * machine, and the SHA-256 the signed manifest gives the archive. A map that
 * does not answer to all three is not read, and one a read cannot use is
 * thrown away and made again. */
static char *map_file(const char *archive)
{
    size_t n = strlen(archive) + 8;
    char *p = (char *)malloc(n);
    if (p != NULL) snprintf(p, n, "%s.pkgmap", archive);
    return p;
}

static int map_head(const char *archive, const struct pkg_manifest *m, char *head, size_t hl)
{
    struct pkg_fs_id id;
    if (pkg_fs_identity(archive, &id) != 0 || !id.exists)
        return -1;
    snprintf(head, hl, "pkgmap 1 %llu %lld %s\n", id.size, id.mtime_s,
             m->archive_sha ? m->archive_sha : "-");
    return 0;
}

static char *map_read(const char *archive, const struct pkg_manifest *m)
{
    char head[160], *mp = map_file(archive), *text = NULL;
    unsigned char *buf = NULL;
    size_t len = 0, hl;
    if (mp == NULL || map_head(archive, m, head, sizeof head) != 0) { free(mp); return NULL; }
    hl = strlen(head);
    if (pkg_fs_read(mp, &buf, &len) == 0 && len > hl && memcmp(buf, head, hl) == 0) {
        text = (char *)malloc(len - hl + 1);
        if (text != NULL) { memcpy(text, buf + hl, len - hl); text[len - hl] = '\0'; }
    }
    free(buf);
    free(mp);
    return text;
}

static void map_write(const char *archive, const struct pkg_manifest *m, const char *text)
{
    char head[160], *mp = map_file(archive), *all;
    size_t hl, tl;
    if (mp == NULL || text == NULL || map_head(archive, m, head, sizeof head) != 0) { free(mp); return; }
    hl = strlen(head);
    tl = strlen(text);
    all = (char *)malloc(hl + tl);
    if (all != NULL) {
        memcpy(all, head, hl);
        memcpy(all + hl, text, tl);
        if (pkg_fs_write_atomic(mp, all, hl + tl) != 0)
            tr("the block map could not be kept at %s; the archive is read whole every time", mp);
        else
            tr("wrote the block map %s, %lu bytes", mp, (unsigned long)(hl + tl));
        free(all);
    }
    free(mp);
}

static void map_drop(const char *archive)
{
    char *mp = map_file(archive);
    if (mp != NULL) { pkg_fs_unlink(mp); free(mp); }
}

/* What the fast path guarantees: every file it took out weighs and hashes
 * as the signed manifest says. Anything else and the map is wrong, not the
 * archive, so the archive is read whole instead. */
static int af_as_signed(const struct arch_fetch *af)
{
    size_t i;
    for (i = 0; i < af->m->nfiles; i++) {
        char hex[PKG_SHA256_HEXLEN + 1];
        if (af->data[i] == NULL || (unsigned long long)af->len[i] != af->m->files[i].size)
            return 0;
        pkg_sha256_hex(af->data[i], af->len[i], hex);
        if (strcmp(hex, af->m->files[i].digest) != 0)
            return 0;
    }
    return af->m->nfiles > 0;
}

static void af_clear(struct arch_fetch *af, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { free(af->data[i]); af->data[i] = NULL; af->len[i] = af->cap[i] = 0; }
    af->cur = 0;
    af->oversize = -1;
}

/* A person may unpack the archive themselves, with tar, and have Pkg read
 * the drawer instead. Every file is weighed and hashed against the signed
 * manifest before anything is written, exactly as from the archive. */
static int fetch_from_dir(const char *dir, struct fetched *f, const char *prefix, const char *what)
{
    struct pkg_writer *w = pkg_writer_new();
    size_t i;
    int rc = 1;

    if (w == NULL) { refuse("out of memory"); return 1; }
    for (i = 0; i < f->m.nfiles; i++) {
        char rel[2200], hex[PKG_SHA256_HEXLEN + 1], *full;
        unsigned char *buf = NULL;
        size_t len = 0;
        snprintf(rel, sizeof rel, "%s%s%s", prefix, prefix[0] ? "/" : "", f->m.files[i].path);
        full = pkg_join(dir, rel);
        if (full == NULL) { refuse("out of memory"); goto done; }
        if (pkg_fs_read(full, &buf, &len) != 0) {
            refuse_c(11, "the unpacked archive at %s lacks %s, which %s's signed manifest lists; "
                     "unpack the archive again, or leave UNPACKED off and let pkg read the "
                     "archive. Nothing was installed", dir, rel, what);
            free(full);
            goto done;
        }
        free(full);
        pkg_sha256_hex(buf, len, hex);
        if ((unsigned long long)len != f->m.files[i].size
            || strcmp(hex, f->m.files[i].digest) != 0) {
            refuse_c(12, "%s in the unpacked archive at %s is not the file %s was signed with: "
                     "%lu bytes and SHA-256 %s, where the manifest says %llu and %s. Nothing was "
                     "installed", rel, dir, what, (unsigned long)len, hex, f->m.files[i].size,
                     f->m.files[i].digest);
            free(buf);
            goto done;
        }
        if (pkg_writer_add(w, f->m.files[i].path, buf, len) != PKG_OK) {
            free(buf);
            refuse("out of memory");
            goto done;
        }
        free(buf);
    }
    if (pkg_writer_finish(w, &f->pkg, &f->pkg_len) != PKG_OK) { refuse("out of memory"); goto done; }
    tr("%s: %lu files taken from the unpacked archive at %s", what, (unsigned long)f->m.nfiles, dir);
    rc = 0;
done:
    pkg_writer_free(w);
    return rc;
}

/* The drawer an unpacked archive was put in: UNPACKED on the line, else the
 * one beside the cached archive, so it need not be named again. NULL when
 * there is none. */
static char *unpacked_dir(const struct pkg_manifest *m, const char *an)
{
    char *cache, *dir, rel[1200];
    if (opt_unpacked != NULL)
        return pkg_join(opt_unpacked, "");
    if (m->archive_sha == NULL)
        return NULL;
    cache = pkg_cache_dir();
    if (cache == NULL) return NULL;
    snprintf(rel, sizeof rel, "upstream/%s/%s.d", m->archive_sha, an);
    dir = pkg_join(cache, rel);
    free(cache);
    if (dir != NULL && pkg_fs_is_dir(dir))
        return dir;
    free(dir);
    return NULL;
}

static int fetch_from_archive(const char *channel, struct fetched *f, const char *what)
{
    char an[1024], prefix[1024], err[300];
    char *ap, *ud, *map = NULL;
    struct arch_fetch af;
    struct pkg_writer *w = NULL;
    size_t i, n = f->m.nfiles ? f->m.nfiles : 1;
    int rc = 1, used_map = 0;

    pkg_archive_split(f->m.source, an, sizeof an, prefix, sizeof prefix);
    ud = unpacked_dir(&f->m, an);
    if (ud != NULL) {
        from_note("unpacked", ud);
        tr("%s: reading its files out of the unpacked archive at %s", what, ud);
        rc = fetch_from_dir(ud, f, prefix, what);
        free(ud);
        return rc;
    }
    from_kind = "channel";
    ap = locate_archive(channel, &f->m, an, what);
    if (ap == NULL)
        return 1;
    memset(&af, 0, sizeof af);
    af.m = &f->m;
    af.prefix = prefix;
    af.oversize = -1;
    af.data = (unsigned char **)calloc(n, sizeof *af.data);
    af.len = (size_t *)calloc(n, sizeof *af.len);
    af.cap = (size_t *)calloc(n, sizeof *af.cap);
    if (af.data == NULL || af.len == NULL || af.cap == NULL) { refuse("out of memory"); goto done; }
    map = map_read(ap, &f->m);
    if (map != NULL) {
        tr("%s: %s has a block map; reading only the blocks its files lie in", what, ap);
        if (pkg_archive_read_mapped(ap, map, af_want, af_data, &af, err, sizeof err) == 0
            && af_as_signed(&af)) {
            used_map = 1;
        } else {
            tr("%s: the block map of %s did not serve this read; reading the archive whole and "
               "writing it again", what, ap);
            af_clear(&af, n);
            map_drop(ap);
        }
        free(map);
        map = NULL;
    }
    from_note(used_map ? "map" : from_kind, ap);
    if (!used_map) {
        tr("%s: reading its files out of %s", what, ap);
        if (pkg_archive_walk_map(ap, af_want, af_data, &af, &map, err, sizeof err) != 0) {
            if (af.oversize >= 0)
                refuse_c(12, "the archive %s holds a %s longer than the %llu bytes %s's signed manifest "
                         "gives it; it was not read further. Nothing was installed", ap,
                         f->m.files[af.oversize].path, f->m.files[af.oversize].size, what);
            else
                refuse_c(12, "the archive %s is refused: %s. Nothing was installed", ap, err[0] ? err : "unreadable");
            goto done;
        }
        map_write(ap, &f->m, map);
    }
    w = pkg_writer_new();
    if (w == NULL) { refuse("out of memory"); goto done; }
    for (i = 0; i < f->m.nfiles; i++) {
        if (af.data[i] == NULL) {
            refuse_c(12, "the archive %s lacks %s/%s, which %s's signed manifest lists. Nothing was "
                     "installed", ap, prefix, f->m.files[i].path, what);
            goto done;
        }
        if (pkg_writer_add(w, f->m.files[i].path, af.data[i], af.len[i]) != PKG_OK) {
            refuse("out of memory");
            goto done;
        }
    }
    if (pkg_writer_finish(w, &f->pkg, &f->pkg_len) != PKG_OK) { refuse("out of memory"); goto done; }
    tr("%s: %lu files taken from %s", what, (unsigned long)f->m.nfiles, an);
    rc = 0;
done:
    free(map);
    if (w) pkg_writer_free(w);
    for (i = 0; af.data && i < n; i++) free(af.data[i]);
    free(af.data); free(af.len); free(af.cap);
    free(ap);
    return rc;
}

static int show_defers_archives;    /* set by SHOW, which checks archives in one pass each */

static int fetch(const char *channel, const struct entry *e, struct fetched *f)
{
    char *mo = object_path(channel, e->digest, "manifest");
    char *so = object_path(channel, e->digest, "sig");
    char *po = NULL;
    char hex[PKG_SHA256_HEXLEN + 1], err[300], what[160];
    int rc = 1;

    memset(f, 0, sizeof *f);
    pkg_manifest_init(&f->m);
    snprintf(what, sizeof what, "%s %s", e->name, e->version);
    memcpy(f->digest, e->digest, sizeof f->digest);
    if (mo == NULL || so == NULL) { refuse("out of memory"); goto out; }

    /* 1. The manifest the index names, byte for byte. */
    if (pkg_fs_read(mo, &f->mtext, &f->mlen) != 0) {
        refuse_c(11, "the channel lists %s but its manifest is missing", what);
        goto out;
    }
    pkg_sha256_hex(f->mtext, f->mlen, hex);
    if (strcmp(hex, e->digest) != 0) {
        refuse_c(12, "the manifest of %s does not match the channel index; expected %s, found %s. "
               "Nothing was installed", what, e->digest, hex);
        goto out;
    }
    tr("%s: manifest %s matches the index", what, e->digest);
    /* 2. Signed. */
    if (check_sig(so, f->mtext, f->mlen, f->signer, what) != 0)
        goto out;
    tr("%s: signature verifies, signer %s", what, f->signer);
    /* 3. And saying what the index says it is. */
    if (pkg_manifest_parse((const char *)f->mtext, f->mlen, &f->m, err, sizeof err) != 0) {
        refuse_c(12, "the manifest of %s is refused: %s", what, err);
        goto out;
    }
    {
        size_t k;
        for (k = 0; k < f->m.ignored.n; k++)
            tr("%s: ignored the key %s, which pkg does not know", what, f->m.ignored.v[k]);
    }
    if (strcmp(f->m.name, e->name) != 0 || pkg_version_cmp(f->m.version, e->version) != 0
        || (f->m.payload == NULL) == (f->m.source == NULL)) {
        refuse_c(12, "the manifest of %s disagrees with the channel index about what it is", what);
        goto out;
    }
    /* 4. The payload the signed manifest names, or its files in the archive
     *    it names, each checked against the manifest when it is placed. */
    if (f->m.source != NULL) {
        /* SHOW checks archives afterwards, each read once for all its entries */
        rc = show_defers_archives ? 0 : fetch_from_archive(channel, f, what);
        goto out;
    }
    po = object_path(channel, f->m.payload, "pkg");
    if (po == NULL) { refuse("out of memory"); goto out; }
    if (pkg_fs_read(po, &f->pkg, &f->pkg_len) != 0) {
        refuse_c(11, "the channel lists %s but its payload is missing", what);
        goto out;
    }
    pkg_sha256_hex(f->pkg, f->pkg_len, hex);
    if (strcmp(hex, f->m.payload) != 0) {
        refuse_c(12, "the payload of %s does not match its signed manifest; expected %s, found %s. "
               "Nothing was installed", what, f->m.payload, hex);
        goto out;
    }
    tr("%s: payload %s matches the signed manifest", what, hex);
    rc = 0;
out:
    free(po); free(mo); free(so);
    return rc;
}

/* ---- the root --------------------------------------------------------- */

static char *root_path(const char *root, const char *dir, const char *name)
{
    char rel[160];
    snprintf(rel, sizeof rel, ".pkg/%s/%s", dir, name);
    return pkg_join(root, rel);
}

static int load_installed(const char *root, const char *name, struct pkg_manifest *m,
                          int quiet)
{
    char *p = root_path(root, "db", name), err[300];
    unsigned char *buf;
    size_t len;
    int rc;

    pkg_manifest_init(m);
    if (p == NULL)
        return refuse("out of memory");
    if (pkg_fs_read(p, &buf, &len) != 0) {
        free(p);
        return quiet ? 1 : refuse_n(11, strcmp(verb_name, "upgrade") == 0 ? "use-install" : "check-name",
                                    "%s is not installed in %s", name, root);
    }
    free(p);
    rc = pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
    free(buf);
    if (rc != 0)
        return refuse_c(12, "the database entry for %s is damaged: %s", name, err);
    return 0;
}

/* The signer a channel entry's signature file claims, unverified: enough to
 * notice that two versions of one package name different publishers. */
static int claimed_signer(const char *channel, const char *digest, char out[65])
{
    char *so = object_path(channel, digest, "sig");
    unsigned char *buf;
    size_t len;
    int ok = 0;
    if (so != NULL && pkg_fs_read(so, &buf, &len) == 0) {
        ok = len < 512u && sscanf((const char *)buf, "Signer: %64s", out) == 1;
        free(buf);
    }
    free(so);
    return ok;
}

/* The other keys that sign versions of `name` in the channel, listed in
 * `others`. The count of them. */
static int other_signers(const struct index *ix, const char *name,
                         const char *signer, char *others, size_t olen)
{
    size_t i, at = 0;
    int n = 0;
    char claim[65], seen[8][65];
    others[0] = '\0';
    for (i = 0; i < ix->n; i++) {
        int j, dup = 0;
        if (strcmp(ix->e[i].name, name) != 0 || !claimed_signer(chan_of(&ix->e[i]), ix->e[i].digest, claim))
            continue;
        if (strcmp(claim, signer) == 0)
            continue;
        for (j = 0; j < n && j < 8; j++)
            if (strcmp(seen[j], claim) == 0) dup = 1;
        if (dup)
            continue;
        if (n < 8) snprintf(seen[n], sizeof seen[n], "%s", claim);
        n++;
        if (at + 90 < olen)
            at += (size_t)snprintf(others + at, olen - at, "%s%s (on %s)", at ? ", " : "",
                                   claim, ix->e[i].version);
    }
    return n;
}

/* The text a withdrawal signs: the entry it names, exactly. */
static int withdrawal_text(const struct entry *e, char *out, size_t len)
{
    return snprintf(out, len, "Withdrawn: %s %s %s\n", e->name, e->version, e->digest);
}

/* A withdrawal counts when its text names this entry and its signature
 * verifies with the key that signed the entry: only the publisher of a
 * version can withdraw it. */
static int withdrawal_valid(const char *channel, const struct entry *e)
{
    char *wo = object_path(channel, e->digest, "withdrawn");
    char *ws = object_path(channel, e->digest, "withdrawn.sig");
    char want[300], signer[65], entry_signer[65];
    unsigned char *buf = NULL;
    size_t len;
    int n = withdrawal_text(e, want, sizeof want), ok = 0, q = quiet, rc = refused_class;
    const char *nx = refused_next;

    if (wo != NULL && ws != NULL && pkg_fs_exists(wo) && pkg_fs_read(wo, &buf, &len) == 0
        && len == (size_t)n && memcmp(buf, want, len) == 0) {
        quiet = 1;
        ok = check_sig(ws, buf, len, signer, "") == 0
             && claimed_signer(channel, e->digest, entry_signer)
             && strcmp(signer, entry_signer) == 0;
        quiet = q;
        refused_class = rc;
        refused_next = nx;
        if (!ok)
            tr("a withdrawal of %s %s is present but not signed by its publisher: ignored",
               e->name, e->version);
    }
    free(buf); free(wo); free(ws);
    return ok;
}

static void mark_withdrawn(const char *channel, struct index *ix)
{
    size_t i;
    for (i = 0; i < ix->n; i++)
        ix->e[i].withdrawn = withdrawal_valid(channel, &ix->e[i]);
}

/* Withdraw a published version: it stays in the channel, as everything
 * published does, but INSTALL and UPGRADE no longer pick it, asking for it
 * by version is refused, and SHOW marks it. Signed by the key that signed
 * the version, so only its publisher can. */
static int cmd_withdraw(const struct pkg_options *a)
{
    struct index ix;
    struct key k;
    const struct entry *e = NULL;
    char text[300], signer[65], *wo = NULL, *ws = NULL;
    size_t i;
    int n, rc = 1;

    if (a->target == NULL)  return refuse_c(20, "name the package to withdraw");
    if (a->version == NULL) return refuse_c(20, "name the version with VERSION <v>: a withdrawal names one version");
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (is_url(a->channel))
        return refuse_c(20, "WITHDRAW writes into a channel on this machine; withdraw in the "
                        "directory you publish from, then PUSH it to %s", a->channel);
    if (load_key(a->sign, &k) != 0) return 1;
    if (read_index(a->channel, &ix) != 0) { memset(&k, 0, sizeof k); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->target) == 0 && pkg_version_cmp(ix.e[i].version, a->version) == 0
            && (a->arch == NULL || strcmp(ix.e[i].arch, a->arch) == 0)) {
            if (e != NULL && strcmp(e->arch, ix.e[i].arch) != 0) {
                refuse_c(20, "%s %s is published for several CPUs; say which with ARCH",
                         a->target, a->version);
                goto out;
            }
            e = &ix.e[i];
        }
    if (e == NULL) {
        say_not_found(&ix, a->target, a->version, a->channel);
        goto out;
    }
    if (e->withdrawn) {
        kv("result", "unchanged");
        kv("name", "%s", e->name);
        kv("version", "%s", e->version);
        if (!machine)
            say_result("%s %s is already withdrawn from %s", e->name, e->version, a->channel);
        rc = 0;
        goto out;
    }
    if (!claimed_signer(a->channel, e->digest, signer) || strcmp(signer, k.pkhex) != 0) {
        kv("signer", "%s", k.pkhex);
        refuse_n(14, "ask-requester", "%s %s was signed by %s, and only that key can withdraw it; "
                 "this key is %s. Nothing was changed", e->name, e->version,
                 signer, k.pkhex);
        goto out;
    }
    n = withdrawal_text(e, text, sizeof text);
    if (dryrun) {
        kv("result", "would-withdraw");
    } else {
        wo = object_path(a->channel, e->digest, "withdrawn");
        ws = object_path(a->channel, e->digest, "withdrawn.sig");
        if (wo == NULL || ws == NULL || pkg_fs_write_atomic(wo, text, (size_t)n) != 0
            || write_sig(ws, &k, (const unsigned char *)text, (size_t)n) != 0) {
            refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
            goto out;
        }
        kv("result", "withdrawn");
    }
    kv("name", "%s", e->name);
    kv("version", "%s", e->version);
    kv("channel", "%s", a->channel);
    if (!machine)
        say_result("%s %s %s from %s: it stays in the channel, and nothing installs it any more",
            dryrun ? "would withdraw" : "withdrew", e->name, e->version, a->channel);
    rc = 0;
out:
    memset(&k, 0, sizeof k);
    free(wo); free(ws);
    free(ix.e);
    return rc;
}

/* The key pinned for a package in this root, and the rule that governs it. */
static int check_pin(const char *root, const char *name, const char *signer,
                     const char *acceptkey)
{
    char *p = root_path(root, "keys", name);
    unsigned char *buf;
    size_t len;
    char pinned[65];

    if (p == NULL)
        return refuse("out of memory");
    if (pkg_fs_read(p, &buf, &len) != 0) {
        free(p);
        tr("no key pinned yet for %s in %s: %s will be pinned", name, root, signer);
        return 0;                     /* first use: pinned after a successful apply */
    }
    free(p);
    if (len < 64u) { free(buf); return refuse_c(12, "the pinned key for %s is damaged", name); }
    memcpy(pinned, buf, 64);
    pinned[64] = '\0';
    free(buf);
    if (strcmp(pinned, signer) == 0) {
        tr("signer of %s is the key pinned in %s", name, root);
        return 0;
    }
    if (acceptkey != NULL && strcmp(acceptkey, signer) == 0) {
        tr("signer of %s differs from the pinned key; accepted because acceptkey names it", name);
        kv("key-changed", "%s %s", pinned, signer);
        if (!machine)
        say_kind(PKG_LINE_DETAIL, "  %s\n", "key for %s changed by explicit ACCEPTKEY\n    was %s\n    now %s",
               name, pinned, signer);
        return 0;
    }
    kv("pinned", "%s", pinned);
    kv("signer", "%s", signer);
    return refuse_c(14, "%s is signed by a different key from the one pinned in %s.\n"
                  "  pinned %s\n  signer %s\n"
                  "Nothing was changed. Either the publisher changed keys or someone else "
                  "signed this; only whoever requested this can tell, by asking the publisher by another "
                  "route than this channel. If they confirm the new key, it is accepted with "
                  "ACCEPTKEY and the key in full", name, root, pinned, signer);
}

static int write_pin(const char *root, const char *name, const char *signer)
{
    char *p = root_path(root, "keys", name), line[80];
    int rc, n = snprintf(line, sizeof line, "%s\n", signer);
    if (p == NULL)
        return -1;
    rc = pkg_fs_write_atomic(p, line, (size_t)n);
    free(p);
    return rc;
}

/* 0 intact, 1 changed, 2 missing. */
static int file_state(const char *root, const char *path, const char *digest,
                      unsigned long long size)
{
    char *p = pkg_join(root, path), hex[PKG_SHA256_HEXLEN + 1];
    unsigned char *buf;
    size_t len;
    int rc;

    if (p == NULL || pkg_fs_read(p, &buf, &len) != 0) { free(p); return 2; }
    free(p);
    pkg_sha256_hex(buf, len, hex);
    rc = ((unsigned long long)len == size && strcmp(hex, digest) == 0) ? 0 : 1;
    free(buf);
    return rc;
}

static const struct pkg_file *find_file(const struct pkg_manifest *m, const char *path)
{
    size_t lo = 0, hi = m->nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2u;
        int c = strcmp(m->files[mid].path, path);
        if (c == 0) return &m->files[mid];
        if (c < 0) lo = mid + 1u; else hi = mid;
    }
    return NULL;
}

struct walk_ctx {
    const struct pkg_manifest *m;
    size_t i;
    char   err[400];
};

static int check_entry(const struct pkg_entry *e, void *ctx)
{
    struct walk_ctx *w = (struct walk_ctx *)ctx;
    char hex[PKG_SHA256_HEXLEN + 1];
    const struct pkg_file *f;

    if (w->i >= w->m->nfiles) {
        snprintf(w->err, sizeof w->err, "the container holds \"%s\", which the manifest does not list", e->path);
        return 1;
    }
    f = &w->m->files[w->i];
    if (strcmp(f->path, e->path) != 0) {
        snprintf(w->err, sizeof w->err, "the container holds \"%s\" where the manifest lists \"%s\"",
                 e->path, f->path);
        return 1;
    }
    pkg_sha256_hex(e->data, e->data_len, hex);
    if ((unsigned long long)e->data_len != f->size || strcmp(hex, f->digest) != 0) {
        snprintf(w->err, sizeof w->err, "\"%s\" does not match its manifest digest", e->path);
        return 1;
    }
    w->i++;
    return 0;
}

struct stage_ctx {
    size_t                     done, todo;  /* files written so far, of how many */
    const char                *staging;
    const struct pkg_manifest *m;
    const unsigned char       *keep;    /* files not to stage: already in place */
    char                       err[400];
};

static int stage_entry(const struct pkg_entry *e, void *ctx)
{
    struct stage_ctx *s = (struct stage_ctx *)ctx;
    char *p;
    if (s->keep != NULL) {
        const struct pkg_file *pf = find_file(s->m, e->path);
        if (pf != NULL && s->keep[pf - s->m->files] >= 2)
            return 0;           /* stays as it is: nothing to write */
    }
    progress("writing", s->done++, s->todo);
    p = pkg_join(s->staging, e->path);
    if (p == NULL || pkg_fs_write_new(p, e->data, e->data_len) != 0) {
        snprintf(s->err, sizeof s->err, "cannot stage \"%s\": %s", e->path, strerror(errno));
        free(p);
        return 1;
    }
    free(p);
    return 0;
}

/* Move a root from `old` (NULL for a fresh install) to the package in `f`.
 * Install, upgrade and rollback all come through here, so they share every
 * check. Refuses before touching anything when:
 *   - the container disagrees with its manifest in any entry;
 *   - a new path exists in the root and belongs to no previous version;
 *   - a path both versions own was edited by the user since install.
 * Files the old version owned and the new one drops are removed when
 * unchanged, and kept, with a note, when the user edited them. */
/* ---- Amiga attributes on the installed files -------------------------- */

/* Record one file's word and comment in its directory's .ameta, under the
 * directory lock with the identity check (ameta.md, Writing). A default
 * word and no comment remove the entry. 0, or -1 with a warning given. */
static int ameta_set(const char *root, const char *rel, unsigned long long prot, const char *comment)
{
    const char *slash = strrchr(rel, '/');
    const char *base = slash ? slash + 1 : rel;
    char dirrel[1024], *dir, *path;
    int attempt, rc = -1;
    void *lock;
    snprintf(dirrel, sizeof dirrel, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    dir = dirrel[0] ? pkg_join(root, dirrel) : pkg_join(root, "");
    path = dir ? pkg_join(dir, ".ameta") : NULL;
    if (path == NULL) { free(dir); return -1; }
    if ((prot & ~PKG_AMETA_RECORD_MASK) == 0 && (comment == NULL || !comment[0]) && !pkg_fs_exists(path)) {
        free(path); free(dir);
        return 0;                       /* nothing to record, nothing to take back */
    }
    lock = pkg_fs_lock_dir(dir);
    for (attempt = 0; attempt < 5 && rc < 0; attempt++) {
        struct pkg_fs_id id;
        struct pkg_ameta a;
        unsigned char *buf = NULL;
        size_t len = 0;
        char *out = NULL;
        size_t olen = 0;
        struct present_ctx pc;
        int r;
        if (pkg_fs_identity(path, &id) != 0) break;
        pkg_ameta_init(&a);
        if (id.exists && t_read(path, &buf, &len) != 0) break;
        if (id.exists && (pkg_ameta_parse(buf, len, &a) != 0 || !a.usable)) {
            warn("%s is not an .ameta this pkg can write: %s's attributes were not recorded",
                 path, rel);
            free(buf); pkg_ameta_free(&a);
            attempt = 5;
            break;
        }
        free(buf);
        pc.dir = dir;
        pkg_ameta_mark_stale(&a, present_in, &pc);
        if ((prot & ~PKG_AMETA_RECORD_MASK) == 0 && (comment == NULL || !comment[0])) {
            pkg_ameta_delete(&a, (const unsigned char *)base, strlen(base));
        } else {
            struct pkg_ameta_entry *e = pkg_ameta_get(&a, (const unsigned char *)base, strlen(base));
            if (e == NULL) { pkg_ameta_free(&a); break; }
            e->prot = prot;
            e->has_prot = 1;
            pkg_ameta_set_comment(e, (const unsigned char *)(comment ? comment : ""),
                                  comment ? strlen(comment) : 0);
        }
        if (pkg_ameta_emit(&a, &out, &olen) != 0) { pkg_ameta_free(&a); break; }
        pkg_ameta_free(&a);
        r = pkg_fs_replace_if_same(path, &id, out, olen);
        free(out);
        if (r == 0) { rc = 0; tr("recorded the attributes of %s in %s", rel, path); }
        else if (r < 0) break;
    }
    pkg_fs_unlock_dir(lock);
    if (rc != 0 && attempt < 5)
        warn("the attributes of %s could not be recorded in %s", rel, path);
    free(path);
    free(dir);
    return rc;
}

/* Give each installed file its protection and comment: on AROS the file
 * system holds them; elsewhere the host mode takes owner Execute and the
 * directory's .ameta the rest. */
static void attrs_one(const char *root, const struct pkg_file *f)
{
    char latin[PKG_COMMENT_MAX + 1], *full = pkg_join(root, f->path);
    int r;
    if (full == NULL) return;
    latin[0] = '\0';
    if (f->comment) pkg_comment_latin1(f->comment, latin, sizeof latin);
    r = pkg_fs_amiga_set(full, f->prot, latin);
    if (r < 0) {
        warn("the protection or comment of %s could not be set", f->path);
    } else if (r == 0) {
        if (pkg_fs_set_owner_exec(full, (f->prot & PKG_AMETA_OWNER_EXECUTE) == 0) != 0)
            warn("the host mode of %s could not be set", f->path);
        ameta_set(root, f->path, f->prot, f->comment);
    }
    free(full);
}

static void apply_attrs(const char *root, const struct pkg_manifest *m)
{
    size_t i;
    for (i = 0; i < m->nfiles; i++)
        attrs_one(root, &m->files[i]);
}

struct installed {
    struct pkg_manifest *m;
    size_t               n;
};

static void installed_free(struct installed *in)
{
    size_t i;
    for (i = 0; i < in->n; i++)
        pkg_manifest_free(&in->m[i]);
    free(in->m);
    in->m = NULL;
    in->n = 0;
}

/* Every package in the root's database. */
static int load_all(const char *root, struct installed *in)
{
    char *dir = pkg_join(root, ".pkg/db"), **names;
    size_t n, i;

    in->m = NULL;
    in->n = 0;
    if (dir == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(dir)) {
        free(dir);
        return 0;
    }
    if (pkg_fs_list(dir, &names, &n) != 0) {
        free(dir);
        return refuse_c(17, "cannot read the database of %s", root);
    }
    free(dir);
    in->m = calloc(n ? n : 1, sizeof *in->m);
    if (in->m == NULL) {
        for (i = 0; i < n; i++) free(names[i]);
        free(names);
        return refuse("out of memory");
    }
    for (i = 0; i < n; i++) {
        if (load_installed(root, names[i], &in->m[in->n], 1) == 0)
            in->n++;
        free(names[i]);
    }
    free(names);
    return 0;
}

/* The name beside `path` that Pkg sets a file down under: `path` plus
 * `suffix`, the file name shortened when needed to stay within the 30
 * characters an FFS name may have. Allocated. */
static char *beside(const char *path, const char *suffix)
{
    const char *base = strrchr(path, '/');
    size_t dl = base ? (size_t)(base - path) + 1 : 0, bl = strlen(path) - dl, sl = strlen(suffix);
    char *out;
    if (bl + sl > 30) bl = 30 > sl ? 30 - sl : 1;
    out = malloc(dl + bl + sl + 1);
    if (out == NULL) return NULL;
    memcpy(out, path, dl + bl);
    memcpy(out + dl + bl, suffix, sl + 1);
    return out;
}

static int apply(const char *root, const struct pkg_manifest *old, const struct fetched *f,
                 unsigned long *placed, unsigned long *dropped, unsigned long *kept)
{
    const struct pkg_manifest *m = &f->m;
    struct walk_ctx wc;
    struct stage_ctx sc;
    char *staging, *dbp;
    unsigned long adopted;  /* files already there, byte for byte the package's */
    struct installed others;/* loaded when a file is already there */
    int others_loaded = 0;
    unsigned long same;     /* files the old and new versions share, intact on disk */
    int fs;
    unsigned char *keep;    /* per file: 1 a person's configuration kept, the new one set
                               beside it; 2 kept, and the new version is what they edited;
                               3 already in place, byte for byte: neither staged nor moved */
    int stopped;
    size_t i;
    enum pkg_status st;

    *placed = *dropped = *kept = 0;
    wc.m = m; wc.i = 0; wc.err[0] = '\0';
    st = pkg_read(f->pkg, f->pkg_len, check_entry, &wc, &stopped);
    if (st != PKG_OK || wc.i != m->nfiles)
        return refuse_c(12, "the payload of %s %s is refused: %s; nothing was changed", m->name, m->version,
                      st == PKG_E_STOPPED ? wc.err
                      : st != PKG_OK ? pkg_strstatus(st) : "the manifest lists files the container lacks");

    keep = calloc(m->nfiles ? m->nfiles : 1, 1);
    if (keep == NULL)
        return refuse("out of memory");
    adopted = 0;
    same = 0;
    others.m = NULL;
    others.n = 0;
    for (i = 0; i < m->nfiles; i++) {
        const struct pkg_file *of = old ? find_file(old, m->files[i].path) : NULL;
        progress("checking", i, m->nfiles);
        if (m->files[i].config) {
            /* A configuration file: a person's version stays where it is. */
            if (of != NULL && file_state(root, of->path, of->digest, of->size) == 1)
                keep[i] = strcmp(of->digest, m->files[i].digest) == 0 ? 2 : 1;
            else if (of == NULL) {
                int cs = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size);
                if (cs == 1) keep[i] = 1;
                else if (cs == 0) { keep[i] = 3; adopted++; }
            } else if (strcmp(of->digest, m->files[i].digest) == 0 && of->size == m->files[i].size
                       && file_state(root, of->path, of->digest, of->size) == 0) {
                keep[i] = 3;
                same++;
            }
            continue;
        }
        if (of == NULL) {
            char *t = pkg_join(root, m->files[i].path);
            int there = t ? pkg_fs_exists(t) : 1;
            free(t);
            if (there && file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 0) {
                /* Already there with the package's own bytes, as an AROS set up
                 * with InstallAROS holds its files: the package takes it over,
                 * unless another installed package lists it. */
                const char *owner = NULL;
                size_t k;
                if (!others_loaded) {
                    if (load_all(root, &others) != 0) { free(keep); return 1; }
                    others_loaded = 1;
                }
                for (k = 0; k < others.n && owner == NULL; k++)
                    if (strcmp(others.m[k].name, m->name) != 0
                        && find_file(&others.m[k], m->files[i].path) != NULL)
                        owner = others.m[k].name;
                if (owner == NULL) {
                    adopted++;
                    keep[i] = 3;
                    continue;
                }
                refuse_c(15, "\"%s\" belongs to %s, which is installed; %s %s ships it too, and "
                         "nothing was changed. One file has one owner: the publisher of one of "
                         "the two leaves it out", m->files[i].path, owner, m->name, m->version);
                installed_free(&others);
                free(keep);
                return 1;
            }
            if (there) {
                free(keep);
                installed_free(&others);
                return refuse_c(15, "\"%s\" already exists in %s with other content than %s %s "
                              "ships, and no installed package lists it; nothing was changed. It "
                              "may be the requester's own file, or another version's",
                              m->files[i].path, root, m->name, m->version);
            }
        } else if ((fs = file_state(root, of->path, of->digest, of->size)) == 0) {
            if (strcmp(of->digest, m->files[i].digest) == 0 && of->size == m->files[i].size) {
                keep[i] = 3;            /* the same in both versions, and intact */
                same++;
            }
        } else if (fs == 1) {
            free(keep);
            installed_free(&others);
            return refuse_c(15, "\"%s\" was edited since %s %s was installed, and %s %s ships it too; "
                          "nothing was changed. The edit belongs to whoever made it: the requester decides whether "
                          "to keep it elsewhere first. A publisher who means it to be edited declares it "
                          "with CONFIG, and pkg then keeps the edit", of->path, old->name, old->version,
                          m->name, m->version);
        }
    }
    if (adopted) {
        if (machine) kv("adopted", "%lu", adopted);
        else say_item("adopted", "%lu file%s already there, identical to %s %s's", adopted,
                 adopted == 1 ? "" : "s", m->name, m->version);
    }
    if (same) {
        if (machine) kv("unchanged-files", "%lu", same);
        else say_item("unchanged", "%lu file%s the same in %s and %s, not written again", same,
                 same == 1 ? "" : "s", old->version, m->version);
    }
    for (i = 0; i < m->nfiles; i++)
        if (keep[i] == 1 || keep[i] == 2) {
            (*kept)++;
            char *nw = keep[i] == 1 ? beside(m->files[i].path, ".pkgnew") : NULL;
            if (machine) {
                kv("config-kept", "%s", m->files[i].path);
                if (nw) kv("config-new", "%s", nw);
            } else if (nw) {
                say_item("kept", "%s (edited; %s %s's version is beside it as %s)",
                    m->files[i].path, m->name, m->version, nw);
            } else if (keep[i] == 1) {
                say_item("kept", "%s (edited)", m->files[i].path);
            } else {
                say_item("kept", "%s (edited; %s %s ships it unchanged)", m->files[i].path,
                    m->name, m->version);
            }
            free(nw);
        }

    installed_free(&others);
    progress("checking", m->nfiles, m->nfiles);
    tr("%s %s: every file checked against the container, nothing in the way", m->name, m->version);
    if (dryrun) {
        /* Every check above has passed; say what would move, move nothing. */
        *placed = 0;
        for (i = 0; i < m->nfiles; i++)
            if (!keep[i]) (*placed)++;
        for (i = 0; old != NULL && i < old->nfiles; i++)
            if (find_file(m, old->files[i].path) == NULL)
                (*dropped)++;
        free(keep);
        return 0;
    }
    {
        char rel[128];
        snprintf(rel, sizeof rel, ".pkg/staging/%s", m->name);
        staging = pkg_join(root, rel);
    }
    if (staging == NULL || pkg_fs_rmtree(staging) != 0) {
        free(staging);
        free(keep);
        return refuse_c(17, "cannot prepare staging in %s", root);
    }
    sc.staging = staging; sc.m = m; sc.keep = keep; sc.err[0] = '\0';
    sc.done = 0;
    for (i = 0, sc.todo = 0; i < m->nfiles; i++)
        if (keep[i] < 2) sc.todo++;
    if (pkg_read(f->pkg, f->pkg_len, stage_entry, &sc, &stopped) != PKG_OK) {
        progress("writing", sc.todo, sc.todo);
        refuse_c(PKGRC_IO, "%s; nothing was changed", sc.err);
        pkg_fs_rmtree(staging);
        free(staging);
        free(keep);
        return 1;
    }
    progress("writing", sc.todo, sc.todo);

    for (i = 0; i < m->nfiles; i++) {
        char *from, *to;
        int good;
        progress("placing", i, m->nfiles);
        from = pkg_join(staging, m->files[i].path);
        to = pkg_join(root, m->files[i].path);
        if (keep[i] == 2 || keep[i] == 3) {
            free(from);
            free(to);
            continue;
        }
        if (keep[i] == 1 && to != NULL) {
            char *nw = beside(to, ".pkgnew");
            free(to);
            to = nw;
        }
        if (to != NULL && pkg_fs_exists(to))
            pkg_fs_unprotect(to);           /* the version it replaces may forbid Delete */
        good = from && to && pkg_fs_rename(from, to) == 0;
        free(from);
        free(to);
        if (!good) {
            refuse_c(17, "cannot place \"%s\": %s. %lu of %lu files were placed",
                   m->files[i].path, strerror(errno), (unsigned long)i, (unsigned long)m->nfiles);
            free(staging);
            free(keep);
            return 1;
        }
        if (!keep[i]) (*placed)++;
    }
    progress("placing", m->nfiles, m->nfiles);
    free(keep);

    if (old != NULL) {
        for (i = 0; i < old->nfiles; i++) {
            const struct pkg_file *of = &old->files[i];
            int s;
            if (find_file(m, of->path) != NULL)
                continue;
            s = file_state(root, of->path, of->digest, of->size);
            if (s == 0) {
                char *p = pkg_join(root, of->path);
                if (p != NULL && pkg_fs_unlink(p) == 0) {
                    (*dropped)++;
                    pkg_fs_prune_empty_parents(root, of->path);
                }
                free(p);
            } else if (s == 1) {
                (*kept)++;
                if (machine) kv("kept", "%s", of->path);
                else say_item("kept", "%s (edited, and no longer part of %s)", of->path, m->name);
            }
        }
    }

    dbp = root_path(root, "db", m->name);
    if (dbp == NULL || pkg_fs_write_atomic(dbp, f->mtext, f->mlen) != 0) {
        refuse_c(17, "the files are placed but the database entry could not be written: %s",
               strerror(errno));
        free(dbp);
        free(staging);
        return 1;
    }
    free(dbp);
    apply_attrs(root, m);
    if (write_pin(root, m->name, f->signer) != 0)
        warn("the signing key could not be pinned");
    if (old != NULL) {
        char *pp = root_path(root, "prev", m->name), line[160];
        int n = snprintf(line, sizeof line, "%s\n", old->version);
        if (pp == NULL || pkg_fs_write_atomic(pp, line, (size_t)n) != 0)
            warn("the previous version could not be recorded for ROLLBACK");
        free(pp);
    }
    pkg_fs_rmtree(staging);
    free(staging);
    return 0;
}

/* ---- dependencies ----------------------------------------------------- */

/* A package installed only because another needed it carries a mark in
 * .pkg/auto. REMOVE ORPHANS takes out marked packages nothing depends on. */
static int is_auto(const char *root, const char *name)
{
    char *p = root_path(root, "auto", name);
    int there = p != NULL && pkg_fs_exists(p);
    free(p);
    return there;
}

static void set_auto(const char *root, const char *name, int on)
{
    char *p = root_path(root, "auto", name);
    if (p == NULL)
        return;
    if (on) {
        if (pkg_fs_write_atomic(p, "dependency\n", 11) != 0)
            warn("%s could not be marked as a dependency", name);
    } else if (pkg_fs_exists(p)) {
        pkg_fs_unlink(p);
    }
    free(p);
}

/* The installed packages that name `name` among their Depends, comma-joined. */
static int needed_by(const struct installed *in, const char *name, char *out, size_t len)
{
    size_t i, j, at = 0;
    int found = 0;
    out[0] = '\0';
    for (i = 0; i < in->n; i++)
        for (j = 0; j < in->m[i].ndeps; j++) {
            if (strcmp(in->m[i].deps[j].name, name) != 0)
                continue;
            /* Found counts whether or not the name still fits in `out`. */
            if (at + 70 < len)
                at += (size_t)snprintf(out + at, len - at, "%s%s %s", found ? ", " : "",
                                       in->m[i].name, in->m[i].version);
            found = 1;
        }
    return found;
}

/* What an install, upgrade or rollback will do, decided and verified in full
 * before anything is placed: dependencies first, the named package last. */
struct plan {
    struct fetched *f;
    int            *dep;         /* 1: pulled in as a dependency */
    size_t          n;
    const char     *stack[32];   /* the path being resolved, for cycles */
    size_t          depth;
    const char     *root, *acceptkey;
    const struct index *ix;
};

static void plan_free(struct plan *p)
{
    size_t i;
    for (i = 0; i < p->n; i++)
        fetched_free(&p->f[i]);
    free(p->f);
    free(p->dep);
}

/* During a dry-run UPGRADE ALL, the version a package upgraded earlier in the
 * same run would be at; NULL otherwise. Defined with UPGRADE ALL below. */
static const char *planned_version(const char *name);

/* Plan `name`: exactly `exact` when given, else the highest the channel offers,
 * which must be at least `min`. `from` is the package that needs it, NULL for
 * the one the person named. */
static int plan_one(struct plan *p, const char *name, const char *min, const char *exact,
                    const char *from)
{
    const struct entry *e;
    struct fetched f;
    struct pkg_manifest cur;
    const char *have;
    size_t i;

    if (cancelled("while resolving, before anything was placed"))
        return 1;
    for (i = 0; i < p->depth; i++) {
        if (strcmp(p->stack[i], name) == 0) {
            char path[512];
            size_t at = 0, j;
            for (j = i; j < p->depth && at + 70 < sizeof path; j++)
                at += (size_t)snprintf(path + at, sizeof path - at, "%s -> ", p->stack[j]);
            snprintf(path + at, sizeof path - at, "%s", name);
            return refuse_c(16, "the dependencies form a cycle: %s; nothing was changed", path);
        }
    }
    if (from != NULL)
        tr("%s needs %s%s%s", from, name, min ? " >= " : "", min ? min : "");
    if (from != NULL) {
        if (load_installed(p->root, name, &cur, 1) == 0) {
            int low;
            have = planned_version(name);
            if (have == NULL)
                have = cur.version;
            low = min != NULL && pkg_version_cmp(have, min) < 0;
            if (!low)
                tr("%s is satisfied by the installed %s %s", name, name, have);
            if (low)
                refuse_c(16, "%s needs %s >= %s, and %s has %s %s; nothing was changed. "
                         "Upgrading %s would change it for everything that uses it",
                         from, name, min, p->root, name, have, name);
            pkg_manifest_free(&cur);
            return low;
        }
        for (i = 0; i < p->n; i++)
            if (strcmp(p->f[i].m.name, name) == 0) {
                tr("%s is already part of this plan, at %s", name, p->f[i].m.version);
                if (min != NULL && pkg_version_cmp(p->f[i].m.version, min) < 0)
                    return refuse_c(16, "%s needs %s >= %s, and this install brings %s %s; "
                                    "nothing was changed", from, name, min, name, p->f[i].m.version);
                return 0;
            }
    }
    e = pick(p->ix, name, exact);
    if (e == NULL && pick_refused)
        return 1;
    if (e == NULL) {
        if (from == NULL) {
            say_not_found(p->ix, name, exact, chans_text());
            return 1;
        }
        return refuse_c(16, "%s depends on %s%s%s, which the channel %s does not offer; "
                        "nothing was changed", from, name, min ? " >= " : "", min ? min : "",
                        chans_text());
    }
    if (e->withdrawn)
        return refuse_n(18, "ask-requester", "%s %s was withdrawn by its publisher in %s; nothing "
                        "was changed. Installing it anyway is the requester's decision, and pkg "
                        "does not take it", e->name, e->version, chan_of(e));
    if (min != NULL && pkg_version_cmp(e->version, min) < 0)
        return refuse_c(16, "%s needs %s >= %s, and the highest the channel offers is %s; "
                        "nothing was changed", from ? from : "the request", name, min, e->version);
    if (p->depth >= sizeof p->stack / sizeof p->stack[0])
        return refuse_c(16, "the dependencies of %s go more than %u levels deep", name,
                        (unsigned)(sizeof p->stack / sizeof p->stack[0]));
    if (fetch(chan_of(e), e, &f) != 0) {
        fetched_free(&f);
        return 1;
    }
    if (check_pin(p->root, e->name, f.signer, p->acceptkey) != 0) {
        fetched_free(&f);
        return 1;
    }
    {
        /* Nothing pinned yet: the first install would trust this signer from
         * now on. The key that signed the channel's first version of the
         * package is presumed the publisher's; a first install signed by
         * another key is the case a key added later by someone else would
         * make, and trusting it is the requester's decision. */
        char *kp = root_path(p->root, "keys", e->name), first_signer[65];
        const struct entry *oldest = NULL;
        int first = kp != NULL && !pkg_fs_exists(kp);
        size_t o;
        free(kp);
        for (o = 0; first && o < p->ix->n; o++)
            if (strcmp(p->ix->e[o].name, e->name) == 0 && p->ix->e[o].ch == e->ch
                && (oldest == NULL || pkg_version_cmp(p->ix->e[o].version, oldest->version) < 0))
                oldest = &p->ix->e[o];
        if (first && oldest != NULL && oldest != e
            && claimed_signer(chan_of(oldest), oldest->digest, first_signer)
            && strcmp(first_signer, f.signer) != 0
            && (p->acceptkey == NULL || strcmp(p->acceptkey, f.signer) != 0)) {
            kv("signer", "%s", f.signer);
            kv("first-signer", "%s", first_signer);
            refuse_n(14, "ask-requester", "%s %s is signed by %s, but %s %s, the first version in "
                     "this channel, was signed by %s. This root trusts no key for %s yet, and "
                     "the first install would trust this one from now on; only the requester can "
                     "say which key is the publisher's. Nothing was changed", e->name, e->version,
                     f.signer, e->name, oldest->version, first_signer, e->name);
            fetched_free(&f);
            return 1;
        }
    }
    p->stack[p->depth++] = f.m.name;
    pick_quiet = 0;
    for (i = 0; i < f.m.ndeps; i++) {
        if (plan_one(p, f.m.deps[i].name, f.m.deps[i].min, NULL, f.m.name) != 0) {
            p->depth--;
            fetched_free(&f);
            return 1;
        }
    }
    p->depth--;
    {
        struct fetched *g = realloc(p->f, (p->n + 1) * sizeof *g);
        int *d;
        if (g == NULL) { fetched_free(&f); return refuse("out of memory"); }
        p->f = g;
        d = realloc(p->dep, (p->n + 1) * sizeof *d);
        if (d == NULL) { fetched_free(&f); return refuse("out of memory"); }
        p->dep = d;
        p->f[p->n] = f;
        p->dep[p->n] = from != NULL;
        p->n++;
    }
    return 0;
}

/* Remove a package's unchanged files and its records. Edited files stay. */
static int remove_files(const char *root, const struct pkg_manifest *m,
                        size_t *removed, size_t *kept, size_t *gone, int report)
{
    size_t i;
    char *dbp, *pp;
    *removed = *kept = *gone = 0;
    for (i = 0; i < m->nfiles; i++) {
        int s = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size);
        if (s == 0 && dryrun) {
            (*removed)++;
        } else if (s == 0) {
            char *p = pkg_join(root, m->files[i].path);
            if (p != NULL)
                pkg_fs_unprotect(p);            /* a Delete-forbidden file is still Pkg's to remove */
            if (p != NULL && pkg_fs_unlink(p) == 0) {
                (*removed)++;
                ameta_set(root, m->files[i].path, 0, NULL);   /* its entry goes too */
                pkg_fs_prune_empty_parents(root, m->files[i].path);
            } else if (p != NULL && report) {
                warn("%s could not be deleted: %s", m->files[i].path, strerror(errno));
            }
            free(p);
        } else if (s == 1) {
            (*kept)++;
            if (!report)
                continue;
            if (machine) kv("kept", "%s", m->files[i].path);
            else say_item("kept", "%s (changed since install, so it is yours now)", m->files[i].path);
        } else {
            (*gone)++;
        }
    }
    if (dryrun)
        return 0;
    dbp = root_path(root, "db", m->name);
    if (dbp == NULL || pkg_fs_unlink(dbp) != 0) {
        free(dbp);
        return refuse_c(17, "the files of %s are removed but its database entry could not be: %s",
                        m->name, strerror(errno));
    }
    free(dbp);
    pp = root_path(root, "prev", m->name);
    if (pp != NULL && pkg_fs_exists(pp)) pkg_fs_unlink(pp);
    free(pp);
    set_auto(root, m->name, 0);
    /* The pinned key stays: reinstalling the package later is still held to it. */
    return 0;
}

/* Carry out a plan. The last entry is the named package, moved from `cur`
 * (NULL for a fresh install); the others are new dependencies. A failure
 * takes the dependencies this run placed back out. */
static int run_plan(struct plan *p, const struct pkg_manifest *cur,
                    unsigned long *placed, unsigned long *dropped, unsigned long *kept)
{
    size_t i;
    int *had_pin = calloc(p->n ? p->n : 1, sizeof *had_pin);
    if (had_pin == NULL)
        return refuse("out of memory");
    for (i = 0; i < p->n; i++) {
        int last = i + 1 == p->n;
        unsigned long pl, dr, ke;
        char when[200], *kp = root_path(p->root, "keys", p->f[i].m.name);
        had_pin[i] = kp != NULL && pkg_fs_exists(kp);
        free(kp);
        snprintf(when, sizeof when, "before placing %s %s%s", p->f[i].m.name, p->f[i].m.version,
                 i > 0 ? "; what this operation had placed was taken back out" : "");
        if (cancelled(when) || apply(p->root, last ? cur : NULL, &p->f[i], &pl, &dr, &ke) != 0) {
            /* Undo in reverse: files, records, and the keys this operation
             * pinned, so the root is as it was. */
            while (!dryrun && i-- > 0) {
                size_t r, k, g;
                remove_files(p->root, &p->f[i].m, &r, &k, &g, 0);
                if (!had_pin[i]) {
                    char *pin = root_path(p->root, "keys", p->f[i].m.name);
                    if (pin != NULL) pkg_fs_unlink(pin);
                    free(pin);
                }
                tr("took %s %s back out", p->f[i].m.name, p->f[i].m.version);
            }
            free(had_pin);
            return 1;
        }
        if (!last) {
            if (!dryrun)
                set_auto(p->root, p->f[i].m.name, 1);
        } else {
            *placed = pl;
            *dropped = dr;
            *kept = ke;
        }
    }
    free(had_pin);
    /* Only now, with every package in place, say which dependencies came in:
     * a front end never shows a package that is about to be taken out. */
    for (i = 0; i + 1 < p->n; i++) {
        if (machine) {
            char j[140];
            snprintf(j, sizeof j, "%s %s", p->f[i].m.name, p->f[i].m.version);
            rec_item("dependency", j, "name", p->f[i].m.name, "version", p->f[i].m.version, NULL);
        } else {
            say_item(dryrun ? "would add" : "added", "%s %s, a dependency",
                p->f[i].m.name, p->f[i].m.version);
        }
    }
    return 0;
}

static int plan_target(struct plan *p, const struct pkg_options *a, const struct index *ix,
                       const char *name, const char *exact)
{
    int rc;
    memset(p, 0, sizeof *p);
    p->root = a->root;
    p->acceptkey = a->acceptkey;
    p->ix = ix;
    /* The caller picked `exact` and traced why. */
    pick_quiet = 1;
    rc = plan_one(p, name, NULL, exact, NULL);
    pick_quiet = 0;
    return rc;
}

/* The marked packages nothing installed depends on. */
static size_t find_orphans(const struct installed *in, const char *root, size_t *which)
{
    size_t i, n = 0;
    char who[1];
    for (i = 0; i < in->n; i++)
        if (is_auto(root, in->m[i].name) && !needed_by(in, in->m[i].name, who, sizeof who))
            which[n++] = i;
    return n;
}

/* ---- verbs ------------------------------------------------------------ */

/* The mount entry of an installed image, and the AmigaDOS steps around it.
 * Pkg's part in an application ends with the image in place; this only
 * writes down what mounting it takes, with the geometry read from the signed
 * manifest, so nobody has to work it out. */
static int cmd_mountlist(const struct pkg_options *a)
{
    struct pkg_manifest m;
    struct installed in;
    char text[1600], handler[512], fdsk[64], unitbuf[16], *img;
    const char *unit = a->unit ? a->unit : "20";
    unsigned long blocks;
    size_t i, j;
    int n, libs = 0;

    if (a->target == NULL)  return refuse_c(20, "name the installed image");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (strspn(unit, "0123456789") != strlen(unit) || strlen(unit) == 0 || strlen(unit) > 4)
        return refuse_c(20, "UNIT is a number, the fdsk.device unit");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    if (strcmp(m.kind, "image") != 0 || m.nfiles != 1) {
        refuse_c(20, "%s is a %s, not an image; only an image is mounted", m.name, m.kind);
        pkg_manifest_free(&m);
        return 1;
    }
    blocks = (unsigned long)(m.files[0].size / PKG_IMAGE_BLOCK);
    handler[0] = '\0';
    if (a->handler != NULL) {
        snprintf(handler, sizeof handler, "%s", a->handler);
    } else if (load_all(a->root, &in) == 0) {
        /* An FFS handler installed into the same root as a component. */
        for (i = 0; i < in.n && !handler[0]; i++)
            for (j = 0; j < in.m[i].nfiles; j++)
                if (strcmp(in.m[i].kind, "device") == 0
                    && strcmp(in.m[i].files[j].path, "L/afs-handler") == 0) {
                    char *h = pkg_join(a->root, "L/afs-handler");
                    if (h) snprintf(handler, sizeof handler, "%s", h);
                    free(h);
                }
        for (i = 0; i < in.n; i++)
            for (j = 0; j < m.ndeps; j++)
                if (strcmp(in.m[i].name, m.deps[j].name) == 0)
                    libs = 1;
        installed_free(&in);
    }
    n = snprintf(text, sizeof text,
        "%s%s%s"
        "Device          = fdsk.device\n"
        "Unit            = %s\n"
        "Flags           = 0\n"
        "Surfaces        = 1\n"
        "BlocksPerTrack  = %u\n"
        "LowCyl          = 0\n"
        "HighCyl         = %lu\n"
        "Reserved        = 2\n"
        "BlockSize       = %u\n"
        "Buffers         = 20\n"
        "BufMemType      = 1\n"
        "Mask            = 0\n"
        "StackSize       = 16384\n"
        "Priority        = 5\n"
        "GlobVec         = -1\n"
        "DosType         = 0x444F5303\n"
        "Activate        = 1\n",
        handler[0] ? "FileSystem      = " : "", handler, handler[0] ? "\n" : "",
        unit, PKG_IMAGE_TRACK, blocks / PKG_IMAGE_TRACK - 1, PKG_IMAGE_BLOCK);
    img = pkg_join(a->root, m.files[0].path);
    snprintf(unitbuf, sizeof unitbuf, "Unit%s", unit);
    snprintf(fdsk, sizeof fdsk, "RAM:fdsk");
    if (a->out != NULL && !dryrun && pkg_fs_write_atomic(a->out, text, (size_t)n) != 0) {
        free(img);
        pkg_manifest_free(&m);
        return refuse_c(17, "cannot write \"%s\": %s", a->out, strerror(errno));
    }
    kv("result", "%s", a->out ? res("created", "would-create") : "shown");
    kv("name", "%s", m.name);
    kv("image", "%s", img ? img : m.files[0].path);
    kv("blocks", "%lu", blocks);
    kv("highcyl", "%lu", blocks / PKG_IMAGE_TRACK - 1);
    kv("unit", "%s", unit);
    kv("handler", "%s", handler[0] ? handler : "none: the system's FFS for DOS\\3");
    if (a->out) kv("file", "%s", a->out);
    kv("step", "MakeDir %s", fdsk);
    kv("step", "Assign FDSK: %s", fdsk);
    kv("step", "MakeLink %s/%s %s", fdsk, unitbuf, img ? img : m.files[0].path);
    kv("step", "Protect %s w SUB", img ? img : m.files[0].path);
    if (libs) kv("step", "Assign LIBS: %s/Libs ADD", a->root);
    kv("step", "Mount %s", a->out ? a->out : "<this mountlist, saved as a file named after the device>");
    if (!machine) {
        if (a->out == NULL)
            say_raw(text, (size_t)n);
        else
            say_result("%s %s, the mount entry for %s (%lu blocks)",
                    dryrun ? "would write" : "wrote", a->out, m.name, blocks);
        say_kind(PKG_LINE_NOTE, "\n%s\n", "On AROS, the device is named after the mountlist file:\n"
                "  MakeDir %s\n  Assign FDSK: %s\n  MakeLink %s/%s %s\n  Protect %s w SUB\n%s%s%s"
                "  Mount %s", fdsk, fdsk, fdsk, unitbuf, img ? img : "", img ? img : "",
                libs ? "  Assign LIBS: " : "", libs ? a->root : "", libs ? "/Libs ADD\n" : "",
                a->out ? a->out : "<file>");
    }
    if (a->out == NULL)
        hint("Mount reads the entry from a file named after the device: add OUT <file>, "
             "for example OUT RAM:%s, and pkg writes it", m.name);
    if (!handler[0])
        hint("no FFS handler is installed in this root, so the entry relies on the system's. "
             "Native AROS has one; hosted AROS built on macOS has none: there, install one "
             "into the root as a device package, or name one with HANDLER <path>");
    free(img);
    pkg_manifest_free(&m);
    return 0;
}

/* What a channel offers, each version checked the way INSTALL would check
 * it: manifest against the index, signature, payload against the manifest.
 * Nothing is installed. Exits with the class of the first bad entry. */
/* One row of SHOW: an entry, its checks, and what was found. */
struct show_row {
    struct fetched f;
    int            rc;          /* 0, or 1 with cls and reason */
    int            cls;
    char           reason[2048];
    const char    *here;
    int            selected;
    int            archive_checked;
    int            upstream_only;   /* its archive is only upstream: not read here */
};

/* One file an archive must hold, for one row. */
struct expect {
    const char *path;           /* inside the archive: <prefix>/<file> */
    char       *owned;          /* the string path points into */
    const char *digest;
    unsigned long long size;
    size_t      row;
    int         seen;
};

static int by_expect(const void *x, const void *y)
{
    return strcmp(((const struct expect *)x)->path, ((const struct expect *)y)->path);
}

struct arch_check {
    struct expect     *ex;
    size_t             n;
    long               cur, last;   /* every entry claiming this path: cur .. last */
    struct pkg_sha256  sha;
    unsigned long long got;
};

static int ac_want(const struct pkg_archive_entry *e, void *ctx)
{
    struct arch_check *ac = (struct arch_check *)ctx;
    struct expect key, *hit;
    if (e->is_dir) return 0;
    key.path = e->path;
    hit = (struct expect *)bsearch(&key, ac->ex, ac->n, sizeof *ac->ex, by_expect);
    if (hit == NULL) return 0;
    ac->cur = ac->last = (long)(hit - ac->ex);
    /* two packages may list the same file: all of them get this one read */
    while (ac->cur > 0 && strcmp(ac->ex[ac->cur - 1].path, e->path) == 0) ac->cur--;
    while (ac->last + 1 < (long)ac->n && strcmp(ac->ex[ac->last + 1].path, e->path) == 0) ac->last++;
    pkg_sha256_init(&ac->sha);
    ac->got = 0;
    return 1;
}

static int ac_data(const struct pkg_archive_entry *e, const unsigned char *buf, size_t len, void *ctx)
{
    struct arch_check *ac = (struct arch_check *)ctx;
    struct expect *x = &ac->ex[ac->cur];
    (void)e;
    if (len > 0) {
        ac->got += len;
        if (ac->got <= 0xFFFFFFFFFFFFull)   /* the sizes are compared at the end */
            pkg_sha256_update(&ac->sha, buf, len);
        return 0;
    }
    {
        unsigned char dg[PKG_SHA256_LEN];
        char hex[PKG_SHA256_HEXLEN + 1];
        size_t k;
        long c;
        pkg_sha256_final(&ac->sha, dg);
        for (k = 0; k < PKG_SHA256_LEN; k++) snprintf(hex + 2 * k, 3, "%02x", dg[k]);
        for (c = ac->cur; c <= ac->last; c++)
            ac->ex[c].seen = (ac->got == ac->ex[c].size && strcmp(hex, ac->ex[c].digest) == 0) ? 1 : 2;
        (void)x;
    }
    return 0;
}

/* SHOW <name>: the catalogue fields of its newest version, as a person
 * reads them on the portal, and as records for a program. */
static void show_about(const struct index *ix, const char *name)
{
    const struct entry *best = NULL;
    struct pkg_manifest m;
    const struct pkg_about *ab;
    unsigned char *buf;
    size_t len, o;
    char *mp, err[200];

    for (o = 0; o < ix->n; o++)
        if (strcmp(ix->e[o].name, name) == 0
            && (best == NULL || pkg_version_cmp(ix->e[o].version, best->version) > 0))
            best = &ix->e[o];
    if (best == NULL || (mp = object_path(chan_of(best), best->digest, "manifest")) == NULL)
        return;
    if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); return; }
    free(mp);
    if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) != 0) { free(buf); return; }
    free(buf);
    ab = &m.about;
    if (machine) {
        size_t i;
        if (ab->short_desc) kv("short", "%s", ab->short_desc);
        if (ab->category) kv("category", "%s", ab->category);
        for (i = 0; i < ab->tags.n; i++) kv("tag", "%s", ab->tags.v[i]);
        for (i = 0; i < ab->authors.n; i++) kv("author", "%s", ab->authors.v[i]);
        if (ab->homepage) kv("homepage", "%s", ab->homepage);
        if (ab->repository) kv("repository", "%s", ab->repository);
        if (ab->license) kv("license", "%s", ab->license);
        if (ab->distribution) kv("distribution", "%s", ab->distribution);
        for (i = 0; i < ab->description.n; i++) kv("description", "%s", ab->description.v[i]);
        for (i = 0; i < ab->changes.n; i++) kv("changes", "%s", ab->changes.v[i]);
    } else {
        size_t i;
        char line[1100];
        size_t at;
        if (ab->short_desc) say_kind(PKG_LINE_DETAIL, "  %s\n", "%s %s: %s", m.name, m.version, ab->short_desc);
        if (ab->category) say_kind(PKG_LINE_DETAIL, "  %s\n", "category   %s", ab->category);
        if (ab->tags.n) {
            for (i = 0, at = 0; i < ab->tags.n && at < sizeof line; i++)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? ", " : "", ab->tags.v[i]);
            say_kind(PKG_LINE_DETAIL, "  %s\n", "tags       %s", line);
        }
        if (ab->authors.n) {
            for (i = 0, at = 0; i < ab->authors.n && at < sizeof line; i++)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? ", " : "", ab->authors.v[i]);
            say_kind(PKG_LINE_DETAIL, "  %s\n", "author     %s", line);
        }
        if (ab->license) say_kind(PKG_LINE_DETAIL, "  %s\n", "license    %s%s%s", ab->license,
                                  ab->distribution ? ", " : "", ab->distribution ? ab->distribution : "");
        if (ab->homepage) say_kind(PKG_LINE_DETAIL, "  %s\n", "homepage   %s", ab->homepage);
        if (ab->repository) say_kind(PKG_LINE_DETAIL, "  %s\n", "repository %s", ab->repository);
        if (ab->description.n) say_kind(PKG_LINE_DETAIL, "%s\n", "%s", "");
        for (i = 0; i < ab->description.n; i++)
            say_kind(PKG_LINE_DETAIL, "  %s\n", "%s", ab->description.v[i]);
        if (ab->changes.n) say_kind(PKG_LINE_DETAIL, "  %s\n", "changes in %s:", m.version);
        for (i = 0; i < ab->changes.n; i++)
            say_kind(PKG_LINE_DETAIL, "    %s\n", "%s", ab->changes.v[i]);
    }
    pkg_manifest_free(&m);
}

static int cmd_show(const struct pkg_options *a)
{
    struct index ix;
    struct show_row *row = NULL;
    size_t i, shown = 0, bad = 0;
    int first_bad = 0;

    if (open_channels(a, &ix) != 0) return 1;
    row = (struct show_row *)calloc(ix.n ? ix.n : 1, sizeof *row);
    if (row == NULL) { free(ix.e); return refuse("out of memory"); }
    kv("result", "shown");

    /* 1. Each entry: manifest against the index, signature, withdrawal, and
     *    the payload of a .pkg package. */
    show_defers_archives = 1;
    for (i = 0; i < ix.n; i++) {
        struct show_row *r = &row[i];
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        if (cancelled("while checking the channel, which was not changed")) {
            show_defers_archives = 0;
            for (i = 0; i < ix.n; i++) if (row[i].selected) fetched_free(&row[i].f);
            free(row); free(ix.e);
            return 1;
        }
        quiet = 1;
        refused_class = 0;
        quiet_reason[0] = '\0';
        r->rc = fetch(chan_of(&ix.e[i]), &ix.e[i], &r->f);
        quiet = 0;
        r->cls = refused_class;
        snprintf(r->reason, sizeof r->reason, "%s", quiet_reason);
        refused_class = 0;
        r->selected = 1;
        if (a->archive != NULL) {
            /* only what this archive holds */
            char an[1024], ap[1024];
            if (r->rc != 0 || r->f.m.source == NULL
                || !pkg_archive_split(r->f.m.source, an, sizeof an, ap, sizeof ap)
                || strcmp(an, a->archive) != 0) {
                fetched_free(&r->f);
                r->selected = 0;
            }
        }
    }
    show_defers_archives = 0;

    /* 2. Each archive once: every file of every entry that names it. */
    if (!a->metadata) {
        size_t r0;
        for (r0 = 0; r0 < ix.n; r0++) {
            char an[1024], ap[1024], *path;
            struct arch_check ac;
            size_t n = 0, cap = 0, r1, fl;
            char err[300];
            if (!row[r0].selected || row[r0].rc != 0 || row[r0].f.m.source == NULL
                || row[r0].archive_checked)
                continue;
            pkg_archive_split(row[r0].f.m.source, an, sizeof an, ap, sizeof ap);
            memset(&ac, 0, sizeof ac);
            for (r1 = r0; r1 < ix.n; r1++) {
                char bn[1024], bp[1024];
                struct pkg_manifest *m = &row[r1].f.m;
                if (!row[r1].selected || row[r1].rc != 0 || m->source == NULL
                    || !pkg_archive_split(m->source, bn, sizeof bn, bp, sizeof bp) || strcmp(bn, an) != 0)
                    continue;
                row[r1].archive_checked = 1;
                for (fl = 0; fl < m->nfiles; fl++) {
                    size_t pl = strlen(bp) + strlen(m->files[fl].path) + 2;
                    if (n == cap) {
                        struct expect *g;
                        cap = cap ? cap * 2 : 256;
                        g = (struct expect *)realloc(ac.ex, cap * sizeof *g);
                        if (g == NULL) break;
                        ac.ex = g;
                    }
                    ac.ex[n].owned = (char *)malloc(pl);
                    if (ac.ex[n].owned == NULL) break;
                    snprintf(ac.ex[n].owned, pl, "%s%s%s", bp, bp[0] ? "/" : "", m->files[fl].path);
                    ac.ex[n].path = ac.ex[n].owned;
                    ac.ex[n].digest = m->files[fl].digest;
                    ac.ex[n].size = m->files[fl].size;
                    ac.ex[n].row = r1;
                    ac.ex[n].seen = 0;
                    n++;
                }
            }
            ac.n = n;
            qsort(ac.ex, n, sizeof *ac.ex, by_expect);
            path = archive_path(chan_of(&ix.e[r0]), an);
            if ((path == NULL || !pkg_fs_exists(path)) && n > 0 && row[ac.ex[0].row].f.m.archive_sha) {
                /* published upstream: a copy downloaded before, else nothing to read here */
                const struct pkg_manifest *um = &row[ac.ex[0].row].f.m;
                char *cd = pkg_cache_dir(), rel[1200];
                free(path);
                snprintf(rel, sizeof rel, "upstream/%s/%s", um->archive_sha, an);
                path = cd ? pkg_join(cd, rel) : NULL;
                free(cd);
                if (path == NULL || !pkg_fs_exists(path)) {
                    for (fl = 0; fl < n; fl++) {
                        row[ac.ex[fl].row].upstream_only = 1;
                        ac.ex[fl].seen = 1;
                    }
                }
            }
            tr("checking %lu files of %s in one read", (unsigned long)n, an);
            if (n > 0 && row[ac.ex[0].row].upstream_only) {
                tr("%s is published upstream and not downloaded here: not read", an);
            } else if (path == NULL || !pkg_fs_exists(path)) {
                for (fl = 0; fl < n; fl++) ac.ex[fl].seen = 3;
            } else if (pkg_archive_walk(path, ac_want, ac_data, &ac, err, sizeof err) != 0) {
                for (fl = 0; fl < n; fl++) if (ac.ex[fl].seen == 0) ac.ex[fl].seen = 4;
            }
            for (fl = 0; fl < n; fl++) {
                struct show_row *r = &row[ac.ex[fl].row];
                if (ac.ex[fl].seen == 1 || r->rc != 0)
                    continue;
                r->rc = 1;
                if (ac.ex[fl].seen == 3) {
                    r->cls = 11;
                    snprintf(r->reason, sizeof r->reason, "it comes from the archive %s, which the "
                             "channel does not have", an);
                } else {
                    r->cls = 12;
                    snprintf(r->reason, sizeof r->reason, "the archive %s %s %s, which the signed "
                             "manifest lists", an, ac.ex[fl].seen == 2 ? "holds a different" :
                             ac.ex[fl].seen == 4 ? "could not be read for" : "lacks", ac.ex[fl].path);
                }
            }
            for (fl = 0; fl < n; fl++) free(ac.ex[fl].owned);
            free(ac.ex);
            free(path);
        }
    }

    /* 3. What was found, entry by entry. */
    {
        static const int widths[] = { 20, 8, 11, 8, 10, 0 };
        if (!machine && ix.n > 0)
            tbl_head(widths, "Package\tVersion\tKind\tArch\tStatus\tSigner");
    }
    for (i = 0; i < ix.n; i++) {
        struct show_row *r = &row[i];
        struct fetched *fp = &r->f;
        const char *status = "ok", *here = NULL;
        int rc = r->rc;
        if (!r->selected)
            continue;
        shown++;
        if (rc != 0) {
            status = class_name(r->cls);
            bad++;
            if (!first_bad) first_bad = r->cls;
        } else if (ix.e[i].withdrawn) {
            status = "withdrawn";
        }
        if (a->root != NULL) {
            /* Against a root: is this the version installed there? */
            struct pkg_manifest cur;
            int q = quiet;
            quiet = 1;
            if (load_installed(a->root, ix.e[i].name, &cur, 1) == 0) {
                here = pkg_version_cmp(cur.version, ix.e[i].version) == 0
                       && (strcmp(cur.architecture, ix.e[i].arch) == 0
                           || strcmp(ix.e[i].arch, "generic") == 0) ? "installed"
                       : "other-version";
                pkg_manifest_free(&cur);
            } else {
                here = "no";
            }
            quiet = q;
            refused_class = 0;
        }
        if (machine) {
            char j[400];
            snprintf(j, sizeof j, "%s %s %s %s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                     rc == 0 ? fp->m.kind : "-", rc == 0 ? fp->m.architecture : "-", status,
                     rc == 0 ? fp->signer : "-", here ? " " : "", here ? here : "");
            rec_item("entry", j, "name", ix.e[i].name, "version", ix.e[i].version,
                     "kind", rc == 0 ? fp->m.kind : "-", "architecture", rc == 0 ? fp->m.architecture : "-",
                     "status", status, "signer", rc == 0 ? fp->signer : "-",
                     here ? "installed" : NULL, here, NULL);
            if (rc == 0 && fp->m.source != NULL && a->metadata) {
                snprintf(j, sizeof j, "%s %s unchecked", ix.e[i].name, ix.e[i].version);
                rec_item("archive", j, "package", ix.e[i].name, "version", ix.e[i].version,
                         "state", "unchecked", NULL);
            } else if (rc == 0 && r->upstream_only) {
                snprintf(j, sizeof j, "%s %s upstream %s", ix.e[i].name, ix.e[i].version,
                         fp->m.archive_url);
                rec_item("archive", j, "package", ix.e[i].name, "version", ix.e[i].version,
                         "state", "upstream", "url", fp->m.archive_url, NULL);
            }
            if (rc == 0) {
                size_t d;
                for (d = 0; d < fp->m.ndeps; d++) {
                    snprintf(j, sizeof j, "%s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                             fp->m.deps[d].name, fp->m.deps[d].min ? " >= " : "",
                             fp->m.deps[d].min ? fp->m.deps[d].min : "");
                    rec_item("depends", j, "package", ix.e[i].name, "version", ix.e[i].version,
                             "needs", fp->m.deps[d].name, "min", fp->m.deps[d].min ? fp->m.deps[d].min : "",
                             NULL);
                }
            } else {
                char jp[2600];
                snprintf(jp, sizeof jp, "%s %s %s", ix.e[i].name, ix.e[i].version, r->reason);
                rec_item("problem", jp, "package", ix.e[i].name, "version", ix.e[i].version,
                         "reason", r->reason, NULL);
            }
        } else {
            tbl_row("%s\t%s\t%s\t%s\t%s\t%.16s%s", ix.e[i].name, ix.e[i].version,
                    rc == 0 ? fp->m.kind : "-", rc == 0 ? fp->m.architecture : "-", status,
                    rc == 0 ? fp->signer : "-",
                    rc == 0 && fp->m.source != NULL && a->metadata ? "  (archive not checked)" : "");
            if (rc != 0)
                say_kind(PKG_LINE_DETAIL, "  %s\n", "%s", r->reason);
        }
        if (rc == 0 && fp->m.ignored.n > 0) {
            /* lines for other systems: kept, signed, not acted on; a typo shows here */
            char keys[600];
            size_t k, at = 0;
            for (k = 0; k < fp->m.ignored.n && at < sizeof keys; k++)
                at += (size_t)snprintf(keys + at, sizeof keys - at, "%s%s", k ? ", " : "", fp->m.ignored.v[k]);
            if (machine) {
                for (k = 0; k < fp->m.ignored.n; k++)
                    kv("ignored", "%s %s %s", ix.e[i].name, ix.e[i].version, fp->m.ignored.v[k]);
            } else {
                say_kind(PKG_LINE_DETAIL, "  %s\n", "ignored, for other systems: %s", keys);
            }
        }
        fetched_free(fp);
    }
    if (!machine && ix.n > 0)
        tbl_end();
    free(row);
    for (i = 0; i < ix.n; i++) {
        char others[600], claim[65];
        size_t j;
        int earlier = 0;
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        for (j = 0; j < i; j++)
            if (strcmp(ix.e[j].name, ix.e[i].name) == 0) earlier = 1;
        if (earlier || !claimed_signer(chan_of(&ix.e[i]), ix.e[i].digest, claim))
            continue;
        if (other_signers(&ix, ix.e[i].name, claim, others, sizeof others) > 0) {
            if (machine)
                kv("warning", "%s is signed by more than one key: %s on %s, and %s", ix.e[i].name,
                   claim, ix.e[i].version, others);
            else
                say_kind(PKG_LINE_WARNING, "  warning: %s\n", "%s is signed by more than one key: %.16s on %s, and %s",
                    ix.e[i].name, claim, ix.e[i].version, others);
        }
    }
    if (a->target != NULL && shown > 0)
        show_about(&ix, a->target);
    kv("count", "%lu", (unsigned long)shown);
    kv("bad", "%lu", (unsigned long)bad);
    if (!machine && shown == 0)
        say_result("%s%s offers nothing%s%s", a->channel, "", a->target ? " named " : "",
                a->target ? a->target : "");
    if (shown == 0 && a->target != NULL)
        hint("no package is published as %s in this channel: before a first publish, the "
             "name is free; otherwise check the spelling with SHOW CHANNEL alone", a->target);
    free(ix.e);
    refused_class = first_bad;
    if (bad == 0)
        return 0;
    refused_next = next_default(first_bad);
    if (!machine)
        say_err("pkg show: %lu of %lu entries fail their checks\n  next: %s\n",
                (unsigned long)bad, (unsigned long)shown, next_words(refused_next));
    else
        kv("next", "%s", refused_next);
    return 1;
}

/* The image alone, for a person who wants the file without a channel. */
static int cmd_image(const struct pkg_options *a)
{
    struct drawer d;
    unsigned skipped = 0;
    const char *vol;

    if (a->target == NULL) return refuse_c(20, "name the drawer to turn into an image");
    if (a->out == NULL) return refuse_c(20, "name the image file with OUT <file>");
    if (!pkg_fs_is_dir(a->target)) return refuse_c(20, "\"%s\" is not a directory", a->target);
    memset(&d, 0, sizeof d);
    d.root = a->target;
    if (pkg_fs_walk(a->target, load_one, leave_out, &d, &skipped, d.err, sizeof d.err) != 0) {
        refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
        drawer_free(&d);
        return 1;
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);
    if (drawer_attrs(&d) != 0) { drawer_free(&d); return 1; }
    {
        char vn[65], vv[64], seen[400];
        const char *vf;
        vol = a->name;
        if (vol == NULL && find_ver(&d, NULL, vn, sizeof vn, vv, sizeof vv, &vf, seen, sizeof seen) > 0) {
            static char named[65];
            snprintf(named, sizeof named, "%s", vn);
            vol = named;
        }
        if (vol == NULL)
            vol = "image";
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->target);
    }
    if (to_image(&d, vol) != 0) { drawer_free(&d); return 1; }
    if (pkg_fs_write_atomic(a->out, d.v[0].data, d.v[0].len) != 0) {
        drawer_free(&d);
        return refuse_c(17, "cannot write \"%s\": %s", a->out, strerror(errno));
    }
    kv("result", "created");
    kv("file", "%s", a->out);
    kv("volume", "%s", vol);
    kv("blocks", "%lu", (unsigned long)(d.v[0].len / PKG_IMAGE_BLOCK));
    if (!machine)
        say_result("wrote %s: volume %s, %lu blocks of %u bytes", a->out, vol,
                (unsigned long)(d.v[0].len / PKG_IMAGE_BLOCK), PKG_IMAGE_BLOCK);
    drawer_free(&d);
    return 0;
}

static int cmd_manifest(const struct pkg_options *a)
{
    struct built b;
    if (build(a, &b) != 0) { built_free(&b); return 1; }
    if (machine) {
        /* The manifest is already "Key: value" lines: one field each. */
        size_t at = 0;
        kv("result", "shown");
        while (at < b.text_len) {
            const char *ls = b.text + at, *nl = memchr(ls, '\n', b.text_len - at);
            size_t ll = nl ? (size_t)(nl - ls) : b.text_len - at;
            const char *colon = memchr(ls, ':', ll);
            if (colon != NULL && colon + 2 <= ls + ll) {
                char key[32];
                size_t kl = (size_t)(colon - ls);
                if (kl < sizeof key) {
                    memcpy(key, ls, kl);
                    key[kl] = '\0';
                    kv(key, "%.*s", (int)(ll - kl - 2), colon + 2);
                }
            }
            at += ll + 1;
        }
    } else {
        say_raw(b.text, b.text_len);
    }
    built_free(&b);
    return 0;
}

/* 1 when the file at `path` holds exactly the bytes whose SHA-256 is `digest`. */
static int object_good(const char *path, const char *digest)
{
    unsigned char *buf;
    size_t len;
    char hex[PKG_SHA256_HEXLEN + 1];
    if (path == NULL || pkg_fs_read(path, &buf, &len) != 0)
        return 0;
    pkg_sha256_hex(buf, len, hex);
    free(buf);
    return strcmp(hex, digest) == 0;
}

/* The same drawer published again as the same version. Nothing to do when
 * the channel holds it intact; when its manifest, payload or signature is
 * missing or damaged, and the key is the publisher's (the key of the
 * package's first version here), those objects are written again: the bytes
 * are the ones the index already names, so nothing published changes. */
static int republish(const struct pkg_options *a, const struct index *ix, const struct built *b,
                     const struct key *k, const char *mdigest)
{
    char *mo = object_path(a->channel, mdigest, "manifest");
    char *po = b->m.payload ? object_path(a->channel, b->m.payload, "pkg") : NULL;
    char *so = object_path(a->channel, mdigest, "sig");
    char signer[65], first_signer[65];
    const struct entry *oldest = NULL;
    int good_m, good_p, good_s = 0, rc = 0;
    size_t o;

    good_m = object_good(mo, mdigest);
    good_p = b->m.payload ? object_good(po, b->m.payload) : 1;   /* else the archive holds it */
    if (so != NULL) {
        int q = quiet;
        quiet = 1;
        good_s = check_sig(so, (const unsigned char *)b->text, b->text_len, signer, "") == 0;
        quiet = q;
        refused_class = 0;
        refused_next = NULL;
    }
    if (good_m && good_p && good_s) {
        kv("result", "unchanged");
        kv("name", "%s", b->m.name);
        kv("version", "%s", b->m.version);
        if (!machine)
            say_result("%s %s is already published with this exact content; nothing to do",
                b->m.name, b->m.version);
        goto out;
    }
    for (o = 0; o < ix->n; o++)
        if (strcmp(ix->e[o].name, b->m.name) == 0
            && (oldest == NULL || pkg_version_cmp(ix->e[o].version, oldest->version) < 0))
            oldest = &ix->e[o];
    if (oldest == NULL || !claimed_signer(a->channel, oldest->digest, first_signer)
        || strcmp(first_signer, k->pkhex) != 0) {
        rc = refuse_n(14, "ask-requester", "%s %s in %s is damaged, and only its publisher's key "
                      "(the key of its first version here) may write it again; this key is %s. "
                      "Nothing was changed", b->m.name, b->m.version, a->channel, k->pkhex);
        goto out;
    }
    tr("repairing %s %s: manifest %s, payload %s, signature %s", b->m.name, b->m.version,
       good_m ? "intact" : "damaged", good_p ? "intact" : "damaged", good_s ? "intact" : "damaged");
    if (dryrun) {
        kv("result", "would-repair");
    } else {
        if ((!good_m && pkg_fs_write_atomic(mo, b->text, b->text_len) != 0)
            || (!good_p && pkg_fs_write_atomic(po, b->pkg, b->pkg_len) != 0)
            || (!good_s && write_sig(so, k, (const unsigned char *)b->text, b->text_len) != 0)) {
            rc = refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
            goto out;
        }
        kv("result", "repaired");
    }
    kv("name", "%s", b->m.name);
    kv("version", "%s", b->m.version);
    if (!good_m) kv("repaired", "manifest");
    if (!good_p) kv("repaired", "payload");
    if (!good_s) kv("repaired", "signature");
    if (!machine)
        say_result("%s %s %s in %s:%s%s%s written again from the same bytes",
            dryrun ? "would repair" : "repaired", b->m.name, b->m.version, a->channel,
            good_m ? "" : " manifest", good_p ? "" : " payload", good_s ? "" : " signature");
out:
    free(mo); free(po); free(so);
    return rc;
}

/* A new version against the highest one published: the files that changed,
 * and the slips that show there (the old build again, a $VER not raised). */
/* Whether two versions say the same about themselves: dependencies,
 * Provides and the catalogue fields, all but files and identity. */
static int same_description(const struct pkg_manifest *x, const struct pkg_manifest *y)
{
    struct pkg_manifest tx = *x, ty = *y;
    char *ox = NULL, *oy = NULL;
    size_t lx = 0, ly = 0;
    int same;
    tx.version = ty.version = (char *)"0";
    tx.payload = ty.payload = NULL;
    tx.source = ty.source = NULL;
    tx.archive_sha = ty.archive_sha = NULL;
    tx.nfiles = ty.nfiles = 0;
    tx.ncontent = ty.ncontent = 0;
    if (pkg_manifest_emit(&tx, &ox, &lx) != 0 || pkg_manifest_emit(&ty, &oy, &ly) != 0) {
        free(ox);
        free(oy);
        return 0;
    }
    same = lx == ly && memcmp(ox, oy, lx) == 0;
    free(ox);
    free(oy);
    return same;
}

static long compare_last(const struct pkg_manifest *em, const struct built *b)
{
    const struct pkg_file *nv = b->m.ncontent ? b->m.content : b->m.files;
    const struct pkg_file *ov = em->ncontent ? em->content : em->files;
    size_t nn = b->m.ncontent ? b->m.ncontent : b->m.nfiles;
    size_t on = em->ncontent ? em->ncontent : em->nfiles;
    size_t i, j, changed = 0;
    int comparable = (b->m.ncontent > 0) == (em->ncontent > 0);

    if (pkg_version_cmp(b->m.version, em->version) == 0)
        return -1;
    if (b->cookie_ver[0] && pkg_version_cmp(b->cookie_ver, em->version) == 0)
        warn("the $VER cookie in %s says %s, the version already published: this is the %s build "
             "again, changed or not, or a new build whose $VER was not raised. Ask which before "
             "publishing it as %s", b->cookie_file, em->version, em->version, b->m.version);
    if (!comparable) {
        if (dryrun)
            hint("%s %s was published before its image listed its files; nothing to compare "
                 "file by file", em->name, em->version);
        return -1;
    }
    if (dryrun)
        kv("compared-with", "%s %s", em->name, em->version);
    for (i = 0; i < nn; i++) {
        for (j = 0; j < on; j++)
            if (strcmp(nv[i].path, ov[j].path) == 0)
                break;
        if (j == on) {
            changed++;
            if (dryrun) kv("added", "%s %llu", nv[i].path, nv[i].size);
        } else if (strcmp(nv[i].digest, ov[j].digest) != 0 || nv[i].prot != ov[j].prot
                   || strcmp(nv[i].comment ? nv[i].comment : "", ov[j].comment ? ov[j].comment : "") != 0) {
            changed++;
            if (dryrun) kv("changed", "%s %llu %llu", nv[i].path, ov[j].size, nv[i].size);
        } else if (dryrun) {
            kv("same", "%s", nv[i].path);
        }
    }
    for (j = 0; j < on; j++) {
        for (i = 0; i < nn; i++)
            if (strcmp(nv[i].path, ov[j].path) == 0)
                break;
        if (i == nn) {
            changed++;
            if (dryrun) kv("gone", "%s", ov[j].path);
        }
    }
    if (changed == 0 && b->m.source == NULL)
        warn("every file is identical to %s %s's: nothing changed but the version number",
             em->name, em->version);
    return (long)changed;
}

static int cmd_publish(const struct pkg_options *a)
{
    struct built b;
    struct index ix;
    struct key k;
    size_t i;
    char *po = NULL, *mo = NULL, *so = NULL, s12[13];
    char mdigest[PKG_SHA256_HEXLEN + 1];
    int rc = 1, new_channel, keyless = 0;
    char same_as[160] = "";

    if (a->channel == NULL)
        return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (is_url(a->channel))
        return refuse_c(20, "PUBLISH writes into a channel on this machine; publish into a "
                        "directory, then PUSH it to %s", a->channel);
    new_channel = !pkg_fs_exists(a->channel);
    memset(&k, 0, sizeof k);
    if (a->sign == NULL && dryrun) {
        /* A preview needs no key: nobody should pick up someone else's key
         * only to see what a publish would do. */
        keyless = 1;
        snprintf(k.pkhex, sizeof k.pkhex, "none");
    } else if (load_key(a->sign, &k) != 0)
        return 1;
    if (read_index(a->channel, &ix) != 0) return 1;
    inherit_ix = &ix;
    inherit_channel = a->channel;
    rc = build(a, &b);
    inherit_ix = NULL;
    inherit_channel = NULL;
    if (rc != 0) { free(ix.e); built_free(&b); return 1; }
    rc = 1;
    if (b.m.source != NULL && b.m.archive_sha == NULL) {
        /* The files stay in the archive; installers find it in the channel. */
        char an[1024], ai[1024], *ap;
        pkg_archive_split(b.m.source, an, sizeof an, ai, sizeof ai);
        ap = archive_path(a->channel, an);
        if (ap == NULL || !pkg_fs_exists(ap)) {
            refuse_c(20, "the files of %s come from %s, which installers look for as %s; put the "
                     "archive there (a copy or a link) and publish again", b.m.name, an,
                     ap ? ap : "archives/<name> in the channel");
            free(ap);
            free(ix.e);
            built_free(&b);
            return 1;
        }
        free(ap);
    }
    if (strcmp(b.m.architecture, "generic") == 0
        && (strcmp(b.m.kind, "image") == 0 || strcmp(b.m.kind, "application") == 0
            || strcmp(b.m.kind, "library") == 0 || strcmp(b.m.kind, "device") == 0
            || strcmp(b.m.kind, "class") == 0))
        warn("no executable in the drawer: kind %s is usually a program, and pkg found no ELF or "
             "hunk header, so it is published as generic, for every CPU. Check the drawer holds "
             "the build, not a script or a placeholder", b.m.kind);
    /* A version is named by its manifest, which names its payload. */
    pkg_sha256_hex(b.text, b.text_len, mdigest);

    for (i = 0; i < ix.n; i++) {
        if (strcmp(ix.e[i].name, b.m.name) == 0
            && pkg_version_cmp(ix.e[i].version, b.m.version) == 0
            && strcmp(ix.e[i].arch, b.m.architecture) == 0) {
            if (strcmp(ix.e[i].digest, mdigest) == 0) {
                rc = republish(a, &ix, &b, &k, mdigest);
            } else {
                if (b.ver_from[0])
                    refuse_c(15, "%s %s is already published with a different payload, and %s "
                             "came from the $VER cookie in %s: either this is a new build whose "
                             "$VER was not raised (raise it, or give VERSION), or it is the old "
                             "build, changed; check which before publishing",
                             b.m.name, ix.e[i].version, ix.e[i].version, b.ver_from);
                else
                    refuse_c(15, "%s %s is already published with a different payload; a "
                             "published version never changes, so publish this as a new version",
                             b.m.name, ix.e[i].version);
            }
            free(ix.e);
            built_free(&b);
            return rc;
        }
    }

    {
        /* A later version signed by another key than the channel's first
         * version of the package is refused by every root that trusts that
         * key. Changing keys is the publisher's decision, stated with
         * ACCEPTKEY and the new key. */
        const struct entry *oldest = NULL;
        char first_signer[65];
        size_t o;
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, b.m.name) == 0
                && (oldest == NULL || pkg_version_cmp(ix.e[o].version, oldest->version) < 0))
                oldest = &ix.e[o];
        if (keyless && oldest != NULL && claimed_signer(a->channel, oldest->digest, first_signer)) {
            kv("first-signer", "%s", first_signer);
            hint("the real publish must be signed with the key that signed %s %s, %s: find out "
                 "from whoever requested this whose key that is and whether it is theirs to use",
                 b.m.name, oldest->version, first_signer);
        } else if (keyless)
            hint("the real publish needs a key: the requester's, if they have published before");
        if (!keyless && oldest != NULL && claimed_signer(a->channel, oldest->digest, first_signer)
            && strcmp(first_signer, k.pkhex) != 0
            && (a->acceptkey == NULL || strcmp(a->acceptkey, k.pkhex) != 0)) {
            kv("signer", "%s", k.pkhex);
            kv("first-signer", "%s", first_signer);
            refuse_n(14, "ask-requester", "%s %s, the first version in %s, is signed by %s, and this "
                     "key is %s: every machine that trusts the first key would refuse this "
                     "version. Sign it with the publisher's key; changing keys is the requester's "
                     "decision. Nothing was published", b.m.name, oldest->version, a->channel,
                     first_signer, k.pkhex);
            free(ix.e);
            built_free(&b);
            memset(&k, 0, sizeof k);
            return 1;
        }
    }
    {
        /* Compared with the highest version already published: a kind that
         * changes (files installed loose, then an image) or a dependency that
         * disappears is usually a slip, and it is said before it ships. */
        size_t o, d2, d3;
        const struct entry *last = NULL;
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, b.m.name) == 0
                && (last == NULL || pkg_version_cmp(ix.e[o].version, last->version) > 0))
                last = &ix.e[o];
        if (last != NULL) {
            char *mp = object_path(a->channel, last->digest, "manifest"), err[200];
            unsigned char *mbuf;
            size_t mlen;
            struct pkg_manifest em;
            if (mp != NULL && pkg_fs_read(mp, &mbuf, &mlen) == 0) {
                if (pkg_manifest_parse((const char *)mbuf, mlen, &em, err, sizeof err) == 0) {
                    if (strcmp(em.kind, b.m.kind) != 0 && a->name == NULL) {
                        /* The name came from a $VER cookie: another program's
                         * cookie would make this package replace that one. */
                        refuse_c(20, "the name %s comes from the $VER cookie in %s, and %s is "
                                 "already published as kind %s, not %s: this would replace it "
                                 "wherever it is installed. If the drawer holds the right "
                                 "program, add NAME <its package name>; nothing was published",
                                 b.m.name, b.name_from[0] ? b.name_from : "the drawer",
                                 em.name, em.kind, b.m.kind);
                        pkg_manifest_free(&em);
                        free(mbuf);
                        free(mp);
                        free(ix.e);
                        built_free(&b);
                        memset(&k, 0, sizeof k);
                        return 1;
                    }
                    if (compare_last(&em, &b) == 0 && a->build != NULL
                        && strcmp(em.kind, b.m.kind) == 0 && strcmp(em.architecture, b.m.architecture) == 0
                        && same_description(&em, &b.m)) {
                        /* A new build of the same files is no new version. */
                        snprintf(same_as, sizeof same_as, "%s %s", em.name, em.version);
                    }
                    if (strcmp(em.kind, b.m.kind) != 0)
                        warn("%s %s was published as kind %s, and this version is kind %s",
                             em.name, em.version, em.kind, b.m.kind);
                    for (d2 = 0; d2 < em.ndeps; d2++) {
                        int kept_dep = 0;
                        for (d3 = 0; d3 < b.m.ndeps; d3++)
                            if (strcmp(em.deps[d2].name, b.m.deps[d3].name) == 0)
                                kept_dep = 1;
                        if (!kept_dep)
                            warn("%s %s depends on %s, and this version does not: DEPENDS is "
                                 "not carried from one version to the next", em.name,
                                 em.version, em.deps[d2].name);
                    }
                    pkg_manifest_free(&em);
                }
                free(mbuf);
            }
            free(mp);
        }
    }
    if (same_as[0]) {
        kv("result", "unchanged");
        kv("name", "%s", b.m.name);
        kv("version", "%s", b.m.version);
        kv("same-as", "%s", same_as);
        if (!machine)
            say_result("%s %s: every file is that of %s, so no new version is published", b.m.name,
                b.m.version, same_as);
        rc = 0;
        goto out;
    }
    if (dryrun) {
        size_t d;
        kv("result", "would-publish");
        kv("name", "%s", b.m.name);
        kv("version", "%s", b.m.version);
        kv("kind", "%s", b.m.kind);
        kv("architecture", "%s", b.m.architecture);
        kv("channel", "%s", a->channel);
        if (b.m.ndeps == 0)
            kv("depends", "none");
        for (d = 0; d < b.m.ndeps; d++)
            kv("depends", "%s%s%s", b.m.deps[d].name, b.m.deps[d].min ? " >= " : "",
               b.m.deps[d].min ? b.m.deps[d].min : "");
        for (d = 0; d < b.m.nfiles; d++)
            kv("file", "%s %llu", b.m.files[d].path, b.m.files[d].size);
        for (d = 0; d < b.m.ncontent; d++)
            kv("content", "%s %llu", b.m.content[d].path, b.m.content[d].size);
        kv("signer", "%s", k.pkhex);
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.name_from[0]) kv("name-from", "%s", b.name_from);
        if (b.kind_from[0]) kv("kind-from", "%s", b.kind_from);
        if (b.nconfig) kv("config-files", "%lu", (unsigned long)b.nconfig);
        if (b.config_from[0]) kv("config-from", "%s", b.config_from);
        if (b.about_from[0]) kv("about-from", "%s", b.about_from);
        if (b.info_from[0]) kv("info-from", "%s", b.info_from);
        if (b.info_fields[0]) kv("info-fields", "%s", b.info_fields);
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
        if (b.arch_from[0]) kv("arch-from", "%s", b.arch_from);
        for (d = 0; d < b.nleft; d++)
            kv("left-out", "%s", b.left_out[d]);
        if (!machine) {
            say_result("would publish %s %s (%s, %s) to %s, signed by %.16s", b.m.name, b.m.version,
                    b.m.kind, b.m.architecture, a->channel, k.pkhex);
            for (d = 0; d < b.m.ndeps; d++)
                say_item("depends", "%s%s%s", b.m.deps[d].name, b.m.deps[d].min ? " >= " : "",
                        b.m.deps[d].min ? b.m.deps[d].min : "");
            if (b.m.ndeps == 0)
                say_item("depends", "nothing");
            for (d = 0; d < b.m.nfiles; d++)
                say_item("file", "%s (%llu bytes)", b.m.files[d].path, b.m.files[d].size);
            for (d = 0; d < b.m.ncontent; d++)
                say_item("content", "%s (%llu bytes)", b.m.content[d].path, b.m.content[d].size);
            for (d = 0; d < b.nleft; d++)
                say_item("left out", "%s", b.left_out[d]);
            if (b.ver_from[0] || b.name_from[0])
                say_detail("%s from $VER: in %s", b.ver_from[0] && b.name_from[0] ? "name and version"
                    : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
            if (b.kind_from[0] || b.deps_from[0])
                say_detail("%s from %s, published before", b.kind_from[0] && b.deps_from[0] ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies", b.kind_from[0] ? b.kind_from : b.deps_from);
            say_info_from(&b);
        }
        if (new_channel)
            hint("there is no channel at %s yet: publishing creates it. Check it is the one "
                 "meant", a->channel);
        rc = 0;
        goto out;
    }
    po = b.m.payload ? object_path(a->channel, b.m.payload, "pkg")   /* content-addressed */
                     : pkg_strdup("");                                 /* the archive holds it */
    mo = object_path(a->channel, mdigest, "manifest");      /* one per version */
    so = object_path(a->channel, mdigest, "sig");
    if (po == NULL || mo == NULL || so == NULL
        || (b.m.payload && pkg_fs_write_atomic(po, b.pkg, b.pkg_len) != 0)
        || pkg_fs_write_atomic(mo, b.text, b.text_len) != 0
        || write_sig(so, &k, (const unsigned char *)b.text, b.text_len) != 0) {
        refuse_c(17, "cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
        goto out;
    }
    {
        struct entry *w = (struct entry *)realloc(ix.e, (ix.n + 1u) * sizeof *w);
        if (w == NULL) { refuse("out of memory"); goto out; }
        ix.e = w;
        snprintf(ix.e[ix.n].name, sizeof ix.e[ix.n].name, "%s", b.m.name);
        snprintf(ix.e[ix.n].version, sizeof ix.e[ix.n].version, "%s", b.m.version);
        snprintf(ix.e[ix.n].arch, sizeof ix.e[ix.n].arch, "%s", b.m.architecture);
        snprintf(ix.e[ix.n].digest, sizeof ix.e[ix.n].digest, "%s", mdigest);
        ix.n++;
    }
    if (write_index(a->channel, &ix) != 0) {
        refuse_c(17, "cannot write the channel index: %s", strerror(errno));
        goto out;
    }
    if (b.m.payload) short12(b.m.payload, s12);
    kv("result", "published");
    kv("name", "%s", b.m.name);
    kv("version", "%s", b.m.version);
    kv("channel", "%s", a->channel);
    kv("manifest", "%s", mdigest);
    if (b.m.payload) kv("payload", "%s", b.m.payload);
    else kv("source", "%s", b.m.source);
    kv("signer", "%s", k.pkhex);
    kv("files", "%lu", (unsigned long)b.m.nfiles);
    if (!machine)
    say_result("published %s %s to %s: %lu file%s, %s%s, signed by %.16s", b.m.name,
           b.m.version, a->channel, (unsigned long)b.m.nfiles, b.m.nfiles == 1 ? "" : "s",
           b.m.payload ? "payload " : "from ",
           b.m.payload ? s12 : b.m.source, k.pkhex);
    if (b.arch_from[0] && machine)
        kv("arch-from", "%s", b.arch_from);
    else if (b.arch_from[0])
        say_item("architecture", "%s, read from %s", b.m.architecture, b.arch_from);
    if (machine) {
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.name_from[0]) kv("name-from", "%s", b.name_from);
        if (b.kind_from[0]) kv("kind-from", "%s", b.kind_from);
        if (b.nconfig) kv("config-files", "%lu", (unsigned long)b.nconfig);
        if (b.config_from[0]) kv("config-from", "%s", b.config_from);
        if (b.about_from[0]) kv("about-from", "%s", b.about_from);
        if (b.info_from[0]) kv("info-from", "%s", b.info_from);
        if (b.info_fields[0]) kv("info-fields", "%s", b.info_fields);
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
    } else if (b.ver_from[0] || b.name_from[0]) {
        say_detail("%s taken from $VER: in %s", b.ver_from[0] && b.name_from[0] ? "name and version"
            : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
    }
    if (!machine && (b.kind_from[0] || b.deps_from[0]))
        say_detail("%s from %s, published before", b.kind_from[0] && b.deps_from[0]
            ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies",
            b.kind_from[0] ? b.kind_from : b.deps_from);
    if (!machine)
        say_info_from(&b);
    for (i = 0; i < b.nleft; i++) {
        if (machine)
            kv("left-out", "%s", b.left_out[i]);
        else
            say_item("left out", "%s, host metadata no Amiga uses", b.left_out[i]);
    }
    if (new_channel)
        hint("the channel %s did not exist and was created", a->channel);
    /* PUBLISH says "published", and a person who has only ever sent packages
     * to a portal reads that as "it is online now". It is not: the package
     * is in a channel on this disk, which is a channel like any other, and
     * PUSH is what sends it to a portal. */
    if (!dryrun)
        hint("the package is in the local channel %s: any machine that can read that directory "
             "installs from it with INSTALL %s ROOT <root> CHANNEL <that directory, as the "
             "machine names it>, and PUSH CHANNEL %s TO <portal channel> sends it to a portal",
             a->channel, b.m.name, a->channel);
    rc = 0;
out:
    memset(&k, 0, sizeof k);
    free(po); free(mo); free(so);
    free(ix.e);
    built_free(&b);
    return rc;
}

static int need_root_channel(const struct pkg_options *a)
{
    if (a->target == NULL)     return refuse_c(20, "name the package");
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->version && pkg_check_version(a->version))
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    return 0;
}

/* One package. `line` is NULL for INSTALL <name>, which reports for itself;
 * with several names the batch reports instead, and this leaves its sentence
 * there. */
static int install_one(const struct pkg_options *a, const char *target, char *line, size_t ll)
{
    struct index ix;
    const struct entry *e;
    struct plan p;
    struct pkg_manifest cur;
    unsigned long placed, dropped, kept;
    char s12[13];
    int rc = 1;

    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, &ix) != 0) return 1;
    {
        char cpus[200];
        if (arch_ambiguous(&ix, target, cpus, sizeof cpus)) {
            refuse_c(20, "%s is offered for several CPUs (%s), and nothing says which machine %s "
                     "is for: add ARCH <cpu>; the root remembers it from then on", target,
                     cpus, a->root);
            free(ix.e);
            return 1;
        }
    }
    e = pick(&ix, target, a->version);
    if (e == NULL) {
        if (!pick_refused) say_not_found(&ix, target, a->version, chans_text());
        free(ix.e);
        return 1;
    }
    if (load_installed(a->root, e->name, &cur, 1) == 0) {
        if (is_auto(a->root, cur.name)) {
            /* Asked for by name now: no longer an orphan candidate. */
            set_auto(a->root, cur.name, 0);
            if (line != NULL) {
                snprintf(line, ll, "%s was installed as a dependency; it is now kept for itself",
                         cur.version);
            } else {
                kv("result", "kept");
                kv("name", "%s", cur.name);
                kv("version", "%s", cur.version);
                if (!machine)
                    say_result("%s %s was installed as a dependency; it is now kept for itself",
                            cur.name, cur.version);
            }
            pkg_manifest_free(&cur);
            free(ix.e);
            return 0;
        }
        if (pkg_version_cmp(cur.version, e->version) == 0) {
            /* The state asked for is the state there: a repeated INSTALL, as
             * an agent retrying after a timeout sends, succeeds and changes
             * nothing. */
            if (line != NULL) {
                snprintf(line, ll, "%s is already installed", cur.version);
            } else {
                kv("result", "unchanged");
                kv("name", "%s", cur.name);
                kv("version", "%s", cur.version);
                if (!machine)
                    say_result("%s %s is already installed in %s", cur.name, cur.version, a->root);
            }
            pkg_manifest_free(&cur);
            free(ix.e);
            return 0;
        }
        refuse_n(15, pkg_version_cmp(e->version, cur.version) > 0 ? "use-upgrade" : "ask-requester",
                 "%s %s is installed in %s, not %s; nothing was changed",
                 cur.name, cur.version, a->root, e->version);
        pkg_manifest_free(&cur);
        free(ix.e);
        return 1;
    }
    if (plan_target(&p, a, &ix, e->name, e->version) == 0
        && run_plan(&p, NULL, &placed, &dropped, &kept) == 0) {
        const struct fetched *t = &p.f[p.n - 1];
        struct fetched f = *t;
        if (f.m.payload) short12(f.m.payload, s12);
        if (!dryrun)
            record_arch(a->root, p.f[p.n - 1].m.architecture);
        if (line != NULL) {
            snprintf(line, ll, "%s %s: %lu file%s, signed by %.16s",
                     dryrun ? "would install" : "installed", f.m.version, placed,
                     placed == 1 ? "" : "s", f.signer);
        } else {
        kv("result", "%s", res("installed", "would-install"));
        kv("name", "%s", f.m.name);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("files", "%lu", placed);
        if (f.m.payload) kv("payload", "%s", f.m.payload);
        else kv("source", "%s", f.m.source);
        kv("signer", "%s", f.signer);
        if (!machine)
        say_result("%s %s %s into %s: %lu file%s, %s%s, signed by %.16s",
               dryrun ? "would install" : "installed",
               f.m.name, f.m.version, a->root, placed, placed == 1 ? "" : "s",
               f.m.payload ? "payload " : "from ",
               f.m.payload ? s12 : f.m.source, f.signer);
        }
        if (line == NULL && strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1) {
            kv("image", "%s", f.m.files[0].path);
            kv("blocks", "%llu", f.m.files[0].size / PKG_IMAGE_BLOCK);
            if (!machine)
                say_item("image", "%s, %llu blocks", f.m.files[0].path,
                        f.m.files[0].size / PKG_IMAGE_BLOCK);
            hint("to run it, mount the image: MOUNTLIST %s ROOT %s OUT <file> writes the "
                 "mount entry and lists the steps", f.m.name, a->root);
        }
        rc = 0;
    }
    plan_free(&p);
    free(ix.e);
    return rc;
}


/* INSTALL a b c. Every name is tried, whatever the ones before it did, and
 * each is reported; the code is the worst class any of them refused with,
 * so a caller sees the worst that happened, not the first. One archive read
 * serves every name that comes out of it, since the first read leaves the
 * block map behind. */
static int install_many(const struct pkg_options *a)
{
    unsigned i, n = a->nalso + 1, done = 0, bad = 0;
    int worst = 0;
    const char *worst_next = NULL;
    char refused_names[600];
    size_t at = 0;

    refused_names[0] = '\0';
    if (need_root_channel(a) != 0) return 1;
    if (a->version != NULL || a->acceptkey != NULL || a->downgrade)
        return refuse_c(20, "%s is a decision about one package, never about several at once. "
                        "Give it to INSTALL <name> alone",
                        a->version != NULL ? "VERSION" : a->acceptkey != NULL ? "ACCEPTKEY"
                                                                             : "DOWNGRADE");
    for (i = 0; i < n; i++) {
        const char *name = i == 0 ? a->target : a->also[i - 1];
        char line[600];
        int rc;
        line[0] = '\0';
        quiet = 1;
        quiet_reason[0] = '\0';
        rc = install_one(a, name, line, sizeof line);
        quiet = 0;
        if (rc == 0) {
            done++;
            if (machine) {
                char jn[700];
                snprintf(jn, sizeof jn, "%s %s", name, line);
                rec_item("package", jn, "name", name, "result", line, NULL);
            } else {
                say_pkgline(name, "%s", line);
            }
        } else {
            char *q;
            bad++;
            for (q = quiet_reason; *q; q++)
                if (*q == '\n') *q = ' ';
            if (refused_class > worst) { worst = refused_class; worst_next = refused_next; }
            if (machine) {
                char jn[2700], codes[8];
                snprintf(codes, sizeof codes, "%d", refused_class);
                snprintf(jn, sizeof jn, "%s %s %s", name, class_name(refused_class), quiet_reason);
                rec_item("refused", jn, "name", name, "class", class_name(refused_class),
                         "code", codes, "reason", quiet_reason,
                         "next", refused_next ? refused_next : next_default(refused_class), NULL);
            } else {
                say_pkgline(name, "not installed: %s", quiet_reason);
            }
            if (at + 80 < sizeof refused_names)
                at += (size_t)snprintf(refused_names + at, sizeof refused_names - at, "%s%s (%s)",
                                       at ? ", " : "", name, class_name(refused_class));
        }
        refused_class = 0;
        refused_next = NULL;
    }
    {
        char summary[900];
        if (bad == 0)
            snprintf(summary, sizeof summary, "%s %u package%s into %s",
                     dryrun ? "would install" : "installed", done, done == 1 ? "" : "s", a->root);
        else
            snprintf(summary, sizeof summary, "%s %u of %u package%s into %s; not installed: %s. "
                     "Everything else went ahead", dryrun ? "would install" : "installed", done, n,
                     n == 1 ? "" : "s", a->root, refused_names);
        if (bad == 0) {
            kv("result", "%s", res("installed", "would-install"));
        } else {
            refused_class = worst;
            refused_next = worst_next;
            kv("result", "refused");
            kv("class", "%s", class_name(worst));
            kv("code", "%d", worst);
        }
        kv("root", "%s", a->root);
        kv("installed", "%u", done);
        kv("not-installed", "%u", bad);
        kv("count", "%u", done);
        kv("summary", "%s", summary);
        if (bad > 0)
            kv("next", "%s", worst_next ? worst_next : next_default(worst));
        if (!machine)
            say_result("%s", summary);
        return bad == 0 ? 0 : 1;
    }
}

static int cmd_install(const struct pkg_options *a)
{
    if (a->target == NULL)
        return refuse_c(20, "INSTALL takes the name of a package, or several names");
    if (a->nalso > 0)
        return install_many(a);
    return install_one(a, a->target, NULL, 0);
}

static int move_to(const struct pkg_options *a, const struct index *ix, const struct entry *e,
                   struct pkg_manifest *cur, const char *verb)
{
    struct plan p;
    unsigned long placed, dropped, kept;
    int rc = 1;

    if (plan_target(&p, a, ix, e->name, e->version) == 0
        && run_plan(&p, cur, &placed, &dropped, &kept) == 0) {
        struct fetched f = p.f[p.n - 1];
        kv("result", "%s", verb[0] == 'u' ? res("upgraded", "would-upgrade")
                           : verb[0] == 'd' ? res("downgraded", "would-downgrade")
                           : res("rolled-back", "would-roll-back"));
        kv("name", "%s", f.m.name);
        kv("from", "%s", cur->version);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("placed", "%lu", placed);
        kv("removed", "%lu", dropped);
        kv("signer", "%s", f.signer);
        if (!machine) {
        {
        char keptw[40];
        keptw[0] = '\0';
        if (kept) snprintf(keptw, sizeof keptw, ", %lu kept", kept);
        say_result("%s%s %s from %s to %s in %s: %lu placed, %lu removed%s",
               dryrun ? "would have " : "", verb, f.m.name,
               cur->version, f.m.version, a->root, placed, dropped, keptw);
        }
        }
        if (strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1)
            hint("the image %s is replaced: a machine that has it mounted must Eject it %s, "
                 "and MOUNTLIST %s ROOT %s writes the new entry, since its size may change",
                 f.m.files[0].path, dryrun ? "first" : "and mount it again", f.m.name, a->root);
        rc = 0;
    }
    plan_free(&p);
    return rc;
}

static int upgrade_all(const struct pkg_options *a);

static int cmd_upgrade(const struct pkg_options *a)
{
    struct index ix;
    const struct entry *e;
    struct pkg_manifest cur;
    int rc = 1, c;

    if (a->all) return upgrade_all(a);
    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (load_installed(a->root, a->target, &cur, 0) != 0) return 1;
    if (target_arch == NULL && strcmp(cur.architecture, "generic") != 0)
        target_arch = cur.architecture;       /* stay on the installed CPU */
    if (open_channels(a, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    e = pick(&ix, a->target, a->version);
    if (e == NULL) {
        if (!pick_refused) say_not_found(&ix, a->target, a->version, chans_text());
        goto out;
    }
    c = pkg_version_cmp(e->version, cur.version);
    if (c == 0) {
        size_t o;
        kv("result", "unchanged");
        kv("name", "%s", cur.name);
        kv("version", "%s", cur.version);
        if (!machine)
            say_result("%s is already at %s", cur.name, cur.version);
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, cur.name) == 0 && !arch_matches(&ix.e[o])
                && pkg_version_cmp(ix.e[o].version, cur.version) > 0 && !ix.e[o].withdrawn) {
                if (machine)
                    kv("note", "%s %s is published for %s, not for this root's CPU", ix.e[o].name,
                       ix.e[o].version, ix.e[o].arch);
                else
                    say_detail("%s %s is published for %s, not yet for this root's CPU",
                        ix.e[o].name, ix.e[o].version, ix.e[o].arch);
            }
        rc = 0;
        goto out;
    }
    if (c < 0 && !a->downgrade) {
        refuse_c(18, "%s %s is older than the installed %s; nothing was changed. Going back "
               "a version is the requester's decision", e->name, e->version, cur.version);
        goto out;
    }
    rc = move_to(a, &ix, e, &cur, c < 0 ? "downgraded" : "upgraded");
out:
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

static int cmd_rollback(const struct pkg_options *a)
{
    struct pkg_manifest cur;
    struct index ix;
    struct entry prev;
    const struct entry *e = NULL;
    char *pp;
    unsigned char *buf;
    size_t len, i;
    int rc = 1;

    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (load_installed(a->root, a->target, &cur, 0) != 0) return 1;
    if (target_arch == NULL && strcmp(cur.architecture, "generic") != 0)
        target_arch = cur.architecture;       /* back on the installed CPU */
    pp = root_path(a->root, "prev", a->target);
    if (pp == NULL || pkg_fs_read(pp, &buf, &len) != 0) {
        free(pp);
        pkg_manifest_free(&cur);
        return refuse_c(11, "%s %s has no previous version recorded in %s; nothing to roll back to",
                      a->target, cur.version, a->root);
    }
    free(pp);
    memset(&prev, 0, sizeof prev);
    {
        char tmp[200];
        size_t n = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        free(buf);
        if (sscanf(tmp, "%63s", prev.version) != 1 || pkg_check_version(prev.version) != NULL) {
            pkg_manifest_free(&cur);
            return refuse_c(12, "the rollback record for %s is damaged", a->target);
        }
    }
    if (open_channels(a, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->target) == 0 && pkg_version_cmp(ix.e[i].version, prev.version) == 0
            && (strcmp(ix.e[i].arch, cur.architecture) == 0 || strcmp(ix.e[i].arch, "generic") == 0))
            e = &ix.e[i];
    if (e == NULL)
        refuse_c(11, "the previous version of %s, %s, is no longer in %s",
               a->target, prev.version, chans_text());
    else
        rc = move_to(a, &ix, e, &cur, "rolled back");
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

static int cmd_list(const struct pkg_options *a)
{
    char *dir, **names;
    size_t n, i;

    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    dir = pkg_join(a->root, ".pkg/db");
    if (dir == NULL || pkg_fs_list(dir, &names, &n) != 0) {
        free(dir);
        return refuse_c(17, "cannot read the database of %s", a->root);
    }
    free(dir);
    kv("result", "listed");
    if (!machine && n > 0) {
        static const int widths[] = { 24, 10, 12, 0 };
        tbl_head(widths, "Package\tVersion\tKind\tFiles");
    }
    for (i = 0; i < n; i++) {
        struct pkg_manifest m;
        if (load_installed(a->root, names[i], &m, 0) == 0) {
            int dep = is_auto(a->root, m.name);
            if (machine) {
                char j[300], files[24];
                snprintf(files, sizeof files, "%lu", (unsigned long)m.nfiles);
                snprintf(j, sizeof j, "%s %s %s %s %s", m.name, m.version, m.kind, files,
                         dep ? "dependency" : "explicit");
                rec_item("package", j, "name", m.name, "version", m.version, "kind", m.kind,
                         "files", files, "reason", dep ? "dependency" : "explicit", NULL);
            }
            else
                tbl_row("%s\t%s\t%s\t%lu file%s%s", m.name, m.version, m.kind,
                        (unsigned long)m.nfiles, m.nfiles == 1 ? "" : "s", dep ? ", a dependency" : "");
            pkg_manifest_free(&m);
        }
        free(names[i]);
    }
    free(names);
    if (!machine && n > 0)
        tbl_end();
    if (n == 0 && !machine)
        say_result("nothing installed in %s", a->root);
    kv("count", "%lu", (unsigned long)n);
    return 0;
}

/* Files a person moved by hand: found elsewhere in the root under the same
 * name with the same bytes. Moving software is the Amiga tradition; Pkg
 * reports it and claims nothing. */
struct moved_search {
    const char                *root;
    const struct pkg_manifest *m;
    const unsigned char       *missing;   /* per file: 1 if missing */
    char                     **found;     /* per file: where it is now */
};

static const char *base_of(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int moved_one(const char *rel, void *ctx)
{
    struct moved_search *ms = (struct moved_search *)ctx;
    size_t i;
    if (strncmp(rel, ".pkg/", 5) == 0)
        return 0;
    for (i = 0; i < ms->m->nfiles; i++)
        if (ms->missing[i] && ms->found[i] == NULL
            && strcmp(base_of(rel), base_of(ms->m->files[i].path)) == 0
            && file_state(ms->root, rel, ms->m->files[i].digest, ms->m->files[i].size) == 0) {
            ms->found[i] = (char *)malloc(strlen(rel) + 1);
            if (ms->found[i] != NULL)
                memcpy(ms->found[i], rel, strlen(rel) + 1);
            break;
        }
    return 0;
}

/* VERIFY ALL: every installed package, a line each; the files that differ
 * named with their package. Moved files are looked for by VERIFY <name>. */
static int verify_all(const struct pkg_options *a)
{
    struct installed in;
    size_t p, i, bad = 0, files = 0, edited = 0;
    char first[200] = "";

    if (load_all(a->root, &in) != 0) return 1;
    if (in.n == 0) {
        installed_free(&in);
        kv("result", "empty");
        summary_line("no package is installed in %s", a->root);
        return 0;
    }
    if (!machine && in.n > 0) {
        static const int widths[] = { 24, 10, 9, 0 };
        tbl_head(widths, "Package\tVersion\tFiles\tState");
    }
    for (p = 0; p < in.n; p++) {
        const struct pkg_manifest *m = &in.m[p];
        size_t changed = 0, missing = 0, pedited = 0;
        unsigned char *state = (unsigned char *)calloc(m->nfiles ? m->nfiles : 1, 1);
        if (state == NULL) { installed_free(&in); return refuse_c(17, "out of memory"); }
        /* 1. every file's state, so the package's line can come first */
        for (i = 0; i < m->nfiles; i++) {
            int s = file_state(a->root, m->files[i].path, m->files[i].digest, m->files[i].size);
            state[i] = (unsigned char)s;
            if (s == 1 && m->files[i].config) pedited++;
            else if (s == 1) changed++;
            else if (s == 2) missing++;
        }
        edited += pedited;
        files += m->nfiles;
        /* 2. the package's line */
        if (changed + missing) {
            if (bad++ == 0)
                snprintf(first, sizeof first, "%s: %s", m->name, damage(changed, missing));
            if (machine) kv("package", "%s %s damaged %lu %lu", m->name, m->version,
                            (unsigned long)changed, (unsigned long)missing);
            else tbl_row("%s\t%s\t%lu file%s\t%s", m->name, m->version, (unsigned long)m->nfiles,
                         m->nfiles == 1 ? "" : "s", damage(changed, missing));
        } else if (machine) {
            kv("package", "%s %s intact", m->name, m->version);
        } else {
            tbl_row("%s\t%s\t%lu file%s\t%s", m->name, m->version, (unsigned long)m->nfiles,
                    m->nfiles == 1 ? "" : "s", pedited ? "intact, configuration edited" : "intact");
        }
        /* 3. its files that are not as installed */
        for (i = 0; i < m->nfiles; i++) {
            if (state[i] == 1 && m->files[i].config) {
                if (machine) kv("edited", "%s %s", m->name, m->files[i].path);
                else say_item("edited", "%s (a configuration file)", m->files[i].path);
            } else if (state[i] == 1) {
                if (machine) kv("changed", "%s %s", m->name, m->files[i].path);
                else say_item("changed", "%s", m->files[i].path);
            } else if (state[i] == 2) {
                if (machine) kv("missing", "%s %s", m->name, m->files[i].path);
                else say_item("missing", "%s", m->files[i].path);
            }
        }
        free(state);
    }
    if (!machine && in.n > 0)
        tbl_end();
    kv("packages", "%lu", (unsigned long)in.n);
    kv("files", "%lu", (unsigned long)files);
    if (bad == 0) {
        kv("result", "intact");
        summary_line("%lu package%s, %lu file%s, all intact%s", (unsigned long)in.n,
           in.n == 1 ? "" : "s", (unsigned long)files, files == 1 ? "" : "s",
           edited ? "; configuration files edited, as people do" : "");
        installed_free(&in);
        return 0;
    }
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    {
        char buf[400];
        snprintf(buf, sizeof buf, "%lu of %lu package%s damaged, first %s", (unsigned long)bad,
                 (unsigned long)in.n, in.n == 1 ? "" : "s", first);
        kv("summary", "%s", buf);
        if (!machine)
            say_problem("%lu of %lu package%s damaged", (unsigned long)bad,
                        (unsigned long)in.n, in.n == 1 ? "" : "s");
    }
    hint("REPAIR ALL ROOT <dir> CHANNEL <dir> puts missing and changed files back from the channel. "
         "VERIFY <name> also says which missing files were moved by hand");
    installed_free(&in);
    return 1;
}

static int cmd_verify(const struct pkg_options *a)
{
    struct pkg_manifest m;
    size_t i, changed = 0, missing = 0, edited = 0;

    if (a->all && a->root != NULL) return verify_all(a);
    if (a->target == NULL)  return refuse_c(20, "name the package to verify, or VERIFY ALL");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("files", "%lu", (unsigned long)m.nfiles);
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 1 && m.files[i].config) {
            edited++;           /* a configuration file: editing it is what it is for */
            if (machine) kv("edited", "%s", m.files[i].path);
            else say_item("edited", "%s (a configuration file)", m.files[i].path);
        } else if (s == 1) {
            changed++;
            if (machine) kv("changed", "%s", m.files[i].path);
            else say_item("changed", "%s", m.files[i].path);
        }
        if (s == 2)
            missing++;          /* reported below, once moved files are known */
    }
    if (missing > 0) {
        unsigned char *miss = (unsigned char *)calloc(m.nfiles, 1);
        char **found = (char **)calloc(m.nfiles, sizeof *found);
        size_t moved = 0;
        if (miss != NULL && found != NULL) {
            struct moved_search ms;
            unsigned skipped;
            char err[200];
            for (i = 0; i < m.nfiles; i++)
                miss[i] = file_state(a->root, m.files[i].path, m.files[i].digest,
                                     m.files[i].size) == 2;
            ms.root = a->root; ms.m = &m; ms.missing = miss; ms.found = found;
            pkg_fs_walk(a->root, moved_one, NULL, &ms, &skipped, err, sizeof err);
            for (i = 0; i < m.nfiles; i++)
                if (found[i] != NULL) {
                    moved++;
                    if (machine) kv("moved", "%s %s", m.files[i].path, found[i]);
                    else say_item("moved", "%s -> %s", m.files[i].path, found[i]);
                } else if (miss[i]) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say_item("missing", "%s", m.files[i].path);
                }
        } else {
            for (i = 0; i < m.nfiles; i++)
                if (file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size) == 2) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say_item("missing", "%s", m.files[i].path);
                }
        }
        for (i = 0; found != NULL && i < m.nfiles; i++) free(found[i]);
        free(found);
        free(miss);
        if (moved == missing && changed == 0) {
            kv("result", "moved");
            if (!machine)
                say_result("%s %s: moved by hand, every file intact where it is now", m.name, m.version);
            hint("moving an installed drawer is the person's right, and the package stays listed. "
                 "REMOVE and UPGRADE act on the places pkg recorded: the moved files are left "
                 "where they are, and an upgrade installs beside them");
            pkg_manifest_free(&m);
            return 0;
        }
    }
    if (changed + missing == 0) {
        kv("result", "intact");
        if (edited) kv("edited-config", "%lu", (unsigned long)edited);
        if (!machine)
            say_result("%s %s: %lu file%s, all intact%s", m.name, m.version, (unsigned long)m.nfiles,
                m.nfiles == 1 ? "" : "s", edited ? ", configuration files edited as people do" : "");
        pkg_manifest_free(&m);
        return 0;
    }
    if (!machine) {
        say_problem("%s %s is damaged: %s of %lu file%s", m.name, m.version,
                    damage(changed, missing), (unsigned long)m.nfiles, m.nfiles == 1 ? "" : "s");
        hint("REPAIR %s ROOT <dir> CHANNEL <dir> puts the files back from the channel", m.name);
    }
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    pkg_manifest_free(&m);
    return 1;
}

/* ---- REPAIR ------------------------------------------------------------ *
 *
 * Puts an installed version's own files back from the channel: the missing
 * ones, and the changed ones, whose change is kept beside as <file>.pkgold.
 * An edited configuration file is the person's and stays as it is. Every
 * file written is checked against the digest the root recorded at install. */

struct repair_ctx {
    const char                *root;
    const struct pkg_manifest *m;
    unsigned char             *need;    /* per file of m: 1 missing, 2 changed */
    unsigned long              restored, aside;
    char                       err[400];
};

static int repair_entry(const struct pkg_entry *e, void *ctx)
{
    struct repair_ctx *c = (struct repair_ctx *)ctx;
    const struct pkg_file *pf = find_file(c->m, e->path);
    char hex[PKG_SHA256_HEXLEN + 1], *to, *slash;
    size_t k;

    if (pf == NULL || !c->need[pf - c->m->files])
        return 0;
    k = (size_t)(pf - c->m->files);
    pkg_sha256_hex(e->data, e->data_len, hex);
    if ((unsigned long long)e->data_len != pf->size || strcmp(hex, pf->digest) != 0) {
        snprintf(c->err, sizeof c->err, "the channel's copy of \"%s\" is not the file installed", e->path);
        return 1;
    }
    to = pkg_join(c->root, pf->path);
    if (to == NULL) { snprintf(c->err, sizeof c->err, "out of memory"); return 1; }
    if (c->need[k] == 2) {
        char *old = beside(to, ".pkgold"), *oldrel = beside(pf->path, ".pkgold");
        int good;
        if (old == NULL || oldrel == NULL) {
            free(old); free(oldrel); free(to);
            snprintf(c->err, sizeof c->err, "out of memory");
            return 1;
        }
        if (pkg_fs_exists(old)) {
            /* An earlier change is set aside there already: never lose one. */
            warn("%s already holds an earlier change; %s is left as it is", oldrel, pf->path);
            free(old); free(oldrel);
            free(to);
            return 0;
        }
        pkg_fs_unprotect(to);
        good = pkg_fs_rename(to, old) == 0;
        free(old);
        if (!good) {
            snprintf(c->err, sizeof c->err, "cannot set \"%s\" aside: %s", pf->path, strerror(errno));
            free(oldrel);
            free(to);
            return 1;
        }
        c->aside++;
        if (machine) kv("set-aside", "%s %s", pf->path, oldrel);
        else say_item("aside", "%s -> %s", pf->path, oldrel);
        free(oldrel);
    }
    slash = strrchr(to, '/');
    if (slash != NULL) {
        *slash = '\0';
        pkg_fs_mkdirs(to);
        *slash = '/';
    }
    if (pkg_fs_write_atomic(to, e->data, e->data_len) != 0) {
        snprintf(c->err, sizeof c->err, "cannot write \"%s\": %s", pf->path, strerror(errno));
        free(to);
        return 1;
    }
    free(to);
    attrs_one(c->root, pf);
    c->restored++;
    if (machine) kv("restored", "%s", pf->path);
    else say_item("restored", "%s", pf->path);
    return 0;
}

/* One package: 0 intact or repaired, 1 refused (reason given). */
static int repair_one(const struct pkg_options *a, const struct index *ix, const char *name,
                      unsigned long *restored, unsigned long *aside)
{
    struct pkg_manifest m;
    struct fetched f;
    struct repair_ctx c;
    const struct entry *e;
    size_t i, needed = 0;
    int stopped, rc = 1;

    *restored = *aside = 0;
    if (load_installed(a->root, name, &m, 0) != 0) return 1;
    c.need = calloc(m.nfiles ? m.nfiles : 1, 1);
    if (c.need == NULL) { pkg_manifest_free(&m); return refuse("out of memory"); }
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 2 || (s == 1 && !m.files[i].config)) {
            c.need[i] = (unsigned char)(s == 2 ? 1 : 2);
            needed++;
        }
    }
    if (needed == 0) {
        free(c.need);
        pkg_manifest_free(&m);
        return 0;
    }
    e = pick(ix, m.name, m.version);
    if (e == NULL && pick_refused)
        goto out;
    if (e == NULL) {
        refuse_c(11, "%s %s is installed and the channel no longer offers it, so its %lu damaged "
                 "file%s cannot be put back; UPGRADE to a version the channel has", m.name,
                 m.version, (unsigned long)needed, needed == 1 ? "" : "s");
        goto out;
    }
    if (fetch(chan_of(e), e, &f) != 0)
        goto out;
    if (!dryrun) {
        c.root = a->root; c.m = &m; c.restored = c.aside = 0; c.err[0] = '\0';
        if (pkg_read(f.pkg, f.pkg_len, repair_entry, &c, &stopped) != PKG_OK) {
            refuse_c(c.err[0] && strstr(c.err, "is not the file") ? 12 : 17, "%s %s: %s; %lu file%s "
                     "put back before it", m.name, m.version, c.err[0] ? c.err : "the payload is unreadable",
                     c.restored, c.restored == 1 ? "" : "s");
            fetched_free(&f);
            goto out;
        }
        *restored = c.restored;
        *aside = c.aside;
    } else {
        *restored = (unsigned long)needed;
    }
    fetched_free(&f);
    rc = 0;
out:
    free(c.need);
    pkg_manifest_free(&m);
    return rc;
}

/* ---- RESOLVE ----------------------------------------------------------- *
 *
 * What OpenLibrary(<name>, <version>) gives a program, and why, for a
 * developer whose program does not start or opens the wrong library. The
 * places LDDemon looks, in its order (rom/lddemon/lddemon.c, LDLoad): the
 * caller's current directory and its libs/ (devs/ for a device), the
 * program's directory and its libs/, then LIBS: (DEVS:). A library already
 * in memory wins over every file. Each file gets the checks the loader
 * makes: LoadSeg reads it only if it is an executable for this CPU; it is
 * a library only if it holds a resident tag of the right type; the first
 * file that passes is taken whatever its version, and the open fails if
 * that copy is older than asked. On a host, FROM stands for the program's
 * directory and the current one, ROOT for SYS:, and LIBS: is ROOT/Libs then
 * ROOT/Classes, or the directories PKG_LIBS_PATH lists. */

enum { C_NOFILE, C_DIR, C_UNREADABLE, C_NOTEXEC, C_WRONGCPU, C_NORESIDENT, C_WRONGTYPE, C_OK };

struct cand {
    char        where[48];      /* as AROS names the place: "PROGDIR:libs/", "LIBS:" */
    char        path[1100];     /* the file looked for */
    int         state;
    char        ver[64];        /* from its $VER, else its resident's version, or "" */
    const char *cpu;            /* from its header, or NULL */
    int         rtype;          /* the resident's node type, or -1 */
    char        pkg[80];        /* the installed package that lists it, or "" */
    char        verdict[160];
    char        next[400];
    int         fails;          /* this row is why the open fails */
};

struct resolution {
    struct cand c[16];
    size_t      n;
    size_t      taken;          /* the candidate the loader takes, or (size_t)-1 */
    int         loaded;         /* 1 in memory, 0 not, -1 cannot tell */
    char        loaded_ver[32];
    unsigned    opencnt;
    int         rc;             /* 0, 11 nothing, 18 too old or the taken file fails */
    int         shadows;        /* the copy taken hides a newer one found later */
    char        summary[300];
    char        offer[160];     /* "sdl2 2.30" when a channel has a package providing it */
    char        offer_chan[600];/* the channel that offers it */
};

/* "SDL2.library 2.30 (1.1.2026)" -> "2.30": the version of a $VER cookie
 * whatever its name looks like. */
static void file_version(const unsigned char *p, size_t len, char *ver, size_t vl)
{
    size_t j;
    ver[0] = '\0';
    for (j = 0; j + 6 < len; j++) {
        size_t k = j + 6, b = 0;
        if (memcmp(p + j, "$VER: ", 6) != 0) continue;
        while (k < len && p[k] > ' ' && p[k] < 0x7F) k++;          /* the name */
        while (k < len && p[k] == ' ') k++;
        while (k < len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.') && b + 1 < vl)
            ver[b++] = (char)p[k++];
        while (b > 0 && ver[b - 1] == '.') b--;
        ver[b] = '\0';
        if (b > 0 && pkg_check_version(ver) == NULL) return;
        ver[0] = '\0';
    }
}

/* A resident tag in the file's bytes, laid out as the file's CPU lays out
 * struct Resident: 0x4AFC, then after the two pointers rt_Flags,
 * rt_Version, rt_Type. The type of the first plausible one, and its
 * version; -1 when there is none. */
static int find_resident(const unsigned char *p, size_t len, int *version)
{
    int be, align, flags_at;
    size_t o;
    if (len >= 20 && memcmp(p, "\x7f" "ELF", 4) == 0) {
        unsigned mach = p[5] == 2 ? (unsigned)p[18] << 8 | p[19] : (unsigned)p[19] << 8 | p[18];
        be = p[5] == 2;
        if (mach == 4) { align = 2; flags_at = 10; }           /* m68k: packed on 2 */
        else if (p[4] == 2) { align = 8; flags_at = 24; }      /* 64-bit pointers */
        else { align = 4; flags_at = 12; }
    } else if (len >= 4 && pkg_be32_get(p) == 0x000003F3ul) {
        be = 1; align = 2; flags_at = 10;                      /* a hunk file: m68k */
    } else {
        return -1;
    }
    for (o = 0; o + (size_t)flags_at + 4 <= len; o += (size_t)align) {
        int flags, type;
        if (!(be ? (p[o] == 0x4A && p[o + 1] == 0xFC) : (p[o] == 0xFC && p[o + 1] == 0x4A)))
            continue;
        flags = p[o + flags_at];
        type = p[o + flags_at + 2];
        if ((type == 3 || type == 8 || type == 9) && (flags & 0x30) == 0) {
            *version = p[o + flags_at + 1];
            return type;
        }
    }
    return -1;
}

static void cand_add(struct resolution *r, const char *where, const char *path)
{
    size_t k;
    if (r->n >= sizeof r->c / sizeof r->c[0]) return;
    for (k = 0; k < r->n; k++)          /* FROM is both the current and the program directory */
        if (strcmp(r->c[k].path, path) == 0) return;
    memset(&r->c[r->n], 0, sizeof r->c[r->n]);
    r->c[r->n].rtype = -1;
    snprintf(r->c[r->n].where, sizeof r->c[r->n].where, "%s", where);
    snprintf(r->c[r->n].path, sizeof r->c[r->n].path, "%s", path);
    r->n++;
}

/* A package of the channels whose newest version provides `name`:
 * "sdl2 2.30", and the channel it is in. */
static int channel_offers(const struct pkg_options *a, const char *name, char *out, size_t ol,
                          char *where, size_t wl)
{
    struct index ix;
    size_t i, j;
    int found = 0, rc;
    where[0] = '\0';
    if (a->channel == NULL && a->root == NULL) return 0;
    /* RESOLVE offers this as an extra: a root with no channel to ask is not
     * a refusal here, and nothing about it is printed. */
    quiet = 1;
    rc = open_channels(a, &ix);
    quiet = 0;
    quiet_reason[0] = '\0';
    if (rc != 0) { refused_class = 0; refused_next = NULL; return 0; }
    for (i = 0; i < ix.n && !found; i++) {
        const struct entry *best = &ix.e[i];
        struct pkg_manifest m;
        char *mp, err[200];
        unsigned char *buf;
        size_t len;
        for (j = 0; j < ix.n; j++)
            if (strcmp(ix.e[j].name, best->name) == 0 && pkg_version_cmp(ix.e[j].version, best->version) > 0)
                best = &ix.e[j];
        if (best != &ix.e[i]) continue;
        if ((mp = object_path(chan_of(best), best->digest, "manifest")) == NULL) continue;
        if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
        free(mp);
        if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) == 0) {
            if (strs_has_nocase(&m.provides, name)) {
                snprintf(out, ol, "%s %s", m.name, m.version);
                snprintf(where, wl, "%s", chan_of(best));
                found = 1;
            }
            pkg_manifest_free(&m);
        }
        free(buf);
    }
    free(ix.e);
    return found;
}

static void resolve_one(const struct pkg_options *a, const char *name, const char *want,
                        const char *root, const char *from, const struct installed *in,
                        struct resolution *r)
{
    const char *base = strrchr(name, ':'), *mach = target_arch;
    char buf[1200];
    size_t i, rl, better = (size_t)-1;
    int device;
    unsigned lv = 0, lr = 0;

    memset(r, 0, sizeof *r);
    r->taken = (size_t)-1;
    base = base ? base + 1 : name;
    if (strrchr(base, '/')) base = strrchr(base, '/') + 1;
    rl = strlen(base);
    device = rl > 7 && ascii_casecmp(base + rl - 7, ".device") == 0;
    if (strchr(name, ':') != NULL) {
#if defined(__AROS__)
        cand_add(r, "the path given", name);
#else
        if (ascii_casecmp_n(name, "SYS:", 4) == 0 && root != NULL)
            snprintf(buf, sizeof buf, "%s/%s", root, name + 4);
        else if (ascii_casecmp_n(name, "PROGDIR:", 8) == 0 && from != NULL)
            snprintf(buf, sizeof buf, "%s/%s", from, name + 8);
        else
            snprintf(buf, sizeof buf, "%s", name);
        cand_add(r, "the path given", buf);
#endif
    } else {
        const char *sub = device ? "devs" : "libs";
        if (from != NULL && from[0]) {
            const char *sep = from[strlen(from) - 1] == ':' ? "" : "/";
            snprintf(buf, sizeof buf, "%s%s%s", from, sep, name);
            cand_add(r, "PROGDIR:", buf);
            snprintf(buf, sizeof buf, "%s%s%s/%s", from, sep, sub, name);
            cand_add(r, device ? "PROGDIR:devs/" : "PROGDIR:libs/", buf);
        }
#if defined(__AROS__)
        if (from == NULL) {
            cand_add(r, "current directory", name);
            snprintf(buf, sizeof buf, "%s/%s", sub, name);
            cand_add(r, device ? "current dir devs/" : "current dir libs/", buf);
        }
        snprintf(buf, sizeof buf, "%s%s", device ? "DEVS:" : "LIBS:", name);
        cand_add(r, device ? "DEVS:" : "LIBS:", buf);
#else
        {
            const char *list = getenv("PKG_LIBS_PATH");
            if (list != NULL && !device) {
                const char *q = list;
                while (*q) {
                    size_t k = strcspn(q, ":");
                    if (k > 0) {
                        snprintf(buf, sizeof buf, "%.*s/%s", (int)k, q, name);
                        cand_add(r, "LIBS:", buf);
                    }
                    q += k;
                    if (*q == ':') q++;
                }
            } else if (root != NULL && device) {
                snprintf(buf, sizeof buf, "%s/Devs/%s", root, name);
                cand_add(r, "DEVS:", buf);
            } else if (root != NULL) {
                snprintf(buf, sizeof buf, "%s/Libs/%s", root, name);
                cand_add(r, "LIBS: (SYS:Libs)", buf);
                snprintf(buf, sizeof buf, "%s/Classes/%s", root, name);
                cand_add(r, "LIBS: (SYS:Classes)", buf);
            }
        }
#endif
    }

    /* each file, as the loader sees it */
    for (i = 0; i < r->n; i++) {
        struct cand *c = &r->c[i];
        unsigned char *data;
        size_t len, k;
        int rv = -1;
        if (!pkg_fs_exists(c->path)) { c->state = C_NOFILE; continue; }
        if (pkg_fs_is_dir(c->path)) { c->state = C_DIR; continue; }
        if (pkg_fs_read(c->path, &data, &len) != 0) { c->state = C_UNREADABLE; continue; }
        file_version(data, len, c->ver, sizeof c->ver);
        c->cpu = file_arch(data, len);
        c->rtype = find_resident(data, len, &rv);
        free(data);
        if (!c->ver[0] && rv >= 0) snprintf(c->ver, sizeof c->ver, "%d", rv);
        if (c->cpu == NULL) c->state = C_NOTEXEC;
        else if (mach != NULL && strcmp(c->cpu, mach) != 0) c->state = C_WRONGCPU;
        else if (c->rtype < 0) c->state = C_NORESIDENT;
        else if (c->rtype != (device ? 3 : 9)) c->state = C_WRONGTYPE;
        else c->state = C_OK;
#if defined(__AROS__)
        /* which directory of the LIBS: (DEVS:) assign holds it */
        if (ascii_casecmp_n(c->path, "LIBS:", 5) == 0 || ascii_casecmp_n(c->path, "DEVS:", 5) == 0)
            pkg_fs_fullpath(c->path, c->path, sizeof c->path);
#endif
        if (root != NULL && in != NULL) {
            /* the installed package that lists it, when it lies in the root */
            size_t rlen = strlen(root);
            const char *rel = NULL;
            if (ascii_casecmp(root, "SYS:") == 0 && strchr(c->path, ':'))
                rel = strchr(c->path, ':') + 1;
            else if (strncmp(c->path, root, rlen) == 0 && (c->path[rlen] == '/' || root[rlen - 1] == ':'))
                rel = c->path + rlen + (c->path[rlen] == '/');
            for (k = 0; rel != NULL && k < in->n && !c->pkg[0]; k++) {
                size_t f;
                for (f = 0; f < in->m[k].nfiles; f++)
                    if (ascii_casecmp(in->m[k].files[f].path, rel) == 0) {
                        snprintf(c->pkg, sizeof c->pkg, "%s %s", in->m[k].name, in->m[k].version);
                        break;
                    }
            }
        }
    }
    r->loaded = pkg_fs_loaded(base, device, &lv, &lr, &r->opencnt);
    if (r->loaded == 1) snprintf(r->loaded_ver, sizeof r->loaded_ver, "%u.%u", lv, lr);
    channel_offers(a, base, r->offer, sizeof r->offer, r->offer_chan, sizeof r->offer_chan);

    /* the loader's walk: what each place gives, and where it stops */
    for (i = 0; i < r->n; i++) {
        struct cand *c = &r->c[i];
        if (r->loaded == 1) {
            snprintf(c->verdict, sizeof c->verdict, "%s", c->state == C_NOFILE ? "no file"
                     : "not consulted: the copy in memory is used");
            continue;
        }
        if (r->taken != (size_t)-1) {
            snprintf(c->verdict, sizeof c->verdict, "%s", c->state == C_NOFILE ? "no file"
                     : "not reached: found earlier");
            if (c->state == C_OK && want && c->ver[0] && pkg_version_cmp(c->ver, want) >= 0 && better == (size_t)-1)
                better = i;
            continue;
        }
        switch (c->state) {
        case C_NOFILE: snprintf(c->verdict, sizeof c->verdict, "no file"); break;
        case C_DIR: snprintf(c->verdict, sizeof c->verdict, "a directory, passed over"); break;
        case C_UNREADABLE: snprintf(c->verdict, sizeof c->verdict, "unreadable, passed over"); break;
        case C_NOTEXEC:
            snprintf(c->verdict, sizeof c->verdict, "not a program file: LoadSeg fails, passed over");
            snprintf(c->next, sizeof c->next, "remove %s: it is no library, and hides nothing now, but "
                     "confuses whoever looks", c->path);
            break;
        case C_WRONGCPU:
            snprintf(c->verdict, sizeof c->verdict, "wrong CPU (%s build on a %s system): LoadSeg "
                     "fails, passed over", c->cpu, mach);
            snprintf(c->next, sizeof c->next, "this file is a %s build; rebuild it for %s AROS, or "
                     "remove it", c->cpu, mach);
            break;
        case C_NORESIDENT:
            snprintf(c->verdict, sizeof c->verdict, "not a library (no resident tag): passed over; an "
                     "AROS without that check stops here and the open fails");
            snprintf(c->next, sizeof c->next, "remove %s, or replace it with the real %s", c->path, base);
            break;
        case C_WRONGTYPE:
            r->taken = i;
            c->fails = 1;
            snprintf(c->verdict, sizeof c->verdict, "taken, but it is a %s, not a %s: the open fails",
                     c->rtype == 3 ? "device" : c->rtype == 9 ? "library" : "resource",
                     device ? "device" : "library");
            snprintf(c->next, sizeof c->next, "remove %s: it is not the %s the program asks for",
                     c->path, device ? "device" : "library");
            break;
        default:
            r->taken = i;
            if (want != NULL && c->ver[0] && pkg_version_cmp(c->ver, want) < 0) {
                c->fails = 1;
                snprintf(c->verdict, sizeof c->verdict, "taken, too old (%s, %s needed): the open fails",
                         c->ver, want);
            } else {
                snprintf(c->verdict, sizeof c->verdict, "taken%s", want != NULL && !c->ver[0]
                         ? " (its version cannot be read here)" : "");
            }
            {
                /* an older copy found first hides a newer one: say so, asked or not */
                size_t k;
                for (k = i + 1; k < r->n && !c->fails; k++)
                    if (r->c[k].state == C_OK && r->c[k].ver[0] && c->ver[0]
                        && pkg_version_cmp(r->c[k].ver, c->ver) > 0) {
                        r->shadows = 1;
                        snprintf(c->verdict, sizeof c->verdict, "taken, and hides the newer %s %s",
                                 r->c[k].path, r->c[k].ver);
                        snprintf(c->next, sizeof c->next, "remove %s: %s %s is newer and would "
                                 "then be taken", c->path, r->c[k].path, r->c[k].ver);
                        break;
                    }
            }
        }
    }

    /* the answer, and what to do */
    if (r->loaded == 1) {
        if (want != NULL && pkg_version_cmp(r->loaded_ver, want) < 0) {
            size_t k, newer = (size_t)-1;
            for (k = 0; k < r->n; k++)
                if (r->c[k].state == C_OK && r->c[k].ver[0] && pkg_version_cmp(r->c[k].ver, want) >= 0) { newer = k; break; }
            r->rc = 18;
            snprintf(r->summary, sizeof r->summary, "%s is in memory at %s, older than the %s asked "
                     "for: every program gets that copy", base, r->loaded_ver, want);
            if (newer != (size_t)-1) {
                char nx[400];
                snprintf(nx, sizeof nx, "Avail FLUSH, then start the program again: the copy in "
                         "memory (%s) is older than this file (%s)", r->loaded_ver, r->c[newer].ver);
                memcpy(r->c[newer].next, nx, sizeof nx);
            }
        } else {
            snprintf(r->summary, sizeof r->summary, "%s is in memory at %s (opened %u times): every "
                     "program gets that copy, whatever the files say", base, r->loaded_ver, r->opencnt);
        }
        return;
    }
    if (r->taken == (size_t)-1) {
        r->rc = 11;
        snprintf(r->summary, sizeof r->summary, "%s is found nowhere: none of the %lu places the "
                 "loader looks holds a %s it can use", base, (unsigned long)r->n, device ? "device" : "library");
        if (r->n > 0) {
            struct cand *last = &r->c[r->n - 1];
            if (!last->next[0]) {
                if (r->offer[0])
                    snprintf(last->next, sizeof last->next, "install it: package %s provides %s "
                             "(pkg INSTALL %.*s ROOT <root> CHANNEL %.150s)", r->offer, base,
                             (int)strcspn(r->offer, " "), r->offer, r->offer_chan);
                else
                    snprintf(last->next, sizeof last->next, "install %s%s%s into %s", base,
                             want ? " " : "", want ? want : "", device ? "SYS:Devs" : "SYS:Libs");
            }
        }
        return;
    }
    {
        struct cand *t = &r->c[r->taken];
        if (!t->fails) {
            snprintf(r->summary, sizeof r->summary, "%s resolves to %s%s%s", base, t->path,
                     t->ver[0] ? " " : "", t->ver);
            return;
        }
        r->rc = 18;
        snprintf(r->summary, sizeof r->summary, "%s resolves to %s%s%s, and the open fails: %s",
                 base, t->path, t->ver[0] ? " " : "", t->ver, t->verdict);
        if (!t->next[0]) {
            char nx[400];
            if (better != (size_t)-1)
                snprintf(nx, sizeof nx, "remove %s: %s %s is newer and would be taken",
                         t->path, r->c[better].path, r->c[better].ver);
            else if (r->offer[0])
                snprintf(nx, sizeof nx, "install a newer one: package %s provides %s "
                         "(pkg UPGRADE or INSTALL %.*s ROOT <root> CHANNEL %.150s)", r->offer, base,
                         (int)strcspn(r->offer, " "), r->offer, r->offer_chan);
            else
                snprintf(nx, sizeof nx, "copy or install a %s %s or newer into %s, and "
                         "remove %s", base, want, device ? "SYS:Devs" : "SYS:Libs", t->path);
            memcpy(t->next, nx, sizeof nx);
        }
    }
}

static void resolve_print(const struct resolution *r, const char *base)
{
    size_t i;
    if (r->loaded == 1 && machine) {
        char j[200];
        snprintf(j, sizeof j, "%s %s %u", base, r->loaded_ver, r->opencnt);
        rec_item("loaded", j, "name", base, "version", r->loaded_ver, NULL);
    }
    if (machine) {
        for (i = 0; i < r->n; i++) {
            const struct cand *c = &r->c[i];
            char j[1400];
            int chosen = r->loaded != 1 && i == r->taken;
            snprintf(j, sizeof j, "%s %s %s %s %s", c->path, c->state == C_NOFILE ? "no" : "yes",
                     c->ver[0] ? c->ver : "-", c->pkg[0] ? c->pkg : "-", chosen ? "chosen" : "-");
            rec_item("candidate", j, "path", c->path, "exists", c->state == C_NOFILE ? "no" : "yes",
                     "version", c->ver[0] ? c->ver : "-", "package", c->pkg[0] ? c->pkg : "-",
                     "chosen", chosen ? "yes" : "no", "where", c->where, "verdict", c->verdict,
                     "next", c->next[0] ? c->next : "-", NULL);
            kv("verdict", "%s %s", c->path, c->verdict);
            if (c->next[0]) kv("next-step", "%s %s", c->path, c->next);
        }
        if (r->loaded == 1) kv("winner", "memory %s", r->loaded_ver);
        else if (r->taken != (size_t)-1) kv("winner", "%s %s", r->c[r->taken].path,
                                            r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-");
        return;
    }
    {
        static const int widths[] = { 20, 40, 8, 18, 0 };
        tbl_head(widths, "Where\tFile\tVersion\tPackage\tVerdict");
        if (r->loaded == 1)
            tbl_row("%s\t%s\t%s\t%s\t%s", "memory", base, r->loaded_ver, "-", "taken: already loaded");
        for (i = 0; i < r->n; i++) {
            const struct cand *c = &r->c[i];
            tbl_row("%s\t%s\t%s\t%s\t%s", c->where, c->path, c->ver[0] ? c->ver : "-",
                    c->state == C_NOFILE ? "-" : c->pkg[0] ? c->pkg : "not from a package", c->verdict);
        }
        tbl_end();
        for (i = 0; i < r->n; i++)
            if (r->c[i].next[0]) say_detail("%s: %s", r->c[i].where, r->c[i].next);
    }
}

static int cmd_resolve(const struct pkg_options *a)
{
    struct installed in;
    struct resolution *r;
    const char *name = a->target, *root = a->root, *from = a->from;
    unsigned char *data = NULL;
    size_t len = 0;
    int program = 0, rc = 0;
    char fromdir[1100];

    if (name == NULL) return refuse_c(20, "name the library or device, RESOLVE SDL2.library, or a "
                                      "program, RESOLVE Work:Game/Game");
    if (a->version != NULL && pkg_check_version(a->version) != NULL)
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    {
        size_t nl = strlen(name);
        int libname = (nl > 8 && ascii_casecmp(name + nl - 8, ".library") == 0)
                      || (nl > 7 && ascii_casecmp(name + nl - 7, ".device") == 0);
        if (!libname && pkg_fs_exists(name) && !pkg_fs_is_dir(name) && pkg_fs_read(name, &data, &len) == 0) {
            if (file_arch(data, len) == NULL) {
                free(data);
                return refuse_c(20, "%s is neither a library name nor a program: RESOLVE takes "
                                "SDL2.library, or the path of an executable", name);
            }
            program = 1;
        }
    }
#if defined(__AROS__)
    if (root == NULL) root = "SYS:";
#else
    if (root == NULL && getenv("PKG_LIBS_PATH") == NULL) {
        free(data);
        return refuse_c(20, "name the directory that stands for SYS: with ROOT <dir>, or list the "
                        "directories of LIBS: in PKG_LIBS_PATH");
    }
#endif
    if (program && from == NULL) {
        /* the program's own directory is its PROGDIR: */
        const char *sl = strrchr(name, '/'), *co = strrchr(name, ':');
        const char *cut = sl && (!co || sl > co) ? sl : co;
        if (cut == NULL) snprintf(fromdir, sizeof fromdir, ".");
        else snprintf(fromdir, sizeof fromdir, "%.*s", (int)(cut - name + (cut == co)), name);
        from = fromdir;
    }
#if !defined(__AROS__)
    if (from == NULL) from = ".";
#endif
    {
        const char *w = a->arch;
        struct pkg_options aa = *a;
        aa.arch = w;
        aa.root = root;
        if (resolve_arch(&aa) != 0) { free(data); return 1; }
    }
    in.m = NULL;
    in.n = 0;
    if (root != NULL) {
        char *dbd = pkg_join(root, ".pkg/db");
        if (dbd != NULL && pkg_fs_exists(dbd) && load_all(root, &in) != 0) { free(dbd); free(data); return 1; }
        free(dbd);
    }
    r = (struct resolution *)malloc(sizeof *r);
    if (r == NULL) { installed_free(&in); free(data); return refuse("out of memory"); }

    if (!program) {
        const char *base = strrchr(name, ':');
        base = base ? base + 1 : name;
        resolve_one(a, name, a->version, root, from, &in, r);
        resolve_print(r, base);
        if (a->version != NULL)
            kv("satisfies", "%s", r->rc == 0 ? "yes" : "no");
        rc = r->rc;
        if (rc == 0) {
            kv("result", "resolved");
            kv("summary", "%s", r->summary);
            if (!machine) say_result("%s", r->summary);
        } else {
            refuse_c(rc, "%s", r->summary);
        }
    } else {
        struct pkg_strs names = { NULL, 0 };
        size_t i, bad = 0, first = 0;
        static const int widths[] = { 22, 36, 8, 0 };
        lib_names(data, len, &names);
        kv("program", "%s", name);
        if (!machine) tbl_head(widths, "Library\tTaken from\tVersion\tVerdict");
        for (i = 0; i < names.n; i++) {
            const char *verdict;
            char where[1100];
            resolve_one(a, names.v[i], NULL, root, from, &in, r);
            if (r->loaded == 1) snprintf(where, sizeof where, "memory");
            else if (r->taken != (size_t)-1) snprintf(where, sizeof where, "%s", r->c[r->taken].path);
            else snprintf(where, sizeof where, "-");
            verdict = r->loaded == 1 ? "loaded" : r->taken != (size_t)-1 ? r->c[r->taken].verdict
                    : is_system_lib(names.v[i]) ? "part of AROS, not in this root" : "found nowhere";
            if (r->rc != 0 && !(r->rc == 11 && is_system_lib(names.v[i]))) {
                if (!bad++) first = (size_t)r->rc;
            }
            if (machine) {
                char j[1400];
                snprintf(j, sizeof j, "%s %s %s", names.v[i], where,
                         r->loaded == 1 ? r->loaded_ver : r->taken != (size_t)-1 && r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-");
                rec_item("library", j, "name", names.v[i], "from", where, "verdict", verdict, NULL);
            } else {
                tbl_row("%s\t%s\t%s\t%s", names.v[i], where,
                        r->loaded == 1 ? r->loaded_ver : r->taken != (size_t)-1 && r->c[r->taken].ver[0] ? r->c[r->taken].ver : "-",
                        verdict);
            }
            if ((r->rc != 0 && !(r->rc == 11 && is_system_lib(names.v[i]))) || r->shadows) {
                size_t k;
                for (k = 0; k < r->n; k++)
                    if (r->c[k].next[0]) {
                        if (machine) kv("next-step", "%s %s", names.v[i], r->c[k].next);
                        else say_detail("%s: %s", names.v[i], r->c[k].next);
                    }
            }
        }
        if (!machine) tbl_end();
        if (names.n == 0) {
            kv("result", "resolved");
            if (!machine) say_result("%s names no library or device", name);
        } else if (bad == 0) {
            kv("result", "resolved");
            kv("summary", "all %lu libraries %s names resolve", (unsigned long)names.n, name);
            if (!machine) say_result("all %lu libraries %s names resolve", (unsigned long)names.n, name);
        } else {
            rc = (int)first;
            refuse_c(rc, "%lu of the %lu libraries %s names would fail to open; RESOLVE <library> "
                     "shows each place the loader looks", (unsigned long)bad, (unsigned long)names.n, name);
        }
        pkg_strs_free(&names);
    }
    free(r);
    free(data);
    installed_free(&in);
    return rc == 0 ? 0 : 1;
}

static int cmd_repair(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    size_t p, npk = 0, refused = 0;
    unsigned long total = 0, total_aside = 0, fixed_pk = 0;
    int first = 0;
    char firstwhy[200] = "";

    if (!a->all && a->target == NULL) return refuse_c(20, "name the package to repair, or REPAIR ALL");
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, &ix) != 0) return 1;
    if (a->all) {
        if (load_all(a->root, &in) != 0) { free(ix.e); return 1; }
    } else {
        in.m = NULL;
        in.n = 0;
    }
    npk = a->all ? in.n : 1;
    for (p = 0; p < npk; p++) {
        const char *name = a->all ? in.m[p].name : a->target;
        unsigned long r = 0, s = 0;
        if (repair_one(a, &ix, name, &r, &s) != 0) {
            refused++;
            if (!first) {
                first = refused_class ? refused_class : 17;
                snprintf(firstwhy, sizeof firstwhy, "%s", name);
            }
            if (machine) kv("refused", "%s", name);
            refused_class = 0;
            continue;
        }
        total += r;
        total_aside += s;
        if (r) fixed_pk++;
        if (machine) kv("package", "%s %s", name, r ? "repaired" : "intact");
        else if (r) say_pkgline(name, "%lu file%s put back", r, r == 1 ? "" : "s");
    }
    installed_free(&in);
    free(ix.e);
    kv("restored", "%lu", total);
    kv("set-aside", "%lu", total_aside);
    if (refused) {
        refused_class = first;
        kv("result", "refused");
        summary_line("%lu file%s put back in %lu package%s; %lu package%s could not be repaired, "
           "first %s", total, total == 1 ? "" : "s", fixed_pk, fixed_pk == 1 ? "" : "s",
           (unsigned long)refused, refused == 1 ? "" : "s", firstwhy);
        return 1;
    }
    kv("result", total ? (dryrun ? "would-repair" : "repaired") : "unchanged");
    if (total == 0)
        summary_line("nothing needed repair: every file is the one installed, or a configuration "
           "file someone edited");
    else
        summary_line("%lu file%s %sput back in %lu package%s%s", total, total == 1 ? "" : "s",
           dryrun ? "would be " : "", fixed_pk, fixed_pk == 1 ? "" : "s",
           total_aside ? "; the changed ones kept beside as <file>.pkgold" : "");
    return 0;
}

/* Take one package out of an in-memory list, as removing it would. */
static void drop_installed(struct installed *in, const char *name)
{
    size_t i;
    for (i = 0; i < in->n; i++) {
        if (strcmp(in->m[i].name, name) != 0)
            continue;
        pkg_manifest_free(&in->m[i]);
        memmove(&in->m[i], &in->m[i + 1], (in->n - i - 1) * sizeof in->m[0]);
        in->n--;
        return;
    }
}

static int remove_orphans(const struct pkg_options *a)
{
    struct installed in;
    size_t *which, n, i, total = 0, nfailed = 0, at_f = 0;
    int loaded = 0, first_class = 0;
    const char *first_next = NULL;
    char failed[2048];              /* " name name ... ": those that could not go, not tried again */

    failed[0] = '\0';

    for (;;) {
        /* A dry run works on the list in memory, since nothing leaves the disk. */
        if (!(dryrun && loaded) && load_all(a->root, &in) != 0)
            return 1;
        loaded = 1;
        which = malloc((in.n ? in.n : 1) * sizeof *which);
        if (which == NULL) { installed_free(&in); return refuse("out of memory"); }
        n = find_orphans(&in, a->root, which);
        {
            /* the ones that already failed are not tried again */
            size_t kept_n = 0;
            for (i = 0; i < n; i++) {
                char key[80];
                snprintf(key, sizeof key, " %s ", in.m[which[i]].name);
                if (strstr(failed, key) == NULL)
                    which[kept_n++] = which[i];
            }
            n = kept_n;
        }
        for (i = 0; i < n; i++) {
            const struct pkg_manifest *m = &in.m[which[i]];
            size_t r, k, g;
            int ok_rm;
            quiet = 1;
            ok_rm = remove_files(a->root, m, &r, &k, &g, 1) == 0;
            quiet = 0;
            if (!ok_rm) {
                /* this one stays, with its reason; the others still go */
                char *q, jn[2600], codes[8];
                for (q = quiet_reason; *q; q++) if (*q == '\n') *q = ' ';
                if (first_class == 0) { first_class = refused_class; first_next = refused_next; }
                snprintf(codes, sizeof codes, "%d", refused_class);
                snprintf(jn, sizeof jn, "%s %s %s %s", m->name, m->version, class_name(refused_class),
                         quiet_reason);
                if (machine)
                    rec_item("refused", jn, "name", m->name, "version", m->version, "class",
                             class_name(refused_class), "code", codes, "reason", quiet_reason, NULL);
                else
                    say_pkgline(m->name, "not removed: %s", quiet_reason);
                if (at_f + 80 < sizeof failed)
                    at_f += (size_t)snprintf(failed + at_f, sizeof failed - at_f, "%s%s ",
                                             at_f ? "" : " ", m->name);
                nfailed++;
                refused_class = 0;
                refused_next = NULL;
                continue;
            }
            total++;
            if (machine)
            {
                char j[140];
                snprintf(j, sizeof j, "%s %s", m->name, m->version);
                rec_item("package", j, "name", m->name, "version", m->version, NULL);
            }
            else
                say_pkgline(m->name, "%s %s, which nothing needed (%lu file%s%s)",
                        m->version, dryrun ? "would be removed" : "removed",
                        (unsigned long)r, r == 1 ? "" : "s", k ? ", edited files kept" : "");
        }
        if (dryrun) {
            char names[64][65];
            size_t k2, nn = n < 64 ? n : 64;
            for (k2 = 0; k2 < nn; k2++)
                snprintf(names[k2], sizeof names[k2], "%s", in.m[which[k2]].name);
            for (k2 = 0; k2 < nn; k2++)
                drop_installed(&in, names[k2]);
        }
        free(which);
        if (!dryrun || n == 0)
            installed_free(&in);
        if (n == 0)
            break;              /* removing one orphan can orphan its own dependencies */
    }
    {
        char summary[600];
        if (total == 0 && nfailed == 0)
            snprintf(summary, sizeof summary, "nothing to remove: every installed package is "
                     "wanted or needed by one that is");
        else if (nfailed == 0)
            snprintf(summary, sizeof summary, "%s %lu package%s nothing needed any more",
                     dryrun ? "would remove" : "removed", (unsigned long)total, total == 1 ? "" : "s");
        else
            snprintf(summary, sizeof summary, "%s %lu package%s nothing needed; %lu could not be "
                     "removed:%s", dryrun ? "would remove" : "removed", (unsigned long)total,
                     total == 1 ? "" : "s", (unsigned long)nfailed, failed);
        if (nfailed == 0) {
            kv("result", "%s", total ? res("removed", "would-remove") : "unchanged");
        } else {
            refused_class = first_class;
            refused_next = first_next;
            kv("result", "refused");
            kv("class", "%s", class_name(first_class));
            kv("code", "%d", first_class);
        }
        kv("count", "%lu", (unsigned long)total);
        kv("summary", "%s", summary);
        if (nfailed)
            kv("next", "%s", first_next ? first_next : next_default(first_class));
        if (!machine)
            say_result("%s", summary);
    }
    return nfailed ? 1 : 0;
}

static int cmd_remove(const struct pkg_options *a)
{
    struct pkg_manifest m;
    struct installed in;
    size_t removed, kept, gone, i, n, *which;
    char who[400];

    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (a->orphans && a->target == NULL) return remove_orphans(a);
    if (a->target == NULL)  return refuse_c(20, "name the package to remove, or ORPHANS");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    if (load_all(a->root, &in) != 0) { pkg_manifest_free(&m); return 1; }
    if (needed_by(&in, m.name, who, sizeof who)) {
        installed_free(&in);
        refuse_c(16, "%s is needed by %s; nothing was removed. Removing it would break %s",
                 m.name, who, strchr(who, ',') ? "them" : "it");
        pkg_manifest_free(&m);
        return 1;
    }
    installed_free(&in);
    if (remove_files(a->root, &m, &removed, &kept, &gone, 1) != 0) {
        pkg_manifest_free(&m);
        return 1;
    }
    kv("result", "%s", res("removed", "would-remove"));
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("root", "%s", a->root);
    kv("removed", "%lu", (unsigned long)removed);
    kv("gone", "%lu", (unsigned long)gone);
    if (!machine) {
    {
    char tail[80];
    int at = 0;
    tail[0] = '\0';
    if (kept) at += snprintf(tail + at, sizeof tail - (size_t)at, ", %lu kept", (unsigned long)kept);
    if (gone) snprintf(tail + at, sizeof tail - (size_t)at, ", %lu already gone", (unsigned long)gone);
    say_result("%s %s %s from %s: %lu file%s %s%s", dryrun ? "would remove" : "removed", m.name,
           m.version, a->root, (unsigned long)removed, removed == 1 ? "" : "s",
           dryrun ? "to remove" : "removed", tail);
    }
    }
    /* Say what this leaves behind; removing it is a separate, explicit act. */
    if (load_all(a->root, &in) == 0) {
        if (dryrun)
            drop_installed(&in, m.name);
        which = malloc((in.n ? in.n : 1) * sizeof *which);
        n = which ? find_orphans(&in, a->root, which) : 0;
        for (i = 0; i < n; i++) {
            if (machine)
            {
                char j[140];
                snprintf(j, sizeof j, "%s %s", in.m[which[i]].name, in.m[which[i]].version);
                rec_item("orphan", j, "name", in.m[which[i]].name, "version",
                         in.m[which[i]].version, NULL);
            }
            else
                say_detail("%s %s is no longer needed by anything; REMOVE ORPHANS takes it out",
                        in.m[which[i]].name, in.m[which[i]].version);
        }
        free(which);
        installed_free(&in);
    }
    /* Pkg removing itself leaves the root's records and pinned keys, which a
     * later Pkg picks up; say so, since nothing else would. */
    if (!dryrun && strcmp(m.name, "pkg") == 0)
        hint("pkg is gone, but %s%s.pkg still holds what is installed, the keys pinned for it "
             "and the downloads; a later pkg takes over from there, and the guide to removing "
             "pkg says what to delete when nothing should stay", a->root,
             *a->root && strchr(":/", a->root[strlen(a->root) - 1]) ? "" : "/");
    pkg_manifest_free(&m);
    return 0;
}

/* ---- keeping a root current: STATUS and UPGRADE ALL ------------------- *
 *
 * Pkg carries no scheduler and no daemon: a person, a startup script or any
 * scheduler runs these. STATUS compares every installed package with a
 * channel, choosing as UPGRADE chooses; UPGRADE ALL upgrades each package a
 * newer version is offered for, exactly as UPGRADE <name> would, a package
 * before what depends on it, and stops at the first refusal. Neither asks
 * anything, ever: what belongs to the requester (going back a version, a
 * new key, an edited file) is refused or reported, never decided. */

struct planned_up { const char *name, *version; };
static struct planned_up *planned;
static size_t nplanned;

static const char *planned_version(const char *name)
{
    size_t i;
    for (i = 0; i < nplanned; i++)
        if (strcmp(planned[i].name, name) == 0)
            return planned[i].version;
    return NULL;
}

/* One installed package against the channel. */
struct standing {
    const struct pkg_manifest *m;
    const struct entry *offer;   /* what UPGRADE <name> would take; NULL: nothing */
    const char *state;           /* current, upgradable, withdrawn, not-offered, edited */
    int newer;                   /* offer is higher than the installed version */
    int withdrawn;               /* the installed version was withdrawn by its publisher */
    int edited;                  /* a file differs from the installed manifest */
    int conflict;                /* two listed channels offer it under different keys */
    char why[2400];              /* the conflict, in full */
};

/* The machine UPGRADE <name> chooses for: the root's, else the installed
 * package's own CPU. */
static void arch_for(const char *base, const struct pkg_manifest *m)
{
    target_arch = base;
    if (target_arch == NULL && strcmp(m->architecture, "generic") != 0)
        target_arch = m->architecture;
}

static void stand(const char *root, const struct index *ix, const char *base,
                  const struct pkg_manifest *m, struct standing *s)
{
    size_t i;
    memset(s, 0, sizeof *s);
    s->m = m;
    arch_for(base, m);
    for (i = 0; i < ix->n; i++) {
        const struct entry *e = &ix->e[i];
        if (e->withdrawn && strcmp(e->name, m->name) == 0
            && pkg_version_cmp(e->version, m->version) == 0
            && (strcmp(e->arch, m->architecture) == 0 || strcmp(e->arch, "generic") == 0))
            s->withdrawn = 1;
    }
    s->offer = pick(ix, m->name, NULL);
    if (s->offer == NULL && pick_refused) {
        /* Reported per package: one package two channels disagree about
         * must not stop the others being compared or upgraded. */
        s->conflict = 1;
        snprintf(s->why, sizeof s->why, "%s", pick_why);
        refused_class = 0;
        refused_next = NULL;
    }
    s->newer = s->offer != NULL && pkg_version_cmp(s->offer->version, m->version) > 0;
    /* The check VERIFY makes, size and digest, stopping at the first edit. */
    for (i = 0; i < m->nfiles && !s->edited; i++)
        s->edited = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 1;
    if (s->conflict)       s->state = "conflict";
    else if (s->offer == NULL)  s->state = s->withdrawn ? "withdrawn" : "not-offered";
    else if (s->newer)     s->state = s->edited ? "edited" : "upgradable";
    else if (s->withdrawn) s->state = "withdrawn";
    else if (s->edited)    s->state = "edited";
    else                   s->state = "current";
    tr("%s %s: %s, the channel offers %s", m->name, m->version, s->state,
       s->offer ? s->offer->version : "nothing for it");
}

static void note(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("note", "%s", buf);
    else
        say_note("%s", buf);
}

/* ROOT and CHANNEL, the root's machine, the channel's index and every
 * installed package. */
static int keep_current_setup(const struct pkg_options *a, struct index *ix, struct installed *in)
{
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->channel != NULL && !is_url(a->channel) && !pkg_fs_is_dir(a->channel))
        return refuse_c(11, "there is no channel at %s: not mounted, or not the path meant; "
                        "nothing was checked or changed", a->channel);
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, ix) != 0) return 1;
    if (load_all(a->root, in) != 0) { free(ix->e); return 1; }
    return 0;
}

/* ---- CHANNEL: the list a root keeps ------------------------------------ */

/* The distinct package names a channel offers, for what ADD says back. */
static size_t offered_count(const struct index *ix)
{
    size_t i, j, n = 0;
    for (i = 0; i < ix->n; i++) {
        int seen = 0;
        for (j = 0; j < i && !seen; j++)
            if (strcmp(ix->e[j].name, ix->e[i].name) == 0)
                seen = 1;
        if (!seen)
            n++;
    }
    return n;
}

static int cmd_channel(const struct pkg_options *a)
{
    struct chanlist cl;
    const char *what = a->target, *ch = a->nalso > 0 ? a->also[0] : NULL;
    size_t i;
    int add, remove, list;

    if (what == NULL)
        return refuse_c(20, "CHANNEL takes ADD, LIST or REMOVE");
    add = ascii_casecmp(what, "ADD") == 0;
    remove = ascii_casecmp(what, "REMOVE") == 0;
    list = ascii_casecmp(what, "LIST") == 0;
    if (!add && !remove && !list)
        return refuse_c(20, "\"%s\" is not one of CHANNEL's words; they are ADD, LIST and REMOVE",
                        what);
    if (a->root == NULL)
        return refuse_c(20, "name the root whose channels these are with ROOT <dir>");
    if ((add || remove) && ch == NULL)
        return refuse_c(20, "CHANNEL %s takes the channel: a directory, or an http(s) URL",
                        add ? "ADD" : "REMOVE");
    if (a->nalso > 1)
        return refuse_c(20, "CHANNEL takes one channel at a time");
    if (chanlist_read(a->root, &cl) != 0)
        return 1;

    if (list) {
        kv("result", "shown");
        if (!machine && cl.n > 0) {
            static const int widths[] = { 4, 0 };
            tbl_head(widths, "In\tChannel");
        }
        for (i = 0; i < cl.n; i++) {
            if (machine)
                rec_item("channel", cl.v[i], "channel", cl.v[i], NULL);
            else
                tbl_row("%lu\t%s", (unsigned long)i + 1, cl.v[i]);
        }
        if (!machine && cl.n > 0)
            tbl_end();
        kv("count", "%lu", (unsigned long)cl.n);
        kv("root", "%s", a->root);
        if (cl.n == 0) {
            kv("summary", "%s lists no channel", a->root);
            if (!machine)
                say_result("%s lists no channel", a->root);
            hint("CHANNEL ADD <dir|url> ROOT %s adds one; from then on INSTALL, UPGRADE, STATUS, "
                 "SEARCH and SHOW read it without CHANNEL on the line", a->root);
        } else {
            kv("summary", "%lu channel%s, asked in this order",
               (unsigned long)cl.n, cl.n == 1 ? "" : "s");
            if (!machine)
                say_result("%lu channel%s in %s, asked in this order", (unsigned long)cl.n,
                           cl.n == 1 ? "" : "s", a->root);
        }
        chanlist_free(&cl);
        return 0;
    }

    for (i = 0; i < cl.n; i++)
        if (strcmp(cl.v[i], ch) == 0)
            break;
    if (add) {
        size_t offers = 0;
        if (i < cl.n) {
            chanlist_free(&cl);
            return refuse_c(15, "%s already lists the channel %s, in place %lu; nothing was "
                            "changed. CHANNEL LIST ROOT %s shows them", a->root, ch,
                            (unsigned long)i + 1, a->root);
        }
        if (cl.n >= PKG_MAX_CHANNELS) {
            chanlist_free(&cl);
            return refuse_c(15, "%s already lists %d channels, which is as many as pkg reads at "
                            "once; remove one first", a->root, PKG_MAX_CHANNELS);
        }
        if (!dryrun) {
            /* A channel that cannot be read is not added: the mistake is
             * found now, not at the next INSTALL. */
            struct index ix;
            if (!is_url(ch) && !pkg_fs_is_dir(ch)) {
                chanlist_free(&cl);
                return refuse_c(11, "there is no channel at %s: not mounted, or not the path "
                                "meant; nothing was added", ch);
            }
            if (read_index(ch, &ix) != 0) { chanlist_free(&cl); return 1; }
            offers = offered_count(&ix);
            free(ix.e);
        }
        cl.v[cl.n] = pkg_strdup(ch);
        if (cl.v[cl.n] == NULL) { chanlist_free(&cl); return refuse("out of memory"); }
        cl.n++;
        if (!dryrun && chanlist_write(a->root, &cl) != 0) {
            chanlist_free(&cl);
            return refuse_c(17, "cannot write the channel list in %s: %s", a->root,
                            strerror(errno));
        }
        kv("result", "%s", res("added", "would-add"));
        kv("channel", "%s", ch);
        kv("root", "%s", a->root);
        kv("position", "%lu", (unsigned long)cl.n);
        if (!dryrun)
            kv("packages", "%lu", (unsigned long)offers);
        if (!machine) {
            if (dryrun)
                say_result("would add %s to %s, in place %lu", ch, a->root, (unsigned long)cl.n);
            else
                say_result("added %s to %s, in place %lu: it offers %lu package%s", ch, a->root,
                           (unsigned long)cl.n, (unsigned long)offers, offers == 1 ? "" : "s");
        }
        if (cl.n == 1)
            hint("INSTALL, UPGRADE, STATUS, SHOW, REPAIR, ROLLBACK and SEARCH now read this "
                 "channel when CHANNEL is left out; CHANNEL <dir|url> on the line still means "
                 "that channel alone");
        chanlist_free(&cl);
        return 0;
    }

    /* REMOVE */
    if (i == cl.n) {
        chanlist_free(&cl);
        return refuse_c(11, "%s does not list the channel %s; nothing was changed. CHANNEL LIST "
                        "ROOT %s shows the ones it lists", a->root, ch, a->root);
    }
    free(cl.v[i]);
    for (; i + 1 < cl.n; i++)
        cl.v[i] = cl.v[i + 1];
    cl.n--;
    if (!dryrun && chanlist_write(a->root, &cl) != 0) {
        chanlist_free(&cl);
        return refuse_c(17, "cannot write the channel list in %s: %s", a->root, strerror(errno));
    }
    kv("result", "%s", res("removed", "would-remove"));
    kv("channel", "%s", ch);
    kv("root", "%s", a->root);
    kv("count", "%lu", (unsigned long)cl.n);
    if (!machine)
        say_result("%s %s from %s; %lu channel%s left", dryrun ? "would remove" : "removed", ch,
                   a->root, (unsigned long)cl.n, cl.n == 1 ? "" : "s");
    note("what was installed from it stays installed; nothing was removed from this root");
    chanlist_free(&cl);
    return 0;
}

/* ---- SEARCH ------------------------------------------------------------ */

struct hit {
    char name[65];
    char version[64];
    char arch[120];
    char short_desc[200];
    char chan[700];
};

struct hits {
    struct hit *v;
    size_t      n, cap;
};

static int hit_add(struct hits *h, const struct hit *x)
{
    size_t i;
    for (i = 0; i < h->n; i++)
        if (strcmp(h->v[i].name, x->name) == 0 && strcmp(h->v[i].chan, x->chan) == 0)
            return 0;               /* one row per package per channel */
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 32;
        struct hit *w = (struct hit *)realloc(h->v, cap * sizeof *w);
        if (w == NULL) return -1;
        h->v = w;
        h->cap = cap;
    }
    h->v[h->n++] = *x;
    return 0;
}

static int by_hit(const void *x, const void *y)
{
    const struct hit *a = (const struct hit *)x, *b = (const struct hit *)y;
    int c = ascii_casecmp(a->name, b->name);
    return c ? c : strcmp(a->chan, b->chan);
}

/* Does `text` hold `word`, whatever the case? */
static int holds_word(const char *text, const char *word)
{
    size_t wl = strlen(word), i;
    if (text == NULL) return 0;
    for (i = 0; text[i]; i++)
        if (ascii_casecmp_n(text + i, word, wl) == 0)
            return 1;
    return 0;
}

static int strs_hold_word(const struct pkg_strs *l, const char *word)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (holds_word(l->v[i], word))
            return 1;
    return 0;
}

/* Every field SEARCH looks in, for one word. */
static int manifest_holds(const struct pkg_manifest *m, const char *word)
{
    const struct pkg_about *ab = &m->about;
    return holds_word(m->name, word) || holds_word(ab->short_desc, word)
        || holds_word(ab->category, word) || strs_hold_word(&ab->tags, word)
        || strs_hold_word(&ab->description, word) || strs_hold_word(&m->provides, word);
}

/* One channel read package by package: the index, then the manifest of each
 * newest version that is not withdrawn. Directory channels and plain web
 * servers are read this way, and so is a portal whose API did not answer. */
static int search_local(const char *channel, const char *const *words, unsigned nwords,
                        struct hits *h)
{
    struct index ix;
    size_t i, j;

    if (read_index(channel, &ix) != 0) { refused_class = 0; refused_next = NULL; return 1; }
    for (i = 0; i < ix.n; i++) {
        const struct entry *best = NULL;
        struct pkg_manifest m;
        struct hit x;
        unsigned char *buf;
        char *mp, err[200];
        size_t len;
        unsigned w;
        int seen = 0, all = 1;

        for (j = 0; j < i; j++)
            if (strcmp(ix.e[j].name, ix.e[i].name) == 0) seen = 1;
        if (seen)
            continue;
        for (j = 0; j < ix.n; j++) {
            const struct entry *e = &ix.e[j];
            if (strcmp(e->name, ix.e[i].name) != 0 || e->withdrawn || !arch_matches(e))
                continue;
            if (best == NULL || pkg_version_cmp(e->version, best->version) > 0)
                best = e;
        }
        if (best == NULL)
            continue;
        if (cancelled("while reading the channel; nothing was changed")) { free(ix.e); return 1; }
        if ((mp = object_path(channel, best->digest, "manifest")) == NULL)
            continue;
        if (pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
        free(mp);
        if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) != 0) {
            free(buf);
            continue;
        }
        free(buf);
        for (w = 0; w < nwords && all; w++)
            if (!manifest_holds(&m, words[w]))
                all = 0;
        if (all) {
            memset(&x, 0, sizeof x);
            snprintf(x.name, sizeof x.name, "%s", m.name);
            snprintf(x.version, sizeof x.version, "%s", best->version);
            snprintf(x.chan, sizeof x.chan, "%s", channel);
            snprintf(x.short_desc, sizeof x.short_desc, "%s",
                     m.about.short_desc ? m.about.short_desc : "-");
            /* every CPU this version is published for */
            for (j = 0; j < ix.n; j++) {
                size_t at = strlen(x.arch);
                if (strcmp(ix.e[j].name, m.name) != 0
                    || pkg_version_cmp(ix.e[j].version, best->version) != 0
                    || strstr(x.arch, ix.e[j].arch) != NULL)
                    continue;
                if (at + strlen(ix.e[j].arch) + 2 < sizeof x.arch)
                    snprintf(x.arch + at, sizeof x.arch - at, "%s%s", at ? "," : "",
                             ix.e[j].arch);
            }
            if (hit_add(h, &x) != 0) { pkg_manifest_free(&m); free(ix.e); return refuse("out of memory"); }
        }
        pkg_manifest_free(&m);
    }
    free(ix.e);
    return 0;
}

/* ---- the portal's search API ------------------------------------------- *
 *
 * A portal channel holds hundreds of packages, and reading every manifest
 * over the network to answer one question is minutes of waiting. The portal
 * answers the same question itself at /api/search (portal/src/Portal/Api,
 * Channels/Search.cs), so that is asked first: one request per word, the
 * answers intersected by name, which is Pkg's rule that every word must
 * match, expressed in the portal's own index, description and files
 * included. Anything unusable in the answer and the channel is read the
 * long way instead, without a word about it: the result is the same, only
 * slower.
 *
 * Pkg carries no JSON library and gains none for this. The reader below
 * takes exactly the fields these records hold, and refuses anything it does
 * not recognise rather than guessing. */

/* The value of "key" in the JSON object at *p, which must be a string,
 * copied unescaped into out. 1 when it was found. */
static int json_str(const char *obj, const char *key, char *out, size_t ol)
{
    char pat[64];
    const char *p;
    size_t at = 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(obj, pat);
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && at + 1 < ol) {
        if (*p == '\\') {
            p++;
            if (*p == '\0') break;
            out[at++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            p++;
            continue;
        }
        out[at++] = *p++;
    }
    out[at] = '\0';
    return 1;
}

/* "key": [ "a", "b" ] -> "a,b". */
static int json_strs(const char *obj, const char *key, char *out, size_t ol)
{
    char pat[64];
    const char *p;
    size_t at = 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(obj, pat);
    out[0] = '\0';
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            if (at && at + 1 < ol) out[at++] = ',';
            while (*p && *p != '"' && at + 1 < ol) {
                if (*p == '\\') p++;
                if (*p) out[at++] = *p++;
            }
        }
        if (*p) p++;
    }
    out[at] = '\0';
    return 1;
}

/* The end of the JSON object that starts at `p` (which points at its '{'),
 * or NULL when it does not end. Strings and their escapes are stepped over
 * so a brace inside a description does not close the record. */
static const char *json_object_end(const char *p)
{
    int depth = 0, instr = 0;
    for (; *p; p++) {
        if (instr) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') instr = 0;
            continue;
        }
        if (*p == '"') instr = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p;
            if (depth < 0) return NULL;
        }
    }
    return NULL;
}

/* https://host/contrib-nightly -> the site, and the channel's name there. */
static int portal_parts(const char *url, char *site, size_t sl, char *name, size_t nl)
{
    const char *after = strstr(url, "://"), *slash;
    size_t n;
    if (after == NULL) return 0;
    after += 3;
    slash = strrchr(after, '/');
    if (slash == NULL || slash[1] == '\0') return 0;
    n = (size_t)(slash - url);
    if (n + 1 >= sl) return 0;
    memcpy(site, url, n);
    site[n] = '\0';
    snprintf(name, nl, "%s", slash + 1);
    return name[0] != '\0';
}

static void url_escape(const char *s, char *out, size_t ol)
{
    size_t at = 0;
    for (; *s && at + 4 < ol; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~')
            out[at++] = (char)c;
        else
            at += (size_t)snprintf(out + at, ol - at, "%%%02X", c);
    }
    out[at] = '\0';
}

/* One /api/search request, its records appended to `h`, keeping only the
 * ones this channel published. -1 when the answer was not usable and the
 * channel must be read the long way instead. */
static int api_words(const char *channel, const char *const *words, unsigned nwords,
                     struct hits *h)
{
    char site[900], cname[200], *cache, *local = NULL;
    struct hits got[8];
    unsigned w, nq = nwords ? nwords : 1;
    int rc = -1;
    size_t i, j;

    memset(got, 0, sizeof got);
    if (nq > 8) nq = 8;
    if (!portal_parts(channel, site, sizeof site, cname, sizeof cname))
        return -1;
    cache = pkg_cache_dir();
    if (cache == NULL) return -1;
    local = pkg_join(cache, "search.json");
    free(cache);
    if (local == NULL) return -1;
    for (w = 0; w < nq; w++) {
        char url[2300], q[600], arch[80], err[400];
        unsigned char *buf;
        size_t len;
        const char *p, *end;
        url_escape(nwords ? words[w] : "", q, sizeof q);
        arch[0] = '\0';
        if (target_arch != NULL)
            snprintf(arch, sizeof arch, "&arch=%s", target_arch);
        snprintf(url, sizeof url, "%s/api/search?q=%s&channel=%s%s", site, q, cname, arch);
        if (pkg_net_get(url, local, err, sizeof err) != 0) {
            tr("the portal's search API did not answer (%s): reading %s the long way", err,
               channel);
            goto out;
        }
        if (pkg_fs_read(local, &buf, &len) != 0)
            goto out;
        p = strstr((const char *)buf, "\"results\":");
        if (p == NULL) { free(buf); tr("the portal's answer holds no results: reading %s the long way", channel); goto out; }
        p = strchr(p, '[');
        if (p == NULL) { free(buf); goto out; }
        end = (const char *)buf + len;
        for (p++; p < end && *p; ) {
            const char *stop;
            char rec[9000], chn[200];
            struct hit x;
            size_t rl;
            while (p < end && *p && *p != '{' && *p != ']') p++;
            if (p >= end || *p != '{') break;
            stop = json_object_end(p);
            if (stop == NULL) break;
            rl = (size_t)(stop - p) + 1u;
            if (rl >= sizeof rec) { p = stop + 1; continue; }
            memcpy(rec, p, rl);
            rec[rl] = '\0';
            p = stop + 1;
            memset(&x, 0, sizeof x);
            if (!json_str(rec, "name", x.name, sizeof x.name)
                || !json_str(rec, "version", x.version, sizeof x.version)
                || !json_str(rec, "channel", chn, sizeof chn)) {
                free(buf);
                tr("a record of the portal's answer is not what pkg expects: reading %s the long way",
                   channel);
                goto out;
            }
            if (strcmp(chn, cname) != 0)
                continue;
            json_strs(rec, "archs", x.arch, sizeof x.arch);
            if (!json_str(rec, "short", x.short_desc, sizeof x.short_desc) || !x.short_desc[0])
                snprintf(x.short_desc, sizeof x.short_desc, "%s", "-");
            if (x.arch[0] == '\0') snprintf(x.arch, sizeof x.arch, "%s", "-");
            snprintf(x.chan, sizeof x.chan, "%s", channel);
            if (hit_add(&got[w], &x) != 0) { free(buf); goto out; }
        }
        free(buf);
    }
    /* every word must match: a name that is in every answer */
    for (i = 0; i < got[0].n; i++) {
        int all = 1;
        for (w = 1; w < nq && all; w++) {
            int here = 0;
            for (j = 0; j < got[w].n && !here; j++)
                if (strcmp(got[w].v[j].name, got[0].v[i].name) == 0)
                    here = 1;
            all = here;
        }
        if (all && hit_add(h, &got[0].v[i]) != 0)
            goto out;
    }
    tr("%s answered for %u word%s through its search API", channel, nq, nq == 1 ? "" : "s");
    rc = 0;
out:
    for (w = 0; w < nq; w++)
        free(got[w].v);
    pkg_fs_unlink(local);
    free(local);
    return rc;
}

static int cmd_search(const struct pkg_options *a)
{
    struct chanlist cl;
    struct hits h;
    const char *words[16];
    unsigned nwords = 0, w;
    size_t i;
    int rc = 1, several;
    char line[900];

    memset(&h, 0, sizeof h);
    if (a->target == NULL)
        return refuse_c(20, "SEARCH takes the words to look for: SEARCH <word>...");
    words[nwords++] = a->target;
    for (w = 0; w < a->nalso && nwords < sizeof words / sizeof words[0]; w++)
        words[nwords++] = a->also[w];
    if (a->arch != NULL) {
        const char *why = pkg_check_arch(a->arch);
        if (why != NULL)
            return refuse_c(20, "ARCH \"%s\": %s", a->arch, why);
        target_arch = a->arch;
    } else if (a->root != NULL) {
        if (resolve_arch(a) != 0)
            return 1;
    } else {
        target_arch = NULL;
    }
    cl.n = 0;
    if (a->channel != NULL) {
        cl.v[cl.n] = pkg_strdup(a->channel);
        if (cl.v[cl.n] == NULL) return refuse("out of memory");
        cl.n++;
    } else if (a->root == NULL) {
        return refuse_c(20, "name the channel to search with CHANNEL <dir|url>, or the root "
                        "whose channels to search with ROOT <dir>");
    } else {
        if (chanlist_read(a->root, &cl) != 0)
            return 1;
        if (cl.n == 0) {
            chanlist_free(&cl);
            return refuse_c(20, "%s lists no channel and no CHANNEL was given; "
                            "CHANNEL ADD <dir|url> ROOT %s adds one", a->root, a->root);
        }
    }
    several = cl.n > 1;
    for (i = 0; i < cl.n; i++) {
        if (is_url(cl.v[i]) && api_words(cl.v[i], words, nwords, &h) == 0)
            continue;
        if (search_local(cl.v[i], words, nwords, &h) != 0 && refused_class)
            goto out;
    }
    qsort(h.v, h.n, sizeof h.v[0], by_hit);
    kv("result", "shown");
    if (!machine && h.n > 0) {
        static const int widths[] = { 22, 14, 10, 0 };
        if (several) {
            static const int wide[] = { 22, 14, 10, 34, 0 };
            tbl_head(wide, "Package\tVersion\tArch\tShort\tChannel");
        } else {
            tbl_head(widths, "Package\tVersion\tArch\tShort");
        }
    }
    for (i = 0; i < h.n; i++) {
        const struct hit *x = &h.v[i];
        if (machine) {
            /* the short description last: it is the only field with spaces */
            if (several) {
                snprintf(line, sizeof line, "%s %s %s %s %s", x->name, x->version, x->arch,
                         x->chan, x->short_desc);
                rec_item("package", line, "name", x->name, "version", x->version, "arch", x->arch,
                         "channel", x->chan, "short", x->short_desc, NULL);
            } else {
                snprintf(line, sizeof line, "%s %s %s %s", x->name, x->version, x->arch,
                         x->short_desc);
                rec_item("package", line, "name", x->name, "version", x->version, "arch", x->arch,
                         "short", x->short_desc, NULL);
            }
        } else if (several) {
            tbl_row("%s\t%s\t%s\t%s\t%s", x->name, x->version, x->arch, x->short_desc, x->chan);
        } else {
            tbl_row("%s\t%s\t%s\t%s", x->name, x->version, x->arch, x->short_desc);
        }
    }
    if (!machine && h.n > 0)
        tbl_end();
    kv("count", "%lu", (unsigned long)h.n);
    {
        char asked[700];
        size_t at = 0;
        asked[0] = '\0';
        for (w = 0; w < nwords && at + 40 < sizeof asked; w++)
            at += (size_t)snprintf(asked + at, sizeof asked - at, "%s%s", at ? " " : "", words[w]);
        if (h.n == 0) {
            kv("summary", "nothing matches %s", asked);
            if (!machine)
                say_result("nothing in %s matches %s", chanlist_text(&cl), asked);
            hint("every word must match, in the name, the short description, the tags, the "
                 "category, the description or what the package provides; fewer words match more");
        } else {
            kv("summary", "%lu package%s match%s %s", (unsigned long)h.n, h.n == 1 ? "" : "s",
               h.n == 1 ? "es" : "", asked);
            if (!machine)
                say_result("%lu package%s match%s %s", (unsigned long)h.n, h.n == 1 ? "" : "s",
                           h.n == 1 ? "es" : "", asked);
        }
    }
    rc = 0;
out:
    free(h.v);
    chanlist_free(&cl);
    return rc;
}

static int cmd_status(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    size_t i, shown = 0, upgradable = 0, at_e = 0, at_w = 0, at_c = 0;
    char edited[400], withdrawn[400], conflicts[400];
    const char *base;
    int rc = 1;

    if (keep_current_setup(a, &ix, &in) != 0) return 1;
    base = target_arch;
    edited[0] = withdrawn[0] = conflicts[0] = '\0';
    if (a->target != NULL) {
        for (i = 0; i < in.n && strcmp(in.m[i].name, a->target) != 0; i++)
            ;
        if (i == in.n) {
            refuse_n(11, "use-install", "%s is not installed in %s", a->target, a->root);
            goto out;
        }
    }
    kv("result", "shown");
    if (!machine && in.n > 0) {
        static const int widths[] = { 24, 12, 0 };
        tbl_head(widths, "Package\tInstalled\tState");
    }
    for (i = 0; i < in.n; i++) {
        struct standing s;
        const char *avail;
        if (a->target != NULL && strcmp(in.m[i].name, a->target) != 0)
            continue;
        if (cancelled("while comparing the root with the channel; nothing was changed"))
            goto out;
        stand(a->root, &ix, base, &in.m[i], &s);
        avail = s.offer ? s.offer->version : "-";
        shown++;
        if (s.newer)
            upgradable++;
        if (s.edited && at_e + 70 < sizeof edited)
            at_e += (size_t)snprintf(edited + at_e, sizeof edited - at_e, "%s%s", at_e ? ", " : "",
                                     s.m->name);
        if (s.conflict && at_c + 70 < sizeof conflicts)
            at_c += (size_t)snprintf(conflicts + at_c, sizeof conflicts - at_c, "%s%s",
                                     at_c ? ", " : "", s.m->name);
        if (s.withdrawn && !s.newer && at_w + 70 < sizeof withdrawn)
            at_w += (size_t)snprintf(withdrawn + at_w, sizeof withdrawn - at_w, "%s%s %s",
                                     at_w ? ", " : "", s.m->name, s.m->version);
        if (machine) {
            char j[500];
            const char *from = nchans > 1 && s.offer ? chan_of(s.offer) : NULL;
            snprintf(j, sizeof j, "%s %s %s %s%s%s", s.m->name, s.m->version, avail, s.state,
                     from ? " " : "", from ? from : "");
            if (from != NULL)
                rec_item("package", j, "name", s.m->name, "installed", s.m->version,
                         "available", avail, "state", s.state, "channel", from, NULL);
            else
                rec_item("package", j, "name", s.m->name, "installed", s.m->version,
                         "available", avail, "state", s.state, NULL);
            if (s.conflict)
                kv("warning", "%s", s.why);
            if (s.withdrawn && s.newer)
                note("%s %s, installed, was withdrawn by its publisher; UPGRADE takes %s",
                     s.m->name, s.m->version, avail);
        } else {
            char what[300], from[200];
            from[0] = '\0';
            if (nchans > 1 && s.offer != NULL)
                snprintf(from, sizeof from, " from %s", chan_of(s.offer));
            if (strcmp(s.state, "conflict") == 0)
                snprintf(what, sizeof what, "two channels offer it under different keys");
            else if (strcmp(s.state, "upgradable") == 0)
                snprintf(what, sizeof what, "upgradable to %s%s", avail, from);
            else if (strcmp(s.state, "withdrawn") == 0)
                snprintf(what, sizeof what, "withdrawn by its publisher%s%s",
                         s.offer ? "; the channel offers " : ", and nothing else is offered",
                         s.offer ? avail : "");
            else if (strcmp(s.state, "not-offered") == 0)
                snprintf(what, sizeof what, "no longer offered by the channel");
            else if (strcmp(s.state, "edited") == 0)
                snprintf(what, sizeof what, "files edited since install%s%s", s.newer ? "; " : "",
                         s.newer ? "upgradable to " : "");
            else
                snprintf(what, sizeof what, "current");
            tbl_row("%s\t%s\t%s%s%s", s.m->name, s.m->version, what,
                strcmp(s.state, "edited") == 0 && s.newer ? avail : "",
                s.withdrawn && s.newer ? " (the installed version was withdrawn)" : "");
        }
    }
    if (!machine && in.n > 0)
        tbl_end();
    kv("count", "%lu", (unsigned long)shown);
    kv("upgradable", "%lu", (unsigned long)upgradable);
    if (shown == 0)
        kv("summary", "nothing is installed in this root");
    else if (upgradable == 0)
        kv("summary", "everything is up to date: %lu package%s, none with a newer version",
           (unsigned long)shown, shown == 1 ? "" : "s");
    else
        kv("summary", "%lu of %lu package%s can be updated", (unsigned long)upgradable,
           (unsigned long)shown, shown == 1 ? "" : "s");
    if (!machine) {
        if (shown == 0)
            say_result("nothing installed in %s", a->root);
        else if (upgradable == 0)
            say_result("%lu package%s in %s, all up to date with %s", (unsigned long)shown,
                shown == 1 ? "" : "s", a->root, chans_text());
        else
            say_result("%lu of %lu package%s in %s can be updated from %s", (unsigned long)upgradable,
                (unsigned long)shown, shown == 1 ? "" : "s", a->root, chans_text());
    }
    if (upgradable > 0) {
        if (a->channel != NULL)
            hint("UPGRADE ALL ROOT %s CHANNEL %s upgrades every one of them, a package before "
                 "what depends on it; with DRYRUN it only says what it would do", a->root,
                 a->channel);
        else
            hint("UPGRADE ALL ROOT %s upgrades every one of them from this root's channels, a "
                 "package before what depends on it; with DRYRUN it only says what it would do",
                 a->root);
    }
    if (at_e > 0)
        hint("files were edited in %s since install: VERIFY <name> names them. An upgrade that "
             "would replace an edited file is refused; what to do with the edit is the "
             "requester's decision", edited);
    if (at_c > 0)
        hint("%s: two of this root's channels offer it under different keys, so nothing is "
             "chosen for it. INSTALL it from the channel whose key is the publisher's, with "
             "CHANNEL <that one>, and this root pins that key from then on; CHANNEL LIST ROOT %s "
             "shows the channels", conflicts, a->root);
    if (at_w > 0)
        hint("%s: withdrawn by the publisher, with nothing newer offered. Going back to the "
             "previous version (ROLLBACK) or waiting for a fixed one is the requester's decision",
             withdrawn);
    rc = 0;
out:
    installed_free(&in);
    free(ix.e);
    return rc;
}

/* The manifest a channel entry names, unverified: only for ordering. The
 * upgrade itself fetches and checks it in full. */
static void offered_manifest(const char *channel, const struct entry *e, struct pkg_manifest *m)
{
    char *mp = object_path(channel, e->digest, "manifest"), err[200];
    unsigned char *buf;
    size_t len;
    pkg_manifest_init(m);
    if (mp != NULL && pkg_fs_read(mp, &buf, &len) == 0) {
        pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
        free(buf);
    }
    free(mp);
}

static int names_dep(const struct pkg_manifest *m, const char *name)
{
    size_t d;
    for (d = 0; d < m->ndeps; d++)
        if (strcmp(m->deps[d].name, name) == 0)
            return 1;
    return 0;
}

/* UPGRADE ALL, as far as possible: one item per package upgraded (package),
 * refused (with its class and reason) or waiting for a refused one
 * (skipped), then counts and a summary sentence. When something was not
 * upgraded, the result is a refusal with the first one's class and next,
 * which is the exit code; running it again once the requester has decided
 * takes what waited. */
static int upgrade_all(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    struct standing *st = NULL;
    struct pkg_manifest *nm = NULL;
    size_t *cand = NULL, *order = NULL, ncand = 0, i, j, pos, done = 0;
    unsigned char *emitted = NULL;
    const char *base;
    int rc = 1, worst_class = 0;
    const char *worst_next = NULL;
    size_t nrefused = 0, nskipped = 0, nconflict = 0, at_r = 0;
    unsigned char *failed = NULL;
    char refused_names[600];

    refused_names[0] = '\0';
    if (a->target != NULL)
        return refuse_c(20, "UPGRADE ALL upgrades every package a newer version is offered for; "
                        "name no package with it (UPGRADE <name> upgrades one)");
    if (a->version != NULL)
        return refuse_c(20, "VERSION names one package's version; UPGRADE ALL takes, for each "
                        "package, the version UPGRADE <name> would take");
    if (a->downgrade || a->acceptkey != NULL)
        return refuse_c(20, "%s is a decision about one package, never about all of them at "
                        "once: UPGRADE ALL never downgrades and never accepts a new key. Give it "
                        "to UPGRADE <name>", a->downgrade ? "DOWNGRADE" : "ACCEPTKEY");
    if (keep_current_setup(a, &ix, &in) != 0) return 1;
    base = target_arch;
    st = (struct standing *)calloc(in.n ? in.n : 1, sizeof *st);
    cand = (size_t *)calloc(in.n ? in.n : 1, sizeof *cand);
    order = (size_t *)calloc(in.n ? in.n : 1, sizeof *order);
    emitted = (unsigned char *)calloc(in.n ? in.n : 1, 1);
    nm = (struct pkg_manifest *)calloc(in.n ? in.n : 1, sizeof *nm);
    failed = (unsigned char *)calloc(in.n ? in.n : 1, 1);
    planned = (struct planned_up *)calloc(in.n ? in.n : 1, sizeof *planned);
    nplanned = 0;
    if (st == NULL || cand == NULL || order == NULL || emitted == NULL || nm == NULL || planned == NULL
        || failed == NULL) {
        refuse("out of memory");
        goto out;
    }
    for (i = 0; i < in.n; i++) {
        stand(a->root, &ix, base, &in.m[i], &st[i]);
        if (st[i].conflict) {
            /* Two listed channels disagree about it: reported like any other
             * package that needs the requester, and every other goes ahead. */
            char one[2400], *q;
            nrefused++;
            nconflict++;
            snprintf(one, sizeof one, "%s", st[i].why);
            for (q = one; *q; q++)
                if (*q == '\n') *q = ' ';
            if (14 > worst_class) { worst_class = 14; worst_next = "ask-requester"; }
            if (machine) {
                char jn[2600];
                snprintf(jn, sizeof jn, "%s %s %s %s", in.m[i].name, in.m[i].version,
                         class_name(14), one);
                rec_item("refused", jn, "name", in.m[i].name, "installed", in.m[i].version,
                         "class", class_name(14), "code", "14", "reason", one,
                         "next", "ask-requester", NULL);
            } else {
                say_pkgline(in.m[i].name, "not upgraded: %s", one);
            }
            if (at_r + 80 < sizeof refused_names)
                at_r += (size_t)snprintf(refused_names + at_r, sizeof refused_names - at_r,
                                         "%s%s (%s)", at_r ? ", " : "", in.m[i].name,
                                         class_name(14));
        } else if (st[i].newer) {
            offered_manifest(chan_of(st[i].offer), st[i].offer, &nm[ncand]);
            cand[ncand++] = i;
        } else if (st[i].withdrawn) {
            note("%s %s was withdrawn by its publisher, and nothing newer is offered; going back "
                 "is the requester's decision, and UPGRADE ALL never does it", in.m[i].name,
                 in.m[i].version);
        }
    }
    /* A package before what depends on it: by the dependencies of the
     * version it moves to and of the one installed. Candidates are in name
     * order, the database's; a cycle is left to the plan, which names it. */
    for (pos = 0; pos < ncand; pos++) {
        size_t next = ncand;
        for (i = 0; i < ncand && next == ncand; i++) {
            int blocked = 0;
            if (emitted[i])
                continue;
            for (j = 0; j < ncand && !blocked; j++) {
                const char *dn = st[cand[j]].m->name;
                if (j != i && !emitted[j]
                    && (names_dep(&nm[i], dn) || names_dep(st[cand[i]].m, dn)))
                    blocked = 1;
            }
            if (!blocked)
                next = i;
        }
        for (i = 0; next == ncand && i < ncand; i++)
            if (!emitted[i])
                next = i;
        emitted[next] = 1;
        order[pos] = next;
        tr("upgrade %lu of %lu: %s %s to %s", (unsigned long)pos + 1, (unsigned long)ncand,
           st[cand[next]].m->name, st[cand[next]].m->version, st[cand[next]].offer->version);
    }
    for (pos = 0; pos < ncand; pos++) {
        const struct standing *s = &st[cand[order[pos]]];
        struct plan p;
        unsigned long placed, dropped, kept;
        size_t f;
        int ok_plan, blocked_by = -1;
        /* A package whose new version needs one that could not be upgraded
         * waits: it is skipped, and said why. */
        for (f = 0; f < pos && blocked_by < 0; f++)
            if (failed[f] && names_dep(&nm[order[pos]], st[cand[order[f]]].m->name))
                blocked_by = (int)f;
        if (blocked_by >= 0) {
            const char *dn = st[cand[order[blocked_by]]].m->name;
            failed[pos] = 1;
            nskipped++;
            if (machine) {
                char jn[300];
                snprintf(jn, sizeof jn, "%s %s %s", s->m->name, s->m->version, dn);
                rec_item("skipped", jn, "name", s->m->name, "installed", s->m->version,
                         "waits-for", dn, NULL);
            } else {
                say_pkgline(s->m->name, "skipped: it needs %s, which could not be upgraded", dn);
            }
            continue;
        }
        arch_for(base, s->m);
        quiet = 1;
        ok_plan = plan_target(&p, a, &ix, s->m->name, s->offer->version) == 0
                  && run_plan(&p, s->m, &placed, &dropped, &kept) == 0;
        quiet = 0;
        if (!ok_plan) {
            char codes[8];
            plan_free(&p);
            failed[pos] = 1;
            nrefused++;
            /* The owner's rule for a batch: go as far as possible, report
             * each, and exit with the worst class of the refusals. */
            if (refused_class > worst_class) {
                worst_class = refused_class;
                worst_next = refused_next;
            }
            snprintf(codes, sizeof codes, "%d", refused_class);
            {
                /* one line, as every record: a reason may have several */
                char *q;
                for (q = quiet_reason; *q; q++)
                    if (*q == '\n') *q = ' ';
            }
            if (machine) {
                char jn[2600];
                /* the reason last, since it has spaces: "<name> <version> <class> <reason>" */
                snprintf(jn, sizeof jn, "%s %s %s %s", s->m->name, s->m->version,
                         class_name(refused_class), quiet_reason);
                rec_item("refused", jn, "name", s->m->name, "installed", s->m->version,
                         "class", class_name(refused_class), "code", codes, "reason", quiet_reason,
                         "next", refused_next ? refused_next : next_default(refused_class), NULL);
            } else {
                say_pkgline(s->m->name, "not upgraded: %s", quiet_reason);
            }
            if (at_r + 80 < sizeof refused_names)
                at_r += (size_t)snprintf(refused_names + at_r, sizeof refused_names - at_r, "%s%s (%s)",
                                         at_r ? ", " : "", s->m->name, class_name(refused_class));
            refused_class = 0;
            refused_next = NULL;
            continue;
        }
        if (machine) {
            char jn[500];
            const char *from = nchans > 1 ? chan_of(s->offer) : NULL;
            snprintf(jn, sizeof jn, "%s %s %s%s%s", s->m->name, s->m->version, s->offer->version,
                     from ? " " : "", from ? from : "");
            if (from != NULL)
                rec_item("package", jn, "name", s->m->name, "from", s->m->version,
                         "version", s->offer->version, "channel", from, NULL);
            else
                rec_item("package", jn, "name", s->m->name, "from", s->m->version,
                         "version", s->offer->version, NULL);
        } else {
            char keptw[40];
            keptw[0] = '\0';
        if (kept) snprintf(keptw, sizeof keptw, ", %lu kept", kept);
            say_pkgline(s->m->name, "%s from %s to %s%s%s: %lu placed, %lu removed%s",
                dryrun ? "would upgrade" : "upgraded",
                s->m->version, s->offer->version,
                nchans > 1 ? " from " : "", nchans > 1 ? chan_of(s->offer) : "",
                placed, dropped, keptw);
        }
        {
            const struct fetched *f = &p.f[p.n - 1];
            if (strcmp(f->m.kind, "image") == 0 && f->m.nfiles == 1)
                hint("the image %s is replaced: a machine that has it mounted must Eject it %s, "
                     "and MOUNTLIST %s ROOT %s writes the new entry, since its size may change",
                     f->m.files[0].path, dryrun ? "first" : "and mount it again", f->m.name, a->root);
        }
        plan_free(&p);
        if (dryrun) {
            planned[nplanned].name = s->m->name;
            planned[nplanned].version = s->offer->version;
            nplanned++;
        }
        done++;
    }
    {
        char summary[900];
        size_t tried = ncand + nconflict;
        if (nrefused + nskipped == 0 && tried == 0)
            snprintf(summary, sizeof summary, "nothing needs an update: %lu package%s, none with a "
                     "newer version in the channel", (unsigned long)in.n, in.n == 1 ? "" : "s");
        else if (nrefused + nskipped == 0)
            snprintf(summary, sizeof summary, "%s %lu package%s", dryrun ? "would update" : "updated",
                     (unsigned long)done, done == 1 ? "" : "s");
        else
        {
            char waiting[80] = "";
            if (nskipped)
                snprintf(waiting, sizeof waiting, "; %lu waiting for one of them", (unsigned long)nskipped);
            snprintf(summary, sizeof summary, "%s %lu of %lu package%s; not upgraded: %s%s. "
                     "Everything else went ahead", dryrun ? "would update" : "updated",
                     (unsigned long)done, (unsigned long)tried, tried == 1 ? "" : "s",
                     refused_names, waiting);
        }
        if (nrefused + nskipped == 0) {
            kv("result", "%s", tried == 0 ? "unchanged" : res("upgraded", "would-upgrade"));
        } else {
            /* Some needed a decision: the answer is a refusal, with the
             * worst class of them, so the exit code and next say what to do. */
            refused_class = worst_class;
            refused_next = worst_next;
            kv("result", "refused");
            kv("class", "%s", class_name(worst_class));
            kv("code", "%d", worst_class);
        }
        kv("upgraded", "%lu", (unsigned long)done);
        kv("not-upgraded", "%lu", (unsigned long)(nrefused + nskipped));
        kv("count", "%lu", (unsigned long)done);
        kv("summary", "%s", summary);
        if (nrefused + nskipped > 0)
            kv("next", "%s", worst_next ? worst_next : next_default(worst_class));
        if (!machine)
            say_result("%s", summary);
        rc = nrefused + nskipped == 0 ? 0 : 1;
    }
out:
    for (i = 0; nm != NULL && i < ncand; i++)
        pkg_manifest_free(&nm[i]);
    free(nm);
    free(st);
    free(cand);
    free(order);
    free(emitted);
    free(failed);
    free(planned);
    planned = NULL;
    nplanned = 0;
    installed_free(&in);
    free(ix.e);
    return rc;
}

/* ---- the interface in pkg.h ------------------------------------------- */

static int cancelled(const char *when)
{
    if (sink == NULL || sink->cancel == NULL || !sink->cancel(sink->user))
        return 0;
    refuse_n(PKGRC_REFUSED, "report", "cancelled by the caller %s", when);
    return 1;
}

typedef int (*op_fn)(const struct pkg_options *);

/* Every operation starts from the same clean state and ends with its code. */
/* What a program passes, like what a person types, holds no line break or
 * control character: each would end up in a path, a name or a record. */
static int options_clean(const struct pkg_options *o)
{
    const struct { const char *what, *v; } f[] = {
        { "the name", o->target }, { "ROOT", o->root }, { "CHANNEL", o->channel },
        { "NAME", o->name }, { "VERSION", o->version }, { "ARCH", o->arch }, { "KIND", o->kind },
        { "DEPENDS", o->depends }, { "SIGN", o->sign }, { "FILE", o->file }, { "KEY", o->key },
        { "NAMESPACE", o->nspace },
        { "OUT", o->out }, { "ACCEPTKEY", o->acceptkey }, { "UNIT", o->unit },
        { "HANDLER", o->handler }, { "FILES", o->files }, { "BUILD", o->build },
        { "ARCHIVE", o->archive }, { "TO", o->to }, { "PKG_PUSHKEY", o->pushkey },
        { "CONFIG", o->config }, { "UPSTREAM", o->upstream },
        { "SHORT", o->short_desc }, { "DESCRIPTION", o->description }, { "CATEGORY", o->category },
        { "TAGS", o->tags }, { "AUTHOR", o->author }, { "HOMEPAGE", o->homepage },
        { "REPOSITORY", o->repository }, { "LICENSE", o->license },
        { "DISTRIBUTION", o->distribution }, { "CHANGES", o->changes }, { "ICON", o->icon },
        { "SCREENSHOT", o->screenshot }, { "README", o->readme }, { "INFO", o->info },
        { "FROM", o->from }
    };
    size_t i, j;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        for (j = 0; f[i].v && f[i].v[j]; j++)
            if ((unsigned char)f[i].v[j] < 0x20 || (unsigned char)f[i].v[j] == 0x7F)
                return refuse_c(20, "%s holds a %s at character %lu; give it without",
                                f[i].what, f[i].v[j] == '\n' || f[i].v[j] == '\r' ? "line break"
                                : f[i].v[j] == '\t' ? "tab" : "control character",
                                (unsigned long)j + 1);
    return 0;
}

static int call(const struct pkg_sink *s, const char *verb, op_fn fn, const struct pkg_options *o)
{
    static const struct pkg_options none;
    int rc;
    sink = s;
    verb_name = verb;
    machine = s != NULL && s->structured;
    dryrun = o != NULL && o->dryrun;
    refused_class = 0;
    refused_next = NULL;
    pending_next = NULL;
    quiet = 0;
    target_arch = NULL;
    root_arch[0] = '\0';
    told_source = 0;
    pick_root = NULL;
    pick_refused = 0;
    chans_clear();
    opt_unpacked = o != NULL ? o->unpacked : NULL;
    rc = options_clean(o != NULL ? o : &none) != 0 ? 1 : fn(o != NULL ? o : &none);
    rc = rc == 0 ? PKGRC_OK : refused_class ? refused_class : PKGRC_REFUSED;
    chans_clear();
    sink = NULL;
    return rc;
}

int pkg_keygen   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keygen", cmd_keygen, o); }
int pkg_sign     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "sign", cmd_sign, o); }
int pkg_checksig (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "checksig", cmd_checksig, o); }
int pkg_keyinfo  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keyinfo", cmd_keyinfo, o); }
int pkg_withdraw (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "withdraw", cmd_withdraw, o); }
int pkg_manifest (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "manifest", cmd_manifest, o); }
int pkg_publish  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "publish", cmd_publish, o); }
int pkg_install  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "install", cmd_install, o); }
int pkg_upgrade  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "upgrade", cmd_upgrade, o); }
int pkg_rollback (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "rollback", cmd_rollback, o); }
int pkg_list     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "list", cmd_list, o); }
int pkg_verify   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "verify", cmd_verify, o); }
int pkg_repair   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "repair", cmd_repair, o); }
int pkg_resolve  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "resolve", cmd_resolve, o); }
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "remove", cmd_remove, o); }
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "image", cmd_image, o); }
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "mountlist", cmd_mountlist, o); }
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "show", cmd_show, o); }
int pkg_status   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "status", cmd_status, o); }
int pkg_channel  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "channel", cmd_channel, o); }
int pkg_search   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "search", cmd_search, o); }

static const char *usage_reason;
static int usage_op(const struct pkg_options *o)
{
    (void)o;
    return refuse_c(PKGRC_USAGE, "%s", usage_reason);
}

const char *pkg_field(int n, const char *const *keys, const char *const *values, const char *key)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(keys[i], key) == 0)
            return values[i];
    return NULL;
}

int pkg_usage_error(const struct pkg_sink *s, const char *verb, const char *reason)
{
    usage_reason = reason;
    return call(s, verb, usage_op, NULL);
}

/* ---- PUSH: a local channel to a portal ---------------------------------- *
 *
 * The portal serves a channel file for file; PUSH sends what a local one
 * has and it lacks: plan (which files the portal needs), files (large ones
 * in parts that resume), commit (the local index, merged by the portal,
 * which checks the result with Pkg itself). The key travels in a header
 * file, never on a command line, and only over https, or http to this
 * machine for tests. The portal's answers are records in this program's
 * own form, relayed as they come. */

#define PUSH_PART_DEFAULT (32ul << 20)

static int push_path_ok(const char *rel)
{
    size_t n = strlen(rel);
    if (strncmp(rel, "objects/", 8) == 0)
        return 1;
    if (strncmp(rel, "archives/", 9) == 0)
        return !(n > 7 && strcmp(rel + n - 7, ".pkgidx") == 0) && !(n > 7 && strcmp(rel + n - 7, ".sha256") == 0)
           && !(n > 7 && strcmp(rel + n - 7, ".pkgmap") == 0);
    if (strcmp(rel, "Bootstrap/SHA256SUMS") == 0 || strcmp(rel, "Bootstrap/SHA256SUMS.sig") == 0)
        return 1;   /* the bootstraps' digests, signed for ssh-keygen -Y verify */
    if (strncmp(rel, "Bootstrap/", 10) == 0) {
        /* Bootstrap/<cpu>/Pkg for AROS, Bootstrap/<platform>/pkg or pkg.exe for a host */
        const char *plat = rel + 10, *slash = strchr(plat, '/');
        size_t k;
        if (slash == NULL || slash == plat || strchr(slash + 1, '/') != NULL)
            return 0;
        for (k = 0; plat + k < slash; k++)
            if (!((plat[k] >= 'a' && plat[k] <= 'z') || (plat[k] >= '0' && plat[k] <= '9')
                  || plat[k] == '-' || plat[k] == '_'))
                return 0;
        /* the file itself: Pkg on AROS, as AmigaDOS names a command, pkg on a host */
        return strcmp(slash + 1, "Pkg") == 0 || strcmp(slash + 1, "pkg") == 0
               || strcmp(slash + 1, "pkg.exe") == 0;
    }
    return strcmp(rel, "Install-Pkg") == 0 || strcmp(rel, "ReadMe") == 0;
}

struct push_list { char **rel; size_t n, cap; };

static int push_collect(const char *rel, void *ctx)
{
    struct push_list *pl = (struct push_list *)ctx;
    if (!push_path_ok(rel)) return 0;
    if (pl->n == pl->cap) {
        size_t nc = pl->cap ? pl->cap * 2 : 64;
        char **g = (char **)realloc(pl->rel, nc * sizeof *g);
        if (g == NULL) return -1;
        pl->rel = g;
        pl->cap = nc;
    }
    pl->rel[pl->n] = pkg_strdup(rel);
    return pl->rel[pl->n++] ? 0 : -1;
}

/* sha256 and size of a file read in pieces: an archive is large. */
static int file_digest(const char *path, char hex[PKG_SHA256_HEXLEN + 1], unsigned long long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char buf[65536], dg[PKG_SHA256_LEN];
    struct pkg_sha256 c;
    size_t n, k;
    if (f == NULL) return -1;
    pkg_sha256_init(&c);
    *size = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        pkg_sha256_update(&c, buf, n);
        *size += n;
    }
    fclose(f);
    pkg_sha256_final(&c, dg);
    for (k = 0; k < PKG_SHA256_LEN; k++) snprintf(hex + 2 * k, 3, "%02x", dg[k]);
    return 0;
}

/* The value of the first "key: " record in an answer file, or NULL. */
static char *answer_field(const char *path, const char *key, char *out, size_t ol)
{
    unsigned char *buf;
    size_t len, kl = strlen(key);
    const char *p, *end;
    if (pkg_fs_read(path, &buf, &len) != 0) return NULL;
    for (p = (const char *)buf, end = p + len; p < end; ) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll > kl + 1 && memcmp(p, key, kl) == 0 && p[kl] == ':' && p[kl + 1] == ' ') {
            snprintf(out, ol, "%.*s", (int)(ll - kl - 2), p + kl + 2);
            free(buf);
            return out;
        }
        p = nl ? nl + 1 : end;
    }
    free(buf);
    return NULL;
}

/* An archive whose every package records where it is published upstream
 * (Archive:) is downloaded from there by installs: PUSH leaves it out. */
static void push_drop_upstream(const char *channel, struct push_list *pl)
{
    struct index ix;
    size_t e, i, o;
    if (read_index(channel, &ix) != 0) { refused_class = 0; return; }
    for (i = 0, o = 0; i < pl->n; i++) {
        const char *rel = pl->rel[i];
        int upstream = 0, local = 0;
        if (strncmp(rel, "archives/", 9) == 0) {
            for (e = 0; e < ix.n; e++) {
                char *mp = object_path(channel, ix.e[e].digest, "manifest"), err[200], an[1024], ap[1024];
                unsigned char *buf;
                size_t len;
                struct pkg_manifest m;
                if (mp == NULL || pkg_fs_read(mp, &buf, &len) != 0) { free(mp); continue; }
                free(mp);
                if (pkg_manifest_parse((const char *)buf, len, &m, err, sizeof err) == 0) {
                    if (m.source && pkg_archive_split(m.source, an, sizeof an, ap, sizeof ap)
                        && strcmp(an, rel + 9) == 0) {
                        if (m.archive_sha) upstream++; else local++;
                    }
                    pkg_manifest_free(&m);
                }
                free(buf);
            }
        }
        if (upstream > 0 && local == 0) {
            tr("push leaves out %s: its %d packages download it from upstream", rel, upstream);
            free(pl->rel[i]);
            continue;
        }
        pl->rel[o++] = pl->rel[i];
    }
    pl->n = o;
    free(ix.e);
}

/* How a push proves who sends it. Over https: the portal's key, a secret, in
 * each request. Over plain http nothing secret may travel, so each request is
 * signed with the publisher's own key instead, which is also how a publisher
 * with no portal key pushes: the portal gives a session, and the signature
 * covers the method, the path, the session, a number that only grows and the
 * SHA-256 of the body, so a request cannot be altered, replayed or moved to
 * another address. */
struct push_auth {
    const char *bearer;         /* https: PKG_PUSHKEY */
    struct key  k;              /* http: the signing key */
    char        session[80];
    unsigned long seq;
    const char *hdr;            /* the header file each request is sent with */
};

static int push_send(struct push_auth *pa, const char *method, const char *url, const char *body,
                     const char *range, const char *out, int *code, char *err, size_t errlen)
{
    char h[3200];
    if (pa->bearer != NULL) {
        snprintf(h, sizeof h, "Authorization: Bearer %s\n%s%s%s", pa->bearer,
                 range ? "Content-Range: " : "", range ? range : "", range ? "\n" : "");
    } else {
        char digest[PKG_SHA256_HEXLEN + 1], text[2800], sighex[2 * PKG_ED25519_SIG + 1];
        unsigned char sig[PKG_ED25519_SIG];
        unsigned long long size;
        const char *path = strstr(url, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        if (path == NULL || file_digest(body, digest, &size) != 0) {
            snprintf(err, errlen, "cannot read %s to sign the request", body);
            return -1;
        }
        pa->seq++;
        snprintf(text, sizeof text, "pkg-push-1\n%s\n%s\n%s\n%lu\n%s\n%s\n", method, path, pa->session,
                 pa->seq, digest, range ? range : "-");
        pkg_ed25519_sign(sig, (const unsigned char *)text, strlen(text), pa->k.sk);
        tohex(sig, sizeof sig, sighex);
        snprintf(h, sizeof h, "Authorization: Pkg-Signature key=%s,session=%s,seq=%lu,sha256=%s,sig=%s\n%s%s%s",
                 pa->k.pkhex, pa->session, pa->seq, digest, sighex,
                 range ? "Content-Range: " : "", range ? range : "", range ? "\n" : "");
    }
    if (pkg_fs_write_private(pa->hdr, h, strlen(h)) != 0) {
        snprintf(err, errlen, "cannot write the request's headers where only this user reads them");
        return -1;
    }
    if (pkg_net_send(method, url, body, pa->hdr, out, code, err, errlen) != 0)
        return -1;
    if (*code == 426) {           /* the portal no longer takes this Pkg: its words, not a number */
        char why[500] = "", nx[500] = "";
        answer_field(out, "reason", why, sizeof why);
        answer_field(out, "next", nx, sizeof nx);
        snprintf(err, errlen, "%s%s%s", why[0] ? why : "the portal asks for a newer pkg", nx[0] ? ". " : "", nx);
        return -1;
    }
    return 0;
}

static int cmd_push(const struct pkg_options *a)
{
    struct push_auth pa;
    int signed_push;
    struct push_list pl = { NULL, 0, 0 };
    char *cache = NULL, *tmp = NULL, *hdr = NULL, *plan = NULL, *out = NULL, *part = NULL, *ix = NULL;
    char url[2300], err[400], line[2200];
    const char *base;
    unsigned skipped = 0;
    unsigned long long sent_bytes = 0;
    unsigned long partsz = PUSH_PART_DEFAULT;
    size_t i, sent = 0, nneed = 0;
    int code = 0, rc = 1;
    FILE *f;

    if (a->channel == NULL) return refuse_c(20, "name the local channel with CHANNEL <dir>");
    if (a->to == NULL) return refuse_c(20, "name the portal's channel with TO <url>");
    if (is_url(a->channel))
        return refuse_c(20, "CHANNEL is the channel on this machine that PUSH sends; the portal's is TO");
    if (strncmp(a->to, "https://", 8) != 0 && strncmp(a->to, "http://", 7) != 0)
        return refuse_c(20, "TO is the portal's channel, an https:// or http:// address: %s", a->to);
    memset(&pa, 0, sizeof pa);
    /* The portal's key where it is safe to send: https, or a test portal on this
     * machine. Anywhere else, and whenever there is no such key, each request is
     * signed with the publisher's own key. */
    {
        int has_key = a->pushkey != NULL && a->pushkey[0] != '\0';
        int local = strncmp(a->to, "http://127.0.0.1", 16) == 0 || strncmp(a->to, "http://localhost", 16) == 0;
        signed_push = !(has_key && (strncmp(a->to, "https://", 8) == 0 || local));
    }
    if (signed_push) {
        if (a->sign == NULL || a->sign[0] == '\0') {
            /* the portal's own page on becoming a publisher: scheme and host of TO */
            const char *h = strstr(a->to, "://") + 3, *e = strchr(h, '/');
            int bl = (int)(e ? (size_t)(e - a->to) : strlen(a->to));
            return refuse_n(14, "ask-requester", "no key to push with. A portal takes pushes only from "
                            "publishers its maintainers have registered, and nobody can register themselves "
                            "yet: ask them, as %.*s/publishers explains. Once registered, sign the push with SIGN "
                            "<keyfile> (or PKG_SIGNKEY), the key you registered, or over https with "
                            "PKG_PUSHKEY, the key the portal gave you. Never make a key up", bl, a->to);
        }
        if (load_key(a->sign, &pa.k) != 0)
            return 1;
    } else {
        pa.bearer = a->pushkey;
    }
    ix = pkg_join(a->channel, "index");
    if (ix == NULL || !pkg_fs_exists(ix)) {
        free(ix);
        return refuse_c(11, "%s has no index: nothing is published there to push", a->channel);
    }
    if (getenv("PKG_PUSH_PART_BYTES"))            /* tests make parts small */
        partsz = strtoul(getenv("PKG_PUSH_PART_BYTES"), NULL, 10);
    if (partsz < 1024) partsz = 1024;
    base = a->to;
    if (pkg_fs_walk(a->channel, push_collect, NULL, &pl, &skipped, err, sizeof err) != 0) {
        refuse_c(17, "cannot read %s: %s", a->channel, err);
        goto out;
    }
    push_drop_upstream(a->channel, &pl);
    cache = pkg_cache_dir();
    {
        char name[64];
        snprintf(name, sizeof name, "push-%ld", (long)time(NULL));
        tmp = cache ? pkg_join(cache, name) : NULL;
    }
    if (tmp == NULL || pkg_fs_mkdirs(tmp) != 0) { refuse_c(17, "cannot make a working directory"); goto out; }
    hdr = pkg_join(tmp, "headers");
    plan = pkg_join(tmp, "plan");
    out = pkg_join(tmp, "answer");
    part = pkg_join(tmp, "part");
    if (hdr == NULL || plan == NULL || out == NULL || part == NULL) { refuse_c(17, "out of memory"); goto out; }
    pa.hdr = hdr;
    if (signed_push) {
        /* the session: asked for by public key, answered with a number to sign against */
        snprintf(line, sizeof line, "key: %s\n", pa.k.pkhex);
        snprintf(url, sizeof url, "%s/_push/session", base);
        if (pkg_fs_write_private(plan, line, strlen(line)) != 0 || pkg_fs_write_private(hdr, "", 0) != 0
            || pkg_net_send("POST", url, plan, hdr, out, &code, err, sizeof err) != 0) {
            refuse_c(17, "cannot reach %s: %s", base, err);
            goto out;
        }
        if (code != 200 || answer_field(out, "session", pa.session, sizeof pa.session) == NULL) {
            char why[600];
            refuse_n(14, "ask-requester", "the portal gave no session for the key %.16s (HTTP %d)%s%s", pa.k.pkhex, code,
                     answer_field(out, "reason", why, sizeof why) ? ": " : "", answer_field(out, "reason", why, sizeof why) ? why : "");
            goto out;
        }
    }

    /* 1. plan: every file, its digest and size; the portal says what it needs */
    f = fopen(plan, "wb");
    if (f == NULL) { refuse_c(17, "cannot write the plan"); goto out; }
    for (i = 0; i < pl.n; i++) {
        char hex[PKG_SHA256_HEXLEN + 1], *full = pkg_join(a->channel, pl.rel[i]);
        unsigned long long size;
        if (full == NULL || file_digest(full, hex, &size) != 0) {
            free(full); fclose(f);
            refuse_c(17, "cannot read %s", pl.rel[i]);
            goto out;
        }
        fprintf(f, "%s %s %llu\n", pl.rel[i], hex, size);
        free(full);
    }
    fclose(f);
    snprintf(url, sizeof url, "%s/_push/plan", base);
    if (push_send(&pa, "POST", url, plan, NULL, out, &code, err, sizeof err) != 0) {
        refuse_c(17, "cannot reach %s: %s", base, err);
        goto out;
    }
    if (code == 401 || code == 403) {
        char why[600];
        refuse_n(14, "ask-requester", "the portal refused the key (HTTP %d)%s%s", code,
                 answer_field(out, "reason", why, sizeof why) ? ": " : "", answer_field(out, "reason", why, sizeof why) ? why : "");
        goto out;
    }
    if (code != 200) { refuse_c(17, "the portal answered the plan with HTTP %d", code); goto out; }

    /* 2. the files it needs, whole or in parts */
    {
        unsigned char *ans;
        size_t alen;
        char **need = NULL;
        const char *p2, *end;
        if (pkg_fs_read(out, &ans, &alen) != 0) { refuse_c(17, "cannot read the plan's answer"); goto out; }
        for (p2 = (const char *)ans, end = p2 + alen; p2 < end; ) {
            const char *nl = memchr(p2, '\n', (size_t)(end - p2));
            size_t ll = nl ? (size_t)(nl - p2) : (size_t)(end - p2);
            if (ll > 6 && memcmp(p2, "need: ", 6) == 0) {
                char **g = (char **)realloc(need, (nneed + 1) * sizeof *g);
                if (g == NULL) break;
                need = g;
                need[nneed] = (char *)malloc(ll - 5);
                if (need[nneed] == NULL) break;
                snprintf(need[nneed], ll - 5, "%.*s", (int)(ll - 6), p2 + 6);
                nneed++;
            }
            p2 = nl ? nl + 1 : end;
        }
        free(ans);
        for (i = 0; i < nneed; i++) {
            char *full = pkg_join(a->channel, need[i]), hex[PKG_SHA256_HEXLEN + 1], result[64], rec[64];
            unsigned long long size = 0, off = 0;
            int ok_file = 0;
            if (!push_path_ok(need[i]) || full == NULL || file_digest(full, hex, &size) != 0) {
                warn("the portal asked for %s, which this channel does not send", need[i]);
                free(full);
                continue;
            }
            snprintf(url, sizeof url, "%s/_push/files/%s", base, need[i]);
            if (size <= partsz) {
                if (push_send(&pa, "PUT", url, full, NULL, out, &code, err, sizeof err) == 0 && code == 200
                    && answer_field(out, "result", result, sizeof result)
                    && (strcmp(result, "received") == 0 || strcmp(result, "unchanged") == 0))
                    ok_file = 1;
            } else {
                /* in parts, each resuming where the portal says it got to */
                FILE *src = fopen(full, "rb");
                while (src != NULL && off < size) {
                    unsigned long long end2 = off + partsz < size ? off + partsz : size;
                    FILE *pf = fopen(part, "wb");
                    char buf[65536], ph[2600];
                    unsigned long long left = end2 - off;
                    if (pf == NULL) break;
                    fseek(src, (long)off, SEEK_SET);
                    while (left > 0) {
                        size_t k = fread(buf, 1, left < sizeof buf ? (size_t)left : sizeof buf, src);
                        if (k == 0) break;
                        fwrite(buf, 1, k, pf);
                        left -= k;
                    }
                    fclose(pf);
                    snprintf(ph, sizeof ph, "bytes %llu-%llu/%llu", off, end2 - 1, size);
                    if (push_send(&pa, "PUT", url, part, ph, out, &code, err, sizeof err) != 0 || code != 200
                        || answer_field(out, "result", result, sizeof result) == NULL)
                        break;
                    if (strcmp(result, "received") == 0 || strcmp(result, "unchanged") == 0) { ok_file = 1; break; }
                    if (strcmp(result, "partial") != 0 || answer_field(out, "received", rec, sizeof rec) == NULL)
                        break;
                    off = strtoull(rec, NULL, 10);         /* the portal's count, so a resume skips */
                }
                if (src) fclose(src);
            }
            if (!ok_file) {
                char why[600];
                refuse_c(17, "the portal did not take %s: %s", need[i],
                         answer_field(out, "reason", why, sizeof why) ? why : code ? "an unexpected answer" : err);
                for (; i < nneed; i++) free(need[i]);
                free(need);
                free(full);
                goto out;
            }
            sent++;
            sent_bytes += size;
            free(full);
            free(need[i]);
        }
        free(need);
    }

    /* 3. commit: the local index; the portal merges, checks and answers */
    snprintf(url, sizeof url, "%s/_push/commit", base);
    if (push_send(&pa, "POST", url, ix, NULL, out, &code, err, sizeof err) != 0) {
        refuse_c(17, "cannot reach %s to commit: %s", base, err);
        goto out;
    }
    {
        unsigned char *ans;
        size_t alen;
        char result[64] = "", cls[16] = "", nextw[40] = "", summary[1200] = "";
        const char *p2, *end;
        if (pkg_fs_read(out, &ans, &alen) != 0) { refuse_c(17, "cannot read the commit's answer"); goto out; }
        /* relayed record by record: the portal speaks this program's form */
        for (p2 = (const char *)ans, end = p2 + alen; p2 < end; ) {
            const char *nl = memchr(p2, '\n', (size_t)(end - p2));
            size_t ll = nl ? (size_t)(nl - p2) : (size_t)(end - p2);
            char k[64], v[2048];
            const char *colon = memchr(p2, ':', ll);
            if (colon && (size_t)(colon - p2) < sizeof k && colon + 1 < p2 + ll && colon[1] == ' ') {
                snprintf(k, sizeof k, "%.*s", (int)(colon - p2), p2);
                snprintf(v, sizeof v, "%.*s", (int)(ll - (size_t)(colon - p2) - 2), colon + 2);
                if (strcmp(k, "result") == 0) snprintf(result, sizeof result, "%.63s", v);
                else if (strcmp(k, "code") == 0) snprintf(cls, sizeof cls, "%.15s", v);
                else if (strcmp(k, "next") == 0) snprintf(nextw, sizeof nextw, "%.39s", v);
                else if (strcmp(k, "summary") == 0) snprintf(summary, sizeof summary, "%.1199s", v);
                if (machine && sink && sink->record)
                    { one_line(v); sink->record(sink->user, k, v); }
                else if (strcmp(k, "refused") == 0 || strcmp(k, "published") == 0)
                    say_item(k, "%s", v);
            }
            p2 = nl ? nl + 1 : end;
        }
        free(ans);
        if (result[0] == '\0' && code != 200) {
            /* no record at all: the portal itself failed, or something in front of it answered */
            refuse_c(17, "%s answered the commit with HTTP %d and no record of what it did: nothing says "
                     "the push was published. Read the channel with SHOW, and push again: what was "
                     "sent is not sent twice", base, code);
            goto out;
        }
        kv("uploaded", "%lu", (unsigned long)sent);
        kv("uploaded-bytes", "%llu", sent_bytes);
        if (!machine)
        {
            say_result("%s", summary[0] ? summary : result);
            say_detail("%lu file%s sent to %s (%llu bytes)",
                (unsigned long)sent, sent == 1 ? "" : "s", base, sent_bytes);
        }
        if (code == 401 || code == 403) {
            refused_class = 14;
            refused_next = "ask-requester";
            rc = 1;
        } else if (strcmp(result, "refused") == 0 || code != 200) {
            refused_class = cls[0] ? atoi(cls) : 10;
            refused_next = strcmp(nextw, "stop") == 0 ? "stop" : strcmp(nextw, "fix-command") == 0 ? "fix-command"
                         : strcmp(nextw, "ask-requester") == 0 ? "ask-requester" : "report";
            rc = 1;
        } else {
            rc = 0;
        }
    }
out:
    memset(&pa, 0, sizeof pa);
    if (hdr) pkg_fs_unlink(hdr);
    if (tmp) pkg_fs_rmtree(tmp);
    for (i = 0; i < pl.n; i++) free(pl.rel[i]);
    free(pl.rel);
    free(cache); free(tmp); free(hdr); free(plan); free(out); free(part); free(ix);
    return rc;
}

int pkg_push(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "push", cmd_push, o); }
