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
#include "pkg_fs.h"
#include "pkg_image.h"
#include "pkg_manifest.h"
#include "pkg_sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void emit(int is_error, const char *fmt, va_list ap)
{
    char small[1024], *big = NULL;
    va_list cp;
    int n;
    if (sink == NULL || sink->text == NULL)
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
    sink->text(sink->user, is_error, big ? big : small);
    free(big);
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
    if (sink == NULL || sink->text == NULL)
        return;
    s = (char *)malloc(len + 1u);
    if (s == NULL)
        return;
    memcpy(s, buf, len);
    s[len] = '\0';
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
static void kv(const char *key, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (!machine || sink == NULL || sink->record == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sink->record(sink->user, key, buf);
}

/* A record with several fields: the joined value goes to `record`, as the
 * command line prints it, and the fields one by one to `item`, for a program
 * that should not have to split strings. Key/value pairs, NULL-terminated. */
static void rec_item(const char *kind, const char *joined, ...)
{
    const char *keys[12], *vals[12];
    int n = 0;
    va_list ap;
    if (!machine || sink == NULL)
        return;
    if (sink->record != NULL)
        sink->record(sink->user, kind, joined);
    if (sink->item == NULL)
        return;
    va_start(ap, joined);
    while (n < 12) {
        const char *k = va_arg(ap, const char *);
        if (k == NULL)
            break;
        keys[n] = k;
        vals[n] = va_arg(ap, const char *);
        n++;
    }
    va_end(ap);
    sink->item(sink->user, kind, n, keys, vals);
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
static char quiet_reason[512];

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
        for (q = buf; *q; q++)
            if (*q == '\n') *q = ' ';
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
    if (sink && sink->text) {
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
    else
        say("pkg %s: warning: %s\n", verb_name, buf);
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
    if (pkg_fs_random(seed, sizeof seed) != 0)
        return refuse_c(17, "cannot read the system random source");
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
    if (!machine)
        say("key written to %s, readable by you alone\npublic key %s\n", a->file, k.pkhex);
    return 0;
}

static int load_key(const char *path, struct key *k)
{
    unsigned char *buf, seed[PKG_ED25519_SEED], pk[PKG_ED25519_PUBLIC];
    size_t len;
    char seedhex[65], pubhex[65];

    if (path == NULL)
        return refuse_n(20, "ask-requester", "no signing key: give SIGN <keyfile>, or set PKG_SIGNKEY. "
                        "A publisher who has published before must sign with the same key, or "
                        "every machine that installed their packages refuses the new ones: ask "
                        "whoever requested this where theirs is before creating one with KEYGEN");
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(17, "cannot read the key \"%s\": %s", path, strerror(errno));
    if (len > 512u
        || sscanf((const char *)buf, "Pkg-Secret-Key: 1\nSeed: %64s\nPublic: %64s",
                  seedhex, pubhex) != 2
        || fromhex(seed, sizeof seed, seedhex) != 0
        || fromhex(pk, sizeof pk, pubhex) != 0) {
        free(buf);
        return refuse_c(12, "\"%s\" is not a Pkg key file", path);
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

/* Which public key a key file holds, without showing its secret. */
static int cmd_keyinfo(const struct pkg_options *a)
{
    struct key k;
    const char *path = a->file ? a->file : a->target ? a->target : a->sign;
    if (path == NULL)
        return refuse_c(20, "name the key file with FILE <keyfile>");
    if (load_key(path, &k) != 0)
        return 1;
    kv("result", "shown");
    kv("file", "%s", path);
    kv("public", "%s", k.pkhex);
    if (!machine)
        say("%s holds the public key %s\n", path, k.pkhex);
    memset(&k, 0, sizeof k);
    return 0;
}

static int cmd_sign(const struct pkg_options *a)
{
    struct key k;
    unsigned char *buf;
    size_t len;
    if (a->target == NULL || a->key == NULL || a->out == NULL)
        return refuse_c(20, "usage: SIGN <file> KEY <keyfile> OUT <sigfile>");
    if (load_key(a->key, &k) != 0)
        return 1;
    if (pkg_fs_read(a->target, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->target);
    if (write_sig(a->out, &k, buf, len) != 0) {
        free(buf);
        return refuse_c(17, "cannot write \"%s\"", a->out);
    }
    free(buf);
    kv("result", "signed");
    kv("file", "%s", a->target);
    kv("signer", "%s", k.pkhex);
    if (!machine)
        say("signed %s with %.16s\n", a->target, k.pkhex);
    return 0;
}

/* ---- building a package from a drawer --------------------------------- */

struct loaded {
    char          *rel;
    unsigned char *data;
    size_t         len;
};

struct drawer {
    const char    *root;
    struct loaded *v;
    size_t         n, cap;
    char         **left_out;        /* host files not packaged, dirs with '/' */
    size_t         nleft;
    char           err[512];
};

static void leave_out(const char *rel, int is_dir, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    char **g = (char **)realloc(d->left_out, (d->nleft + 1) * sizeof *g);
    size_t n = strlen(rel) + 2;
    char *s = (char *)malloc(n);
    if (g == NULL || s == NULL) { free(s); if (g) d->left_out = g; return; }
    d->left_out = g;
    snprintf(s, n, "%s%s", rel, is_dir ? "/" : "");
    d->left_out[d->nleft++] = s;
}

static int load_one(const char *rel, void *ctx)
{
    struct drawer *d = (struct drawer *)ctx;
    char *full;
    const char *why = pkg_check_path(rel);

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
    full = pkg_join(d->root, rel);
    d->v[d->n].rel = full ? pkg_join("", rel) : NULL;
    if (full == NULL || d->v[d->n].rel == NULL
        || pkg_fs_read(full, &d->v[d->n].data, &d->v[d->n].len) != 0) {
        snprintf(d->err, sizeof d->err, "cannot read \"%s\"", rel);
        free(full);
        return -1;
    }
    free(full);
    d->n++;
    return 0;
}

static int by_rel(const void *a, const void *b)
{
    return strcmp(((const struct loaded *)a)->rel, ((const struct loaded *)b)->rel);
}

static void drawer_free(struct drawer *d)
{
    size_t i;
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); }
    free(d->v);
    for (i = 0; i < d->nleft; i++) free(d->left_out[i]);
    free(d->left_out);
}

/* The first "$VER: name version" cookie of one file, lower-cased. */
static int cookie(const struct loaded *f, char *name, size_t nl, char *ver, size_t vl)
{
    const unsigned char *p = f->data;
    size_t j;
    for (j = 0; j + 6 < f->len; j++) {
        size_t k = j + 6, a = 0, b = 0;
        if (memcmp(p + j, "$VER: ", 6) != 0)
            continue;
        while (k < f->len && p[k] > ' ' && p[k] < 0x7F && a + 1 < nl)
            name[a++] = (char)tolower(p[k++]);
        name[a] = '\0';
        while (k < f->len && p[k] == ' ') k++;
        while (k < f->len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.') && b + 1 < vl)
            ver[b++] = (char)p[k++];
        while (b > 0 && ver[b - 1] == '.') b--;
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
    if (len >= 4 && pkg_be32_get(p) == 0x000003F3ul)
        return "m68k";
    return NULL;
}

/* The drawer's architecture: the one its executables share, "generic" when
 * it has none. 1 found, 0 none, -1 more than one, listed in `seen`. */
static int drawer_arch(const struct drawer *d, const char **arch, const char **from,
                       char *seen, size_t sl)
{
    size_t i, at = 0;
    int clash = 0;
    *arch = NULL;
    seen[0] = '\0';
    for (i = 0; i < d->n; i++) {
        const char *a = file_arch(d->v[i].data, d->v[i].len);
        if (a == NULL)
            continue;
        if (at + 80 < sl)
            at += (size_t)snprintf(seen + at, sl - at, "%s%s (%s)", at ? ", " : "", d->v[i].rel, a);
        if (*arch == NULL) {
            *arch = a;
            *from = d->v[i].rel;
        } else if (strcmp(*arch, a) != 0) {
            clash = 1;
        }
    }
    return clash ? -1 : *arch != NULL;
}

/* DEPENDS "a >= 1.0, b": each item a name, alone or with ">=" and the lowest
 * version that will do, spaces optional around ">=". */
static int add_depends(struct pkg_manifest *m, const char *list)
{
    const char *p = list, *why;
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
    for (i = 0; i < d->n; i++) {
        e[i].path = d->v[i].rel;
        e[i].data = d->v[i].data;
        e[i].len = d->v[i].len;
    }
    if (pkg_image_build(e, d->n, name, &img, &len, err, sizeof err) != 0) {
        free(e);
        return refuse_c(20, "the drawer cannot become an image: %s", err);
    }
    free(e);
    snprintf(rel, sizeof rel, "%s.hdf", name);
    for (i = 0; i < d->n; i++) { free(d->v[i].rel); free(d->v[i].data); }
    d->v[0].rel = malloc(strlen(rel) + 1);
    if (d->v[0].rel == NULL) { free(img); d->n = 0; return refuse("out of memory"); }
    strcpy(d->v[0].rel, rel);
    d->v[0].data = img;
    d->v[0].len = len;
    d->n = 1;
    return 0;
}

struct built {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    char          *text;
    size_t         text_len;
    char           ver_from[512];
    char           arch_from[512];
    unsigned       skipped;
    char         **left_out;
    size_t         nleft;
};

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

static int build(const struct pkg_options *a, struct built *out)
{
    struct drawer d;
    struct pkg_writer *w;
    char vname[65], vver[64];
    const char *from = NULL, *name, *version, *arch, *kind, *why;
    char payload[PKG_SHA256_HEXLEN + 1];
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&d, 0, sizeof d);
    pkg_manifest_init(&out->m);
    if (a->target == NULL)
        return refuse_c(20, "name the drawer to package");
    if (!pkg_fs_is_dir(a->target))
        return refuse_c(20, "\"%s\" is not a directory", a->target);

    d.root = a->target;
    if (pkg_fs_walk(a->target, load_one, leave_out, &d, &out->skipped, d.err, sizeof d.err) != 0) {
        refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
        drawer_free(&d);
        return 1;
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->target);
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);
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
    {
        char seen[600];
        int got = find_ver(&d, name, vname, sizeof vname, vver, sizeof vver, &from,
                           seen, sizeof seen);
        int needed = name == NULL || version == NULL;
        if (!needed && got < 0)
            got = 0;
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
            if (version != NULL && pkg_version_cmp(version, vver) != 0)
                warn("VERSION %s, but the $VER cookie in %s says %s", version, from, vver);
            if (needed)
                snprintf(out->ver_from, sizeof out->ver_from, "%s", from);
            if (name == NULL) name = vname;
            if (version == NULL) version = vver;
        }
    }
    {
        char seen[600];
        const char *found_arch = NULL, *afrom = NULL;
        int got = drawer_arch(&d, &found_arch, &afrom, seen, sizeof seen);
        if (got < 0) {
            drawer_free(&d);
            return refuse_c(20, "the drawer holds executables for different CPUs: %s. One "
                            "package is one architecture; publish each separately", seen);
        }
        if (a->arch != NULL && got > 0 && strcmp(a->arch, found_arch) != 0) {
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
    kind = a->kind ? a->kind : "application";
    if (name == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no NAME given and no $VER: cookie found; add NAME <name>");
    }
    if (version == NULL) {
        drawer_free(&d);
        return refuse_c(20, "no VERSION given and no $VER: cookie found; add VERSION <version>");
    }
    if ((why = pkg_check_name(name)) || (why = pkg_check_version(version))
        || (why = pkg_check_arch(arch)) || (why = pkg_check_kind(kind))) {
        drawer_free(&d);
        return refuse_c(20, "%s", why);
    }

    pkg_manifest_set(&out->m.name, name);
    pkg_manifest_set(&out->m.version, version);
    pkg_manifest_set(&out->m.architecture, arch);
    pkg_manifest_set(&out->m.kind, kind);
    if (add_depends(&out->m, a->depends) != 0) {
        drawer_free(&d);
        return 1;
    }
    if (strcmp(kind, "image") == 0 && to_image(&d, name) != 0) {
        drawer_free(&d);
        return 1;
    }

    w = pkg_writer_new();
    if (w == NULL) { drawer_free(&d); return refuse("out of memory"); }
    for (i = 0; i < d.n; i++) {
        char hex[PKG_SHA256_HEXLEN + 1];
        pkg_sha256_hex(d.v[i].data, d.v[i].len, hex);
        if (pkg_manifest_add_file(&out->m, d.v[i].rel, hex, (unsigned long long)d.v[i].len) != 0
            || pkg_writer_add(w, d.v[i].rel, d.v[i].data, d.v[i].len) != PKG_OK) {
            pkg_writer_free(w); drawer_free(&d);
            return refuse("out of memory");
        }
    }
    drawer_free(&d);
    if (pkg_writer_finish(w, &out->pkg, &out->pkg_len) != PKG_OK) {
        pkg_writer_free(w);
        return refuse_c(17, "cannot assemble the container");
    }
    pkg_writer_free(w);
    pkg_sha256_hex(out->pkg, out->pkg_len, payload);
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
};

struct index {
    struct entry *e;
    size_t        n;
};

static void mark_withdrawn(const char *channel, struct index *ix);

static int read_index(const char *channel, struct index *ix)
{
    char *path = pkg_join(channel, "index");
    unsigned char *buf = NULL;
    size_t len = 0, at = 0;
    unsigned line = 0;

    ix->e = NULL;
    ix->n = 0;
    if (path == NULL)
        return refuse("out of memory");
    if (!pkg_fs_exists(path)) {
        free(path);
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
        if (ll >= sizeof tmp || nl == NULL) {
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

static char *object_path(const char *channel, const char *digest, const char *ext)
{
    char rel[128];
    snprintf(rel, sizeof rel, "objects/%s.%s", digest, ext);
    return pkg_join(channel, rel);
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

/* Pick the entry for name: EXACT when version is given, else the highest. */
static int pick_quiet;     /* the version was already chosen and traced */

static const struct entry *pick(const struct index *ix, const char *name, const char *version)
{
    const struct entry *p = NULL;
    size_t i;
    for (i = 0; i < ix->n; i++) {
        if (strcmp(ix->e[i].name, name) != 0 || !arch_matches(&ix->e[i]))
            continue;
        if (version) {
            if (pkg_version_cmp(ix->e[i].version, version) == 0)
                p = &ix->e[i];
        } else if (ix->e[i].withdrawn) {
            tr("skipping %s %s: withdrawn by its publisher", ix->e[i].name, ix->e[i].version);
        } else if (p == NULL || pkg_version_cmp(ix->e[i].version, p->version) > 0) {
            p = &ix->e[i];
        }
    }
    if (pick_quiet)
        ;
    else if (p != NULL)
        tr("picked %s %s: %s", p->name, p->version, version ? "the version asked for"
           : "the highest the channel offers");
    else
        tr("the channel offers no %s%s%s", name, version ? " " : "", version ? version : "");
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
    if (strstr(a, b) != NULL || strstr(b, a) != NULL)
        return 1;
    if (edits(a, b) <= 2)
        return 1;
    return la == lb && la >= 3 && strncmp(a, b, la) == 0;
}

static void say_not_found(const struct index *ix, const char *name, const char *version,
                          const char *channel)
{
    size_t i, j, at = 0;
    char offered[512];
    offered[0] = '\0';
    for (i = 0; i < ix->n && at + 72 < sizeof offered; i++)
        if (strcmp(ix->e[i].name, name) == 0)
            at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                   at ? ", " : "; versions offered: ", ix->e[i].version);
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
    if (strcmp(f->m.name, e->name) != 0 || pkg_version_cmp(f->m.version, e->version) != 0
        || f->m.payload == NULL) {
        refuse_c(12, "the manifest of %s disagrees with the channel index about what it is", what);
        goto out;
    }
    /* 4. The payload the signed manifest names. */
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
static int other_signers(const char *channel, const struct index *ix, const char *name,
                         const char *signer, char *others, size_t olen)
{
    size_t i, at = 0;
    int n = 0;
    char claim[65], seen[8][65];
    others[0] = '\0';
    for (i = 0; i < ix->n; i++) {
        int j, dup = 0;
        if (strcmp(ix->e[i].name, name) != 0 || !claimed_signer(channel, ix->e[i].digest, claim))
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
            say("%s %s is already withdrawn from %s\n", e->name, e->version, a->channel);
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
        say("%s %s %s from %s: it stays in the channel, and nothing installs it any more\n",
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
        say("  key for %s changed by explicit ACCEPTKEY\n    was %s\n    now %s\n",
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
    const char *staging;
    char        err[400];
};

static int stage_entry(const struct pkg_entry *e, void *ctx)
{
    struct stage_ctx *s = (struct stage_ctx *)ctx;
    char *p = pkg_join(s->staging, e->path);
    if (p == NULL || pkg_fs_write_atomic(p, e->data, e->data_len) != 0) {
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
static int apply(const char *root, const struct pkg_manifest *old, const struct fetched *f,
                 unsigned long *placed, unsigned long *dropped, unsigned long *kept)
{
    const struct pkg_manifest *m = &f->m;
    struct walk_ctx wc;
    struct stage_ctx sc;
    char *staging, *dbp;
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

    for (i = 0; i < m->nfiles; i++) {
        const struct pkg_file *of = old ? find_file(old, m->files[i].path) : NULL;
        if (of == NULL) {
            char *t = pkg_join(root, m->files[i].path);
            int there = t ? pkg_fs_exists(t) : 1;
            free(t);
            if (there)
                return refuse_c(15, "\"%s\" already exists in %s and belongs to no installed version "
                              "of %s; nothing was changed. It may be the requester's own file",
                              m->files[i].path, root, m->name);
        } else if (file_state(root, of->path, of->digest, of->size) == 1) {
            return refuse_c(15, "\"%s\" was edited since %s %s was installed, and %s %s ships it too; "
                          "nothing was changed. The edit belongs to whoever made it: the requester decides whether "
                          "to keep it elsewhere first", of->path, old->name, old->version, m->name, m->version);
        }
    }

    tr("%s %s: every file checked against the container, nothing in the way", m->name, m->version);
    if (dryrun) {
        /* Every check above has passed; say what would move, move nothing. */
        *placed = (unsigned long)m->nfiles;
        for (i = 0; old != NULL && i < old->nfiles; i++)
            if (find_file(m, old->files[i].path) == NULL)
                (*dropped)++;
        return 0;
    }
    {
        char rel[128];
        snprintf(rel, sizeof rel, ".pkg/staging/%s", m->name);
        staging = pkg_join(root, rel);
    }
    if (staging == NULL || pkg_fs_rmtree(staging) != 0) {
        free(staging);
        return refuse_c(17, "cannot prepare staging in %s", root);
    }
    sc.staging = staging; sc.err[0] = '\0';
    if (pkg_read(f->pkg, f->pkg_len, stage_entry, &sc, &stopped) != PKG_OK) {
        refuse_c(PKGRC_IO, "%s; nothing was changed", sc.err);
        pkg_fs_rmtree(staging);
        free(staging);
        return 1;
    }

    for (i = 0; i < m->nfiles; i++) {
        char *from = pkg_join(staging, m->files[i].path);
        char *to = pkg_join(root, m->files[i].path);
        int good = from && to && pkg_fs_rename(from, to) == 0;
        free(from);
        free(to);
        if (!good) {
            refuse_c(17, "cannot place \"%s\": %s. %lu of %lu files were placed",
                   m->files[i].path, strerror(errno), (unsigned long)i, (unsigned long)m->nfiles);
            free(staging);
            return 1;
        }
        (*placed)++;
    }

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
                else say("  kept     %s (edited, and no longer part of %s)\n", of->path, m->name);
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
    const char     *root, *channel, *acceptkey;
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

/* Plan `name`: exactly `exact` when given, else the highest the channel offers,
 * which must be at least `min`. `from` is the package that needs it, NULL for
 * the one the person named. */
static int plan_one(struct plan *p, const char *name, const char *min, const char *exact,
                    const char *from)
{
    const struct entry *e;
    struct fetched f;
    struct pkg_manifest cur;
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
            int low = min != NULL && pkg_version_cmp(cur.version, min) < 0;
            if (!low)
                tr("%s is satisfied by the installed %s %s", name, name, cur.version);
            if (low)
                refuse_c(16, "%s needs %s >= %s, and %s has %s %s; nothing was changed. "
                         "Upgrading %s would change it for everything that uses it",
                         from, name, min, p->root, name, cur.version, name);
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
    if (e == NULL) {
        if (from == NULL) {
            say_not_found(p->ix, name, exact, p->channel);
            return 1;
        }
        return refuse_c(16, "%s depends on %s%s%s, which the channel %s does not offer; "
                        "nothing was changed", from, name, min ? " >= " : "", min ? min : "",
                        p->channel);
    }
    if (e->withdrawn)
        return refuse_n(18, "ask-requester", "%s %s was withdrawn by its publisher in %s; nothing "
                        "was changed. Installing it anyway is the requester's decision, and Pkg "
                        "does not take it", e->name, e->version, p->channel);
    if (min != NULL && pkg_version_cmp(e->version, min) < 0)
        return refuse_c(16, "%s needs %s >= %s, and the highest the channel offers is %s; "
                        "nothing was changed", from ? from : "the request", name, min, e->version);
    if (p->depth >= sizeof p->stack / sizeof p->stack[0])
        return refuse_c(16, "the dependencies of %s go more than %u levels deep", name,
                        (unsigned)(sizeof p->stack / sizeof p->stack[0]));
    if (fetch(p->channel, e, &f) != 0) {
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
            if (strcmp(p->ix->e[o].name, e->name) == 0
                && (oldest == NULL || pkg_version_cmp(p->ix->e[o].version, oldest->version) < 0))
                oldest = &p->ix->e[o];
        if (first && oldest != NULL && oldest != e
            && claimed_signer(p->channel, oldest->digest, first_signer)
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
            if (p != NULL && pkg_fs_unlink(p) == 0) {
                (*removed)++;
                pkg_fs_prune_empty_parents(root, m->files[i].path);
            }
            free(p);
        } else if (s == 1) {
            (*kept)++;
            if (!report)
                continue;
            if (machine) kv("kept", "%s", m->files[i].path);
            else say("  kept     %s (changed since install, so it is yours now)\n", m->files[i].path);
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
            say("  %s %s %s, a dependency\n", dryrun ? "would add" : "added   ",
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
    p->channel = a->channel;
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
            say("%s %s, the mount entry for %s (%lu blocks)\n",
                    dryrun ? "would write" : "wrote", a->out, m.name, blocks);
        say("\nOn AROS, the device is named after the mountlist file:\n"
                "  MakeDir %s\n  Assign FDSK: %s\n  MakeLink %s/%s %s\n  Protect %s w SUB\n%s%s%s"
                "  Mount %s\n", fdsk, fdsk, fdsk, unitbuf, img ? img : "", img ? img : "",
                libs ? "  Assign LIBS: " : "", libs ? a->root : "", libs ? "/Libs ADD\n" : "",
                a->out ? a->out : "<file>");
        if (!handler[0])
            say("No FFS handler is installed in this root; the entry relies on the "
                    "system's. Hosted AROS has none: install one, or name it with HANDLER.\n");
    }
    free(img);
    pkg_manifest_free(&m);
    return 0;
}

/* What a channel offers, each version checked the way INSTALL would check
 * it: manifest against the index, signature, payload against the manifest.
 * Nothing is installed. Exits with the class of the first bad entry. */
static int cmd_show(const struct pkg_options *a)
{
    struct index ix;
    size_t i, shown = 0, bad = 0;
    int first_bad = 0;

    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (read_index(a->channel, &ix) != 0) return 1;
    kv("result", "shown");
    for (i = 0; i < ix.n; i++) {
        struct fetched f;
        const char *status = "ok", *here = NULL;
        int rc;
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        if (cancelled("while checking the channel, which was not changed")) {
            free(ix.e);
            return 1;
        }
        shown++;
        quiet = 1;
        refused_class = 0;
        quiet_reason[0] = '\0';
        rc = fetch(a->channel, &ix.e[i], &f);
        quiet = 0;
        if (rc != 0) {
            status = class_name(refused_class);
            bad++;
            if (!first_bad) first_bad = refused_class;
        } else if (ix.e[i].withdrawn) {
            status = "withdrawn";
        }
        if (a->root != NULL) {
            /* Against a root: is this the version installed there? */
            struct pkg_manifest cur;
            int q = quiet;
            quiet = 1;
            if (load_installed(a->root, ix.e[i].name, &cur, 1) == 0) {
                here = pkg_version_cmp(cur.version, ix.e[i].version) == 0 ? "installed"
                       : "other-version";
                pkg_manifest_free(&cur);
            } else {
                here = "no";
            }
            quiet = q;
        }
        if (machine) {
            char j[400];
            snprintf(j, sizeof j, "%s %s %s %s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                     rc == 0 ? f.m.kind : "-", rc == 0 ? f.m.architecture : "-", status,
                     rc == 0 ? f.signer : "-", here ? " " : "", here ? here : "");
            rec_item("entry", j, "name", ix.e[i].name, "version", ix.e[i].version,
                     "kind", rc == 0 ? f.m.kind : "-", "architecture", rc == 0 ? f.m.architecture : "-",
                     "status", status, "signer", rc == 0 ? f.signer : "-",
                     here ? "installed" : NULL, here, NULL);
            if (rc == 0) {
                size_t d;
                for (d = 0; d < f.m.ndeps; d++) {
                    snprintf(j, sizeof j, "%s %s %s%s%s", ix.e[i].name, ix.e[i].version,
                             f.m.deps[d].name, f.m.deps[d].min ? " >= " : "",
                             f.m.deps[d].min ? f.m.deps[d].min : "");
                    rec_item("depends", j, "package", ix.e[i].name, "version", ix.e[i].version,
                             "needs", f.m.deps[d].name, "min", f.m.deps[d].min ? f.m.deps[d].min : "",
                             NULL);
                }
            } else {
                char jp[700];
                snprintf(jp, sizeof jp, "%s %s %s", ix.e[i].name, ix.e[i].version, quiet_reason);
                rec_item("problem", jp, "package", ix.e[i].name, "version", ix.e[i].version,
                         "reason", quiet_reason, NULL);
            }
        } else {
            say("%-20s %-8s %-11s %-8s %-10s %.16s\n", ix.e[i].name, ix.e[i].version,
                    rc == 0 ? f.m.kind : "-", rc == 0 ? f.m.architecture : "-", status,
                    rc == 0 ? f.signer : "-");
            if (rc != 0)
                say("  %s\n", quiet_reason);
        }
        fetched_free(&f);
    }
    for (i = 0; i < ix.n; i++) {
        char others[600], claim[65];
        size_t j;
        int earlier = 0;
        if (a->target != NULL && strcmp(ix.e[i].name, a->target) != 0)
            continue;
        for (j = 0; j < i; j++)
            if (strcmp(ix.e[j].name, ix.e[i].name) == 0) earlier = 1;
        if (earlier || !claimed_signer(a->channel, ix.e[i].digest, claim))
            continue;
        if (other_signers(a->channel, &ix, ix.e[i].name, claim, others, sizeof others) > 0) {
            if (machine)
                kv("warning", "%s is signed by more than one key: %s on %s, and %s", ix.e[i].name,
                   claim, ix.e[i].version, others);
            else
                say("  warning: %s is signed by more than one key: %.16s on %s, and %s\n",
                    ix.e[i].name, claim, ix.e[i].version, others);
        }
    }
    kv("count", "%lu", (unsigned long)shown);
    kv("bad", "%lu", (unsigned long)bad);
    if (!machine && shown == 0)
        say("%s%s offers nothing%s%s\n", a->channel, "", a->target ? " named " : "",
                a->target ? a->target : "");
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
    vol = a->name ? a->name : "image";
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
        say("wrote %s: volume %s, %lu blocks of %u bytes\n", a->out, vol,
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
    char *po = object_path(a->channel, b->m.payload, "pkg");
    char *so = object_path(a->channel, mdigest, "sig");
    char signer[65], first_signer[65];
    const struct entry *oldest = NULL;
    int good_m, good_p, good_s = 0, rc = 0;
    size_t o;

    good_m = object_good(mo, mdigest);
    good_p = object_good(po, b->m.payload);
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
            say("%s %s is already published with this exact content; nothing to do\n",
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
        say("%s %s %s in %s:%s%s%s written again from the same bytes\n",
            dryrun ? "would repair" : "repaired", b->m.name, b->m.version, a->channel,
            good_m ? "" : " manifest", good_p ? "" : " payload", good_s ? "" : " signature");
out:
    free(mo); free(po); free(so);
    return rc;
}

static int cmd_publish(const struct pkg_options *a)
{
    struct built b;
    struct index ix;
    struct key k;
    size_t i;
    char *po = NULL, *mo = NULL, *so = NULL, s12[13];
    char mdigest[PKG_SHA256_HEXLEN + 1];
    int rc = 1;

    if (a->channel == NULL)
        return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (load_key(a->sign, &k) != 0)
        return 1;
    if (build(a, &b) != 0) { built_free(&b); return 1; }
    if (read_index(a->channel, &ix) != 0) { built_free(&b); return 1; }
    /* A version is named by its manifest, which names its payload. */
    pkg_sha256_hex(b.text, b.text_len, mdigest);

    for (i = 0; i < ix.n; i++) {
        if (strcmp(ix.e[i].name, b.m.name) == 0
            && pkg_version_cmp(ix.e[i].version, b.m.version) == 0
            && strcmp(ix.e[i].arch, b.m.architecture) == 0) {
            if (strcmp(ix.e[i].digest, mdigest) == 0) {
                rc = republish(a, &ix, &b, &k, mdigest);
            } else {
                refuse_c(15, "%s %s is already published with a different payload; a published "
                       "version never changes, so publish this as a new version",
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
        if (oldest != NULL && claimed_signer(a->channel, oldest->digest, first_signer)
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
        kv("signer", "%s", k.pkhex);
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.arch_from[0]) kv("arch-from", "%s", b.arch_from);
        for (d = 0; d < b.nleft; d++)
            kv("left-out", "%s", b.left_out[d]);
        if (!machine) {
            say("would publish %s %s (%s, %s) to %s, signed by %.16s\n", b.m.name, b.m.version,
                    b.m.kind, b.m.architecture, a->channel, k.pkhex);
            for (d = 0; d < b.m.ndeps; d++)
                say("  depends  %s%s%s\n", b.m.deps[d].name, b.m.deps[d].min ? " >= " : "",
                        b.m.deps[d].min ? b.m.deps[d].min : "");
            if (b.m.ndeps == 0)
                say("  depends  nothing\n");
            for (d = 0; d < b.m.nfiles; d++)
                say("  file     %s (%llu bytes)\n", b.m.files[d].path, b.m.files[d].size);
            for (d = 0; d < b.nleft; d++)
                say("  left out %s\n", b.left_out[d]);
            if (b.ver_from[0])
                say("  name and version from $VER: in %s\n", b.ver_from);
        }
        rc = 0;
        goto out;
    }
    po = object_path(a->channel, b.m.payload, "pkg");     /* content-addressed, shareable */
    mo = object_path(a->channel, mdigest, "manifest");      /* one per version */
    so = object_path(a->channel, mdigest, "sig");
    if (po == NULL || mo == NULL || so == NULL
        || pkg_fs_write_atomic(po, b.pkg, b.pkg_len) != 0
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
    short12(b.m.payload, s12);
    kv("result", "published");
    kv("name", "%s", b.m.name);
    kv("version", "%s", b.m.version);
    kv("channel", "%s", a->channel);
    kv("manifest", "%s", mdigest);
    kv("payload", "%s", b.m.payload);
    kv("signer", "%s", k.pkhex);
    kv("files", "%lu", (unsigned long)b.m.nfiles);
    if (!machine)
    say("published %s %s to %s: %lu files, payload %s, signed by %.16s\n", b.m.name,
           b.m.version, a->channel, (unsigned long)b.m.nfiles, s12, k.pkhex);
    if (b.arch_from[0] && machine)
        kv("arch-from", "%s", b.arch_from);
    else if (b.arch_from[0])
        say("  architecture %s, read from %s\n", b.m.architecture, b.arch_from);
    if (b.ver_from[0] && machine)
        kv("version-from", "%s", b.ver_from);
    else if (b.ver_from[0])
        say("  name and version taken from $VER: in %s\n", b.ver_from);
    for (i = 0; i < b.nleft; i++) {
        if (machine)
            kv("left-out", "%s", b.left_out[i]);
        else
            say("  left out %s, host metadata no Amiga uses\n", b.left_out[i]);
    }
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
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (a->version && pkg_check_version(a->version))
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    return 0;
}

static int cmd_install(const struct pkg_options *a)
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
    if (read_index(a->channel, &ix) != 0) return 1;
    {
        char cpus[200];
        if (arch_ambiguous(&ix, a->target, cpus, sizeof cpus)) {
            refuse_c(20, "%s is offered for several CPUs (%s), and nothing says which machine %s "
                     "is for: add ARCH <cpu>; the root remembers it from then on", a->target,
                     cpus, a->root);
            free(ix.e);
            return 1;
        }
    }
    e = pick(&ix, a->target, a->version);
    if (e == NULL) { say_not_found(&ix, a->target, a->version, a->channel); free(ix.e); return 1; }
    if (load_installed(a->root, e->name, &cur, 1) == 0) {
        if (is_auto(a->root, cur.name)) {
            /* Asked for by name now: no longer an orphan candidate. */
            set_auto(a->root, cur.name, 0);
            kv("result", "kept");
            kv("name", "%s", cur.name);
            kv("version", "%s", cur.version);
            if (!machine)
                say("%s %s was installed as a dependency; it is now kept for itself\n",
                        cur.name, cur.version);
            pkg_manifest_free(&cur);
            free(ix.e);
            return 0;
        }
        if (pkg_version_cmp(cur.version, e->version) == 0) {
            /* The state asked for is the state there: a repeated INSTALL, as
             * an agent retrying after a timeout sends, succeeds and changes
             * nothing. */
            kv("result", "unchanged");
            kv("name", "%s", cur.name);
            kv("version", "%s", cur.version);
            if (!machine)
                say("%s %s is already installed in %s\n", cur.name, cur.version, a->root);
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
        short12(f.m.payload, s12);
        if (!dryrun)
            record_arch(a->root, p.f[p.n - 1].m.architecture);
        kv("result", "%s", res("installed", "would-install"));
        kv("name", "%s", f.m.name);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("files", "%lu", placed);
        kv("payload", "%s", f.m.payload);
        kv("signer", "%s", f.signer);
        if (!machine)
        say("%s %s %s into %s: %lu files, payload %s, signed by %.16s\n",
               dryrun ? "would install" : "installed",
               f.m.name, f.m.version, a->root, placed, s12, f.signer);
        if (strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1) {
            kv("image", "%s", f.m.files[0].path);
            kv("blocks", "%llu", f.m.files[0].size / PKG_IMAGE_BLOCK);
            if (!machine)
                say("  image %s, %llu blocks; pkg MOUNTLIST %s ROOT %s OUT <file> writes "
                        "its mount entry\n", f.m.files[0].path,
                        f.m.files[0].size / PKG_IMAGE_BLOCK, f.m.name, a->root);
        }
        rc = 0;
    }
    plan_free(&p);
    free(ix.e);
    return rc;
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
        say("%s%s %s from %s to %s in %s: %lu placed, %lu removed",
               dryrun ? "would have " : "", verb, f.m.name,
               cur->version, f.m.version, a->root, placed, dropped);
        if (kept) say(", %lu kept", kept);
        say("\n");
        }
        rc = 0;
    }
    plan_free(&p);
    return rc;
}

static int cmd_upgrade(const struct pkg_options *a)
{
    struct index ix;
    const struct entry *e;
    struct pkg_manifest cur;
    int rc = 1, c;

    if (need_root_channel(a) != 0) return 1;
    if (resolve_arch(a) != 0) return 1;
    if (load_installed(a->root, a->target, &cur, 0) != 0) return 1;
    if (target_arch == NULL && strcmp(cur.architecture, "generic") != 0)
        target_arch = cur.architecture;       /* stay on the installed CPU */
    if (read_index(a->channel, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    e = pick(&ix, a->target, a->version);
    if (e == NULL) {
        say_not_found(&ix, a->target, a->version, a->channel);
        goto out;
    }
    c = pkg_version_cmp(e->version, cur.version);
    if (c == 0) {
        kv("result", "unchanged");
        kv("name", "%s", cur.name);
        kv("version", "%s", cur.version);
        if (!machine)
            say("%s is already at %s\n", cur.name, cur.version);
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
    if (read_index(a->channel, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->target) == 0 && pkg_version_cmp(ix.e[i].version, prev.version) == 0
            && (strcmp(ix.e[i].arch, cur.architecture) == 0 || strcmp(ix.e[i].arch, "generic") == 0))
            e = &ix.e[i];
    if (e == NULL)
        refuse_c(11, "the previous version of %s, %s, is no longer in the channel %s",
               a->target, prev.version, a->channel);
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
                say("%-24s %-10s %-12s %lu files%s\n", m.name, m.version, m.kind,
                        (unsigned long)m.nfiles, dep ? ", a dependency" : "");
            pkg_manifest_free(&m);
        }
        free(names[i]);
    }
    free(names);
    if (n == 0 && !machine)
        say("nothing installed in %s\n", a->root);
    kv("count", "%lu", (unsigned long)n);
    return 0;
}

static int cmd_verify(const struct pkg_options *a)
{
    struct pkg_manifest m;
    size_t i, changed = 0, missing = 0;

    if (a->target == NULL)  return refuse_c(20, "name the package to verify");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (load_installed(a->root, a->target, &m, 0) != 0) return 1;
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("files", "%lu", (unsigned long)m.nfiles);
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 1) {
            changed++;
            if (machine) kv("changed", "%s", m.files[i].path);
            else say("  changed  %s\n", m.files[i].path);
        }
        if (s == 2) {
            missing++;
            if (machine) kv("missing", "%s", m.files[i].path);
            else say("  missing  %s\n", m.files[i].path);
        }
    }
    if (changed + missing == 0) {
        kv("result", "intact");
        if (!machine)
            say("%s %s: %lu files, all intact\n", m.name, m.version, (unsigned long)m.nfiles);
        pkg_manifest_free(&m);
        return 0;
    }
    if (!machine)
        say("%s %s: %lu changed, %lu missing, of %lu files\n", m.name, m.version,
                (unsigned long)changed, (unsigned long)missing, (unsigned long)m.nfiles);
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    pkg_manifest_free(&m);
    return 1;
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
    size_t *which, n, i, total = 0;
    int loaded = 0;

    for (;;) {
        /* A dry run works on the list in memory, since nothing leaves the disk. */
        if (!(dryrun && loaded) && load_all(a->root, &in) != 0)
            return 1;
        loaded = 1;
        which = malloc((in.n ? in.n : 1) * sizeof *which);
        if (which == NULL) { installed_free(&in); return refuse("out of memory"); }
        n = find_orphans(&in, a->root, which);
        for (i = 0; i < n; i++) {
            const struct pkg_manifest *m = &in.m[which[i]];
            size_t r, k, g;
            if (remove_files(a->root, m, &r, &k, &g, 1) != 0) {
                free(which);
                installed_free(&in);
                return 1;
            }
            total++;
            if (machine)
            {
                char j[140];
                snprintf(j, sizeof j, "%s %s", m->name, m->version);
                rec_item("package", j, "name", m->name, "version", m->version, NULL);
            }
            else
                say("%s %s %s, which nothing needed: %lu files%s\n",
                        dryrun ? "would remove" : "removed", m->name,
                        m->version, (unsigned long)r, k ? ", edited files kept" : "");
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
    kv("result", "%s", res("removed", "would-remove"));
    kv("count", "%lu", (unsigned long)total);
    if (!machine && total == 0)
        say("no orphans in %s\n", a->root);
    return 0;
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
    say("%s %s %s from %s: %lu files %s", dryrun ? "would remove" : "removed", m.name,
           m.version, a->root, (unsigned long)removed, dryrun ? "to remove" : "removed");
    if (kept) say(", %lu kept", (unsigned long)kept);
    if (gone) say(", %lu already gone", (unsigned long)gone);
    say("\n");
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
                say("  %s %s is no longer needed by anything; REMOVE ORPHANS takes it out\n",
                        in.m[which[i]].name, in.m[which[i]].version);
        }
        free(which);
        installed_free(&in);
    }
    pkg_manifest_free(&m);
    return 0;
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
    rc = fn(o != NULL ? o : &none);
    rc = rc == 0 ? PKGRC_OK : refused_class ? refused_class : PKGRC_REFUSED;
    sink = NULL;
    return rc;
}

int pkg_keygen   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keygen", cmd_keygen, o); }
int pkg_sign     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "sign", cmd_sign, o); }
int pkg_keyinfo  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "keyinfo", cmd_keyinfo, o); }
int pkg_withdraw (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "withdraw", cmd_withdraw, o); }
int pkg_manifest (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "manifest", cmd_manifest, o); }
int pkg_publish  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "publish", cmd_publish, o); }
int pkg_install  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "install", cmd_install, o); }
int pkg_upgrade  (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "upgrade", cmd_upgrade, o); }
int pkg_rollback (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "rollback", cmd_rollback, o); }
int pkg_list     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "list", cmd_list, o); }
int pkg_verify   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "verify", cmd_verify, o); }
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "remove", cmd_remove, o); }
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "image", cmd_image, o); }
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "mountlist", cmd_mountlist, o); }
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "show", cmd_show, o); }

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
