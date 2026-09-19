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
#include "pkg_style.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AmigaDOS version cookie: `Version C:Pkg` reads it, and publishing Pkg
 * with Pkg takes its name and version from it. */
const char pkg_version_cookie[] = "$VER: Pkg " PKG_VERSION_STRING " (19.9.2026)";

#ifdef __AROS__
static const int on_aros = 1;
#else
static const int on_aros = 0;
#endif
static const char *verb_name = "pkg";
static int machine;
static int serving_port;   /* PORT: output is captured, never a terminal */

/* LOG <file>: everything printed is also appended to that file (AROS has
 * no tee), less the progress counter, which only a watching person needs. */
static const char *log_path;
static FILE *log_file;

static void to_log(const char *text)
{
    if (log_path == NULL || text[0] == '\r')
        return;
    if (log_file == NULL)
        log_file = fopen(log_path, "a");
    if (log_file != NULL) {
        fputs(text, log_file);
        fflush(log_file);
    }
}

static void print_record(void *user, const char *key, const char *value)
{
    (void)user;
    pkg_out("%s: %s\n", key, value);
    if (log_path != NULL) { to_log(key); to_log(": "); to_log(value); to_log("\n"); }
}

static void print_text(void *user, int is_error, const char *text)
{
    (void)user;
    if (is_error) pkg_err("%s", text); else pkg_out("%s", text);
    to_log(text);
}

/* The styled line to the screen, the plain one to the log. */
static void write_styled(int is_error, const char *styled, const char *plain)
{
    if (is_error) pkg_err("%s", styled); else pkg_out("%s", styled);
    to_log(plain);
}

static void print_line(void *user, int kind, int is_error, const char *text)
{
    (void)user;
    pkg_style_line(write_styled, kind, is_error, text);
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

static struct pkg_sink out_sink = { print_record, print_text, NULL, 0, NULL, NULL, NULL, 0, print_line };

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
        { "NAMESPACE", offsetof(struct pkg_options, nspace) },
        { "OUT",       offsetof(struct pkg_options, out) },
        { "ACCEPTKEY", offsetof(struct pkg_options, acceptkey) },
        { "DEPENDS",   offsetof(struct pkg_options, depends) },
        { "UNIT",      offsetof(struct pkg_options, unit) },
        { "HANDLER",   offsetof(struct pkg_options, handler) },
        { "FILES",     offsetof(struct pkg_options, files) },
        { "BUILD",     offsetof(struct pkg_options, build) },
        { "ARCHIVE",   offsetof(struct pkg_options, archive) },
        { "TO",        offsetof(struct pkg_options, to) },
        { "CONFIG",    offsetof(struct pkg_options, config) },
        { "UPSTREAM",  offsetof(struct pkg_options, upstream) },
        { "SHORT",     offsetof(struct pkg_options, short_desc) },
        { "DESCRIPTION", offsetof(struct pkg_options, description) },
        { "CATEGORY",  offsetof(struct pkg_options, category) },
        { "TAGS",      offsetof(struct pkg_options, tags) },
        { "AUTHOR",    offsetof(struct pkg_options, author) },
        { "HOMEPAGE",  offsetof(struct pkg_options, homepage) },
        { "REPOSITORY", offsetof(struct pkg_options, repository) },
        { "LICENSE",   offsetof(struct pkg_options, license) },
        { "DISTRIBUTION", offsetof(struct pkg_options, distribution) },
        { "CHANGES",   offsetof(struct pkg_options, changes) },
        { "ICON",      offsetof(struct pkg_options, icon) },
        { "SCREENSHOT", offsetof(struct pkg_options, screenshot) },
        { "README",    offsetof(struct pkg_options, readme) },
        { "FROM",      offsetof(struct pkg_options, from) }
};

static int takes_value(const char *w)
{
    size_t k;
    if (ieq(w, "TRACE") || ieq(w, "LOG"))
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

/* A value typed by a person, or pasted: surrounding spaces go, and a line
 * break, tab or other control character inside is refused, naming where, so
 * nothing invisible reaches a name, a path or a record. */
static int clean_value(const char *what, char *v)
{
    size_t n = strlen(v), i, lead = strspn(v, " ");
    while (n > lead && (v[n - 1] == ' ' || v[n - 1] == '\r' || v[n - 1] == '\n'))
        v[--n] = '\0';
    if (lead) memmove(v, v + lead, n - lead + 1);
    for (i = 0; v[i]; i++)
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] == 0x7F)
            return usage_errorf("%s holds a %s at character %lu; type it again without it",
                                what, v[i] == '\n' || v[i] == '\r' ? "line break" :
                                v[i] == '\t' ? "tab" : "control character", (unsigned long)i + 1);
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
        if ((strcmp(verb_name, "sign") == 0 || strcmp(verb_name, "keyinfo") == 0)
            && ieq(argv[i], "SSH")) {
            a->ssh = 1;
            continue;
        }
        if (strcmp(verb_name, "show") == 0 && ieq(argv[i], "METADATA")) {
            a->metadata = 1;
            continue;
        }
        /* A switch for UPGRADE and VERIFY only, so INSTALL all still names a package. */
        if ((strcmp(verb_name, "upgrade") == 0 || strcmp(verb_name, "verify") == 0
             || strcmp(verb_name, "repair") == 0) && ieq(argv[i], "ALL")) {
            a->all = 1;
            continue;
        }
        if (ieq(argv[i], "LOG")) {
            if (i + 1 >= argc)
                return usage_errorf("LOG needs a file to copy the output into");
            log_path = argv[++i];
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
            if (clean_value(argv[i], argv[i + 1]) != 0)
                return PKG_RC_USAGE;
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
            if (clean_value("the name", argv[i]) != 0)
                return PKG_RC_USAGE;
            a->target = argv[i];
        } else {
            return usage_errorf("unexpected argument \"%s\"", argv[i]);
        }
    }
    if (a->sign == NULL)
        a->sign = getenv("PKG_SIGNKEY");
    if (a->pushkey == NULL)
        a->pushkey = getenv("PKG_PUSHKEY");
    return 0;
}

static int usage_is_error = 1;

/* The usage text, drawn from a table so a terminal gets the verbs in bold
 * and the descriptions dimmed, and a pipe gets plain text. */
static const struct { const char *group, *verb, *args, *what; } usage_lines[] = {
    { "Installing and keeping software", NULL, NULL, NULL },
    { NULL, "INSTALL",   "<name> ROOT <dir> CHANNEL <dir|url> [VERSION v] [ARCH cpu] [ACCEPTKEY <hex>]",
                         "install a package and what it depends on" },
    { NULL, "STATUS",    "[<name>] ROOT <dir> CHANNEL <dir|url>",
                         "what is installed and what has a newer version; exit 0 either way" },
    { NULL, "UPGRADE",   "<name>|ALL ROOT <dir> CHANNEL <dir|url> [VERSION v] [ARCH cpu] [DOWNGRADE]",
                         "ALL takes every newer version, dependencies first, and goes as far as it can" },
    { NULL, "ROLLBACK",  "<name> ROOT <dir> CHANNEL <dir|url>",
                         "back to the version installed before" },
    { NULL, "LIST",      "ROOT <dir>", "what a root holds" },
    { NULL, "VERIFY",    "<name>|ALL ROOT <dir>", "every installed file against its signed manifest" },
    { NULL, "REPAIR",    "<name>|ALL ROOT <dir> CHANNEL <dir|url>", "put damaged files back" },
    { NULL, "REMOVE",    "<name>|ORPHANS ROOT <dir>",
                         "take a package out; ORPHANS: what nothing needs any more" },
    { NULL, "SHOW",      "[<name>] CHANNEL <dir|url> [ROOT <dir>] [METADATA] [ARCHIVE <name>]",
                         "what a channel offers, each entry checked" },
    { NULL, "MOUNTLIST", "<image> ROOT <dir> [OUT <file>] [UNIT n] [HANDLER <path>]",
                         "the Mount entry for an installed image" },
    { "Publishing", NULL, NULL, NULL },
    { NULL, "KEYGEN",    "FILE <keyfile>", "a signing key, readable by you alone" },
    { NULL, "KEYINFO",   "FILE <keyfile> [SSH]", "the public key a key file holds; SSH: as ssh-ed25519" },
    { NULL, "MANIFEST",  "<drawer> [NAME n] [VERSION v] [ARCH a] [KIND k] [DEPENDS \"a >= 1, b\"]",
                         "the manifest PUBLISH would sign, to read before publishing" },
    { NULL, "PUBLISH",   "<drawer> CHANNEL <dir> KIND k [SIGN <keyfile>] [NAME n] [VERSION v] [ARCH a]",
                         NULL },
    { NULL, "",          "[DEPENDS \"a >= 1, b\"] [CONFIG \"S/Startup-Sequence\"] [FILES \"C,Libs\"] [BUILD <date>]",
                         "publish a drawer as a version; the channel is created when missing" },
    { NULL, "WITHDRAW",  "<name> VERSION v CHANNEL <dir> [SIGN <keyfile>]",
                         "a version nothing installs any more" },
    { NULL, "SIGN",      "<file> KEY <keyfile> OUT <sigfile> [SSH NAMESPACE <ns>]",
      "a detached signature; SSH: one ssh-keygen -Y verify checks" },
    { NULL, "CHECKSIG",  "<file> FILE <sigfile> [KEY <public key>]", "whether SIGN's signature over a file is good, and whose it is" },
    { NULL, "IMAGE",     "<drawer> OUT <file> [NAME <volume>]", "an FFS volume image of a drawer" },
    { NULL, "PUSH",      "CHANNEL <dir> TO <url> [SIGN <keyfile>]", "a channel to the portal: https with PKG_PUSHKEY, or signed requests" },
    { "On any verb", NULL, NULL, NULL },
    { NULL, "DRYRUN",    "", "every check, no write" },
    { NULL, "MACHINE",   "", "key: value lines for a program; the exit code names the class of a refusal" },
    { NULL, "TRACE",     "", "<file>: every step, file, check and choice, for finding out why; - for stderr" },
    { NULL, "LOG",       "", "<file>: a copy of the output" },
    { NULL, NULL, NULL, NULL }
};

static void usage_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (usage_is_error) pkg_verr(fmt, ap);
    else { char buf[2048]; vsnprintf(buf, sizeof buf, fmt, ap); pkg_out("%s", buf); }
    va_end(ap);
}

static int usage(void)
{
    int e = usage_is_error;
    const char *b = pkg_style_sgr(e, "1"), *d = pkg_style_sgr(e, "2"), *r = pkg_style_sgr(e, "0");
    size_t i;
    usage_line("%s%s%s  the AROS package tool\n", b, pkg_version_cookie + 6, r);
    usage_line("%susage:%s pkg VERB [<name>] KEYWORD <value> ...   keywords in any order, any case\n",
               d, r);
    for (i = 0; usage_lines[i].group || usage_lines[i].verb; i++) {
        if (usage_lines[i].group) {
            usage_line("\n%s%s%s\n", b, usage_lines[i].group, r);
        } else {
            if (usage_lines[i].args[0] == '\0' && usage_lines[i].what)
                usage_line("  %s%-9s%s %s%s%s\n", b, usage_lines[i].verb, r, d, usage_lines[i].what, r);
            else if (usage_lines[i].verb[0] == '\0')
                usage_line("            %s\n", usage_lines[i].args);
            else {
                usage_line("  %s%-9s%s %s\n", b, usage_lines[i].verb, r, usage_lines[i].args);
                if (usage_lines[i].what)
                    usage_line("            %s%s%s\n", d, usage_lines[i].what, r);
            }
        }
    }
    usage_line("\n%sKinds%s     image (a program on one volume to mount), application (loose files), library,\n"
               "          device, class, font, catalog, startup, boot, data, sdk, slave\n", b, r);
    usage_line("%sSettings%s  PKG_SIGNKEY the key SIGN defaults to; PKG_PUSHKEY; PKG_OUTPUT=machine;\n"
               "          PKG_TRACE=<file>; PKG_COLOR=always|never; PKG_PROGRESS=1\n", b, r);
    usage_line("%sExit code%s 0 done; 10 to 18 refused, the number is the class; 20 a wrong command\n", b, r);
    usage_line("%sAROS%s      pkg PORT [<portname>] serves every verb on an ARexx port, PKG by default\n", b, r);
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
        { "CHECKSIG",  "checksig",  pkg_checksig },
        { "KEYINFO",   "keyinfo",   pkg_keyinfo },
        { "INSTALL",   "install",   pkg_install },
        { "UPGRADE",   "upgrade",   pkg_upgrade },
        { "ROLLBACK",  "rollback",  pkg_rollback },
        { "LIST",      "list",      pkg_list },
        { "VERIFY",    "verify",    pkg_verify },
        { "REPAIR",    "repair",    pkg_repair },
        { "RESOLVE",   "resolve",   pkg_resolve },
        { "REMOVE",    "remove",    pkg_remove },
        { "IMAGE",     "image",     pkg_image },
        { "MOUNTLIST", "mountlist", pkg_mountlist },
        { "SHOW",      "show",      pkg_show },
        { "PUSH",      "push",      pkg_push },
        { "STATUS",    "status",    pkg_status }
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
    out_sink.line = machine ? NULL : print_line;
    pkg_style_init(!serving_port && pkg_out_interactive(0),
                   !serving_port && pkg_out_interactive(1), on_aros);
    trace_path = getenv("PKG_TRACE");
    out_sink.trace = trace_path != NULL && *trace_path ? print_trace : NULL;
    /* a person at a terminal sees long steps count; PKG_PROGRESS=1 asks for it anywhere */
    out_sink.progress = !machine && ((getenv("PKG_PROGRESS") && *getenv("PKG_PROGRESS") == '1')
                                     || pkg_fs_interactive());
    if (argc < 2) {
        if (machine) pkg_usage_error(&out_sink, "pkg", "no verb given");
        else usage();
        machine = saved_machine;
        return PKG_RC_USAGE;
    }
    if (ieq(argv[1], "HELP") || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        /* Asked for: to stdout, and a success. */
        usage_is_error = 0;
        usage();
        usage_is_error = 1;
        machine = saved_machine;
        return PKG_RC_OK;
    }
    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (ieq(argv[1], verbs[i].verb)) {
            verb_name = verbs[i].name;
            pkg_style_verb(verb_name);
            rc = parse_args(argc, argv, &a) != 0 ? PKG_RC_USAGE : verbs[i].fn(&out_sink, &a);
            pkg_style_flush(write_styled);
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

static int pkg_main(int argc, char **argv)
{
    int rc;

    if (pkg_host_args(&argc, &argv) != 0) {
        pkg_err("pkg: cannot read the command line\n");
        return PKG_RC_IO;
    }

    /* PORT is not a verb the port itself may run, so it is handled here. */
    if (argc >= 2 && ieq(argv[1], "PORT")) {
        verb_name = "port";
        serving_port = 1;
        if (argc > 3) {
            usage();
            return PKG_RC_USAGE;
        }
        rc = pkg_port_serve(argc == 3 ? argv[2] : "PKG", run_verb);
        return rc == 0 ? PKG_RC_OK : PKG_RC_REFUSED;
    }
    return run_verb(argc, argv);
}

#ifdef __AROS__
#include <proto/exec.h>
#include <exec/tasks.h>

/* The Shell gives a command 40 KB of stack unless someone typed Stack, and
 * AROS's startup has no convention for a program to ask for more. Publishing
 * (the payload, the archive readers, the manifest) goes well past that, and
 * a stack overrun on AROS is a Software Failure, not a refusal. So Pkg runs
 * on a stack of its own whenever the one it was given is smaller. */
#define PKG_STACK_BYTES (1024ul * 1024ul)

struct entry_args { int argc; char **argv; };

static IPTR on_own_stack(struct entry_args *a)
{
    return (IPTR)pkg_main(a->argc, a->argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    struct entry_args a;
    UBYTE *stack;
    int rc;

    if ((IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower >= PKG_STACK_BYTES)
        return pkg_main(argc, argv);
    stack = (UBYTE *)AllocVec(PKG_STACK_BYTES, MEMF_ANY);
    if (stack == NULL) {
        pkg_err("pkg: not enough memory for a %lu KB stack\n", PKG_STACK_BYTES / 1024ul);
        return PKG_RC_IO;
    }
    a.argc = argc;
    a.argv = argv;
    sss.stk_Lower = stack;
    sss.stk_Upper = stack + PKG_STACK_BYTES;
    sss.stk_Pointer = sss.stk_Upper;
    ssa.Args[0] = (IPTR)&a;
    rc = (int)NewStackSwap(&sss, on_own_stack, &ssa);
    FreeVec(stack);
    return rc;
}
#else
int main(int argc, char **argv)
{
    return pkg_main(argc, argv);
}
#endif
