/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * STATUS and UPGRADE ALL, and the CHANNEL command over the list a root
 * keeps.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- keeping a root current: STATUS and UPGRADE ALL ------------------- *
 *
 * Pkg carries no scheduler and no daemon: a person, a startup script or any
 * scheduler runs these. STATUS compares every installed package with a
 * channel, choosing as UPGRADE chooses; UPGRADE ALL upgrades each package a
 * newer version is offered for, exactly as UPGRADE <name> would, a package
 * before what depends on it, and stops at the first refusal. Neither asks
 * anything, ever: what belongs to the requester (going back a version, a
 * new key, an edited file) is refused or reported, never decided. */
struct planned_up *planned;
size_t nplanned;

const char *planned_version(const char *name)
{
    size_t i;
    for (i = 0; i < nplanned; i++)
        if (strcmp(planned[i].name, name) == 0)
            return planned[i].version;
    return NULL;
}

/* The machine UPGRADE <name> chooses for: the root's, else the installed
 * package's own CPU. */
void arch_for(const char *base, const struct pkg_manifest *m)
{
    target_arch = base;
    if (target_arch == NULL && strcmp(m->architecture, "generic") != 0)
        target_arch = m->architecture;
}

void stand(const char *root, const struct index *ix, const char *base,
           const struct pkg_manifest *m, struct standing *s)
{
    size_t i;
    memset(s, 0, sizeof *s);
    s->m = m;
    arch_for(base, m);
    for (i = 0; i < ix->n; i++) {
        const struct entry *e = &ix->e[i];
        if (strcmp(e->name, m->name) == 0
            && pkg_version_cmp(e->version, m->version) == 0
            && (strcmp(e->arch, m->architecture) == 0 || strcmp(e->arch, "generic") == 0)
            && is_withdrawn(e))
            s->withdrawn = 1;
    }
    s->offer = pick(ix, m->name, NULL);
    if (s->offer == NULL && pick_refused) {
        /* Reported per package: one package two channels disagree about
         * must not stop the others being compared or upgraded. */
        s->conflict = 1;
        snprintf(s->why, sizeof s->why, "%s", pick_why);
        refused_class = 0;
        refused_next = NULL;
    }
    s->newer = s->offer != NULL && pkg_version_cmp(s->offer->version, m->version) > 0;
    /* The check VERIFY makes, size and digest, stopping at the first edit. */
    for (i = 0; i < m->nfiles && !s->edited; i++)
        s->edited = file_state(root, m->files[i].path, m->files[i].digest, m->files[i].size) == 1;
    if (s->conflict)       s->state = "conflict";
    else if (s->offer == NULL)  s->state = s->withdrawn ? "withdrawn" : "not-offered";
    else if (s->newer)     s->state = s->edited ? "edited" : "upgradable";
    else if (s->withdrawn) s->state = "withdrawn";
    else if (s->edited)    s->state = "edited";
    else                   s->state = "current";
    tr("%s %s: %s, the channel offers %s", m->name, m->version, s->state,
       s->offer ? s->offer->version : "nothing for it");
}

void note(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (machine)
        kv("note", "%s", buf);
    else
        say_note("%s", buf);
}

/* ROOT and CHANNEL, the root's machine, the channel's index and every
 * installed package. */
int keep_current_setup(const struct pkg_options *a, struct index *ix, struct installed *in)
{
    if (a->root == NULL)    return refuse_c(20, "name the root with ROOT <dir>");
    if (a->channel != NULL && !is_url(a->channel) && !pkg_fs_is_dir(a->channel))
        return refuse_c(11, "there is no channel at %s: not mounted, or not the path meant; "
                        "nothing was checked or changed", a->channel);
    if (resolve_arch(a) != 0) return 1;
    if (open_channels(a, ix) != 0) return 1;
    if (load_all(a->root, in) != 0) { free(ix->e); return 1; }
    return 0;
}

/* ---- CHANNEL: the list a root keeps ------------------------------------ */

/* The distinct package names a channel offers, for what ADD says back. */
static size_t offered_count(const struct index *ix)
{
    size_t i, j, n = 0;
    for (i = 0; i < ix->n; i++) {
        int seen = 0;
        for (j = 0; j < i && !seen; j++)
            if (strcmp(ix->e[j].name, ix->e[i].name) == 0)
                seen = 1;
        if (!seen)
            n++;
    }
    return n;
}

int cmd_channel(const struct pkg_options *a)
{
    struct chanlist cl;
    const char *what = a->target, *ch = a->nalso > 0 ? a->also[0] : NULL;
    char named[64];
    size_t i;
    int add, remove, list;

    if (what == NULL)
        return refuse_c(20, "CHANNEL takes ADD, LIST or REMOVE");
    add = ascii_casecmp(what, "ADD") == 0;
    remove = ascii_casecmp(what, "REMOVE") == 0;
    list = ascii_casecmp(what, "LIST") == 0;
    if (!add && !remove && !list)
        return refuse_c(20, "\"%s\" is not one of CHANNEL's words; they are ADD, LIST and REMOVE",
                        what);
    if (a->root == NULL)
        return refuse_c(20, "name the root whose channels these are with ROOT <dir>");
    if ((add || remove) && ch == NULL)
        return refuse_c(20, "CHANNEL %s takes the channel: a directory, an http(s) URL, or the "
                        "name this root knows one by", add ? "ADD" : "REMOVE");
    if (a->nalso > 1)
        return refuse_c(20, "CHANNEL takes one channel at a time");
    if (a->name != NULL && !add)
        return refuse_c(20, "NAME belongs to CHANNEL ADD: it is the short name this root will "
                        "know the channel by");
    if (a->name != NULL && !chan_name_ok(a->name))
        return refuse_c(20, "\"%s\" cannot be a channel's name here: letters, digits, a dash, an "
                        "underscore or a dot, up to 63 of them, and nothing that reads as a path",
                        a->name);
    if (chanlist_read(a->root, &cl) != 0)
        return 1;

    /* The name the channel will answer to: the one asked for, or the last
     * part of its address, which is what the channel is called anyway. A
     * name already taken here is refused rather than made unique behind the
     * person's back: two channels answering to one word is the confusion
     * this feature exists to remove. */
    named[0] = '\0';
    if (add) {
        if (a->name != NULL)
            snprintf(named, sizeof named, "%s", a->name);
        else
            chan_name_of(ch, named, sizeof named);
        for (i = 0; named[0] && i < cl.n; i++)
            if (cl.name[i] != NULL && ascii_casecmp(cl.name[i], named) == 0) {
                if (a->name != NULL) {
                    char taken[1100];
                    snprintf(taken, sizeof taken, "%s", cl.v[i]);
                    chanlist_free(&cl);
                    return refuse_n(15, "fix-command", "%s already knows a channel as %s: %s. "
                                    "Give this one another name with NAME <name>, or remove "
                                    "that one first", a->root, named, taken);
                }
                named[0] = '\0';   /* it named itself, and the name is taken: leave it unnamed */
            }
    }

    if (list) {
        kv("result", "shown");
        if (!machine && cl.n > 0) {
            static const int widths[] = { 4, 12, 0 };
            tbl_head(widths, "In\tName\tChannel");
        }
        for (i = 0; i < cl.n; i++) {
            if (machine) {
                /* The address keeps the record it has always had, so what
                 * reads these is untouched; the name follows it on its own
                 * line, for a script that would rather use it. */
                rec_item("channel", cl.v[i], "channel", cl.v[i],
                         cl.name[i] ? "name" : NULL, cl.name[i], NULL);
                if (cl.name[i] != NULL)
                    kv("channel-name", "%s", cl.name[i]);
            }
            else
                tbl_row("%lu\t%s\t%s", (unsigned long)i + 1,
                        cl.name[i] ? cl.name[i] : "-", cl.v[i]);
        }
        if (!machine && cl.n > 0)
            tbl_end();
        kv("count", "%lu", (unsigned long)cl.n);
        kv("root", "%s", a->root);
        if (cl.n == 0) {
            kv("summary", "%s lists no channel", a->root);
            if (!machine)
                say_result("%s lists no channel", a->root);
            hint("CHANNEL ADD <dir|url> ROOT %s adds one; from then on INSTALL, UPGRADE, STATUS, "
                 "SEARCH and SHOW read it without CHANNEL on the line", a->root);
        } else {
            kv("summary", "%lu channel%s, asked in this order",
               (unsigned long)cl.n, cl.n == 1 ? "" : "s");
            if (!machine)
                say_result("%lu channel%s in %s, asked in this order", (unsigned long)cl.n,
                           cl.n == 1 ? "" : "s", a->root);
        }
        chanlist_free(&cl);
        return 0;
    }

    /* REMOVE takes either what was added or the name it answers to, since
     * the name is what the person has been typing since. */
    for (i = 0; i < cl.n; i++)
        if (strcmp(cl.v[i], ch) == 0
            || (remove && cl.name[i] != NULL && ascii_casecmp(cl.name[i], ch) == 0))
            break;
    if (add) {
        size_t offers = 0;
        if (i < cl.n) {
            chanlist_free(&cl);
            return refuse_c(15, "%s already lists the channel %s, in place %lu; nothing was "
                            "changed. CHANNEL LIST ROOT %s shows them", a->root, ch,
                            (unsigned long)i + 1, a->root);
        }
        if (cl.n >= PKG_MAX_CHANNELS) {
            chanlist_free(&cl);
            return refuse_c(15, "%s already lists %d channels, which is as many as pkg reads at "
                            "once; remove one first", a->root, PKG_MAX_CHANNELS);
        }
        if (!dry_run) {
            /* A channel that cannot be read is not added: the mistake is
             * found now, not at the next INSTALL. */
            struct index ix;
            if (!is_url(ch) && !pkg_fs_is_dir(ch)) {
                chanlist_free(&cl);
                return refuse_c(11, "there is no channel at %s: not mounted, or not the path "
                                "meant; nothing was added", ch);
            }
            if (read_index(ch, &ix) != 0) { chanlist_free(&cl); return 1; }
            offers = offered_count(&ix);
            free(ix.e);
        }
        cl.v[cl.n] = pkg_strdup(ch);
        cl.name[cl.n] = named[0] ? pkg_strdup(named) : NULL;
        if (cl.v[cl.n] == NULL || (named[0] && cl.name[cl.n] == NULL)) {
            chanlist_free(&cl);
            return refuse("out of memory");
        }
        cl.n++;
        if (!dry_run && chanlist_write(a->root, &cl) != 0) {
            chanlist_free(&cl);
            return refuse_c(17, "cannot write the channel list in %s: %s", a->root,
                            strerror(errno));
        }
        kv("result", "%s", res("added", "would-add"));
        kv("channel", "%s", ch);
        if (named[0]) kv("name", "%s", named);
        kv("root", "%s", a->root);
        kv("position", "%lu", (unsigned long)cl.n);
        if (!dry_run)
            kv("packages", "%lu", (unsigned long)offers);
        if (!machine) {
            if (dry_run)
                say_result("would add %s to %s, in place %lu", ch, a->root, (unsigned long)cl.n);
            else
                say_result("added %s to %s, in place %lu: it offers %lu package%s", ch, a->root,
                           (unsigned long)cl.n, (unsigned long)offers, offers == 1 ? "" : "s");
        }
        /* Worth saying only when the name is shorter than what it stands
         * for: "CHANNEL channel says the same as CHANNEL channel" helps
         * nobody. */
        if (named[0] && strcmp(named, ch) != 0)
            hint("this root now knows it as %s: CHANNEL %s says the same as CHANNEL %s", named,
                 named, ch);
        if (cl.n == 1)
            hint("INSTALL, UPGRADE, STATUS, SHOW, REPAIR, ROLLBACK and SEARCH now read this "
                 "channel when CHANNEL is left out; CHANNEL <dir|url> on the line still means "
                 "that channel alone");
        chanlist_free(&cl);
        return 0;
    }

    /* REMOVE */
    if (i == cl.n) {
        chanlist_free(&cl);
        return refuse_c(11, "%s does not list the channel %s; nothing was changed. CHANNEL LIST "
                        "ROOT %s shows the ones it lists", a->root, ch, a->root);
    }
    {
        /* What is said afterwards names the channel itself, whether the
         * person typed it or the name it answers to. */
        static char gone[1100];
        snprintf(gone, sizeof gone, "%s", cl.v[i]);
        ch = gone;
    }
    free(cl.v[i]);
    free(cl.name[i]);
    for (; i + 1 < cl.n; i++) {
        cl.v[i] = cl.v[i + 1];
        cl.name[i] = cl.name[i + 1];
    }
    cl.n--;
    if (!dry_run && chanlist_write(a->root, &cl) != 0) {
        chanlist_free(&cl);
        return refuse_c(17, "cannot write the channel list in %s: %s", a->root, strerror(errno));
    }
    kv("result", "%s", res("removed", "would-remove"));
    kv("channel", "%s", ch);
    kv("root", "%s", a->root);
    kv("count", "%lu", (unsigned long)cl.n);
    if (!machine)
        say_result("%s %s from %s; %lu channel%s left", dry_run ? "would remove" : "removed", ch,
                   a->root, (unsigned long)cl.n, cl.n == 1 ? "" : "s");
    note("what was installed from it stays installed; nothing was removed from this root");
    chanlist_free(&cl);
    return 0;
}
