/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Pkg, command line. Keywords follow AmigaDOS usage and are case-insensitive.
 *
 *   pkg KEYGEN   FILE <keyfile>
 *   pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH a] [KIND k]
 *   pkg PUBLISH  <drawer> CHANNEL <dir> [SIGN <keyfile>] [NAME n] [VERSION v] ...
 *   pkg SIGN     <file>   KEY <keyfile> OUT <sigfile>
 *   pkg INSTALL  <name>   ROOT <dir> CHANNEL <dir> [VERSION v] [ACCEPTKEY <hex>]
 *   pkg UPGRADE  <name>   ROOT <dir> CHANNEL <dir> [VERSION v] [DOWNGRADE] [ACCEPTKEY <hex>]
 *   pkg ROLLBACK <name>   ROOT <dir> CHANNEL <dir>
 *   pkg LIST              ROOT <dir>
 *   pkg VERIFY   <name>   ROOT <dir>
 *   pkg REMOVE   <name>   ROOT <dir>
 *
 * A channel is a directory: `index` holds one "name version digest" line per
 * published version, `objects/` holds each payload, its manifest and its
 * signature under the payload's SHA-256.
 *
 * Every package is signed; there is no development mode. The signature covers
 * the manifest, and the manifest names the payload digest, so one signature
 * covers every byte installed. A root pins the key that signed each package the
 * first time it was installed, in `.pkg/keys`, and refuses a different key
 * later unless ACCEPTKEY names the new key in full.
 *
 * Selection: with no VERSION, the highest published version (COMPATIBLE with
 * any). With VERSION, exactly that one (EXACT) or a refusal.
 */

#include "pkg_container.h"
#include "pkg_ed25519.h"
#include "pkg_fs.h"
#include "pkg_manifest.h"
#include "pkg_out.h"
#include "pkg_port.h"
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
    PKGRC_OK         = 0,
    PKGRC_REFUSED    = 10,   /* anything below with no better name */
    PKGRC_NOTFOUND   = 11,   /* package, version, installed entry, channel object absent */
    PKGRC_INTEGRITY  = 12,   /* digest, container, manifest or database disagrees */
    PKGRC_SIGNATURE  = 13,   /* unsigned, malformed or invalid signature */
    PKGRC_KEY        = 14,   /* a key other than the one pinned */
    PKGRC_CONFLICT   = 15,   /* already there: installed, file, edit, published version */
    PKGRC_DEPENDENCY = 16,   /* a dependency cannot be satisfied or is still needed */
    PKGRC_IO         = 17,   /* the filesystem refused a read or a write */
    PKGRC_POLICY     = 18,   /* allowed only with an explicit keyword, such as DOWNGRADE */
    PKGRC_USAGE      = 20    /* the command itself is wrong */
};

static const char *class_name(int c)
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

/* ---- output ----------------------------------------------------------- */

static const char *verb_name = "pkg";
static int refused_class;
static int machine;              /* MACHINE, or PKG_OUTPUT=machine */

/* A result field, in machine mode only: "key: value", the manifest's syntax. */
static void kv(const char *key, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (!machine)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pkg_out("%s: %s\n", key, buf);
}

static int refuse_c(int cls, const char *fmt, ...)
{
    va_list ap;
    if (refused_class == 0)
        refused_class = cls;
    if (machine) {
        char buf[2048], *q;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        for (q = buf; *q; q++)
            if (*q == '\n') *q = ' ';
        pkg_out("result: refused\nclass: %s\ncode: %d\nreason: %s\n",
                class_name(refused_class), refused_class, buf);
        return 1;
    }
    pkg_err("pkg %s: ", verb_name);
    va_start(ap, fmt);
    pkg_verr(fmt, ap);
    va_end(ap);
    pkg_err("\n");
    return 1;
}

#define refuse(...) refuse_c(PKGRC_REFUSED, __VA_ARGS__)

/* A warning is not a failure and leaves the command's class alone. */
static void warn(const char *fmt, ...)
{
    va_list ap;
    if (machine) {
        char buf[1024];
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        pkg_out("warning: %s\n", buf);
        return;
    }
    pkg_err("pkg %s: warning: ", verb_name);
    va_start(ap, fmt);
    pkg_verr(fmt, ap);
    va_end(ap);
    pkg_err("\n");
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

/* ---- arguments -------------------------------------------------------- */

struct args {
    const char *pos;
    const char *root, *channel, *name, *version, *arch, *kind;
    const char *file, *sign, *key, *out, *acceptkey;
    int downgrade;
};

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static const struct { const char *kw; size_t off; } kws[] = {
        { "ROOT",      offsetof(struct args, root) },
        { "CHANNEL",   offsetof(struct args, channel) },
        { "NAME",      offsetof(struct args, name) },
        { "VERSION",   offsetof(struct args, version) },
        { "ARCH",      offsetof(struct args, arch) },
        { "KIND",      offsetof(struct args, kind) },
        { "FILE",      offsetof(struct args, file) },
        { "SIGN",      offsetof(struct args, sign) },
        { "KEY",       offsetof(struct args, key) },
        { "OUT",       offsetof(struct args, out) },
        { "ACCEPTKEY", offsetof(struct args, acceptkey) }
};

static int takes_value(const char *w)
{
    size_t k;
    for (k = 0; k < sizeof kws / sizeof kws[0]; k++)
        if (ieq(w, kws[k].kw))
            return 1;
    return 0;
}

/* MACHINE anywhere a switch can stand, the value of a keyword excepted:
 * `NAME machine` names a package. Read before parsing, so that a usage error
 * found earlier on the line still answers in the contract asked for. */
static int wants_machine(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (takes_value(argv[i]))
            i++;
        else if (ieq(argv[i], "MACHINE"))
            return 1;
    }
    return 0;
}

static int parse_args(int argc, char **argv, struct args *a)
{
    int i;
    size_t k;

    memset(a, 0, sizeof *a);
    for (i = 2; i < argc; i++) {
        const char **slot = NULL;
        if (ieq(argv[i], "DOWNGRADE")) {
            a->downgrade = 1;
            continue;
        }
        if (ieq(argv[i], "MACHINE")) {
            machine = 1;
            continue;
        }
        for (k = 0; k < sizeof kws / sizeof kws[0]; k++)
            if (ieq(argv[i], kws[k].kw))
                slot = (const char **)(void *)((char *)a + kws[k].off);
        if (slot != NULL) {
            if (i + 1 >= argc)
                return refuse_c(20, "%s needs a value", argv[i]), -1;
            *slot = argv[++i];
        } else if (a->pos == NULL) {
            a->pos = argv[i];
        } else {
            return refuse_c(20, "unexpected argument \"%s\"", argv[i]), -1;
        }
    }
    if (a->sign == NULL)
        a->sign = getenv("PKG_SIGNKEY");
    return 0;
}

/* ---- keys ------------------------------------------------------------- */

struct key {
    unsigned char pk[PKG_ED25519_PUBLIC];
    unsigned char sk[PKG_ED25519_SECRET];
    char          pkhex[2 * PKG_ED25519_PUBLIC + 1];
};

static int cmd_keygen(const struct args *a)
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
        pkg_out("key written to %s, readable by you alone\npublic key %s\n", a->file, k.pkhex);
    return 0;
}

static int load_key(const char *path, struct key *k)
{
    unsigned char *buf, seed[PKG_ED25519_SEED], pk[PKG_ED25519_PUBLIC];
    size_t len;
    char seedhex[65], pubhex[65];

    if (path == NULL)
        return refuse_c(20, "no signing key: give SIGN <keyfile>, or set PKG_SIGNKEY. "
                      "Every package is signed; create a key with KEYGEN FILE <path>");
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

static int cmd_sign(const struct args *a)
{
    struct key k;
    unsigned char *buf;
    size_t len;
    if (a->pos == NULL || a->key == NULL || a->out == NULL)
        return refuse_c(20, "usage: SIGN <file> KEY <keyfile> OUT <sigfile>");
    if (load_key(a->key, &k) != 0)
        return 1;
    if (pkg_fs_read(a->pos, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->pos);
    if (write_sig(a->out, &k, buf, len) != 0) {
        free(buf);
        return refuse_c(17, "cannot write \"%s\"", a->out);
    }
    free(buf);
    kv("result", "signed");
    kv("file", "%s", a->pos);
    kv("signer", "%s", k.pkhex);
    if (!machine)
        pkg_out("signed %s with %.16s\n", a->pos, k.pkhex);
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
    char           err[512];
};

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
}

/* Find "$VER: name version" in the files, first file in path order wins. */
static int find_ver(const struct drawer *d, char *name, size_t nl,
                    char *ver, size_t vl, const char **from)
{
    size_t i, j;
    for (i = 0; i < d->n; i++) {
        const unsigned char *p = d->v[i].data;
        size_t len = d->v[i].len;
        for (j = 0; j + 6 < len; j++) {
            size_t k = j + 6, a = 0, b = 0;
            if (memcmp(p + j, "$VER: ", 6) != 0)
                continue;
            while (k < len && p[k] > ' ' && p[k] < 0x7F && a + 1 < nl)
                name[a++] = (char)tolower(p[k++]);
            name[a] = '\0';
            while (k < len && p[k] == ' ') k++;
            while (k < len && ((p[k] >= '0' && p[k] <= '9') || p[k] == '.') && b + 1 < vl)
                ver[b++] = (char)p[k++];
            while (b > 0 && ver[b - 1] == '.') b--;
            ver[b] = '\0';
            if (a > 0 && b > 0 && pkg_check_name(name) == NULL
                && pkg_check_version(ver) == NULL) {
                *from = d->v[i].rel;
                return 1;
            }
        }
    }
    return 0;
}

struct built {
    struct pkg_manifest m;
    unsigned char *pkg;
    size_t         pkg_len;
    char          *text;
    size_t         text_len;
    char           ver_from[512];
    unsigned       skipped;
};

static void built_free(struct built *b)
{
    pkg_manifest_free(&b->m);
    free(b->pkg);
    free(b->text);
}

static int build(const struct args *a, struct built *out)
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
    if (a->pos == NULL)
        return refuse_c(20, "name the drawer to package");
    if (!pkg_fs_is_dir(a->pos))
        return refuse_c(20, "\"%s\" is not a directory", a->pos);

    d.root = a->pos;
    if (pkg_fs_walk(a->pos, load_one, &d, &out->skipped, d.err, sizeof d.err) != 0) {
        refuse_c(20, "%s", d.err[0] ? d.err : "cannot read the drawer");
        drawer_free(&d);
        return 1;
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse_c(20, "\"%s\" holds no files", a->pos);
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);

    name = a->name;
    version = a->version;
    if ((name == NULL || version == NULL)
        && find_ver(&d, vname, sizeof vname, vver, sizeof vver, &from)) {
        if (name == NULL) name = vname;
        if (version == NULL) version = vver;
        snprintf(out->ver_from, sizeof out->ver_from, "%s", from);
    }
    arch = a->arch ? a->arch : "generic";
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

    w = pkg_writer_new();
    if (w == NULL) { drawer_free(&d); return refuse("out of memory"); }
    pkg_manifest_set(&out->m.name, name);
    pkg_manifest_set(&out->m.version, version);
    pkg_manifest_set(&out->m.architecture, arch);
    pkg_manifest_set(&out->m.kind, kind);
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
    char digest[PKG_SHA256_HEXLEN + 1];
};

struct index {
    struct entry *e;
    size_t        n;
};

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
        if (sscanf(tmp, "%64s %63s %64s", en.name, en.version, en.digest) != 3
            || pkg_check_name(en.name) || pkg_check_version(en.version)
            || strlen(en.digest) != PKG_SHA256_HEXLEN) {
            free(buf); free(ix->e);
            return refuse_c(12, "the channel index is malformed at line %u", line);
        }
        w = (struct entry *)realloc(ix->e, (ix->n + 1u) * sizeof *w);
        if (w == NULL) { free(buf); return refuse("out of memory"); }
        ix->e = w;
        ix->e[ix->n++] = en;
    }
    free(buf);
    return 0;
}

static int by_entry(const void *a, const void *b)
{
    const struct entry *x = (const struct entry *)a, *y = (const struct entry *)b;
    int c = strcmp(x->name, y->name);
    return c ? c : pkg_version_cmp(x->version, y->version);
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
        len += (size_t)snprintf(buf + len, cap - len, "%s %s %s\n",
                                ix->e[i].name, ix->e[i].version, ix->e[i].digest);
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

/* Pick the entry for name: EXACT when version is given, else the highest. */
static const struct entry *pick(const struct index *ix, const char *name, const char *version)
{
    const struct entry *p = NULL;
    size_t i;
    for (i = 0; i < ix->n; i++) {
        if (strcmp(ix->e[i].name, name) != 0)
            continue;
        if (version) {
            if (pkg_version_cmp(ix->e[i].version, version) == 0)
                p = &ix->e[i];
        } else if (p == NULL || pkg_version_cmp(ix->e[i].version, p->version) > 0) {
            p = &ix->e[i];
        }
    }
    return p;
}

static void say_not_found(const struct index *ix, const char *name, const char *version,
                          const char *channel)
{
    size_t i, at = 0;
    char offered[512];
    offered[0] = '\0';
    for (i = 0; i < ix->n && at + 72 < sizeof offered; i++)
        if (strcmp(ix->e[i].name, name) == 0)
            at += (size_t)snprintf(offered + at, sizeof offered - at, "%s%s",
                                   at ? ", " : "; versions offered: ", ix->e[i].version);
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
    /* 2. Signed. */
    if (check_sig(so, f->mtext, f->mlen, f->signer, what) != 0)
        goto out;
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
        return quiet ? 1 : refuse_c(11, "%s is not installed in %s", name, root);
    }
    free(p);
    rc = pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
    free(buf);
    if (rc != 0)
        return refuse_c(12, "the database entry for %s is damaged: %s", name, err);
    return 0;
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
        return 0;                     /* first use: pinned after a successful apply */
    }
    free(p);
    if (len < 64u) { free(buf); return refuse_c(12, "the pinned key for %s is damaged", name); }
    memcpy(pinned, buf, 64);
    pinned[64] = '\0';
    free(buf);
    if (strcmp(pinned, signer) == 0)
        return 0;
    if (acceptkey != NULL && strcmp(acceptkey, signer) == 0) {
        kv("key-changed", "%s %s", pinned, signer);
        if (!machine)
        pkg_out("  key for %s changed by explicit ACCEPTKEY\n    was %s\n    now %s\n",
               name, pinned, signer);
        return 0;
    }
    return refuse_c(14, "%s is signed by a different key from the one pinned in %s.\n"
                  "  pinned %s\n  signer %s\n"
                  "Nothing was changed. If the publisher really changed keys, accept the new "
                  "one explicitly with ACCEPTKEY %s", name, root, pinned, signer, signer);
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
                              "of %s; nothing was changed", m->files[i].path, root, m->name);
        } else if (file_state(root, of->path, of->digest, of->size) == 1) {
            return refuse_c(15, "\"%s\" was edited since %s %s was installed, and %s %s ships it too; "
                          "nothing was changed. Save your edit elsewhere, then REMOVE or "
                          "restore the file", of->path, old->name, old->version, m->name, m->version);
        }
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
                else pkg_out("  kept     %s (edited, and no longer part of %s)\n", of->path, m->name);
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

/* ---- verbs ------------------------------------------------------------ */

static int cmd_manifest(const struct args *a)
{
    struct built b;
    if (build(a, &b) != 0) { built_free(&b); return 1; }
    pkg_outraw(b.text, b.text_len);
    built_free(&b);
    return 0;
}

static int cmd_publish(const struct args *a)
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
            && pkg_version_cmp(ix.e[i].version, b.m.version) == 0) {
            if (strcmp(ix.e[i].digest, mdigest) == 0) {
                kv("result", "unchanged");
                kv("name", "%s", b.m.name);
                kv("version", "%s", b.m.version);
                if (!machine)
                pkg_out("%s %s is already published with this exact content; nothing to do\n",
                       b.m.name, b.m.version);
                rc = 0;
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
    pkg_out("published %s %s to %s: %lu files, payload %s, signed by %.16s\n", b.m.name,
           b.m.version, a->channel, (unsigned long)b.m.nfiles, s12, k.pkhex);
    if (b.ver_from[0] && machine)
        kv("version-from", "%s", b.ver_from);
    else if (b.ver_from[0])
        pkg_out("  name and version taken from $VER: in %s\n", b.ver_from);
    if (b.skipped && machine)
        kv("skipped", "%u", b.skipped);
    else if (b.skipped)
        pkg_out("  skipped %u host metadata file%s (.DS_Store, ._*)\n",
               b.skipped, b.skipped == 1u ? "" : "s");
    rc = 0;
out:
    memset(&k, 0, sizeof k);
    free(po); free(mo); free(so);
    free(ix.e);
    built_free(&b);
    return rc;
}

static int need_root_channel(const struct args *a)
{
    if (a->pos == NULL)     return refuse_c(20, "name the package");
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->channel == NULL) return refuse_c(20, "name the channel with CHANNEL <dir>");
    if (a->version && pkg_check_version(a->version))
        return refuse_c(20, "VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    return 0;
}

static int cmd_install(const struct args *a)
{
    struct index ix;
    const struct entry *e;
    struct fetched f;
    struct pkg_manifest cur;
    unsigned long placed, dropped, kept;
    char s12[13];
    int rc = 1;

    if (need_root_channel(a) != 0) return 1;
    if (read_index(a->channel, &ix) != 0) return 1;
    e = pick(&ix, a->pos, a->version);
    if (e == NULL) { say_not_found(&ix, a->pos, a->version, a->channel); free(ix.e); return 1; }
    if (load_installed(a->root, e->name, &cur, 1) == 0) {
        refuse_c(15, "%s %s is already installed in %s; use UPGRADE, or REMOVE it first",
               cur.name, cur.version, a->root);
        pkg_manifest_free(&cur);
        free(ix.e);
        return 1;
    }
    if (fetch(a->channel, e, &f) == 0
        && check_pin(a->root, e->name, f.signer, a->acceptkey) == 0
        && apply(a->root, NULL, &f, &placed, &dropped, &kept) == 0) {
        short12(f.m.payload, s12);
        kv("result", "installed");
        kv("name", "%s", f.m.name);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("files", "%lu", placed);
        kv("payload", "%s", f.m.payload);
        kv("signer", "%s", f.signer);
        if (!machine)
        pkg_out("installed %s %s into %s: %lu files, payload %s, signed by %.16s\n",
               f.m.name, f.m.version, a->root, placed, s12, f.signer);
        rc = 0;
    }
    fetched_free(&f);
    free(ix.e);
    return rc;
}

static int move_to(const struct args *a, const struct entry *e, struct pkg_manifest *cur,
                   const char *verb)
{
    struct fetched f;
    unsigned long placed, dropped, kept;
    int rc = 1;

    if (fetch(a->channel, e, &f) == 0
        && check_pin(a->root, e->name, f.signer, a->acceptkey) == 0
        && apply(a->root, cur, &f, &placed, &dropped, &kept) == 0) {
        kv("result", "%s", verb[0] == 'u' ? "upgraded" : verb[0] == 'd' ? "downgraded" : "rolled-back");
        kv("name", "%s", f.m.name);
        kv("from", "%s", cur->version);
        kv("version", "%s", f.m.version);
        kv("root", "%s", a->root);
        kv("placed", "%lu", placed);
        kv("removed", "%lu", dropped);
        kv("signer", "%s", f.signer);
        if (!machine) {
        pkg_out("%s %s from %s to %s in %s: %lu placed, %lu removed", verb, f.m.name,
               cur->version, f.m.version, a->root, placed, dropped);
        if (kept) pkg_out(", %lu kept", kept);
        pkg_out("\n");
        }
        rc = 0;
    }
    fetched_free(&f);
    return rc;
}

static int cmd_upgrade(const struct args *a)
{
    struct index ix;
    const struct entry *e;
    struct pkg_manifest cur;
    int rc = 1, c;

    if (need_root_channel(a) != 0) return 1;
    if (load_installed(a->root, a->pos, &cur, 0) != 0) return 1;
    if (read_index(a->channel, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    e = pick(&ix, a->pos, a->version);
    if (e == NULL) {
        say_not_found(&ix, a->pos, a->version, a->channel);
        goto out;
    }
    c = pkg_version_cmp(e->version, cur.version);
    if (c == 0) {
        kv("result", "unchanged");
        kv("name", "%s", cur.name);
        kv("version", "%s", cur.version);
        if (!machine)
            pkg_out("%s is already at %s\n", cur.name, cur.version);
        rc = 0;
        goto out;
    }
    if (c < 0 && !a->downgrade) {
        refuse_c(18, "%s %s is older than the installed %s; nothing was changed. Use ROLLBACK to "
               "return to the previous version, or add DOWNGRADE to move to this one",
               e->name, e->version, cur.version);
        goto out;
    }
    rc = move_to(a, e, &cur, c < 0 ? "downgraded" : "upgraded");
out:
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

static int cmd_rollback(const struct args *a)
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
    if (load_installed(a->root, a->pos, &cur, 0) != 0) return 1;
    pp = root_path(a->root, "prev", a->pos);
    if (pp == NULL || pkg_fs_read(pp, &buf, &len) != 0) {
        free(pp);
        pkg_manifest_free(&cur);
        return refuse_c(11, "%s %s has no previous version recorded in %s; nothing to roll back to",
                      a->pos, cur.version, a->root);
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
            return refuse_c(12, "the rollback record for %s is damaged", a->pos);
        }
    }
    if (read_index(a->channel, &ix) != 0) { pkg_manifest_free(&cur); return 1; }
    for (i = 0; i < ix.n; i++)
        if (strcmp(ix.e[i].name, a->pos) == 0 && pkg_version_cmp(ix.e[i].version, prev.version) == 0)
            e = &ix.e[i];
    if (e == NULL)
        refuse_c(11, "the previous version of %s, %s, is no longer in the channel %s",
               a->pos, prev.version, a->channel);
    else
        rc = move_to(a, e, &cur, "rolled back");
    pkg_manifest_free(&cur);
    free(ix.e);
    return rc;
}

static int cmd_list(const struct args *a)
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
            if (machine)
                kv("package", "%s %s %s %lu", m.name, m.version, m.kind, (unsigned long)m.nfiles);
            else
                pkg_out("%-24s %-10s %-12s %lu files\n", m.name, m.version, m.kind,
                        (unsigned long)m.nfiles);
            pkg_manifest_free(&m);
        }
        free(names[i]);
    }
    free(names);
    if (n == 0 && !machine)
        pkg_out("nothing installed in %s\n", a->root);
    kv("count", "%lu", (unsigned long)n);
    return 0;
}

static int cmd_verify(const struct args *a)
{
    struct pkg_manifest m;
    size_t i, changed = 0, missing = 0;

    if (a->pos == NULL)  return refuse_c(20, "name the package to verify");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (load_installed(a->root, a->pos, &m, 0) != 0) return 1;
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("files", "%lu", (unsigned long)m.nfiles);
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 1) {
            changed++;
            if (machine) kv("changed", "%s", m.files[i].path);
            else pkg_out("  changed  %s\n", m.files[i].path);
        }
        if (s == 2) {
            missing++;
            if (machine) kv("missing", "%s", m.files[i].path);
            else pkg_out("  missing  %s\n", m.files[i].path);
        }
    }
    if (changed + missing == 0) {
        kv("result", "intact");
        if (!machine)
            pkg_out("%s %s: %lu files, all intact\n", m.name, m.version, (unsigned long)m.nfiles);
        pkg_manifest_free(&m);
        return 0;
    }
    if (!machine)
        pkg_out("%s %s: %lu changed, %lu missing, of %lu files\n", m.name, m.version,
                (unsigned long)changed, (unsigned long)missing, (unsigned long)m.nfiles);
    refused_class = PKGRC_INTEGRITY;
    kv("result", "damaged");
    kv("class", "integrity");
    kv("code", "%d", PKGRC_INTEGRITY);
    pkg_manifest_free(&m);
    return 1;
}

static int cmd_remove(const struct args *a)
{
    struct pkg_manifest m;
    size_t i, removed = 0, kept = 0, gone = 0;
    char *dbp, *pp;

    if (a->pos == NULL)  return refuse_c(20, "name the package to remove");
    if (a->root == NULL) return refuse_c(20, "name the root with ROOT <dir>");
    if (load_installed(a->root, a->pos, &m, 0) != 0) return 1;
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, m.files[i].path, m.files[i].digest, m.files[i].size);
        if (s == 0) {
            char *p = pkg_join(a->root, m.files[i].path);
            if (p != NULL && pkg_fs_unlink(p) == 0) {
                removed++;
                pkg_fs_prune_empty_parents(a->root, m.files[i].path);
            }
            free(p);
        } else if (s == 1) {
            kept++;
            if (machine) kv("kept", "%s", m.files[i].path);
            else pkg_out("  kept     %s (changed since install, so it is yours now)\n", m.files[i].path);
        } else {
            gone++;
        }
    }
    dbp = root_path(a->root, "db", m.name);
    if (dbp == NULL || pkg_fs_unlink(dbp) != 0) {
        free(dbp);
        pkg_manifest_free(&m);
        return refuse_c(17, "the files are removed but the database entry could not be: %s",
                      strerror(errno));
    }
    free(dbp);
    pp = root_path(a->root, "prev", m.name);
    if (pp != NULL) pkg_fs_unlink(pp);
    free(pp);
    /* The pinned key stays: reinstalling the package later is still held to it. */
    kv("result", "removed");
    kv("name", "%s", m.name);
    kv("version", "%s", m.version);
    kv("root", "%s", a->root);
    kv("removed", "%lu", (unsigned long)removed);
    kv("gone", "%lu", (unsigned long)gone);
    if (!machine) {
    pkg_out("removed %s %s from %s: %lu files removed", m.name, m.version, a->root,
           (unsigned long)removed);
    if (kept) pkg_out(", %lu kept", (unsigned long)kept);
    if (gone) pkg_out(", %lu already gone", (unsigned long)gone);
    pkg_out("\n");
    }
    pkg_manifest_free(&m);
    return 0;
}

static int usage(void)
{
    pkg_err("usage:\n"
        "  pkg KEYGEN   FILE <keyfile>\n"
        "  pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH a] [KIND k]\n"
        "  pkg PUBLISH  <drawer> CHANNEL <dir> [SIGN <keyfile>] [NAME n] [VERSION v] [ARCH a] [KIND k]\n"
        "  pkg SIGN     <file> KEY <keyfile> OUT <sigfile>\n"
        "  pkg INSTALL  <name> ROOT <dir> CHANNEL <dir> [VERSION v] [ACCEPTKEY <hex>]\n"
        "  pkg UPGRADE  <name> ROOT <dir> CHANNEL <dir> [VERSION v] [DOWNGRADE] [ACCEPTKEY <hex>]\n"
        "  pkg ROLLBACK <name> ROOT <dir> CHANNEL <dir>\n"
        "  pkg LIST     ROOT <dir>\n"
        "  pkg VERIFY   <name> ROOT <dir>\n"
        "  pkg REMOVE   <name> ROOT <dir>\n"
        "  pkg PORT     [<portname>]      (AROS: serve these verbs on an ARexx port, PKG by default)\n"
        "SIGN defaults to $PKG_SIGNKEY. Any verb takes MACHINE, or PKG_OUTPUT=machine:\n"
        "key: value lines, and the exit code names the class of a refusal.\n");
    return PKGRC_USAGE;
}

/* One entry for every caller: the command line below, and the ARexx port,
 * which runs each command it receives through here. Returns the exit code:
 * 0, a refusal class from 10 to 18, or 20 for usage. */
static int run_verb(int argc, char **argv)
{
    static const struct { const char *verb; const char *name; int (*fn)(const struct args *); } verbs[] = {
        { "KEYGEN",   "keygen",   cmd_keygen },
        { "MANIFEST", "manifest", cmd_manifest },
        { "PUBLISH",  "publish",  cmd_publish },
        { "SIGN",     "sign",     cmd_sign },
        { "INSTALL",  "install",  cmd_install },
        { "UPGRADE",  "upgrade",  cmd_upgrade },
        { "ROLLBACK", "rollback", cmd_rollback },
        { "LIST",     "list",     cmd_list },
        { "VERIFY",   "verify",   cmd_verify },
        { "REMOVE",   "remove",   cmd_remove }
    };
    struct args a;
    size_t i;
    const char *saved = verb_name;
    int saved_machine = machine;
    int rc = PKGRC_USAGE;
    const char *env = getenv("PKG_OUTPUT");

    refused_class = 0;
    machine = env != NULL && ieq(env, "machine");
    if (wants_machine(argc, argv))
        machine = 1;
    if (argc < 2) {
        if (machine) refuse_c(PKGRC_USAGE, "no verb given");
        else usage();
        machine = saved_machine;
        return PKGRC_USAGE;
    }
    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (ieq(argv[1], verbs[i].verb)) {
            verb_name = verbs[i].name;
            if (parse_args(argc, argv, &a) != 0)
                rc = PKGRC_USAGE;
            else if (verbs[i].fn(&a) == 0)
                rc = PKGRC_OK;
            else
                rc = refused_class ? refused_class : PKGRC_REFUSED;
            verb_name = saved;
            machine = saved_machine;
            return rc;
        }
    }
    if (machine) refuse_c(PKGRC_USAGE, "unknown verb \"%s\"", argv[1]);
    else usage();
    machine = saved_machine;
    return PKGRC_USAGE;
}

int main(int argc, char **argv)
{
    int rc;

    /* PORT is not a verb the port itself may run, so it is handled here. */
    if (argc >= 2 && ieq(argv[1], "PORT")) {
        verb_name = "port";
        if (argc > 3) {
            usage();
            return PKGRC_USAGE;
        }
        rc = pkg_port_serve(argc == 3 ? argv[2] : "PKG", run_verb);
        return rc == 0 ? PKGRC_OK : PKGRC_REFUSED;
    }
    return run_verb(argc, argv);
}
