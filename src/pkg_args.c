/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * See pkg_args.h: a command's words read against its template.
 */

#include "pkg_args.h"
#include "pkg_manifest.h"   /* pkg_name_edits: one rule for "did you mean" */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int same_n(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (b[i] == '\0' || toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 0;
    return b[n] == '\0';
}

static int capitals(const char *w)
{
    int letters = 0;
    for (; *w; w++) {
        if (islower((unsigned char)*w)) return 0;
        if (isupper((unsigned char)*w)) letters++;
    }
    return letters >= 2;
}

static int fail(char *err, size_t len, const char *fmt, const char *a, const char *b, const char *c)
{
    snprintf(err, len, fmt, a, b ? b : "", c ? c : "");
    return -1;
}

/* ---- the template ----------------------------------------------------- */

int pkg_args_template(const char *tmpl, struct pkg_args *out, char *err, size_t errlen)
{
    const char *p = tmpl;
    int multi = 0;

    memset(out, 0, sizeof *out);
    while (*p) {
        struct pkg_arg *it;
        size_t n = strcspn(p, "/,=");
        if (out->n >= PKG_ARGS_ITEMS)
            return fail(err, errlen, "template has more than %s items%s%s", "48", NULL, NULL);
        if (n == 0 || n >= sizeof out->item[0].name)
            return fail(err, errlen, "template item \"%.20s\" has no name or too long a one%s%s", p, NULL, NULL);
        it = &out->item[out->n++];
        memcpy(it->name, p, n);
        it->name[n] = '\0';
        {
            /* a name is a word in capitals: anything else is a template
             * that was cut in the wrong place */
            size_t k;
            for (k = 0; k < n; k++)
                if (!isupper((unsigned char)it->name[k]) && !isdigit((unsigned char)it->name[k]))
                    return fail(err, errlen, "template item \"%s\" is not a word in capitals%s%s", it->name, NULL, NULL);
        }
        p += n;
        while (*p == '/') {
            switch (toupper((unsigned char)p[1])) {
            case 'K': it->flags |= PKG_ARG_KEY; break;
            case 'S': it->flags |= PKG_ARG_SWITCH; break;
            case 'A': it->flags |= PKG_ARG_NEEDED; break;
            case 'M': it->flags |= PKG_ARG_MULTI; break;
            case 'W': it->flags |= PKG_ARG_WORD; break;
            case 'R': it->flags |= PKG_ARG_ROOTED; break;
            case 'G': it->flags |= PKG_ARG_GLOBAL; break;
            default:
                return fail(err, errlen, "template item %s has an unknown modifier /%.1s%s", it->name, p + 1, NULL);
            }
            p += 2;
        }
        if (*p == '=' && p[1] == '"') {
            /* a shown value in quotes may hold commas: "a >= 1, b" */
            const char *close = strchr(p + 2, '"');
            size_t m;
            if (close == NULL)
                return fail(err, errlen, "template item %s opens a quote it does not close%s%s", it->name, NULL, NULL);
            m = (size_t)(close + 1 - (p + 1));
            if (m >= sizeof it->shown)
                return fail(err, errlen, "template item %s shows too long a value%s%s", it->name, NULL, NULL);
            memcpy(it->shown, p + 1, m);
            it->shown[m] = '\0';
            p = close + 1;
        } else if (*p == '=') {
            size_t m = strcspn(p + 1, ",");
            if (m >= sizeof it->shown)
                return fail(err, errlen, "template item %s shows too long a value%s%s", it->name, NULL, NULL);
            memcpy(it->shown, p + 1, m);
            it->shown[m] = '\0';
            p += 1 + m;
        }
        if ((it->flags & PKG_ARG_KEY) && (it->flags & PKG_ARG_SWITCH))
            return fail(err, errlen, "template item %s is both a keyword and a switch%s%s", it->name, NULL, NULL);
        if (it->flags & PKG_ARG_MULTI) {
            if (multi++)
                return fail(err, errlen, "template has two /M items; %s is the second%s%s", it->name, NULL, NULL);
        }
        if (*p == ',')
            p++;
        else if (*p != '\0')
            return fail(err, errlen, "template item %s is not followed by a comma%s%s", it->name, NULL, NULL);
    }
    return 0;
}

struct pkg_arg *pkg_args_item(struct pkg_args *a, const char *name)
{
    size_t i;
    for (i = 0; i < a->n; i++)
        if (same(a->item[i].name, name))
            return &a->item[i];
    return NULL;
}

static int by_place(const struct pkg_arg *it)
{
    return !(it->flags & (PKG_ARG_KEY | PKG_ARG_SWITCH));
}

/* The keywords and switches a verb takes, for a refusal that names them. */
static void takes(const struct pkg_args *a, char *out, size_t len)
{
    size_t i, at = 0;
    out[0] = '\0';
    for (i = 0; i < a->n && at + 30 < len; i++) {
        const struct pkg_arg *it = &a->item[i];
        if (by_place(it) || (it->flags & PKG_ARG_GLOBAL))
            continue;
        at += (size_t)snprintf(out + at, len - at, "%s%s", at ? ", " : "", it->name);
    }
    if (at == 0)
        snprintf(out, len, "no keyword");
}

/* ---- the words -------------------------------------------------------- */

int pkg_args_read(struct pkg_args *a, const char *verb, int first, int argc, char **argv,
                  int (*elsewhere)(const char *word), char *err, size_t errlen)
{
    size_t i, next = 0, pos[PKG_ARGS_ITEMS], npos = 0;
    char list[400];
    int w;

    for (i = 0; i < a->n; i++) {
        a->item[i].set = 0;
        a->item[i].value = NULL;
        a->item[i].nvalues = 0;
        if (by_place(&a->item[i]))
            pos[npos++] = i;
    }
    takes(a, list, sizeof list);

    for (w = first; w < argc; w++) {
        const char *word = argv[w], *eq;
        struct pkg_arg *it = NULL;

        /* A /W place takes the next word whatever it looks like. */
        if (next < npos && (a->item[pos[next]].flags & PKG_ARG_WORD)) {
            it = &a->item[pos[next++]];
            it->value = word;
            it->set = 1;
            continue;
        }
        /* KEYWORD=value, as AmigaDOS writes it too. */
        eq = strchr(word, '=');
        if (eq != NULL && eq > word) {
            size_t k;
            for (k = 0; k < a->n; k++)
                if ((a->item[k].flags & PKG_ARG_KEY) && same_n(word, (size_t)(eq - word), a->item[k].name))
                    it = &a->item[k];
            if (it != NULL) {
                if (it->set)
                    return fail(err, errlen, "%s is given twice; %s takes it once%s", it->name, verb, NULL);
                it->value = eq + 1;
                it->set = 1;
                continue;
            }
        }
        /* Only keywords and switches are matched by name. A place is taken
         * by its place alone: a drawer may well be called "drawer", and a
         * package "package", and neither may swallow the word after it. */
        it = pkg_args_item(a, word);
        if (it != NULL && by_place(it))
            it = NULL;
        if (it != NULL) {
            if (it->flags & PKG_ARG_SWITCH) {
                if (it->set)
                    return fail(err, errlen, "%s is given twice%s%s", it->name, NULL, NULL);
                it->set = 1;
                continue;
            }
            if (w + 1 >= argc)
                return fail(err, errlen, "%s needs a value after it%s%s", it->name, NULL, NULL);
            if (it->set)
                return fail(err, errlen, "%s is given twice; %s takes it once%s", it->name, verb, NULL);
            it->value = argv[++w];
            it->set = 1;
            continue;
        }
        /* The habit of other tools: say pkg's spelling, never guess. */
        if (word[0] == '-' && word[1] != '\0') {
            char up[32];
            const char *d = word + strspn(word, "-");
            size_t k;
            for (k = 0; d[k] && d[k] != '=' && k + 1 < sizeof up; k++)
                up[k] = (char)toupper((unsigned char)d[k]);
            up[k] = '\0';
            return fail(err, errlen, "\"%s\": pkg keywords have no dashes and take their value as the "
                        "next word, as in ROOT <dir>%s%s", word, up[0] ? "; here perhaps " : NULL, up);
        }
        /* A word in capitals is meant as a keyword: another verb's, or a
         * slip of this one's. A package called "file" is still a package. */
        if (capitals(word)) {
            size_t k, best = 99;
            const char *near = NULL;
            for (k = 0; k < a->n; k++) {
                const struct pkg_arg *c = &a->item[k];
                size_t d, lw = strlen(word), lc = strlen(c->name);
                if (by_place(c) || (c->flags & PKG_ARG_GLOBAL))
                    continue;
                d = pkg_name_edits(word, c->name);
                if (d <= ((lw < lc ? lw : lc) <= 4 ? 1u : 2u) && d < best) {
                    best = d;
                    near = c->name;
                }
            }
            if (near != NULL)
                return fail(err, errlen, "\"%s\" is not a word %s takes; did you mean %s?", word, verb, near);
            if (elsewhere != NULL && elsewhere(word))
                return fail(err, errlen, "%s is not a word %s takes; it takes %s", word, verb, list);
        }
        while (next < npos && a->item[pos[next]].set && !(a->item[pos[next]].flags & PKG_ARG_MULTI))
            next++;
        if (next < npos) {
            it = &a->item[pos[next]];
            if (it->flags & PKG_ARG_MULTI) {
                if (it->nvalues >= PKG_ARGS_WORDS)
                    return fail(err, errlen, "%s takes at most %s words%s", verb, "64", NULL);
                it->values[it->nvalues++] = word;
                it->value = it->values[0];
            } else {
                it->value = word;
                next++;
            }
            it->set = 1;
            continue;
        }
        return fail(err, errlen, "\"%s\" is more than %s takes; it takes %s", word, verb, list);
    }
    for (i = 0; i < a->n; i++) {
        const struct pkg_arg *it = &a->item[i];
        if ((it->flags & PKG_ARG_NEEDED) && !it->set) {
            char shown[64];
            if (by_place(it))
                snprintf(shown, sizeof shown, "%s", it->shown[0] ? it->shown : "a name");
            else
                snprintf(shown, sizeof shown, "%s%s%s", it->name, it->shown[0] ? " " : "", it->shown);
            return fail(err, errlen, "%s needs %s%s", verb, shown, NULL);
        }
    }
    return 0;
}

/* ---- usage ------------------------------------------------------------ */

void pkg_args_syntax(const struct pkg_args *a, const char *(*shown)(const char *name),
                     char *out, size_t len)
{
    size_t i, at = 0;
    out[0] = '\0';
    for (i = 0; i < a->n && at + 2 < len; i++) {
        const struct pkg_arg *it = &a->item[i];
        const char *v = it->shown[0] ? it->shown : shown ? shown(it->name) : "<value>";
        int bracket = !(it->flags & (PKG_ARG_NEEDED | PKG_ARG_ROOTED));
        char one[96];
        if (it->flags & PKG_ARG_GLOBAL)
            continue;
        if (it->flags & PKG_ARG_SWITCH)
            snprintf(one, sizeof one, "%s", it->name);
        else if (it->flags & PKG_ARG_KEY)
            snprintf(one, sizeof one, "%s %s", it->name, v);
        else
            snprintf(one, sizeof one, "%s%s", v, (it->flags & PKG_ARG_MULTI) ? "..." : "");
        at += (size_t)snprintf(out + at, len - at, "%s%s%s%s", at ? " " : "", bracket ? "[" : "",
                               one, bracket ? "]" : "");
    }
}
