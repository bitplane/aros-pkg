/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PKG_SELFUPDATE_H
#define PKG_SELFUPDATE_H
#include <stddef.h>
#include "pkg.h"
/* CLI self-upgrade of the executable returned by the OS. DRYRUN verifies
 * the signed offer and reports its target without changing that target. */
int pkg_selfupdate(const struct pkg_sink *sink, int dryrun);
/* Remove only this running installation, preserving other packages/config. */
int pkg_selfremove(const struct pkg_sink *sink, int dryrun);
/* Strict bootstrap signature verification, also exercised by fixture tests. */
int pkg_selfupdate_verify(const void *message, size_t length,
                          const char *armor, const unsigned char key[32]);
#endif
