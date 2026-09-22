/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PKG_ENVIRONMENT_H
#define PKG_ENVIRONMENT_H
#include <stddef.h>
struct pkg_environment { char *name, *root, *source; int system; };
struct pkg_environments {
    struct pkg_environment *items;
    size_t count;
    char *default_name, *default_source, *system_path, *user_path;
};
/* Initialise before loading. All strings are owned; free releases them.
 * Missing configuration is an empty successful load. Reads create nothing. */
void pkg_environments_init(struct pkg_environments *e);
void pkg_environments_free(struct pkg_environments *e);
int pkg_environments_load(struct pkg_environments *e, char *err, size_t len);
int pkg_environments_load_paths(struct pkg_environments *e, const char *system_path,
                               const char *user_path, char *err, size_t len);
/* name NULL: configured default, or the sole environment. 0 selected,
 * 1 no environment, 2 ambiguous, -1 invalid named/default environment.
 * The selected pointer is borrowed until the next mutation/load/free. */
int pkg_environments_select(const struct pkg_environments *e, const char *name,
                           const struct pkg_environment **selected, char *err, size_t len);
/* Explicit writes to the chosen scope; successful writes reload e.
 * Roots must be absolute. Registration does not create the root directory.
 * Adding an existing name fails. Removing the default clears that default. */
int pkg_environments_add(struct pkg_environments *e, const char *name, const char *root,
                         int system, char *err, size_t len);
int pkg_environments_remove(struct pkg_environments *e, const char *name,
                            int system, char *err, size_t len);
int pkg_environments_default(struct pkg_environments *e, const char *name,
                             int system, char *err, size_t len);
#endif
