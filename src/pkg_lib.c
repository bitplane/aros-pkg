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
#include "pkg_ameta.h"
#include "pkg_archive.h"
#include "pkg_sha256.h"

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
        say("%s\n", buf);
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
        say("  hint: %s\n", buf);
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
    size_t         nconfig;         /* configuration files */
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
    struct drawer d;
    struct pkg_writer *w;
    char vname[65], vver[64];
    const char *from = NULL, *name, *version, *arch, *kind, *why;
    const char *kind_src = a->kind, *deps_src = a->depends;
    char arch_file[1024], arch_prefix[1024];
    char ikind[32], ideps[1024], ifrom[160], iconf[4096];
    const char *conf_src = a->config;
    char payload[PKG_SHA256_HEXLEN + 1];
    size_t i;

    memset(out, 0, sizeof *out);
    memset(&d, 0, sizeof d);
    pkg_manifest_init(&out->m);
    if (a->target == NULL)
        return refuse_c(20, "name the drawer to package");
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
                        "take it from; Pkg does not guess what a package is. KIND image for "
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
    drawer_free(&d);
    if (conf_src != NULL && strcmp(kind, "image") != 0 && ascii_casecmp(conf_src, "none") != 0
        && mark_config(&out->m, conf_src, out->config_from[0] == '\0') != 0) {
        pkg_writer_free(w);
        return 1;
    }
    for (i = 0; i < out->m.nfiles; i++)
        out->nconfig += out->m.files[i].config ? 1 : 0;
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
};

struct index {
    struct entry *e;
    size_t        n;
};

static void mark_withdrawn(const char *channel, struct index *ix);

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
    rc = pkg_net_get(url, local, net_err, sizeof net_err);
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

    ix->e = NULL;
    ix->n = 0;
    net_err[0] = '\0';
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
static int inherited(const char *name, const char *arch, char *kind, size_t kl,
                     char *deps, size_t dl, char *from, size_t fl, char *conf, size_t cl)
{
    const struct entry *best = NULL;
    size_t o, d, at = 0;
    int pass;
    char *mp, err[200];
    unsigned char *buf;
    size_t len;
    struct pkg_manifest em;

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
    if (pkg_manifest_parse((const char *)buf, len, &em, err, sizeof err) != 0) { free(buf); return 0; }
    free(buf);
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
        if (ap != NULL && pkg_fs_exists(ap))
            return ap;
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
        free(dir);
        return dest;
    }
    pkg_fs_mkdirs(dir);
    free(dir);
    if (!machine)
        say("downloading %s (%llu MB) from %s, once for every package it holds\n", an,
            (m->archive_size + 524288ull) / 1048576ull, m->archive_url);
    rc = pkg_net_get(m->archive_url, dest, net_err, sizeof net_err);
    if (rc == 0 && file_digest(dest, hex, &size) == 0
        && size == m->archive_size && strcmp(hex, m->archive_sha) == 0) {
        tr("%s: downloaded %s, %llu bytes, SHA-256 as signed", what, m->archive_url, size);
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
        if (ap != NULL && pkg_fs_exists(ap)) { free(dest); return ap; }
        free(ap);
    }
    refuse_c(rc == 1 ? 11 : 17, "%s comes from the archive %s, which could not be downloaded from %s: %s",
             what, an, m->archive_url, rc == 1 ? "it is not there any more" : net_err);
    free(dest);
    return NULL;
}

static int fetch_from_archive(const char *channel, struct fetched *f, const char *what)
{
    char an[1024], prefix[1024], err[300];
    char *ap;
    struct arch_fetch af;
    struct pkg_writer *w = NULL;
    size_t i, n = f->m.nfiles ? f->m.nfiles : 1;
    int rc = 1;

    pkg_archive_split(f->m.source, an, sizeof an, prefix, sizeof prefix);
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
    tr("%s: reading its files out of %s", what, ap);
    if (pkg_archive_walk(ap, af_want, af_data, &af, err, sizeof err) != 0) {
        if (af.oversize >= 0)
            refuse_c(12, "the archive %s holds a %s longer than the %llu bytes %s's signed manifest "
                     "gives it; it was not read further. Nothing was installed", ap,
                     f->m.files[af.oversize].path, f->m.files[af.oversize].size, what);
        else
            refuse_c(12, "the archive %s is refused: %s. Nothing was installed", ap, err[0] ? err : "unreadable");
        goto done;
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
    p = pkg_join(s->staging, e->path);
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
            warn("%s is not an .ameta this Pkg can write: %s's attributes were not recorded",
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
                          "with CONFIG, and Pkg then keeps the edit", of->path, old->name, old->version,
                          m->name, m->version);
        }
    }
    if (adopted) {
        if (machine) kv("adopted", "%lu", adopted);
        else say("  adopted  %lu file%s already there, identical to %s %s's\n", adopted,
                 adopted == 1 ? "" : "s", m->name, m->version);
    }
    if (same) {
        if (machine) kv("unchanged-files", "%lu", same);
        else say("  unchanged %lu file%s the same in %s and %s, not written again\n", same,
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
                say("  kept     %s (edited; %s %s's version is beside it as %s)\n",
                    m->files[i].path, m->name, m->version, nw);
            } else if (keep[i] == 1) {
                say("  kept     %s (edited)\n", m->files[i].path);
            } else {
                say("  kept     %s (edited; %s %s ships it unchanged)\n", m->files[i].path,
                    m->name, m->version);
            }
            free(nw);
        }

    installed_free(&others);
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
    if (pkg_read(f->pkg, f->pkg_len, stage_entry, &sc, &stopped) != PKG_OK) {
        refuse_c(PKGRC_IO, "%s; nothing was changed", sc.err);
        pkg_fs_rmtree(staging);
        free(staging);
        free(keep);
        return 1;
    }

    for (i = 0; i < m->nfiles; i++) {
        char *from = pkg_join(staging, m->files[i].path);
        char *to = pkg_join(root, m->files[i].path);
        int good;
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
    }
    if (a->out == NULL)
        hint("Mount reads the entry from a file named after the device: add OUT <file>, "
             "for example OUT RAM:%s, and Pkg writes it", m.name);
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

static int cmd_show(const struct pkg_options *a)
{
    struct index ix;
    struct show_row *row = NULL;
    size_t i, shown = 0, bad = 0;
    int first_bad = 0;

    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (read_index(a->channel, &ix) != 0) return 1;
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
        r->rc = fetch(a->channel, &ix.e[i], &r->f);
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
            path = archive_path(a->channel, an);
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
            say("%-20s %-8s %-11s %-8s %-10s %.16s%s\n", ix.e[i].name, ix.e[i].version,
                    rc == 0 ? fp->m.kind : "-", rc == 0 ? fp->m.architecture : "-", status,
                    rc == 0 ? fp->signer : "-",
                    rc == 0 && fp->m.source != NULL && a->metadata ? "  (archive not checked)" : "");
            if (rc != 0)
                say("  %s\n", r->reason);
        }
        fetched_free(fp);
    }
    free(row);
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

/* A new version against the highest one published: the files that changed,
 * and the slips that show there (the old build again, a $VER not raised). */
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
        warn("no executable in the drawer: kind %s is usually a program, and Pkg found no ELF or "
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
                        && strcmp(em.kind, b.m.kind) == 0 && strcmp(em.architecture, b.m.architecture) == 0) {
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
            say("%s %s: every file is that of %s, so no new version is published\n", b.m.name,
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
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
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
            for (d = 0; d < b.m.ncontent; d++)
                say("  content  %s (%llu bytes)\n", b.m.content[d].path, b.m.content[d].size);
            for (d = 0; d < b.nleft; d++)
                say("  left out %s\n", b.left_out[d]);
            if (b.ver_from[0] || b.name_from[0])
                say("  %s from $VER: in %s\n", b.ver_from[0] && b.name_from[0] ? "name and version"
                    : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
            if (b.kind_from[0] || b.deps_from[0])
                say("  %s from %s, published before\n", b.kind_from[0] && b.deps_from[0] ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies", b.kind_from[0] ? b.kind_from : b.deps_from);
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
    say("published %s %s to %s: %lu files, %s%s, signed by %.16s\n", b.m.name,
           b.m.version, a->channel, (unsigned long)b.m.nfiles, b.m.payload ? "payload " : "from ",
           b.m.payload ? s12 : b.m.source, k.pkhex);
    if (b.arch_from[0] && machine)
        kv("arch-from", "%s", b.arch_from);
    else if (b.arch_from[0])
        say("  architecture %s, read from %s\n", b.m.architecture, b.arch_from);
    if (machine) {
        if (b.ver_from[0]) kv("version-from", "%s", b.ver_from);
        if (b.name_from[0]) kv("name-from", "%s", b.name_from);
        if (b.kind_from[0]) kv("kind-from", "%s", b.kind_from);
        if (b.nconfig) kv("config-files", "%lu", (unsigned long)b.nconfig);
        if (b.config_from[0]) kv("config-from", "%s", b.config_from);
        if (b.m.archive_url) kv("upstream", "%s", b.m.archive_url);
        if (b.deps_from[0]) kv("depends-from", "%s", b.deps_from);
    } else if (b.ver_from[0] || b.name_from[0]) {
        say("  %s taken from $VER: in %s\n", b.ver_from[0] && b.name_from[0] ? "name and version"
            : b.ver_from[0] ? "version" : "name", b.ver_from[0] ? b.ver_from : b.name_from);
    }
    if (!machine && (b.kind_from[0] || b.deps_from[0]))
        say("  %s from %s, published before\n", b.kind_from[0] && b.deps_from[0]
            ? "kind and dependencies" : b.kind_from[0] ? "kind" : "dependencies",
            b.kind_from[0] ? b.kind_from : b.deps_from);
    for (i = 0; i < b.nleft; i++) {
        if (machine)
            kv("left-out", "%s", b.left_out[i]);
        else
            say("  left out %s, host metadata no Amiga uses\n", b.left_out[i]);
    }
    if (new_channel)
        hint("the channel %s did not exist and was created: machines install from it with "
             "INSTALL %s ROOT <root> CHANNEL <this channel, as the machine names it>",
             a->channel, b.m.name);
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
        if (f.m.payload) short12(f.m.payload, s12);
        if (!dryrun)
            record_arch(a->root, p.f[p.n - 1].m.architecture);
        kv("result", "%s", res("installed", "would-install"));
        kv("name", "%s", f.m.name);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("files", "%lu", placed);
        if (f.m.payload) kv("payload", "%s", f.m.payload);
        else kv("source", "%s", f.m.source);
        kv("signer", "%s", f.signer);
        if (!machine)
        say("%s %s %s into %s: %lu files, %s%s, signed by %.16s\n",
               dryrun ? "would install" : "installed",
               f.m.name, f.m.version, a->root, placed, f.m.payload ? "payload " : "from ",
               f.m.payload ? s12 : f.m.source, f.signer);
        if (strcmp(f.m.kind, "image") == 0 && f.m.nfiles == 1) {
            kv("image", "%s", f.m.files[0].path);
            kv("blocks", "%llu", f.m.files[0].size / PKG_IMAGE_BLOCK);
            if (!machine)
                say("  image %s, %llu blocks\n", f.m.files[0].path,
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
    if (read_index(a->channel, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    e = pick(&ix, a->target, a->version);
    if (e == NULL) {
        say_not_found(&ix, a->target, a->version, a->channel);
        goto out;
    }
    c = pkg_version_cmp(e->version, cur.version);
    if (c == 0) {
        size_t o;
        kv("result", "unchanged");
        kv("name", "%s", cur.name);
        kv("version", "%s", cur.version);
        if (!machine)
            say("%s is already at %s\n", cur.name, cur.version);
        for (o = 0; o < ix.n; o++)
            if (strcmp(ix.e[o].name, cur.name) == 0 && !arch_matches(&ix.e[o])
                && pkg_version_cmp(ix.e[o].version, cur.version) > 0 && !ix.e[o].withdrawn) {
                if (machine)
                    kv("note", "%s %s is published for %s, not for this root's CPU", ix.e[o].name,
                       ix.e[o].version, ix.e[o].arch);
                else
                    say("  %s %s is published for %s, not yet for this root's CPU\n",
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
    for (p = 0; p < in.n; p++) {
        const struct pkg_manifest *m = &in.m[p];
        size_t changed = 0, missing = 0;
        for (i = 0; i < m->nfiles; i++) {
            int s = file_state(a->root, m->files[i].path, m->files[i].digest, m->files[i].size);
            if (s == 1 && m->files[i].config) {
                edited++;
                if (machine) kv("edited", "%s %s", m->name, m->files[i].path);
                else say("  edited   %s (%s, a configuration file)\n", m->files[i].path, m->name);
            } else if (s == 1) {
                changed++;
                if (machine) kv("changed", "%s %s", m->name, m->files[i].path);
                else say("  changed  %s (%s)\n", m->files[i].path, m->name);
            } else if (s == 2) {
                missing++;
                if (machine) kv("missing", "%s %s", m->name, m->files[i].path);
                else say("  missing  %s (%s)\n", m->files[i].path, m->name);
            }
        }
        files += m->nfiles;
        if (changed + missing) {
            if (bad++ == 0)
                snprintf(first, sizeof first, "%s (%lu changed, %lu missing)", m->name,
                         (unsigned long)changed, (unsigned long)missing);
            if (machine) kv("package", "%s %s damaged %lu %lu", m->name, m->version,
                            (unsigned long)changed, (unsigned long)missing);
            else say("%s %s: %lu changed, %lu missing, of %lu file%s\n", m->name, m->version,
                     (unsigned long)changed, (unsigned long)missing, (unsigned long)m->nfiles,
                     m->nfiles == 1 ? "" : "s");
        } else if (machine) {
            kv("package", "%s %s intact", m->name, m->version);
        } else {
            say("%s %s: %lu file%s, all intact\n", m->name, m->version, (unsigned long)m->nfiles,
                m->nfiles == 1 ? "" : "s");
        }
    }
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
    summary_line("%lu of %lu package%s damaged, first %s", (unsigned long)bad,
       (unsigned long)in.n, in.n == 1 ? "" : "s", first);
    hint("VERIFY <name> also says which missing files were moved by hand. Pkg overwrites no "
         "changed file: whether the change is damage or someone's work is the requester's call");
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
            else say("  edited   %s (a configuration file)\n", m.files[i].path);
        } else if (s == 1) {
            changed++;
            if (machine) kv("changed", "%s", m.files[i].path);
            else say("  changed  %s\n", m.files[i].path);
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
                    else say("  moved    %s -> %s\n", m.files[i].path, found[i]);
                } else if (miss[i]) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say("  missing  %s\n", m.files[i].path);
                }
        } else {
            for (i = 0; i < m.nfiles; i++)
                if (file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size) == 2) {
                    if (machine) kv("missing", "%s", m.files[i].path);
                    else say("  missing  %s\n", m.files[i].path);
                }
        }
        for (i = 0; found != NULL && i < m.nfiles; i++) free(found[i]);
        free(found);
        free(miss);
        if (moved == missing && changed == 0) {
            kv("result", "moved");
            if (!machine)
                say("%s %s: moved by hand, every file intact where it is now\n", m.name, m.version);
            hint("moving an installed drawer is the person's right, and the package stays listed. "
                 "REMOVE and UPGRADE act on the places Pkg recorded: the moved files are left "
                 "where they are, and an upgrade installs beside them");
            pkg_manifest_free(&m);
            return 0;
        }
    }
    if (changed + missing == 0) {
        kv("result", "intact");
        if (edited) kv("edited-config", "%lu", (unsigned long)edited);
        if (!machine)
            say("%s %s: %lu files, all intact%s\n", m.name, m.version, (unsigned long)m.nfiles,
                edited ? ", configuration files edited as people do" : "");
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
        else say("  aside    %s -> %s\n", pf->path, oldrel);
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
    else say("  restored %s\n", pf->path);
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
    if (e == NULL) {
        refuse_c(11, "%s %s is installed and the channel no longer offers it, so its %lu damaged "
                 "file%s cannot be put back; UPGRADE to a version the channel has", m.name,
                 m.version, (unsigned long)needed, needed == 1 ? "" : "s");
        goto out;
    }
    if (fetch(a->channel, e, &f) != 0)
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
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (resolve_arch(a) != 0) return 1;
    if (read_index(a->channel, &ix) != 0) return 1;
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
        else if (r) say("%s: %lu file%s put back\n", name, r, r == 1 ? "" : "s");
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
                    say("%-24s not removed: %s\n", m->name, quiet_reason);
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
            say("%s\n", summary);
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
    s->newer = s->offer != NULL && pkg_version_cmp(s->offer->version, m->version) > 0;
    /* The check VERIFY makes, size and digest, stopping at the first edit. */
    for (i = 0; i < m->nfiles && !s->edited; i++)
        s->edited = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 1;
    if (s->offer == NULL)  s->state = s->withdrawn ? "withdrawn" : "not-offered";
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
        say("  note: %s\n", buf);
}

/* ROOT and CHANNEL, the root's machine, the channel's index and every
 * installed package. */
static int keep_current_setup(const struct pkg_options *a, struct index *ix, struct installed *in)
{
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (!is_url(a->channel) && !pkg_fs_is_dir(a->channel))  /* a URL answers for itself */
        return refuse_c(11, "there is no channel at %s: not mounted, or not the path meant; "
                        "nothing was checked or changed", a->channel);
    if (resolve_arch(a) != 0) return 1;
    if (read_index(a->channel, ix) != 0) return 1;
    if (load_all(a->root, in) != 0) { free(ix->e); return 1; }
    return 0;
}

static int cmd_status(const struct pkg_options *a)
{
    struct index ix;
    struct installed in;
    size_t i, shown = 0, upgradable = 0, at_e = 0, at_w = 0;
    char edited[400], withdrawn[400];
    const char *base;
    int rc = 1;

    if (keep_current_setup(a, &ix, &in) != 0) return 1;
    base = target_arch;
    edited[0] = withdrawn[0] = '\0';
    if (a->target != NULL) {
        for (i = 0; i < in.n && strcmp(in.m[i].name, a->target) != 0; i++)
            ;
        if (i == in.n) {
            refuse_n(11, "use-install", "%s is not installed in %s", a->target, a->root);
            goto out;
        }
    }
    kv("result", "shown");
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
        if (s.withdrawn && !s.newer && at_w + 70 < sizeof withdrawn)
            at_w += (size_t)snprintf(withdrawn + at_w, sizeof withdrawn - at_w, "%s%s %s",
                                     at_w ? ", " : "", s.m->name, s.m->version);
        if (machine) {
            char j[300];
            snprintf(j, sizeof j, "%s %s %s %s", s.m->name, s.m->version, avail, s.state);
            rec_item("package", j, "name", s.m->name, "installed", s.m->version,
                     "available", avail, "state", s.state, NULL);
            if (s.withdrawn && s.newer)
                note("%s %s, installed, was withdrawn by its publisher; UPGRADE takes %s",
                     s.m->name, s.m->version, avail);
        } else {
            char what[200];
            if (strcmp(s.state, "upgradable") == 0)
                snprintf(what, sizeof what, "upgradable to %s", avail);
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
            say("%-24s %-12s %s%s%s\n", s.m->name, s.m->version, what,
                strcmp(s.state, "edited") == 0 && s.newer ? avail : "",
                s.withdrawn && s.newer ? " (the installed version was withdrawn)" : "");
        }
    }
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
            say("nothing installed in %s\n", a->root);
        else
            say("%lu package%s in %s, %lu upgradable from %s\n", (unsigned long)shown,
                shown == 1 ? "" : "s", a->root, (unsigned long)upgradable, a->channel);
    }
    if (upgradable > 0)
        hint("UPGRADE ALL ROOT %s CHANNEL %s upgrades every one of them, a package before what "
             "depends on it; with DRYRUN it only says what it would do", a->root, a->channel);
    if (at_e > 0)
        hint("files were edited in %s since install: VERIFY <name> names them. An upgrade that "
             "would replace an edited file is refused; what to do with the edit is the "
             "requester's decision", edited);
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
    int rc = 1, first_class = 0;
    const char *first_next = NULL;
    size_t nrefused = 0, nskipped = 0, at_r = 0;
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
        if (st[i].newer) {
            offered_manifest(a->channel, st[i].offer, &nm[ncand]);
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
                say("%-24s skipped: it needs %s, which could not be upgraded\n", s->m->name, dn);
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
            if (first_class == 0) {
                first_class = refused_class;
                first_next = refused_next;
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
                say("%-24s not upgraded: %s\n", s->m->name, quiet_reason);
            }
            if (at_r + 80 < sizeof refused_names)
                at_r += (size_t)snprintf(refused_names + at_r, sizeof refused_names - at_r, "%s%s (%s)",
                                         at_r ? ", " : "", s->m->name, class_name(refused_class));
            refused_class = 0;
            refused_next = NULL;
            continue;
        }
        if (machine) {
            char jn[300];
            snprintf(jn, sizeof jn, "%s %s %s", s->m->name, s->m->version, s->offer->version);
            rec_item("package", jn, "name", s->m->name, "from", s->m->version,
                     "version", s->offer->version, NULL);
        } else {
            say("%s %s from %s to %s: %lu placed, %lu removed", dryrun ? "would upgrade" : "upgraded",
                s->m->name, s->m->version, s->offer->version, placed, dropped);
            if (kept) say(", %lu kept", kept);
            say("\n");
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
        if (ncand == 0)
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
                     (unsigned long)done, (unsigned long)ncand, ncand == 1 ? "" : "s",
                     refused_names, waiting);
        }
        if (nrefused + nskipped == 0) {
            kv("result", "%s", ncand == 0 ? "unchanged" : res("upgraded", "would-upgrade"));
        } else {
            /* Some needed a decision: the answer is a refusal, with the class
             * of the first, so the exit code and next say what to do. */
            refused_class = first_class;
            refused_next = first_next;
            kv("result", "refused");
            kv("class", "%s", class_name(first_class));
            kv("code", "%d", first_class);
        }
        kv("upgraded", "%lu", (unsigned long)done);
        kv("not-upgraded", "%lu", (unsigned long)(nrefused + nskipped));
        kv("count", "%lu", (unsigned long)done);
        kv("summary", "%s", summary);
        if (nrefused + nskipped > 0)
            kv("next", "%s", first_next ? first_next : next_default(first_class));
        if (!machine)
            say("%s\n", summary);
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
        { "OUT", o->out }, { "ACCEPTKEY", o->acceptkey }, { "UNIT", o->unit },
        { "HANDLER", o->handler }, { "FILES", o->files }, { "BUILD", o->build },
        { "ARCHIVE", o->archive }, { "TO", o->to }, { "PKG_PUSHKEY", o->pushkey },
        { "CONFIG", o->config }, { "UPSTREAM", o->upstream }
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
    rc = options_clean(o != NULL ? o : &none) != 0 ? 1 : fn(o != NULL ? o : &none);
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
int pkg_repair   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "repair", cmd_repair, o); }
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "remove", cmd_remove, o); }
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "image", cmd_image, o); }
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "mountlist", cmd_mountlist, o); }
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "show", cmd_show, o); }
int pkg_status   (const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "status", cmd_status, o); }

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
        return !(n > 7 && strcmp(rel + n - 7, ".pkgidx") == 0) && !(n > 7 && strcmp(rel + n - 7, ".sha256") == 0);
    if (strncmp(rel, "Bootstrap/", 10) == 0)
        return n > 4 && strcmp(rel + n - 4, "/Pkg") == 0;
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

static int cmd_push(const struct pkg_options *a)
{
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
    if (strncmp(a->to, "https://", 8) != 0
        && strncmp(a->to, "http://127.0.0.1", 16) != 0 && strncmp(a->to, "http://localhost", 16) != 0)
        return refuse_c(20, "PUSH sends a key, so only over https (http is accepted to this "
                        "machine alone, for tests): %s", a->to);
    if (a->pushkey == NULL || a->pushkey[0] == '\0')
        return refuse_n(14, "ask-requester", "no portal key: set PKG_PUSHKEY to the key the portal "
                        "gave the publisher. Ask whoever requested this for it; never make one up");
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
    snprintf(line, sizeof line, "Authorization: Bearer %s\n", a->pushkey);
    if (hdr == NULL || pkg_fs_write_private(hdr, line, strlen(line)) != 0) {
        refuse_c(17, "cannot write the key where only this user reads it");
        goto out;
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
    if (pkg_net_send("POST", url, plan, hdr, out, &code, err, sizeof err) != 0) {
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
                if (pkg_net_send("PUT", url, full, hdr, out, &code, err, sizeof err) == 0 && code == 200
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
                    snprintf(ph, sizeof ph, "Authorization: Bearer %s\nContent-Range: bytes %llu-%llu/%llu\n",
                             a->pushkey, off, end2 - 1, size);
                    if (pkg_fs_write_private(hdr, ph, strlen(ph)) != 0) break;
                    if (pkg_net_send("PUT", url, part, hdr, out, &code, err, sizeof err) != 0 || code != 200
                        || answer_field(out, "result", result, sizeof result) == NULL)
                        break;
                    if (strcmp(result, "received") == 0 || strcmp(result, "unchanged") == 0) { ok_file = 1; break; }
                    if (strcmp(result, "partial") != 0 || answer_field(out, "received", rec, sizeof rec) == NULL)
                        break;
                    off = strtoull(rec, NULL, 10);         /* the portal's count, so a resume skips */
                }
                if (src) fclose(src);
                snprintf(line, sizeof line, "Authorization: Bearer %s\n", a->pushkey);
                pkg_fs_write_private(hdr, line, strlen(line));
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
    if (pkg_net_send("POST", url, ix, hdr, out, &code, err, sizeof err) != 0) {
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
                    say("  %s %s\n", k, v);
            }
            p2 = nl ? nl + 1 : end;
        }
        free(ans);
        kv("uploaded", "%lu", (unsigned long)sent);
        kv("uploaded-bytes", "%llu", sent_bytes);
        if (!machine)
            say("%s\n  %lu file%s sent to %s (%llu bytes)\n", summary[0] ? summary : result,
                (unsigned long)sent, sent == 1 ? "" : "s", base, sent_bytes);
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
    if (hdr) pkg_fs_unlink(hdr);
    if (tmp) pkg_fs_rmtree(tmp);
    for (i = 0; i < pl.n; i++) free(pl.rel[i]);
    free(pl.rel);
    free(cache); free(tmp); free(hdr); free(plan); free(out); free(part); free(ix);
    return rc;
}

int pkg_push(const struct pkg_sink *s, const struct pkg_options *o) { return call(s, "push", cmd_push, o); }
