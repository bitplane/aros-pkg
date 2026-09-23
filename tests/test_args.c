/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The template reader of pkg_args.h, rule by rule: what a template accepts,
 * and each word it refuses with the reason a person would need.
 */

#include "pkg_args.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

static const char *INSTALL =
    "PACKAGE/M/A,ROOT/K/R,AT/K,CHANNEL/K,VERSION/K,ARCH/K,ACCEPTKEY/K=<hex>,UNPACKED/K,DRYRUN/S,"
    "MACHINE/S/G,TRACE/K/G";

static int other_verbs(const char *w)
{
    return strcmp(w, "FILE") == 0 || strcmp(w, "OUT") == 0;
}

/* Reads a line of words ("zip ROOT r") against a template. */
static int run(struct pkg_args *a, const char *tmpl, const char *verb, const char *line,
               char *err, size_t errlen)
{
    static char buf[512];
    char *argv[40];
    int argc = 0;
    char *p;
    char terr[200];
    if (pkg_args_template(tmpl, a, terr, sizeof terr) != 0) {
        snprintf(err, errlen, "template: %s", terr);
        return -2;
    }
    snprintf(buf, sizeof buf, "%s", line);
    for (p = strtok(buf, " "); p && argc < 40; p = strtok(NULL, " "))
        argv[argc++] = p;
    err[0] = '\0';
    return pkg_args_read(a, verb, 0, argc, argv, other_verbs, err, errlen);
}

static const char *shown(const char *name)
{
    if (strcmp(name, "PACKAGE") == 0) return "<name>";
    if (strcmp(name, "ROOT") == 0 || strcmp(name, "AT") == 0 || strcmp(name, "UNPACKED") == 0) return "<dir>";
    if (strcmp(name, "CHANNEL") == 0) return "<dir|url|name>";
    if (strcmp(name, "VERSION") == 0) return "v";
    if (strcmp(name, "ARCH") == 0) return "cpu";
    return "<value>";
}

int main(void)
{
    struct pkg_args a;
    struct pkg_arg *it;
    char err[400], syn[400];

    /* what a template accepts */
    ok(run(&a, INSTALL, "INSTALL", "zip ROOT r", err, sizeof err) == 0, "a name and a keyword");
    it = pkg_args_item(&a, "PACKAGE");
    ok(it && it->nvalues == 1 && strcmp(it->values[0], "zip") == 0, "the name is taken by place");
    ok(strcmp(pkg_args_item(&a, "ROOT")->value, "r") == 0, "ROOT takes the next word");

    ok(run(&a, INSTALL, "INSTALL", "a b c ROOT r CHANNEL ch", err, sizeof err) == 0, "several names");
    it = pkg_args_item(&a, "PACKAGE");
    ok(it->nvalues == 3 && strcmp(it->values[2], "c") == 0, "every name, in order");

    ok(run(&a, INSTALL, "INSTALL", "zip root r dryrun", err, sizeof err) == 0, "keywords in any case");
    ok(pkg_args_item(&a, "DRYRUN")->set && strcmp(pkg_args_item(&a, "ROOT")->value, "r") == 0,
       "a switch and a keyword in lower case");

    ok(run(&a, INSTALL, "INSTALL", "zip ROOT=Work:r", err, sizeof err) == 0, "KEYWORD=value");
    ok(strcmp(pkg_args_item(&a, "ROOT")->value, "Work:r") == 0, "the value after the =");

    ok(run(&a, "DRAWER/A,NAME/K", "PUBLISH", "drawer NAME demo", err, sizeof err) == 0,
       "a place is never matched by its name: a drawer called drawer");
    ok(strcmp(pkg_args_item(&a, "DRAWER")->value, "drawer") == 0 && strcmp(pkg_args_item(&a, "NAME")->value, "demo") == 0,
       "and NAME after it keeps its own value");
    ok(run(&a, INSTALL, "INSTALL", "package ROOT r", err, sizeof err) == 0
       && strcmp(pkg_args_item(&a, "PACKAGE")->values[0], "package") == 0, "a package called package");

    ok(run(&a, INSTALL, "INSTALL", "file ROOT r", err, sizeof err) == 0,
       "another verb's keyword in lower case is a name: a package may be called file");
    ok(strcmp(pkg_args_item(&a, "PACKAGE")->values[0], "file") == 0, "and it is the package");

    ok(run(&a, INSTALL, "INSTALL", "zip ROOT r MACHINE", err, sizeof err) == 0, "a global switch");

    /* what it refuses, and how it says so */
    ok(run(&a, INSTALL, "INSTALL", "ROOT r", err, sizeof err) == -1 && strstr(err, "INSTALL needs a name"),
       "a missing name");
    ok(run(&a, INSTALL, "INSTALL", "zip ROOT a ROOT b", err, sizeof err) == -1 && strstr(err, "ROOT is given twice"),
       "a keyword given twice, instead of the last one winning quietly");
    ok(run(&a, INSTALL, "INSTALL", "zip ROOT", err, sizeof err) == -1 && strstr(err, "ROOT needs a value"),
       "a keyword with nothing after it");
    ok(run(&a, INSTALL, "INSTALL", "zip --root r", err, sizeof err) == -1 && strstr(err, "no dashes"),
       "the habit of other tools");
    ok(run(&a, INSTALL, "INSTALL", "zip FILE x ROOT r", err, sizeof err) == -1
       && strstr(err, "FILE is not a word INSTALL takes; it takes ROOT, AT, CHANNEL"),
       "another verb's keyword in capitals, with what this verb takes");
    ok(strstr(err, "MACHINE") == NULL && strstr(err, "TRACE") == NULL, "the global words are not listed");
    ok(run(&a, INSTALL, "INSTALL", "zip ROT r", err, sizeof err) == -1 && strstr(err, "did you mean ROOT?"),
       "a slip of one of its own keywords");
    ok(run(&a, INSTALL, "INSTALL", "foo ROTO /tmp/r1", err, sizeof err) == -1 && strstr(err, "did you mean ROOT?"),
       "two letters swapped are one slip: ROTO is caught, not taken as a name");
    ok(run(&a, INSTALL, "INSTALL", "foo ROOT CHANNEL", err, sizeof err) == -1 && strstr(err, "the value was left out"),
       "a keyword of the verb in capitals where a value belongs is a value left out");
    ok(run(&a, INSTALL, "INSTALL", "foo ROOT channel", err, sizeof err) == 0
       && strcmp(pkg_args_item(&a, "ROOT")->value, "channel") == 0, "in lower case it is a directory called channel");
    ok(run(&a, INSTALL, "INSTALL", "zip DRYRUN DRYRUN", err, sizeof err) == -1 && strstr(err, "DRYRUN is given twice"),
       "a switch given twice");
    ok(run(&a, "PACKAGE,CHANNEL/K", "SHOW", "a b", err, sizeof err) == -1
       && strstr(err, "\"b\" is more than SHOW takes; it takes CHANNEL"),
       "a word more than the places");
    ok(run(&a, "ROOT/K/R", "LIST", "zip", err, sizeof err) == -1 && strstr(err, "it takes ROOT"),
       "a verb that takes no name");
    ok(run(&a, "FILE/K/A", "KEYGEN", "", err, sizeof err) == -1 && strstr(err, "KEYGEN needs FILE"),
       "a required keyword");

    /* /W: the channel after CHANNEL ADD is taken as it is */
    ok(run(&a, "CHANNEL/A/W,NAME/K,ROOT/K/R", "CHANNEL ADD", "root ROOT r", err, sizeof err) == 0,
       "a /W place takes a word spelled like a keyword");
    ok(strcmp(pkg_args_item(&a, "CHANNEL")->value, "root") == 0 && strcmp(pkg_args_item(&a, "ROOT")->value, "r") == 0,
       "the directory called root, then ROOT itself");

    /* usage */
    pkg_args_template(INSTALL, &a, err, sizeof err);
    pkg_args_syntax(&a, shown, syn, sizeof syn);
    ok(strcmp(syn, "<name>... ROOT <dir> [AT <dir>] [CHANNEL <dir|url|name>] [VERSION v] [ARCH cpu] "
                   "[ACCEPTKEY <hex>] [UNPACKED <dir>] [DRYRUN]") == 0, "the syntax is drawn from the template");
    if (fails) printf("  got: %s\n", syn);

    /* templates that do not parse: a mistake in pkg, said precisely */
    ok(pkg_args_template("A/M,B/M", &a, err, sizeof err) == -1 && strstr(err, "two /M"), "two /M items");
    ok(pkg_args_template("A/K/S", &a, err, sizeof err) == -1 && strstr(err, "both"), "/K and /S at once");
    ok(pkg_args_template("A/Q", &a, err, sizeof err) == -1 && strstr(err, "unknown modifier"), "an unknown modifier");

    ok(pkg_args_template("DEPENDS/K=\"a >= 1, b\",FILES/K=\"C,Libs\",NAME/K", &a, err, sizeof err) == 0
       && a.n == 3 && strcmp(a.item[0].shown, "\"a >= 1, b\"") == 0 && strcmp(a.item[2].name, "NAME") == 0,
       "a shown value in quotes keeps its commas");
    ok(pkg_args_template("DEPENDS/K=a >= 1, b,NAME/K", &a, err, sizeof err) == -1 && strstr(err, "not a word in capitals"),
       "a template cut in the wrong place is caught, not read as items");

    printf("args: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
