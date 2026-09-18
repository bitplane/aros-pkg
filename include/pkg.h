/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * libpkg: Pkg as a library, for any program that wants to publish, install
 * or inspect packages without running the command line: a graphical front
 * end, an installer, an IDE plug-in, an agent's tool adapter. The `pkg`
 * command is one client of it among others, and adds nothing but argument
 * parsing, help text and the ARexx port.
 *
 * Each operation takes its options and a sink, and returns a code:
 *
 *   0        done;
 *   10..18   refused, the number naming the class (PKG_RC_*);
 *   20       the request itself is wrong (a missing option, say).
 *
 * The sink receives the answer in one of two forms, chosen by `structured`:
 *
 *   structured = 1   `record` is called once per field, in order, with the
 *                    same keys and values as the command line's MACHINE
 *                    output: a `result` field in every answer, and for a
 *                    refusal `result: refused`, `class`, `code`, `reason`,
 *                    and `next`, what to do now (stop, ask-requester,
 *                    fix-command, check-name, use-upgrade, use-install,
 *                    report). This is the form for programs.
 *   structured = 0   `text` is called with lines written for a person,
 *                    `is_error` set for refusals and warnings.
 *
 * Nothing is written to stdout or stderr, and no environment variable is
 * read: PKG_SIGNKEY, PKG_OUTPUT and PKG_TRACE belong to the command line.
 *
 * `trace`, when set, receives the operation's own account of itself, one
 * line per step, whatever the form: every file read, written, renamed or
 * removed, every check with what was expected and what was found, every
 * choice with its reason (the version picked, what satisfied a dependency,
 * which files were left out, where the architecture came from), and each
 * refusal as it happens. It is for finding out why an operation did what it
 * did; nothing in it is part of the contract, and its wording may change.
 *
 * A decision that belongs to whoever requested the operation (a person, or
 * a supervising agent that holds the authority) is never taken here. A
 * refusal whose `next` is ask-requester (a new signing key, a downgrade, a
 * file someone edited) goes to them; if they agree, the caller repeats the
 * operation with the option that expresses it (acceptkey, downgrade).
 *
 * Trust: a root pins, per package, the key that signed it the first time it
 * was installed there, and refuses another key later (14, ask-requester). The
 * key of a package's first version in a channel is presumed its
 * publisher's: a first install, or a publish, signed by another key is
 * refused the same way. `acceptkey`, with the other key in full, is how the
 * person's confirmation is passed.
 *
 * Linking: `make build/libpkg.a` builds everything, host layer included; a
 * program links that archive and nothing else.
 *
 * One operation runs at a time in a process: the library keeps the state of
 * the running operation in static storage, as the AmigaDOS tools it sits
 * beside do. A front end that runs operations in the background serialises
 * them.
 */

#ifndef PKG_H
#define PKG_H

#define PKG_API_VERSION 1   /* stays 1 until the first official release */
#define PKG_VERSION_STRING "0.3"   /* the tool's own version, as in its $VER */

enum {
    PKG_RC_OK         = 0,
    PKG_RC_REFUSED    = 10,  /* a refusal with no better class */
    PKG_RC_NOTFOUND   = 11,  /* package, version, installed entry or channel object absent */
    PKG_RC_INTEGRITY  = 12,  /* a digest, container, manifest or database disagrees */
    PKG_RC_SIGNATURE  = 13,  /* unsigned, malformed or invalid signature */
    PKG_RC_KEY        = 14,  /* signed by a key other than the one pinned */
    PKG_RC_CONFLICT   = 15,  /* something is already there */
    PKG_RC_DEPENDENCY = 16,  /* a dependency cannot be satisfied, or is still needed */
    PKG_RC_IO         = 17,  /* the filesystem refused */
    PKG_RC_POLICY     = 18,  /* allowed only with an explicit option, such as downgrade */
    PKG_RC_USAGE      = 20   /* the request is wrong */
};

/* Every callback runs on the thread that called the operation, before the
 * operation returns. The strings it receives are valid only during the
 * callback: copy what you keep. Any callback may be NULL; with structured
 * set, `text` is never called, and without it `record` and `item` are never
 * called. */
struct pkg_sink {
    void (*record)(void *user, const char *key, const char *value);
    void (*text)(void *user, int is_error, const char *text);
    void *user;
    int   structured;
    /* Optional: the operation's account of itself, see above. */
    void (*trace)(void *user, const char *line);
    /* Optional, structured form: a record that has several fields also
     * arrives here with its fields apart, so nothing has to split strings.
     * `kind` is the record's key; `keys` and `values` hold `n` fields, in the
     * order the joined value lists them. Each item arrives right after its
     * joined `record`, in the same order. The records concerned, and their
     * fields, are listed below; pkg_field finds one by name. */
    void (*item)(void *user, const char *kind, int n,
                 const char *const *keys, const char *const *values);
    /* Optional: asked between steps; non-zero stops the operation, which is
     * refused (10, next report) with anything it had placed taken back out,
     * files, records and the keys it pinned alike. It is asked once per
     * package while resolving, once before each package is placed, once
     * per entry while SHOW checks a channel, and once per package while
     * STATUS compares; placing one package's files is not interrupted. */
    int  (*cancel)(void *user);
};

/* Every option any operation takes; each operation reads the ones it needs
 * and ignores the rest. Strings are borrowed for the length of the call. */
struct pkg_options {
    const char *target;     /* the package, drawer, file or image operated on */
    const char *root;       /* the directory packages are installed into */
    const char *channel;    /* the directory packages are published into */
    const char *name, *version, *arch, *kind;  /* publish: identity, else from the drawer;
                                                  publish refuses a NULL kind (20) */
    const char *depends;    /* publish: "a >= 1.0, b" */
    const char *sign;       /* publish: the signing key file */
    const char *file;       /* keygen: the key file to create */
    const char *key;        /* sign: the key file */
    const char *out;        /* sign, image, mountlist: the file to write */
    const char *acceptkey;  /* the publisher key, in full, the person confirmed: install
                               from a new key, trust one of several, or publish with a new one */
    const char *unit;       /* mountlist: the fdsk.device unit, default 20 */
    const char *handler;    /* mountlist: the FFS handler's path, default found in the root */
    const char *files;      /* publish from "archive!/prefix": the paths under the prefix
                               that make this package, comma-separated; all when NULL.
                               The manifest then names the archive (Source) and the
                               channel keeps it as archives/<name> */
    const char *build;      /* publish: the build the files come from, such as a nightly's
                               date; the version becomes <version or $VER or 0>+<build>, and
                               a build whose files equal the last version's is not published */
    int downgrade;          /* upgrade: moving to an older version was asked for */
    int orphans;            /* remove: remove what nothing needs, instead of target */
    int dryrun;             /* every check, no write; results read would-... */
    int all;                /* upgrade: every package a newer version is offered for,
                               target NULL; refused (20) with version, downgrade or
                               acceptkey, which are decisions about one package */
};

/* What each operation answers in the structured form. `result` is always
 * there but not always first: look it up by key. A refusal is result
 * refused, class, code, reason, next, sometimes preceded by fields that
 * help (pinned, signer, first-signer on a key refusal; suggest on a name not
 * found). The return code and these fields agree, except SHOW, which answers
 * `shown` with every entry and returns the class of the first bad one.
 * Fields marked [item] also reach `item`, with the field names in brackets.
 *
 *   keygen     result created; file; public
 *   keyinfo    result shown; file; public                (file or target)
 *   sign       result signed; file; signer
 *   manifest   result shown; then each manifest line as a field (Name, ...)
 *   publish    result published: name, version, channel, manifest, payload,
 *              signer, files, [arch-from], [version-from], [name-from] (the
 *              file whose $VER gave the version or the name), [kind-from],
 *              [depends-from] (the published version they were taken from
 *              when kind or depends was not given), left-out per file.
 *              result unchanged: name, version (that exact content is there).
 *              result repaired: name, version, repaired per object written
 *              again (manifest, payload, signature), when that version's
 *              objects were damaged and the key is its publisher's.
 *   withdraw   result withdrawn (or unchanged): name, version, channel. The
 *              version stays; nothing picks it by default, asking for it
 *              is refused (18, ask-requester), and SHOW gives status
 *              withdrawn.
 *              dryrun: result would-publish, name, version, kind,
 *              architecture, channel, depends (or "none"), file per file,
 *              signer ("none" when no key was given: a dry run needs none),
 *              [first-signer] when keyless and the package exists, left-out;
 *              content (path size) per file inside the image, for kind
 *              image. Against the highest published version: compared-with
 *              (name version), then added (path size), changed (path
 *              oldsize newsize), same (path), gone (path) per file; inside
 *              the image for kind image.
 *   install    result installed: name, version, root, files, payload, signer,
 *              and image, blocks for an image; before it, one [item]
 *              dependency (name version) per dependency this install
 *              placed; those already there are not listed. Dependency
 *              items are sent only once every package is in place, so a
 *              refused or cancelled install sends none. A refused install
 *              into a root that did not exist may leave the root with an
 *              empty .pkg directory.
 *              result unchanged or kept: name, version.
 *   upgrade,   result upgraded, downgraded, rolled-back or unchanged: name,
 *   rollback   from, version, root, placed, removed, signer, with
 *              dependency items as for install.
 *   upgrade    with all set: every installed package whose channel offers a
 *   all        higher version, as upgrade would pick it (the root's CPU,
 *              withdrawn versions skipped), upgraded exactly as upgrade
 *              with that name would, a package before what depends on it.
 *              Per package done, [item] package (name from version), after
 *              its dependency items; then result upgraded, count. Nothing
 *              to do: result unchanged, count 0. A note per installed
 *              version withdrawn with nothing newer: never downgraded.
 *              Goes as far as possible: a package that needs a decision
 *              (a key change, an edited file) is not upgraded, [item]
 *              refused (name installed class reason, with code and next);
 *              one whose new version needs a refused one waits, [item]
 *              skipped (name installed waits-for); every other goes ahead.
 *              Then upgraded, not-upgraded, count, summary (a sentence);
 *              result upgraded or unchanged, or, when something was not
 *              upgraded, result refused with the class, code and next of
 *              the first refusal, which is also the exit code. Calling
 *              again once the requester has decided takes what waited.
 *   status     every installed package (or only target) against channel:
 *              result shown; [item] package (name installed available
 *              state), available "-" when the channel offers nothing for
 *              it; state current, upgradable (available is what upgrade
 *              would take), withdrawn (the installed version was withdrawn
 *              by its publisher, nothing newer offered), not-offered (the
 *              channel has no version for it), or edited (a file differs
 *              from what was installed, by size or digest, as verify
 *              checks; available higher means an upgrade is offered too).
 *              note when an installed withdrawn version has a newer one;
 *              count; upgradable, the packages upgrade all would attempt
 *              (upgradable, and edited ones with a higher available);
 *              hint how to upgrade them. Code 0 whether or not anything is
 *              upgradable. A channel directory that does not exist is
 *              refused (11), as for upgrade all, rather than read as
 *              offering nothing.
 *   list       result listed; [item] package (name version kind files
 *              reason), reason explicit or dependency; count. A root that
 *              does not exist lists nothing and succeeds.
 *   verify     name, version, files; changed and missing per file; moved
 *              (recorded path, where it is now) per file found elsewhere in
 *              the root with the same name and bytes; result intact, moved
 *              (every missing file found moved, nothing changed: code 0), or
 *              damaged with code 12.
 *   remove     result removed: name, version, root, removed, gone, kept per
 *              edited file left in place, [item] orphan (name version) per
 *              package nothing needs any more. With orphans set: [item]
 *              package (name version) per package removed, then result
 *              removed, count.
 *   image      result created: file, volume, blocks
 *   mountlist  result created (with out) or shown: name, image, blocks,
 *              highcyl, unit, handler, file, step per AmigaDOS command.
 *   show       result shown; [item] entry per channel entry, fields name,
 *              version, kind, architecture, status, signer, and, only when
 *              root is given, installed. status is ok, withdrawn or a class
 *              name; installed is installed, other-version or no; kind,
 *              architecture and signer are "-" when the entry cannot be
 *              read. [item] depends (package version needs min) per
 *              dependency, min empty when any version will do; [item] problem (package version reason)
 *              per bad entry; warning when a package has several signers;
 *              count; bad. A name the channel lacks shows count 0.
 *   any        hint: what usually comes next, or what to tell the person
 *              (after keygen: keep the key; publish into a channel that did
 *              not exist: it was created; install of an image and
 *              mountlist: how to mount it, a missing FFS handler). warning:
 *              something to check before going on. note: a fact worth
 *              passing on. None of them is ever a command that overrides a
 *              safeguard. Without MACHINE the CLI prints them as text.
 *   any        [item] suggest (name), before a not-found refusal. The
 *              refusal's reason names the same suggestions: it is complete
 *              by itself; the items are for offering them as choices.
 *
 * `next` takes the values stop, ask-requester, fix-command, check-name,
 * use-upgrade, use-install and report; a value added later is shown with
 * pkg_next_words. dryrun turns results into would-publish, would-install,
 * would-upgrade, would-downgrade, would-roll-back, would-remove,
 * would-create. INSTALL creates the root if it does not exist. */

int pkg_keygen   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_sign     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_keyinfo  (const struct pkg_sink *s, const struct pkg_options *o);  /* file: the public key it holds */
int pkg_manifest (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_publish  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_withdraw (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_install  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_upgrade  (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_rollback (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_list     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_verify   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_remove   (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_image    (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_mountlist(const struct pkg_sink *s, const struct pkg_options *o);
int pkg_show     (const struct pkg_sink *s, const struct pkg_options *o);
int pkg_status   (const struct pkg_sink *s, const struct pkg_options *o);  /* root, channel, [target] */

/* A refusal of the request itself, answered in the same form as the
 * operations' own: for a front end that validates its input first. Returns
 * PKG_RC_USAGE. `verb` names the operation, as in "install". */
int pkg_usage_error(const struct pkg_sink *s, const char *verb, const char *reason);

/* "not-found" for 11, and so on; "ok" for 0. */
const char *pkg_class_name(int code);

/* A sentence addressed to the person, for a `next` value, naming no command,
 * so any front end can show it; NULL or an unknown value gives the one for
 * report. (The command line's text form has its own wording.) */
const char *pkg_next_words(const char *next);

/* The value of field `key` among an item's fields, or NULL. */
const char *pkg_field(int n, const char *const *keys, const char *const *values, const char *key);

#endif
