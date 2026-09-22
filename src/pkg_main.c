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

#define _POSIX_C_SOURCE 200809L
#include "pkg.h"
#include "pkg_environment.h"
#include "pkg_selfupdate.h"
#include "pkg_activity.h"
#include "pkg_fs.h"
#include "pkg_manifest.h"
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
const char pkg_version_cookie[] = "$VER: pkg " PKG_VERSION_STRING " (" PKG_BUILD_DAY ")";

#ifdef __AROS__
static const int on_aros = 1;
#else
static const int on_aros = 0;
#endif
#ifdef __AROS__
#include <proto/dos.h>
#elif defined(_WIN32)
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

static const char *chosen_environment;
static int environment_system;
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

/* What the command line says on its own account, outside the library's
 * lines: one method per kind of thing being said, so that the look of a
 * question or of a note is decided in pkg_style.c and nowhere else. Before
 * these, the prompts went out through pkg_out and came out unstyled, which
 * is how a question ended up looking like a figure. In MACHINE mode a
 * question is a record like any other: nothing is asked there.
 *
 *   say_asked     a question, waiting for an answer on the same line
 *   say_quiet     a figure or a path that supports an answer
 *   say_noted     a remark worth reading, in passing
 *   say_done      the thing asked for happened */
static void say_kind_v(int kind, const char *key, const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (machine) {
        if (key != NULL) print_record(NULL, key, buf);
        return;
    }
    pkg_style_line(write_styled, kind, 0, buf);
}

#define SAY_METHOD(name, kind, key)                       \
    static void name(const char *fmt, ...)                \
    {                                                     \
        va_list ap;                                       \
        va_start(ap, fmt);                                \
        say_kind_v((kind), (key), fmt, ap);               \
        va_end(ap);                                       \
    }

SAY_METHOD(say_asked, PKG_LINE_QUESTION, "question")
SAY_METHOD(say_quiet, PKG_LINE_DETAIL,   NULL)
SAY_METHOD(say_noted, PKG_LINE_NOTE,     "note")
SAY_METHOD(say_done,  PKG_LINE_RESULT,   "result")

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
        { "AT",        offsetof(struct pkg_options, at) },
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
        { "INFO",      offsetof(struct pkg_options, info) },
        { "FROM",      offsetof(struct pkg_options, from) },
        { "UNPACKED",  offsetof(struct pkg_options, unpacked) }
};

static int takes_value(const char *w)
{
    size_t k;
    if (ieq(w, "TRACE") || ieq(w, "LOG") || ieq(w, "ENVIRONMENT"))
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
        /* The first word is the verb, never a keyword: "pkg VERSION MACHINE"
         * asks the version of a machine, and does not give VERSION the value
         * MACHINE the way "INSTALL x VERSION MACHINE" would. */
        if (i > 1 && takes_value(argv[i]))
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

/* INSTALL takes several names. They are kept here, since pkg_options only
 * borrows what it is given for the length of the call. */
static const char *also[64];

static int parse_args(int argc, char **argv, struct pkg_options *a)
{
    int i;
    size_t k;

    memset(a, 0, sizeof *a);
    for (i = 2; i < argc; i++) {
        const char **slot = NULL;
        /* CHANNEL ADD <channel>: the word after ADD or REMOVE is the
         * channel, even when it is spelled like a keyword. A directory
         * really can be called "channel" or "root", and the place of this
         * word says what it is. */
        if (strcmp(verb_name, "channel") == 0 && i == 3 && a->target != NULL
            && a->nalso == 0 && !ieq(a->target, "LIST")) {
            if (clean_value("the channel", argv[i]) != 0)
                return PKG_RC_USAGE;
            also[a->nalso++] = argv[i];
            a->also = also;
            continue;
        }
        if (ieq(argv[i], "ENVIRONMENT")) {
            if (i + 1 >= argc) return usage_errorf("ENVIRONMENT needs a name");
            if (clean_value("ENVIRONMENT", argv[i + 1]) != 0) return PKG_RC_USAGE;
            chosen_environment = argv[++i];
            continue;
        }
        if (strcmp(verb_name, "env") == 0 && ieq(argv[i], "SYSTEM")) {
            environment_system = 1;
            continue;
        }
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
            return usage_errorf("\"%s\": pkg keywords have no dashes and take their value "
                                "as the next word, as in ROOT <dir>%s%s", argv[i],
                                up[0] ? "; here perhaps " : "", up);
        } else if (a->target == NULL) {
            if (clean_value("the name", argv[i]) != 0)
                return PKG_RC_USAGE;
            a->target = argv[i];
        } else if ((strcmp(verb_name, "install") == 0 || strcmp(verb_name, "search") == 0
                    || strcmp(verb_name, "channel") == 0 || strcmp(verb_name, "env") == 0)
                   && a->nalso < sizeof also / sizeof also[0]) {
            /* INSTALL a b c: every name on the line, installed in turn.
             * SEARCH takes its words this way, and CHANNEL ADD its channel. */
            if (clean_value("the name", argv[i]) != 0)
                return PKG_RC_USAGE;
            also[a->nalso++] = argv[i];
            a->also = also;
        } else {
            return usage_errorf("unexpected argument \"%s\"", argv[i]);
        }
    }
    if (a->at != NULL && strcmp(verb_name, "install") != 0)
        return usage_errorf("AT belongs to INSTALL; later operations use the recorded destination");
    if (a->at != NULL && a->nalso)
        return usage_errorf("AT takes one package; install each application separately");
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
    { NULL, "INSTALL",   "<name>... ROOT <dir> [AT <dir>] [CHANNEL <dir|url>] [VERSION v] [ARCH cpu] [ACCEPTKEY <hex>] [UNPACKED <dir>]",
                         "install packages and what they depend on; several names go as far as they can" },
    { NULL, "STATUS",    "[<name>] ROOT <dir> [CHANNEL <dir|url>]",
                         "what is installed and what has a newer version; exit 0 either way" },
    { NULL, "UPGRADE",   "[<name>|ALL ROOT <dir>] [CHANNEL <dir|url>] [VERSION v] [ARCH cpu] [DOWNGRADE] [UNPACKED <dir>]",
                         "ALL takes every newer version, dependencies first, and goes as far as it can" },
    { NULL, "ROLLBACK",  "<name> ROOT <dir> [CHANNEL <dir|url>]",
                         "back to the version installed before" },
    { NULL, "LIST",      "ROOT <dir>", "what a root holds" },
    { NULL, "VERIFY",    "<name>|ALL ROOT <dir>", "every installed file against its signed manifest" },
    { NULL, "REPAIR",    "<name>|ALL ROOT <dir> [CHANNEL <dir|url>] [UNPACKED <dir>]", "put damaged files back" },
    { NULL, "REMOVE",    "[<name>|ORPHANS ROOT <dir>] [DRYRUN]",
                         "without a name: uninstall pkg; ORPHANS: what nothing needs any more" },
    { NULL, "SHOW",      "[<name>] [CHANNEL <dir|url>] [ROOT <dir>] [METADATA] [ARCHIVE <name>]",
                         "what a channel offers, each entry checked" },
    { NULL, "SEARCH",    "<word>... [CHANNEL <dir|url>] [ROOT <dir>] [ARCH cpu]",
                         "the packages every word matches; exit 0 whether or not any do" },
    { NULL, "CHANNEL",   "ADD|LIST|REMOVE [<dir|url|name>] [NAME <name>] ROOT <dir>",
                         "the channels this root reads when CHANNEL is left out, in order; "
                         "each takes a short name that CHANNEL <name> then stands for" },
    { NULL, "MOUNTLIST", "<image> ROOT <dir> [OUT <file>] [UNIT n] [HANDLER <path>]",
                         "the Mount entry for an installed image" },
    { "Publishing", NULL, NULL, NULL },
    { NULL, "KEYGEN",    "FILE <keyfile>", "a signing key, readable by you alone" },
    { NULL, "KEYINFO",   "FILE <keyfile> [SSH]", "the public key a key file holds; SSH: as ssh-ed25519" },
    { NULL, "MANIFEST",  "<drawer> [NAME n] [VERSION v] [ARCH a] [KIND k] [DEPENDS \"a >= 1, b\"] [INFO <file>]",
                         "the manifest PUBLISH would sign, to read before publishing" },
    { NULL, "PUBLISH",   "<drawer> CHANNEL <dir> KIND k [SIGN <keyfile>] [NAME n] [VERSION v] [ARCH a]",
                         NULL },
    { NULL, "",          "PACKAGE is the same verb under another name: nothing reaches a portal until PUSH",
                         NULL },
    { NULL, "",          "[DEPENDS \"a >= 1, b\"] [CONFIG \"S/Startup-Sequence\"] [FILES \"C,Libs\"] [BUILD <date>]",
                         NULL },
    { NULL, "",          "[INFO <file>]: a .pkginfo the port carries says what the package is; keywords win",
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
    usage_line("ENV        pkg ENV ADD <name> ROOT <dir> [SYSTEM]; ENV LIST; ENV REMOVE <name>; ENV DEFAULT <name>\n");
    usage_line("Roots      ROOT wins; ENVIRONMENT <name> selects a registered root. pkg u updates pkg itself; pkg REMOVE uninstalls it.\n");
    usage_line("Asking     pkg HELP shows this; pkg VERSION says which build this is\n");
    usage_line("%sAROS%s      pkg PORT [<portname>] serves every verb on an ARexx port, PKG by default\n", b, r);
    return PKG_RC_USAGE;
}

/* Environment files are optional. Prompts belong to the CLI; library callers
 * use the same configuration API and present their own choices. */
static int can_ask(void)
{
    if (machine || serving_port || !pkg_out_interactive(0)) return 0;
#ifdef __AROS__
    return Input() && IsInteractive(Input());
#elif defined(_WIN32)
    return _isatty(_fileno(stdin));
#else
    return isatty(STDIN_FILENO);
#endif
}

static int answer(char *buf, size_t len)
{
#ifdef __AROS__
    if (FGets(Input(), (STRPTR)buf, (LONG)len) == NULL) return -1;
#else
    if (fgets(buf, (int)len, stdin) == NULL) return -1;
#endif
    if (strchr(buf, '\n') == NULL && strlen(buf) + 1 == len) return -1;
    return clean_value("the answer", buf);
}

static void tell_root(const char *root, const char *source, const char *name)
{
    if (machine) {
        print_record(NULL, "selected-root", root);
        print_record(NULL, "root-source", source);
        if (name) print_record(NULL, "environment", name);
    } else {
        char line[8192];
        if (name) { snprintf(line, sizeof line, "Environment: %s\n", name); print_text(NULL, 1, line); }
        snprintf(line, sizeof line, "Root: %s\nSelected from: %s\n", root, source);
        print_text(NULL, 1, line);
    }
}

static int absolute_root(const char *path, char *buf, size_t len)
{
    if (!path || !*path) return usage_errorf("ROOT needs a non-empty directory");
#ifdef __AROS__
    if (!pkg_fs_fullpath(path, buf, len)) return usage_errorf("cannot resolve root %s", path);
#else
    int n;
    char cwd[4096];
#ifdef _WIN32
    if (path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':'))
#else
    if (path[0] == '/')
#endif
        n = snprintf(buf, len, "%s", path);
    else {
#ifdef _WIN32
        if (!_getcwd(cwd, sizeof cwd)) return usage_errorf("cannot read the current directory");
#else
        if (!getcwd(cwd, sizeof cwd)) return usage_errorf("cannot read the current directory");
#endif
        n = snprintf(buf, len, "%s/%s", cwd, path);
    }
    if (n < 0 || (size_t)n >= len) return usage_errorf("root path is too long");
#endif
    return 0;
}

static int environment_command(const struct pkg_options *a)
{
    struct pkg_environments e;
    char err[1024], root[4096];
    const char *action = a->target ? a->target : "LIST";
    const char *name = a->nalso == 1 ? a->also[0] : NULL;
    const char *path;
    size_t i;
    int rc = PKG_RC_USAGE;
    pkg_environments_init(&e);
    if (pkg_environments_load(&e, err, sizeof err) != 0) { usage_errorf("%s", err); goto end; }
    if (ieq(action, "LIST")) {
        if (a->nalso || a->root) { usage_errorf("ENV LIST takes no name or ROOT"); goto end; }
        if (machine) {
            if (e.system_path) print_record(NULL, "system-config", e.system_path);
            if (e.user_path) print_record(NULL, "user-config", e.user_path);
        } else {
            say_quiet("system configuration: %s", e.system_path ? e.system_path : "unavailable");
            say_quiet("personal configuration: %s", e.user_path ? e.user_path : "unavailable");
        }
        for (i = 0; i < e.count; i++) {
            if (environment_system && !e.items[i].system) continue;
            tell_root(e.items[i].root, e.items[i].source, e.items[i].name);
        }
        if (e.default_name) {
            if (machine) print_record(NULL, "default-environment", e.default_name);
            else say_quiet("default environment: %s", e.default_name);
        }
        if (!e.count && !machine)
            say_noted("no environment is registered; ROOT <directory> works without this configuration");
        rc = 0; goto end;
    }
    if (!name) { usage_errorf("ENV %s needs one environment name", action); goto end; }
    if (!ieq(action, "ADD") && !ieq(action, "REMOVE") && !ieq(action, "DEFAULT")) {
        usage_errorf("ENV takes ADD, LIST, REMOVE or DEFAULT"); goto end;
    }
    if (ieq(action, "ADD")) {
        if (absolute_root(a->root, root, sizeof root) != 0) goto end;
    } else if (a->root) { usage_errorf("ROOT belongs to ENV ADD"); goto end; }
    path = environment_system || on_aros ? e.system_path : e.user_path;
    if (!path) { usage_errorf("the configuration path for this scope is unavailable"); goto end; }
    if (machine) print_record(NULL, "configuration", path);
    else {
        say_quiet("environment configuration: %s", path);
        say_noted("this file names roots; a package's records stay in that root's .pkg drawer");
    }
    if (ieq(action, "ADD")) rc = pkg_environments_add(&e, name, root, environment_system || on_aros, err, sizeof err);
    else if (ieq(action, "REMOVE")) rc = pkg_environments_remove(&e, name, environment_system || on_aros, err, sizeof err);
    else rc = pkg_environments_default(&e, name, environment_system || on_aros, err, sizeof err);
    if (rc != 0) { usage_errorf("%s", err); rc = PKG_RC_USAGE; }
    else if (machine) print_record(NULL, "result", "configured");
    else say_done("environment configuration saved: %s", path);
end:
    pkg_environments_free(&e);
    return rc;
}

static int root_verb(const char *verb)
{
    return ieq(verb,"install") || ieq(verb,"upgrade") || ieq(verb,"rollback")
        || ieq(verb,"list") || ieq(verb,"verify") || ieq(verb,"repair")
        || ieq(verb,"remove") || ieq(verb,"mountlist") || ieq(verb,"status")
        || ieq(verb,"channel");
}

/* CHANNEL <word> where the word is neither a URL nor a directory that is
 * there: the root may know a channel by that name, and typing the name is
 * the whole point of having one. Resolved once, here, so that every verb
 * receives a channel and the library keeps taking addresses only. */
static char *channel_name_used;          /* freed at the end of the command */

static int a_name_not_a_place(const char *s)
{
    if (s == NULL || strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0)
        return 0;
    return !pkg_fs_is_dir(s);
}

static int resolve_channel_name(struct pkg_options *a)
{
    char names[600], *real;
    if (a->root == NULL || !a_name_not_a_place(a->channel))
        return 0;
    real = pkg_channel_named(a->root, a->channel, names, (unsigned long)sizeof names);
    if (real == NULL) {
        /* Not a name this root knows. The word is left as it was: the verb
         * refuses it as it always did, and says the names there are. */
        if (names[0] != '\0' && !pkg_fs_exists(a->channel))
            say_quiet("%s knows no channel named %s; it knows %s", a->root, a->channel, names);
        return 0;
    }
    say_quiet("channel: %s (%s)", real, a->channel);
    free(channel_name_used);
    channel_name_used = real;
    a->channel = real;
    return 0;
}

static int prepare_root(struct pkg_options *a, struct pkg_environments *e, char *buf, size_t len)
{
    const struct pkg_environment *selected = NULL;
    char err[1024];
    int rc, required = root_verb(verb_name);
    size_t i;
    if (a->root) { tell_root(a->root, "command line (ROOT)", NULL); return resolve_channel_name(a); }
    if (!required && !chosen_environment
        && !(ieq(verb_name,"show") || ieq(verb_name,"search")) && !a_name_not_a_place(a->channel))
        return 0;
    /* An explicit channel alone keeps the established catalogue-only use,
     * unless it is a word that may be a name, which needs a root to look in. */
    if (!required && a->channel && !chosen_environment && !a_name_not_a_place(a->channel))
        return 0;
    if (pkg_environments_load(e, err, sizeof err) != 0) return usage_errorf("%s; specify ROOT explicitly to choose a root", err);
    rc = pkg_environments_select(e, chosen_environment, &selected, err, sizeof err);
    if (rc == 0) {
        if (!pkg_fs_is_dir(selected->root)) return usage_errorf("root %s from %s is unavailable; mount it or specify ROOT", selected->root, selected->source);
        a->root = selected->root;
        tell_root(a->root, selected->source, selected->name);
        return resolve_channel_name(a);
    }
    if (rc < 0) return usage_errorf("%s", err);
    if (rc == 1 && !required && !chosen_environment) return 0;
    if (!can_ask()) {
        if (rc == 1 && !chosen_environment) return 0; /* existing missing-ROOT diagnostic */
        return usage_errorf("%s; specify ROOT <directory> or ENVIRONMENT <name>", err);
    }
    if (!e->count) {
        say_noted("no environment is registered");
        say_asked("Which root directory is this operation for?");
        if (answer(buf, len) != 0 || !*buf) return usage_errorf("no root chosen; specify ROOT <directory>");
        if (!pkg_fs_is_dir(buf)) return usage_errorf("root %s is unavailable", buf);
        a->root = buf; tell_root(buf, "interactive choice (not saved)", NULL);
        return resolve_channel_name(a);
    }
    for (i = 0; i < e->count; i++)
        say_quiet("%lu. %s: %s (%s)", (unsigned long)i + 1, e->items[i].name,
                  e->items[i].root, e->items[i].source);
    say_asked("Which root is this operation for? Its number:");
    if (answer(buf, len) != 0) return usage_errorf("no environment chosen");
    { char *end; unsigned long number = strtoul(buf, &end, 10);
      if (!*buf || *end || number == 0 || number > e->count) return usage_errorf("choose a listed number or specify ROOT explicitly");
      selected = &e->items[number - 1]; }
    if (!pkg_fs_is_dir(selected->root)) return usage_errorf("root %s is unavailable", selected->root);
    a->root = selected->root; tell_root(a->root, selected->source, selected->name);
    return resolve_channel_name(a);
}

/* Standalone maintenance accepts only its documented switches. */
static int self_options(int argc, char **argv, int allow_pkg)
{
    int i;
    for (i = 2; i < argc; i++) {
        if (ieq(argv[i], "DRYRUN") || ieq(argv[i], "MACHINE")) continue;
        if (ieq(argv[i], "TRACE") || ieq(argv[i], "LOG")) { i++; continue; }
        if (allow_pkg && ieq(argv[i], "pkg")) { allow_pkg = 0; continue; }
        return usage_errorf("keyword %s is not supported for this executable operation", argv[i]);
    }
    return 0;
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
        { "STATUS",    "status",    pkg_status },
        { "CHANNEL",   "channel",   pkg_channel },
        { "SEARCH",    "search",    pkg_search },
        /* PACKAGE is PUBLISH under the name a person who has not pushed yet
         * expects; the same operation, the same records. */
        { "PACKAGE",   "package",   pkg_publish }
    };
    struct pkg_options a;
    struct pkg_environments environments;
    char root_choice[4096];
    size_t i;
    int saved_machine = machine;
    int rc = PKG_RC_USAGE;
    const char *env = getenv("PKG_OUTPUT");

    chosen_environment = NULL;
    environment_system = 0;
    machine = env != NULL && ieq(env, "machine");
    if (wants_machine(argc, argv))
        machine = 1;
    out_sink.structured = machine;
    out_sink.line = machine ? NULL : print_line;
    pkg_style_init(!serving_port && pkg_out_interactive(0),
                   !serving_port && pkg_out_interactive(1), on_aros);
    /* The activity line's mark holds a middle dot: one Latin-1 byte on the
     * AROS console, two UTF-8 bytes under a UTF-8 locale, and a full stop
     * where neither can be trusted. */
    pkg_activity_charset(on_aros ? PKG_ACTIVITY_LATIN1
                         : pkg_style_caps(0)->utf8 ? PKG_ACTIVITY_UTF8
                         : PKG_ACTIVITY_ASCII);
    trace_path = getenv("PKG_TRACE");
    out_sink.trace = trace_path != NULL && *trace_path ? print_trace : NULL;
    /* A person at a terminal is shown the activity line while a step is
     * long. PKG_PROGRESS decides it outright when it is set: 1 draws it
     * wherever the output goes, anything else switches it off. */
    {
        const char *p = getenv("PKG_PROGRESS");
        out_sink.progress = !machine
                            && (p != NULL && *p != '\0' ? *p == '1' : pkg_fs_interactive());
    }
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
    /* Which build is this? The first question asked of a tool that did not
     * do what someone expected, and the answer to most of them. It used to
     * work by accident, the whole usage being printed for an unknown word
     * and the version being its first line; now the usage is not printed,
     * so the question is answered on purpose. */
    if (ieq(argv[1], "VERSION") || strcmp(argv[1], "--version") == 0
        || strcmp(argv[1], "-v") == 0) {
        if (machine) {
            print_record(NULL, "result", "version");
            print_record(NULL, "version", PKG_VERSION_STRING);
            print_record(NULL, "built", PKG_BUILD_DAY);
        } else {
            pkg_out("%s\n", pkg_version_cookie + 6);
        }
        machine = saved_machine;
        return PKG_RC_OK;
    }
    if (ieq(argv[1], "ENV")) {
        verb_name = "env";
        rc = parse_args(argc, argv, &a) != 0 ? PKG_RC_USAGE : environment_command(&a);
        machine = saved_machine;
        return rc;
    }
    if (ieq(argv[1], "U")) argv[1] = "UPGRADE";
    for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (ieq(argv[1], verbs[i].verb)) {
            verb_name = verbs[i].name;
            pkg_style_verb(verb_name);
            pkg_environments_init(&environments);
            rc = parse_args(argc, argv, &a);
            if (rc == 0 && ieq(verb_name, "upgrade") && !a.root && !a.all
                && (!a.target || ieq(a.target, "pkg")) && !chosen_environment) {
                if (serving_port || a.channel || a.version || a.arch || a.acceptkey || a.downgrade)
                    rc = usage_errorf("self-update uses its trusted channel; use ROOT for a managed package upgrade");
                else if ((rc = self_options(argc, argv, 1)) == 0) rc = pkg_selfupdate(&out_sink, a.dryrun);
            } else if (rc == 0 && ieq(verb_name, "remove") && !a.target && !a.root
                       && !chosen_environment) {
                if (serving_port || a.channel || a.version || a.arch || a.acceptkey || a.all)
                    rc = usage_errorf("self-removal takes REMOVE [DRYRUN]; name a package and ROOT for other removals");
                else if ((rc = self_options(argc, argv, 0)) == 0) rc = pkg_selfremove(&out_sink, a.dryrun);
            } else if (rc == 0) {
                rc = prepare_root(&a, &environments, root_choice, sizeof root_choice);
                if (rc == 0) rc = verbs[i].fn(&out_sink, &a);
            }
            if (rc < 0) rc = PKG_RC_USAGE;
            pkg_environments_free(&environments);
            pkg_style_flush(write_styled);
            machine = saved_machine;
            return rc;
        }
    }
    /* Not a verb. Name the word that was not understood and the verb it is
     * nearest to, instead of printing the whole usage and leaving the person
     * to find the difference. The version is part of the answer because a
     * verb this build does not have is usually a verb a later build does,
     * and a page of usage never says which case it is. */
    {
        static const char *const apart[] = { "ENV", "HELP", "PORT" };
        /* The words another tool would have taken, answered with the verb
         * pkg has for them and not with whatever they look like: UNINSTALL
         * is two edits from INSTALL, and installing is the opposite of what
         * the person came to do. */
        static const struct { const char *typed, *verb; } habit[] = {
            { "UPDATE",    "UPGRADE" }, { "UNINSTALL", "REMOVE"  },
            { "DELETE",    "REMOVE"  }, { "ERASE",     "REMOVE"  },
            { "ADD",       "INSTALL" }, { "GET",       "INSTALL" },
            { "FETCH",     "INSTALL" }, { "INFO",      "SHOW"    },
            { "FIND",      "SEARCH"  }, { "QUERY",     "SEARCH"  },
            { "LS",        "LIST"    }, { "SYNC",      "STATUS"  }
        };
        const char *near = NULL, *instead = NULL;
        size_t best = 99, j, typed_len;
        char typed[64], why[256];

        for (j = 0; j + 1 < sizeof typed && argv[1][j] != '\0'; j++)
            typed[j] = (char)toupper((unsigned char)argv[1][j]);
        typed[j] = '\0';
        typed_len = j;
        for (j = 0; j < sizeof habit / sizeof habit[0]; j++)
            if (strcmp(typed, habit[j].typed) == 0)
                instead = habit[j].verb;
        for (j = 0; instead == NULL && j < sizeof verbs / sizeof verbs[0] + sizeof apart / sizeof apart[0]; j++) {
            const char *cand = j < sizeof verbs / sizeof verbs[0]
                               ? verbs[j].verb : apart[j - sizeof verbs / sizeof verbs[0]];
            size_t len = strlen(cand), shorter = typed_len < len ? typed_len : len;
            size_t d = pkg_name_edits(typed, cand);
            /* The rule SHOW uses for a package name: in a short word two
             * edits make another word, not a slip. */
            if (d <= (shorter <= 4 ? 1u : 2u) && d < best) {
                best = d;
                near = cand;
            }
        }
        if (instead != NULL)
            snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; pkg says %s",
                     argv[1], PKG_VERSION_STRING, instead);
        else if (near != NULL)
            snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; did you mean %s?",
                     argv[1], PKG_VERSION_STRING, near);
        else
            /* Nothing near it: either the word is nonsense, or it is a verb
             * a later build has and this one does not. The second is what
             * brings people here, so the way out is part of the answer. */
            snprintf(why, sizeof why, "\"%.60s\" is not a verb of pkg %s; a later build may "
                     "have it, and pkg U updates pkg itself", argv[1], PKG_VERSION_STRING);
        pkg_usage_error(&out_sink, "pkg", why);
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
