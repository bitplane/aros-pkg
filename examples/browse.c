/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * browse: a package browser on libpkg, the shape a graphical front end
 * takes, in plain C. It shows how to collect an answer (records, and items
 * with their fields apart), how to show a refusal with its suggestions and
 * the sentence for what to do next, how to wire a Cancel button, and how to
 * see libpkg's own trace.
 *
 *   browse list    <channel> [<root>]         what the channel offers, and
 *                                             whether each is in <root>
 *   browse install <name> <channel> <root> [cancel-at]
 *                                             install <name> and what it
 *                                             needs; with cancel-at N, the
 *                                             Cancel button is pressed the
 *                                             Nth time libpkg asks
 *   browse root    <root>                     what <root> holds
 *
 * PKG_BROWSE_TRACE set in the environment prints libpkg's trace.
 *
 * Written first by an agent from pkg.h alone, as a test of that header, then
 * adopted as the example (examples/basic.c is the short one).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg.h"


/* ---- collecting an answer --------------------------------------------- */

static void *xalloc(void *p, size_t size)
{
    p = realloc(p, size);
    if (p == NULL) { perror("browse"); exit(1); }
    return p;
}

static char *dup(const char *s)
{
    size_t len = strlen(s) + 1;
    return memcpy(xalloc(NULL, len), s, len);
}

struct field { char *key, *value; };
struct item  { char *kind; int n; char **keys, **values; };

struct answer {
    struct field *f; size_t n, cap;     /* every record, in order */
    struct item  *it; size_t ni, capi;  /* every item, fields apart */
    /* Cancel, as a GUI's button would set it. Here it is pressed by itself
     * the `cancel_at`th time libpkg asks (0: never). */
    int cancel_at, polls, cancelled;
};

/* Strings passed to a callback live only during it: copy what is kept. */
static void on_record(void *user, const char *key, const char *value)
{
    struct answer *a = user;
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->f = xalloc(a->f, a->cap * sizeof *a->f);
    }
    a->f[a->n].key = dup(key);
    a->f[a->n].value = dup(value);
    a->n++;
}

static void on_item(void *user, const char *kind, int n,
                    const char *const *keys, const char *const *values)
{
    struct answer *a = user;
    struct item *e;
    int i;
    if (a->ni == a->capi) {
        a->capi = a->capi ? a->capi * 2 : 16;
        a->it = xalloc(a->it, a->capi * sizeof *a->it);
    }
    e = &a->it[a->ni++];
    e->kind = dup(kind);
    e->n = n;
    e->keys = xalloc(NULL, (n ? n : 1) * sizeof *e->keys);
    e->values = xalloc(NULL, (n ? n : 1) * sizeof *e->values);
    for (i = 0; i < n; i++) {
        e->keys[i] = dup(keys[i]);
        e->values[i] = dup(values[i]);
    }
}

/* Asked between steps. A GUI would return the state of its Cancel button;
 * this one presses it the `cancel_at`th time it is asked. */
static int on_cancel(void *user)
{
    struct answer *a = user;
    a->polls++;
    if (a->cancel_at > 0 && a->polls >= a->cancel_at) {
        a->cancelled = 1;
        return 1;
    }
    return 0;
}

/* PKG_BROWSE_TRACE set: show libpkg's own account of what it does. */
static void on_trace(void *user, const char *line)
{
    (void)user;
    printf("    | %s\n", line);
}

static void answer_free(struct answer *a)
{
    size_t i;
    int j;
    for (i = 0; i < a->n; i++) { free(a->f[i].key); free(a->f[i].value); }
    for (i = 0; i < a->ni; i++) {
        for (j = 0; j < a->it[i].n; j++) { free(a->it[i].keys[j]); free(a->it[i].values[j]); }
        free(a->it[i].keys); free(a->it[i].values); free(a->it[i].kind);
    }
    free(a->f);
    free(a->it);
    memset(a, 0, sizeof *a);
}

static const char *get(const struct answer *a, const char *key)
{
    size_t i;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->f[i].key, key) == 0) return a->f[i].value;
    return NULL;
}

static const char *opt(const char *s, const char *dflt) { return s ? s : dflt; }

/* One field of an item, by name; NULL when the item has no such field. */
static const char *field(const struct item *e, const char *key)
{
    return pkg_field(e->n, (const char *const *)e->keys, (const char *const *)e->values, key);
}

static struct pkg_sink sink_for(struct answer *a)
{
    struct pkg_sink s;
    memset(&s, 0, sizeof s);
    s.record = on_record;
    s.item = on_item;
    s.cancel = on_cancel;
    if (getenv("PKG_BROWSE_TRACE")) s.trace = on_trace;
    s.user = a;
    s.structured = 1;
    return s;
}

/* ---- refusals, in plain words -------------------------------------------- */

static void show_refusal(const char *doing, const struct answer *a, int rc)
{
    const char *reason = get(a, "reason");
    const char *next = get(a, "next");
    const char *pinned = get(a, "pinned"), *signer = get(a, "signer");
    const char *first = get(a, "first-signer");
    size_t i, nsug = 0;

    printf("\nCould not %s.\n", doing);
    printf("  Why:        %s\n", opt(reason, "no reason was given"));
    printf("  Kind:       %s (code %d)\n", pkg_class_name(rc), rc);
    if ((pinned || first) && signer) {
        printf("  Trusted key:  %s\n", pinned ? pinned : first);
        printf("  Offered key:  %s\n", signer);
    }
    for (i = 0; i < a->ni; i++)
        if (strcmp(a->it[i].kind, "suggest") == 0) {
            const char *nm = field(&a->it[i], "name");
            if (nm == NULL) continue;
            printf(nsug++ ? ", %s" : "  Did you mean: %s", nm);
        }
    if (nsug) printf("?\n");
    /* libpkg's own sentence, which names no command. */
    printf("  What to do: %s\n", pkg_next_words(next));
}

/* ---- listing ------------------------------------------------------------ */

static const char *in_root_words(const char *installed)
{
    if (installed == NULL) return "";
    if (strcmp(installed, "installed") == 0) return "installed";
    if (strcmp(installed, "other-version") == 0) return "other version installed";
    if (strcmp(installed, "no") == 0) return "-";
    return installed;               /* a value newer than this browser */
}

static int do_list(const char *channel, const char *root)
{
    struct answer a = {0};
    struct pkg_sink s = sink_for(&a);
    struct pkg_options o;
    size_t i;
    int rc;

    memset(&o, 0, sizeof o);
    o.channel = channel;
    o.root = root;                  /* show fills `installed` when given */
    rc = pkg_show(&s, &o);
    /* SHOW answers `shown` with every entry even when some are damaged. */
    if (strcmp(opt(get(&a, "result"), ""), "shown") != 0) {
        show_refusal("read the channel", &a, rc);
        answer_free(&a);
        return rc;
    }

    printf("Channel %s\n", channel);
    if (root) printf("Root    %s\n", root);
    printf("\n%-18s %-8s %-12s %-8s %-10s %s\n",
           "PACKAGE", "VERSION", "KIND", "ARCH", "CHECK", root ? "IN ROOT" : "");
    for (i = 0; i < a.ni; i++) {
        const struct item *e = &a.it[i];
        if (strcmp(e->kind, "entry") == 0) {
            printf("%-18s %-8s %-12s %-8s %-10s %s\n",
                   opt(field(e, "name"), "?"), opt(field(e, "version"), "?"),
                   opt(field(e, "kind"), "-"), opt(field(e, "architecture"), "-"),
                   opt(field(e, "status"), "?"),
                   in_root_words(field(e, "installed")));
        } else if (strcmp(e->kind, "depends") == 0) {
            const char *min = field(e, "min");
            printf("%-18s   %s %s needs %s%s%s\n", "",
                   opt(field(e, "package"), "?"), opt(field(e, "version"), "?"),
                   opt(field(e, "needs"), "?"),
                   min && *min ? " >= " : "", min && *min ? min : "");
        } else if (strcmp(e->kind, "problem") == 0) {
            printf("%-18s   %s %s damaged: %s\n", "",
                   opt(field(e, "package"), "?"), opt(field(e, "version"), "?"),
                   opt(field(e, "reason"), "?"));
        }
    }
    printf("\n%s offered, %s damaged.\n", opt(get(&a, "count"), "?"),
           opt(get(&a, "bad"), "?"));
    if (rc != PKG_RC_OK)
        printf("Damaged entries cannot be installed: %s\n", pkg_next_words("stop"));
    answer_free(&a);
    return rc;
}

/* What <root> holds, from pkg_list. */
static int do_root(const char *root)
{
    struct answer a = {0};
    struct pkg_sink s = sink_for(&a);
    struct pkg_options o;
    size_t i;
    int rc;

    memset(&o, 0, sizeof o);
    o.root = root;
    rc = pkg_list(&s, &o);
    if (rc != PKG_RC_OK) {
        show_refusal("read the root", &a, rc);
        answer_free(&a);
        return rc;
    }
    printf("Root %s holds %s package(s)\n", root, opt(get(&a, "count"), "?"));
    for (i = 0; i < a.ni; i++) {
        const struct item *e = &a.it[i];
        if (strcmp(e->kind, "package") != 0) continue;
        printf("  %-18s %-8s %-12s %4s files  %s\n",
               opt(field(e, "name"), "?"), opt(field(e, "version"), "?"),
               opt(field(e, "kind"), "-"), opt(field(e, "files"), "?"),
               strcmp(opt(field(e, "reason"), ""), "dependency") == 0
                   ? "(needed by another)" : "");
    }
    answer_free(&a);
    return 0;
}

/* ---- installing ------------------------------------------------------------ */

static int do_install(const char *name, const char *channel, const char *root,
                      int cancel_at)
{
    struct answer a = {0};
    struct pkg_sink s = sink_for(&a);
    struct pkg_options o;
    const char *result;
    char doing[256];
    size_t i, deps = 0;
    int rc;

    a.cancel_at = cancel_at;
    memset(&o, 0, sizeof o);
    o.target = name;
    o.channel = channel;
    o.root = root;
    rc = pkg_install(&s, &o);
    result = opt(get(&a, "result"), "");

    snprintf(doing, sizeof doing, "install %s", name);
    if (a.cancelled) {
        printf("Cancel pressed at check %d.\n", a.polls);
        if (rc != PKG_RC_OK) show_refusal(doing, &a, rc);
        printf("libpkg took back out the packages it had placed in %s.\n", root);
        answer_free(&a);
        return rc ? rc : PKG_RC_REFUSED;
    }
    if (rc != PKG_RC_OK || strcmp(result, "refused") == 0) {
        show_refusal(doing, &a, rc);
        printf("Nothing was changed in %s.\n", root);
        answer_free(&a);
        return rc ? rc : PKG_RC_REFUSED;
    }

    if (strcmp(result, "unchanged") == 0)
        printf("%s %s is already installed in %s; nothing to do.\n",
               opt(get(&a, "name"), name), opt(get(&a, "version"), ""), root);
    else if (strcmp(result, "kept") == 0)
        printf("%s was already here as a dependency; it is now kept for itself.\n", name);
    else
        printf("Installed %s %s into %s.\n",
               opt(get(&a, "name"), name), opt(get(&a, "version"), ""), root);
    for (i = 0; i < a.ni; i++)
        if (strcmp(a.it[i].kind, "dependency") == 0) {
            if (deps++ == 0) printf("It needed, and so also installed:\n");
            printf("  %s %s\n", opt(field(&a.it[i], "name"), "?"),
                   opt(field(&a.it[i], "version"), "?"));
        }
    if (deps == 0 && strcmp(result, "installed") == 0)
        printf("It needed nothing that was not already there.\n");
    if (get(&a, "image"))
        printf("It is an application image: %s in the root (%s blocks); "
               "mount it to run it.\n", get(&a, "image"), opt(get(&a, "blocks"), "?"));
    answer_free(&a);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "list") == 0)
        return do_list(argv[2], argc == 4 ? argv[3] : NULL);
    if ((argc == 5 || argc == 6) && strcmp(argv[1], "install") == 0)
        return do_install(argv[2], argv[3], argv[4], argc == 6 ? atoi(argv[5]) : 0);
    if (argc == 3 && strcmp(argv[1], "root") == 0)
        return do_root(argv[2]);

    fprintf(stderr, "usage: browse list <channel> [<root>]\n"
                    "       browse install <name> <channel> <root> [cancel-at]\n"
                    "       browse root <root>\n");
    return PKG_RC_USAGE;
}
