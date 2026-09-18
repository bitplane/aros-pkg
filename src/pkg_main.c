/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Pkg, command line. Keywords follow AmigaDOS usage and are case-insensitive:
 *
 *   pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH a] [KIND k]
 *   pkg PUBLISH  <drawer> CHANNEL <dir> [NAME n] [VERSION v] [ARCH a] [KIND k]
 *   pkg INSTALL  <name>   ROOT <dir> CHANNEL <dir> [VERSION v]
 *   pkg LIST              ROOT <dir>
 *   pkg VERIFY   <name>   ROOT <dir>
 *   pkg REMOVE   <name>   ROOT <dir>
 *
 * A channel is a directory: an `index` file, one "name version digest" line
 * per published version, and `objects/<digest>.pkg` plus
 * `objects/<digest>.manifest`. A root holds its own database in `.pkg/db`.
 */

#include "pkg_container.h"
#include "pkg_fs.h"
#include "pkg_manifest.h"
#include "pkg_sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- output ----------------------------------------------------------- */

static const char *verb_name = "pkg";

static int refuse(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "pkg %s: ", verb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 1;
}

static void short12(const char *hex, char out[13])
{
    memcpy(out, hex, 12);
    out[12] = '\0';
}

/* ---- arguments -------------------------------------------------------- */

struct args {
    const char *pos;
    const char *root, *channel, *name, *version, *arch, *kind;
};

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int parse_args(int argc, char **argv, struct args *a)
{
    int i;
    memset(a, 0, sizeof *a);
    for (i = 2; i < argc; i++) {
        const char **slot = NULL;
        if (ieq(argv[i], "ROOT")) slot = &a->root;
        else if (ieq(argv[i], "CHANNEL")) slot = &a->channel;
        else if (ieq(argv[i], "NAME")) slot = &a->name;
        else if (ieq(argv[i], "VERSION")) slot = &a->version;
        else if (ieq(argv[i], "ARCH")) slot = &a->arch;
        else if (ieq(argv[i], "KIND")) slot = &a->kind;
        if (slot != NULL) {
            if (i + 1 >= argc)
                return refuse("%s needs a value", argv[i]), -1;
            *slot = argv[++i];
        } else if (a->pos == NULL) {
            a->pos = argv[i];
        } else {
            return refuse("unexpected argument \"%s\"", argv[i]), -1;
        }
    }
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
        return refuse("name the drawer to package");
    if (!pkg_fs_is_dir(a->pos))
        return refuse("\"%s\" is not a directory", a->pos);

    d.root = a->pos;
    if (pkg_fs_walk(a->pos, load_one, &d, &out->skipped, d.err, sizeof d.err) != 0) {
        refuse("%s", d.err[0] ? d.err : "cannot read the drawer");
        drawer_free(&d);
        return 1;
    }
    if (d.n == 0) {
        drawer_free(&d);
        return refuse("\"%s\" holds no files", a->pos);
    }
    qsort(d.v, d.n, sizeof d.v[0], by_rel);

    name = a->name;
    version = a->version;
    if (name == NULL || version == NULL) {
        if (find_ver(&d, vname, sizeof vname, vver, sizeof vver, &from)) {
            if (name == NULL) name = vname;
            if (version == NULL) version = vver;
            snprintf(out->ver_from, sizeof out->ver_from, "%s", from);
        }
    }
    arch = a->arch ? a->arch : "generic";
    kind = a->kind ? a->kind : "application";
    if (name == NULL) {
        drawer_free(&d);
        return refuse("no NAME given and no $VER: cookie found; add NAME <name>");
    }
    if (version == NULL) {
        drawer_free(&d);
        return refuse("no VERSION given and no $VER: cookie found; add VERSION <version>");
    }
    if ((why = pkg_check_name(name)) || (why = pkg_check_version(version))
        || (why = pkg_check_arch(arch)) || (why = pkg_check_kind(kind))) {
        drawer_free(&d);
        return refuse("%s", why);
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
        return refuse("cannot assemble the container");
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
    if (pkg_fs_read(path, &buf, &len) != 0) {
        free(path);
        if (errno == ENOENT)
            return 0;
        return refuse("cannot read the channel index");
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
        if (ll >= sizeof tmp || nl == NULL
            || sscanf((memcpy(tmp, ls, ll), tmp[ll] = '\0', tmp), "%64s %63s %64s",
                      en.name, en.version, en.digest) != 3
            || pkg_check_name(en.name) || pkg_check_version(en.version)
            || strlen(en.digest) != PKG_SHA256_HEXLEN) {
            free(buf);
            free(ix->e);
            return refuse("the channel index is malformed at line %u", line);
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

/* ---- verbs ------------------------------------------------------------ */

static int cmd_manifest(const struct args *a)
{
    struct built b;
    if (build(a, &b) != 0) { built_free(&b); return 1; }
    fwrite(b.text, 1, b.text_len, stdout);
    built_free(&b);
    return 0;
}

static int cmd_publish(const struct args *a)
{
    struct built b;
    struct index ix;
    size_t i;
    char *po, *mo, s12[13];
    int rc = 1;

    if (a->channel == NULL)
        return refuse("name the channel with CHANNEL <dir>");
    if (build(a, &b) != 0) { built_free(&b); return 1; }
    if (read_index(a->channel, &ix) != 0) { built_free(&b); return 1; }

    for (i = 0; i < ix.n; i++) {
        if (strcmp(ix.e[i].name, b.m.name) == 0
            && pkg_version_cmp(ix.e[i].version, b.m.version) == 0) {
            if (strcmp(ix.e[i].digest, b.m.payload) == 0) {
                printf("%s %s is already published with this exact payload; nothing to do\n",
                       b.m.name, b.m.version);
                rc = 0;
            } else {
                refuse("%s %s is already published with a different payload; a published "
                       "version never changes, so publish this as a new version",
                       b.m.name, ix.e[i].version);
            }
            free(ix.e);
            built_free(&b);
            return rc;
        }
    }

    po = object_path(a->channel, b.m.payload, "pkg");
    mo = object_path(a->channel, b.m.payload, "manifest");
    if (po == NULL || mo == NULL
        || pkg_fs_write_atomic(po, b.pkg, b.pkg_len) != 0
        || pkg_fs_write_atomic(mo, b.text, b.text_len) != 0) {
        refuse("cannot write into the channel \"%s\": %s", a->channel, strerror(errno));
        goto out;
    }
    {
        struct entry *w = (struct entry *)realloc(ix.e, (ix.n + 1u) * sizeof *w);
        if (w == NULL) { refuse("out of memory"); goto out; }
        ix.e = w;
        snprintf(ix.e[ix.n].name, sizeof ix.e[ix.n].name, "%s", b.m.name);
        snprintf(ix.e[ix.n].version, sizeof ix.e[ix.n].version, "%s", b.m.version);
        snprintf(ix.e[ix.n].digest, sizeof ix.e[ix.n].digest, "%s", b.m.payload);
        ix.n++;
    }
    if (write_index(a->channel, &ix) != 0) {
        refuse("cannot write the channel index: %s", strerror(errno));
        goto out;
    }
    short12(b.m.payload, s12);
    printf("published %s %s to %s: %lu files, payload %s\n", b.m.name, b.m.version,
           a->channel, (unsigned long)b.m.nfiles, s12);
    if (b.ver_from[0])
        printf("  name and version taken from $VER: in %s\n", b.ver_from);
    if (b.skipped)
        printf("  skipped %u host metadata file%s (.DS_Store, ._*)\n",
               b.skipped, b.skipped == 1u ? "" : "s");
    rc = 0;
out:
    free(po);
    free(mo);
    free(ix.e);
    built_free(&b);
    return rc;
}

/* The database of a root. */
static char *db_path(const char *root, const char *name)
{
    char rel[128];
    snprintf(rel, sizeof rel, ".pkg/db/%s", name);
    return pkg_join(root, rel);
}

static int load_installed(const char *root, const char *name, struct pkg_manifest *m)
{
    char *p = db_path(root, name), err[300];
    unsigned char *buf;
    size_t len;
    int rc;

    if (p == NULL)
        return refuse("out of memory");
    if (pkg_fs_read(p, &buf, &len) != 0) {
        free(p);
        return refuse("%s is not installed in %s", name, root);
    }
    free(p);
    rc = pkg_manifest_parse((const char *)buf, len, m, err, sizeof err);
    free(buf);
    if (rc != 0)
        return refuse("the database entry for %s is damaged: %s", name, err);
    return 0;
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

static int cmd_install(const struct args *a)
{
    struct index ix;
    const struct entry *pick = NULL;
    struct pkg_manifest m;
    unsigned char *pkg = NULL, *mtext = NULL;
    size_t pkg_len = 0, mlen = 0, i;
    char hex[PKG_SHA256_HEXLEN + 1], err[300], s12[13];
    char *po = NULL, *mo = NULL, *dbp = NULL, *staging = NULL;
    struct walk_ctx wc;
    struct stage_ctx sc;
    enum pkg_status st;
    int rc = 1, stopped;
    unsigned long long bytes = 0;

    pkg_manifest_init(&m);
    if (a->pos == NULL)   return refuse("name the package to install");
    if (a->root == NULL)  return refuse("name the root with ROOT <dir>");
    if (a->channel == NULL) return refuse("name the channel with CHANNEL <dir>");
    if (a->version && pkg_check_version(a->version))
        return refuse("VERSION \"%s\": %s", a->version, pkg_check_version(a->version));
    if (read_index(a->channel, &ix) != 0) return 1;

    for (i = 0; i < ix.n; i++) {
        if (strcmp(ix.e[i].name, a->pos) != 0)
            continue;
        if (a->version) {
            if (pkg_version_cmp(ix.e[i].version, a->version) == 0)
                pick = &ix.e[i];
        } else if (pick == NULL || pkg_version_cmp(ix.e[i].version, pick->version) > 0) {
            pick = &ix.e[i];
        }
    }
    if (pick == NULL) {
        int any = 0;
        fprintf(stderr, "pkg install: %s%s%s is not in the channel %s",
                a->pos, a->version ? " " : "", a->version ? a->version : "", a->channel);
        for (i = 0; i < ix.n; i++)
            if (strcmp(ix.e[i].name, a->pos) == 0) {
                fprintf(stderr, "%s%s", any ? ", " : "; versions offered: ", ix.e[i].version);
                any = 1;
            }
        fputc('\n', stderr);
        free(ix.e);
        return 1;
    }

    dbp = db_path(a->root, pick->name);
    if (dbp == NULL) { refuse("out of memory"); goto out; }
    if (pkg_fs_exists(dbp)) {
        refuse("%s is already installed in %s; remove it first", pick->name, a->root);
        goto out;
    }

    po = object_path(a->channel, pick->digest, "pkg");
    mo = object_path(a->channel, pick->digest, "manifest");
    if (po == NULL || mo == NULL) { refuse("out of memory"); goto out; }
    if (pkg_fs_read(po, &pkg, &pkg_len) != 0) {
        refuse("the channel lists %s %s but its payload is missing", pick->name, pick->version);
        goto out;
    }
    pkg_sha256_hex(pkg, pkg_len, hex);
    if (strcmp(hex, pick->digest) != 0) {
        refuse("the payload of %s %s does not match the channel index; expected %s, found %s. "
               "Nothing was installed",
               pick->name, pick->version, pick->digest, hex);
        goto out;
    }
    if (pkg_fs_read(mo, &mtext, &mlen) != 0) {
        refuse("the channel lists %s %s but its manifest is missing", pick->name, pick->version);
        goto out;
    }
    if (pkg_manifest_parse((const char *)mtext, mlen, &m, err, sizeof err) != 0) {
        refuse("the manifest of %s %s is refused: %s", pick->name, pick->version, err);
        goto out;
    }
    if (strcmp(m.name, pick->name) != 0 || pkg_version_cmp(m.version, pick->version) != 0
        || m.payload == NULL || strcmp(m.payload, pick->digest) != 0) {
        refuse("the manifest of %s %s disagrees with the channel index about what it is",
               pick->name, pick->version);
        goto out;
    }

    /* Every entry must be exactly what the manifest lists, in order. */
    wc.m = &m; wc.i = 0; wc.err[0] = '\0';
    st = pkg_read(pkg, pkg_len, check_entry, &wc, &stopped);
    if (st != PKG_OK || wc.i != m.nfiles) {
        refuse("the payload of %s %s is refused: %s", pick->name, pick->version,
               st == PKG_E_STOPPED ? wc.err
               : st != PKG_OK ? pkg_strstatus(st) : "the manifest lists files the container lacks");
        goto out;
    }

    /* Nothing already present is overwritten. */
    for (i = 0; i < m.nfiles; i++) {
        char *t = pkg_join(a->root, m.files[i].path);
        int there = t ? pkg_fs_exists(t) : 1;
        free(t);
        if (there) {
            refuse("\"%s\" already exists in %s and belongs to no installation of %s; "
                   "nothing was installed", m.files[i].path, a->root, pick->name);
            goto out;
        }
        bytes += m.files[i].size;
    }

    /* Stage inside the root, so every final rename stays on one filesystem. */
    {
        char rel[128];
        snprintf(rel, sizeof rel, ".pkg/staging/%s", pick->name);
        staging = pkg_join(a->root, rel);
    }
    if (staging == NULL || pkg_fs_rmtree(staging) != 0) { refuse("cannot prepare staging"); goto out; }
    sc.staging = staging; sc.err[0] = '\0';
    if (pkg_read(pkg, pkg_len, stage_entry, &sc, &stopped) != PKG_OK) {
        refuse("%s; nothing was installed", sc.err);
        pkg_fs_rmtree(staging);
        goto out;
    }
    for (i = 0; i < m.nfiles; i++) {
        char *from = pkg_join(staging, m.files[i].path);
        char *to = pkg_join(a->root, m.files[i].path);
        int ok = from && to && pkg_fs_rename(from, to) == 0;
        free(from);
        free(to);
        if (!ok) {
            refuse("cannot place \"%s\": %s. %lu of %lu files were placed; run REMOVE %s "
                   "after checking the root", m.files[i].path, strerror(errno),
                   (unsigned long)i, (unsigned long)m.nfiles, pick->name);
            goto out;
        }
    }
    if (pkg_fs_write_atomic(dbp, mtext, mlen) != 0) {
        refuse("the files are placed but the database entry could not be written: %s",
               strerror(errno));
        goto out;
    }
    pkg_fs_rmtree(staging);
    short12(pick->digest, s12);
    printf("installed %s %s into %s: %lu files, %llu bytes, payload %s\n",
           pick->name, pick->version, a->root, (unsigned long)m.nfiles, bytes, s12);
    rc = 0;
out:
    free(ix.e); free(pkg); free(mtext); free(po); free(mo); free(dbp); free(staging);
    pkg_manifest_free(&m);
    return rc;
}

static int cmd_list(const struct args *a)
{
    char *dir, **names;
    size_t n, i;

    if (a->root == NULL) return refuse("name the root with ROOT <dir>");
    dir = pkg_join(a->root, ".pkg/db");
    if (dir == NULL || pkg_fs_list(dir, &names, &n) != 0) {
        free(dir);
        return refuse("cannot read the database of %s", a->root);
    }
    free(dir);
    for (i = 0; i < n; i++) {
        struct pkg_manifest m;
        if (load_installed(a->root, names[i], &m) == 0) {
            printf("%-24s %-10s %-12s %lu files\n", m.name, m.version, m.kind,
                   (unsigned long)m.nfiles);
            pkg_manifest_free(&m);
        }
        free(names[i]);
    }
    free(names);
    if (n == 0)
        printf("nothing installed in %s\n", a->root);
    return 0;
}

/* 0 intact, 1 changed, 2 missing. */
static int file_state(const char *root, const struct pkg_file *f)
{
    char *p = pkg_join(root, f->path), hex[PKG_SHA256_HEXLEN + 1];
    unsigned char *buf;
    size_t len;
    int rc;

    if (p == NULL || pkg_fs_read(p, &buf, &len) != 0) { free(p); return 2; }
    free(p);
    pkg_sha256_hex(buf, len, hex);
    rc = ((unsigned long long)len == f->size && strcmp(hex, f->digest) == 0) ? 0 : 1;
    free(buf);
    return rc;
}

static int cmd_verify(const struct args *a)
{
    struct pkg_manifest m;
    size_t i, changed = 0, missing = 0;

    if (a->pos == NULL)  return refuse("name the package to verify");
    if (a->root == NULL) return refuse("name the root with ROOT <dir>");
    if (load_installed(a->root, a->pos, &m) != 0) return 1;
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, &m.files[i]);
        if (s == 1) { changed++; printf("  changed  %s\n", m.files[i].path); }
        if (s == 2) { missing++; printf("  missing  %s\n", m.files[i].path); }
    }
    if (changed + missing == 0)
        printf("%s %s: %lu files, all intact\n", m.name, m.version, (unsigned long)m.nfiles);
    else
        printf("%s %s: %lu changed, %lu missing, of %lu files\n", m.name, m.version,
               (unsigned long)changed, (unsigned long)missing, (unsigned long)m.nfiles);
    pkg_manifest_free(&m);
    return changed + missing == 0 ? 0 : 1;
}

static int cmd_remove(const struct args *a)
{
    struct pkg_manifest m;
    size_t i, removed = 0, kept = 0, gone = 0;
    char *dbp;

    if (a->pos == NULL)  return refuse("name the package to remove");
    if (a->root == NULL) return refuse("name the root with ROOT <dir>");
    if (load_installed(a->root, a->pos, &m) != 0) return 1;
    for (i = 0; i < m.nfiles; i++) {
        int s = file_state(a->root, &m.files[i]);
        if (s == 0) {
            char *p = pkg_join(a->root, m.files[i].path);
            if (p != NULL && pkg_fs_unlink(p) == 0) {
                removed++;
                pkg_fs_prune_empty_parents(a->root, m.files[i].path);
            }
            free(p);
        } else if (s == 1) {
            kept++;
            printf("  kept     %s (changed since install, so it is yours now)\n", m.files[i].path);
        } else {
            gone++;
        }
    }
    dbp = db_path(a->root, m.name);
    if (dbp == NULL || pkg_fs_unlink(dbp) != 0) {
        free(dbp);
        pkg_manifest_free(&m);
        return refuse("the files are removed but the database entry could not be: %s",
                      strerror(errno));
    }
    free(dbp);
    printf("removed %s %s from %s: %lu files removed", m.name, m.version, a->root,
           (unsigned long)removed);
    if (kept) printf(", %lu kept", (unsigned long)kept);
    if (gone) printf(", %lu already gone", (unsigned long)gone);
    printf("\n");
    pkg_manifest_free(&m);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH a] [KIND k]\n"
        "  pkg PUBLISH  <drawer> CHANNEL <dir> [NAME n] [VERSION v] [ARCH a] [KIND k]\n"
        "  pkg INSTALL  <name> ROOT <dir> CHANNEL <dir> [VERSION v]\n"
        "  pkg LIST     ROOT <dir>\n"
        "  pkg VERIFY   <name> ROOT <dir>\n"
        "  pkg REMOVE   <name> ROOT <dir>\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct args a;
    if (argc < 2)
        return usage();
    verb_name = argv[1];
    if (parse_args(argc, argv, &a) != 0)
        return 2;
    if (ieq(argv[1], "MANIFEST")) { verb_name = "manifest"; return cmd_manifest(&a); }
    if (ieq(argv[1], "PUBLISH"))  { verb_name = "publish";  return cmd_publish(&a); }
    if (ieq(argv[1], "INSTALL"))  { verb_name = "install";  return cmd_install(&a); }
    if (ieq(argv[1], "LIST"))     { verb_name = "list";     return cmd_list(&a); }
    if (ieq(argv[1], "VERIFY"))   { verb_name = "verify";   return cmd_verify(&a); }
    if (ieq(argv[1], "REMOVE"))   { verb_name = "remove";   return cmd_remove(&a); }
    return usage();
}
