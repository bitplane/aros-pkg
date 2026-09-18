/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Pkg, command line: a client of libpkg (pkg.h). It turns the words of a
 * command into pkg_options, prints what the library answers, and returns its
 * code as the exit status. Keywords follow AmigaDOS usage and are
 * case-insensitive; `pkg HELP` lists them.
 *
 * MACHINE on the line, or PKG_OUTPUT=machine in the environment, selects the
 * library's structured form, printed as "key: value" lines on stdout.
 * Otherwise the text form goes to stdout, refusals and warnings to stderr.
 * PKG_SIGNKEY is the default signing key. PORT serves the same verbs on an
 * ARexx port on AROS.
 */

#include "pkg.h"
#include "pkg_fs.h"
#include "pkg_out.h"
#include "pkg_port.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AmigaDOS version cookie: `Version C:Pkg` reads it, and publishing Pkg
 * with Pkg takes its name and version from it. */
const char pkg_version_cookie[] = "$VER: Pkg " PKG_VERSION_STRING " (18.9.2026)";

static const char *verb_name = "pkg";
static int machine;

static void print_record(void *user, const char *key, const char *value)
{
    (void)user;
    pkg_out("%s: %s\n", key, value);
}

static void print_text(void *user, int is_error, const char *text)
{
    (void)user;
    if (is_error) pkg_err("%s", text); else pkg_out("%s", text);
}

/* TRACE <file>, or PKG_TRACE=<file>: the library's account of each step,
 * appended to that file; "-" sends it to stderr. Never mixed into stdout,
 * so the MACHINE contract stays clean. */
static const char *trace_path;
static FILE *trace_file;

static void print_trace(void *user, const char *line)
{
    (void)user;
    if (strcmp(trace_path, "-") == 0) {
        pkg_err("trace %s\n", line);
        return;
    }
    if (trace_file == NULL)
        trace_file = fopen(trace_path, "a");
    if (trace_file != NULL) {
        fprintf(trace_file, "%s\n", line);
        fflush(trace_file);
    }
}

static struct pkg_sink out_sink = { print_record, print_text, NULL, 0, NULL, NULL, NULL };

/* A usage error found while reading the words, answered like any refusal. */
static int usage_errorf(const char *fmt, ...)
{
    char why[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    out_sink.structured = machine;
    pkg_usage_error(&out_sink, verb_name, why);
    return -1;
}

/* ---- arguments -------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static const struct { const char *kw; size_t off; } kws[] = {
        { "ROOT",      offsetof(struct pkg_options, root) },
        { "CHANNEL",   offsetof(struct pkg_options, channel) },
        { "NAME",      offsetof(struct pkg_options, name) },
        { "VERSION",   offsetof(struct pkg_options, version) },
        { "ARCH",      offsetof(struct pkg_options, arch) },
        { "KIND",      offsetof(struct pkg_options, kind) },
        { "FILE",      offsetof(struct pkg_options, file) },
        { "SIGN",      offsetof(struct pkg_options, sign) },
        { "KEY",       offsetof(struct pkg_options, key) },
        { "OUT",       offsetof(struct pkg_options, out) },
        { "ACCEPTKEY", offsetof(struct pkg_options, acceptkey) },
        { "DEPENDS",   offsetof(struct pkg_options, depends) },
        { "UNIT",      offsetof(struct pkg_options, unit) },
        { "HANDLER",   offsetof(struct pkg_options, handler) },
        { "FILES",     offsetof(struct pkg_options, files) },
        { "BUILD",     offsetof(struct pkg_options, build) }
};

static int takes_value(const char *w)
{
    size_t k;
    if (ieq(w, "TRACE"))
        return 1;
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

static int parse_args(int argc, char **argv, struct pkg_options *a)
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
        if (ieq(argv[i], "ORPHANS")) {
            a->orphans = 1;
            continue;
        }
        if (ieq(argv[i], "DRYRUN")) {
            a->dryrun = 1;
            continue;
        }
        if (ieq(argv[i], "TRACE")) {
            if (i + 1 >= argc)
                return usage_errorf("TRACE needs a file, or - for stderr");
            trace_path = argv[++i];
            out_sink.trace = print_trace;
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
                return usage_errorf("%s needs a value", argv[i]);
            *slot = argv[++i];
        } else if (argv[i][0] == '-') {
            /* The habit of other tools. Say the Pkg spelling, never guess. */
            const char *w = argv[i] + strspn(argv[i], "-");
            char up[32];
            size_t j2;
            for (j2 = 0; w[j2] && j2 + 1 < sizeof up && w[j2] != '='; j2++)
                up[j2] = (char)toupper((unsigned char)w[j2]);
            up[j2] = '\0';
            return usage_errorf("\"%s\": Pkg keywords have no dashes and take their value "
                                "as the next word, as in ROOT <dir>%s%s", argv[i],
                                up[0] ? "; here perhaps " : "", up);
        } else if (a->target == NULL) {
            a->target = argv[i];
        } else {
            return usage_errorf("unexpected argument \"%s\"", argv[i]);
        }
    }
    if (a->sign == NULL)
        a->sign = getenv("PKG_SIGNKEY");
    return 0;
}

static void (*usage_to)(const char *fmt, ...) = pkg_err;

static int usage(void)
{
    usage_to("%s\n", pkg_version_cookie + 6);
    usage_to("usage:\n"
        "  pkg KEYGEN   FILE <keyfile>\n"
        "  pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH a] [KIND k] [DEPENDS \"a >= 1, b\"]\n"
        "  pkg PUBLISH  <drawer> CHANNEL <dir> KIND k [SIGN <keyfile>] [NAME n] [VERSION v] [ARCH a]\n"
        "               [DEPENDS \"a >= 1, b\"] [ACCEPTKEY <hex>]\n"
        "               KIND: image (a program people run, one volume to mount), application\n"
        "               (a program as loose files), library, device (handlers too), class, font,\n"
        "               catalog, startup, boot, data, sdk, slave. PUBLISH creates the channel.\n"
        "  pkg KEYINFO  FILE <keyfile>    (the public key it holds)\n"
        "  pkg WITHDRAW <name> VERSION v CHANNEL <dir> [SIGN <keyfile>]\n"
        "  pkg SIGN     <file> KEY <keyfile> OUT <sigfile>\n"
        "  pkg INSTALL  <name> ROOT <dir> CHANNEL <dir> [VERSION v] [ARCH cpu] [ACCEPTKEY <hex>]\n"
        "  pkg UPGRADE  <name> ROOT <dir> CHANNEL <dir> [VERSION v] [ARCH cpu] [DOWNGRADE] [ACCEPTKEY <hex>]\n"
        "  pkg ROLLBACK <name> ROOT <dir> CHANNEL <dir>\n"
        "  pkg LIST     ROOT <dir>\n"
        "  pkg VERIFY   <name> ROOT <dir>\n"
        "  pkg REMOVE   <name> ROOT <dir>\n"
        "  pkg REMOVE   ORPHANS ROOT <dir>\n"
        "  pkg IMAGE    <drawer> OUT <file> [NAME <volume>]\n"
        "  pkg MOUNTLIST <image> ROOT <dir> [OUT <file>] [UNIT n] [HANDLER <path>]\n"
        "  pkg SHOW     [<name>] CHANNEL <dir> [ROOT <dir>]\n"
        "Every verb that changes something takes DRYRUN: all checks, no write.\n"
        "Exit code: 0 done, 10 to 18 refused (the number is the class), 20 a wrong command.\n"
        "  pkg PORT     [<portname>]      (AROS: serve these verbs on an ARexx port, PKG by default)\n"
        "  pkg HELP\n"
        "SIGN defaults to $PKG_SIGNKEY. Any verb takes MACHINE, or PKG_OUTPUT=machine:\n"
        "key: value lines, and the exit code names the class of a refusal.\n"
        "Any verb takes TRACE <file>, or PKG_TRACE=<file> (- for stderr): every step it takes,\n"
        "each file it touches and each check and choice, for finding out why.\n");
    return PKG_RC_USAGE;
}

/* One entry for every caller: the command line below, and the ARexx port,
 * which runs each command it receives through here. Returns the exit code:
 * 0, a refusal class from 10 to 18, or 20 for usage. */
static int run_verb(int argc, char **argv)
{
    static const struct {
        const char *verb, *name;
        int (*fn)(const struct pkg_sink *, const struct pkg_options *);
    } verbs[] = {
        { "KEYGEN",    "keygen",    pkg_keygen },
        { "MANIFEST",  "manifest",  pkg_manifest },
        { "PUBLISH",   "publish",   pkg_publish },
        { "WITHDRAW",  "withdraw",  pkg_withdraw },
        { "SIGN",      "sign",      pkg_sign },
        { "KEYINFO",   "keyinfo",   pkg_keyinfo },
        { "INSTALL",   "install",   pkg_install },
        { "UPGRADE",   "upgrade",   pkg_upgrade },
        { "ROLLBACK",  "rollback",  pkg_rollback },
        { "LIST",      "list",      pkg_list },
        { "VERIFY",    "verify",    pkg_verify },
        { "REMOVE",    "remove",    pkg_remove },
        { "IMAGE",     "image",     pkg_image },
        { "MOUNTLIST", "mountlist", pkg_mountlist },
        { "SHOW",      "show",      pkg_show }
    };
    struct pkg_options a;
    size_t i;
    int saved_machine = machine;
    int rc = PKG_RC_USAGE;
    const char *env = getenv("PKG_OUTPUT");

    machine = env != NULL && ieq(env, "machine");
    if (wants_machine(argc, argv))
        machine = 1;
    out_sink.structured = machine;
    trace_path = getenv("PKG_TRACE");
    out_sink.trace = trace_path != NULL && *trace_path ? print_trace : NULL;
    if (argc < 2) {
        if (machine) pkg_usage_error(&out_sink, "pkg", "no verb given");
        else usage();
        machine = saved_machine;
        return PKG_RC_USAGE;
    }
    if (ieq(argv[1], "HELP") || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        /* Asked for: to stdout, and a success. */
        usage_to = pkg_out;
        usage();
        usage_to = pkg_err;
        machine = saved_machine;
        return PKG_RC_OK;
    }
    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (ieq(argv[1], verbs[i].verb)) {
            verb_name = verbs[i].name;
            rc = parse_args(argc, argv, &a) != 0 ? PKG_RC_USAGE : verbs[i].fn(&out_sink, &a);
            machine = saved_machine;
            return rc;
        }
    }
    if (machine) {
        char why[160];
        snprintf(why, sizeof why, "unknown verb \"%.100s\"", argv[1]);
        pkg_usage_error(&out_sink, "pkg", why);
    } else {
        usage();
    }
    machine = saved_machine;
    return PKG_RC_USAGE;
}

int main(int argc, char **argv)
{
    int rc;

    if (pkg_host_args(&argc, &argv) != 0) {
        pkg_err("pkg: cannot read the command line\n");
        return PKG_RC_IO;
    }

    /* PORT is not a verb the port itself may run, so it is handled here. */
    if (argc >= 2 && ieq(argv[1], "PORT")) {
        verb_name = "port";
        if (argc > 3) {
            usage();
            return PKG_RC_USAGE;
        }
        rc = pkg_port_serve(argc == 3 ? argv[2] : "PKG", run_verb);
        return rc == 0 ? PKG_RC_OK : PKG_RC_REFUSED;
    }
    return run_verb(argc, argv);
}
