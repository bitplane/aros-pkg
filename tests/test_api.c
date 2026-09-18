/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * libpkg driven the way a graphical front end would drive it: through pkg.h
 * alone, with a sink that keeps every record, no command line, no stdout.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void ok(int c, const char *what) { checks++; if (!c) { failures++; printf("  FAIL %s\n", what); } }

/* A front end's view: every field of the last answer, and whether any text
 * arrived at all. */
struct seen {
    char keys[64][32];
    char values[64][300];
    int  n, texts;
};

static void rec(void *user, const char *key, const char *value)
{
    struct seen *s = (struct seen *)user;
    if (s->n < 64) {
        snprintf(s->keys[s->n], sizeof s->keys[0], "%s", key);
        snprintf(s->values[s->n], sizeof s->values[0], "%s", value);
        s->n++;
    }
}

static void txt(void *user, int is_error, const char *text)
{
    (void)is_error;
    (void)text;
    ((struct seen *)user)->texts++;
}

static const char *field(const struct seen *s, const char *key)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->keys[i], key) == 0)
            return s->values[i];
    return NULL;
}

static int is(const struct seen *s, const char *key, const char *value)
{
    const char *v = field(s, key);
    return v != NULL && strcmp(v, value) == 0;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void)
{
    char dirbuf[400], *dir = dirbuf, p[512], key[512], drawer[512], channel[512], root[512];
    const char *tmp = getenv("TMPDIR");
    struct seen s;
    struct pkg_sink sink = { rec, txt, &s, 1 };
    struct pkg_options o;
    struct stat st;
    int rc;

    snprintf(dirbuf, sizeof dirbuf, "%s/pkg-api.%ld", tmp ? tmp : "/tmp", (long)getpid());
    if (mkdir(dir, 0700) != 0) { printf("cannot make %s\n", dir); return 1; }
    snprintf(key, sizeof key, "%s/dev.key", dir);
    snprintf(drawer, sizeof drawer, "%s/drawer", dir);
    snprintf(channel, sizeof channel, "%s/channel", dir);
    snprintf(root, sizeof root, "%s/root", dir);
    snprintf(p, sizeof p, "%s/C", drawer);
    mkdir(drawer, 0755);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/C/Tool", drawer);
    put(p, "tool\n");

    printf("structured\n");
    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.file = key;
    rc = pkg_keygen(&sink, &o);
    ok(rc == PKG_RC_OK && is(&s, "result", "created") && field(&s, "public") != NULL,
       "keygen answers with result and the public key");
    ok(s.texts == 0, "and the structured form sends no text");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.target = drawer; o.channel = channel; o.sign = key;
    o.name = "tool"; o.version = "1.0"; o.kind = "application"; o.dryrun = 1;
    rc = pkg_publish(&sink, &o);
    ok(rc == 0 && is(&s, "result", "would-publish") && is(&s, "depends", "none"),
       "a dry-run publish describes itself");
    ok(stat(channel, &st) != 0, "and writes no channel");

    memset(&s, 0, sizeof s);
    o.dryrun = 0;
    rc = pkg_publish(&sink, &o);
    ok(rc == 0 && is(&s, "result", "published") && is(&s, "name", "tool"), "publish");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.target = "tool"; o.root = root; o.channel = channel;
    rc = pkg_install(&sink, &o);
    ok(rc == 0 && is(&s, "result", "installed") && is(&s, "version", "1.0"), "install");
    ok(strcmp(s.keys[0], "result") == 0 || field(&s, "dependency") != NULL,
       "fields arrive in order, result first when nothing precedes it");
    memset(&s, 0, sizeof s);
    rc = pkg_install(&sink, &o);
    ok(rc == 0 && is(&s, "result", "unchanged"), "installing again changes nothing and succeeds");

    memset(&s, 0, sizeof s);
    o.target = "nosuch";
    rc = pkg_upgrade(&sink, &o);
    ok(rc == PKG_RC_NOTFOUND && is(&s, "result", "refused") && is(&s, "class", "not-found")
       && is(&s, "code", "11") && is(&s, "next", "use-install") && field(&s, "reason") != NULL,
       "a refusal: class, code, reason and next, as fields");

    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.root = root;
    rc = pkg_list(&sink, &o);
    ok(rc == 0 && is(&s, "package", "tool 1.0 application 1 explicit") && is(&s, "count", "1"),
       "list");

    memset(&s, 0, sizeof s);
    rc = pkg_usage_error(&sink, "install", "a front end found no package name");
    ok(rc == PKG_RC_USAGE && is(&s, "class", "usage") && is(&s, "next", "fix-command"),
       "a front end's own usage error has the same shape");

    printf("text\n");
    sink.structured = 0;
    memset(&s, 0, sizeof s);
    memset(&o, 0, sizeof o);
    o.root = root;
    rc = pkg_list(&sink, &o);
    ok(rc == 0 && s.texts > 0 && s.n == 0, "the text form sends text and no fields");

    printf("names\n");
    ok(strcmp(pkg_class_name(PKG_RC_KEY), "key") == 0 && strcmp(pkg_class_name(0), "ok") == 0,
       "pkg_class_name");
    ok(strstr(pkg_next_words("ask-person"), "person") != NULL, "pkg_next_words");

    snprintf(p, sizeof p, "rm -rf '%s'", dir);
    if (system(p) != 0) printf("  note: could not remove %s\n", dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
