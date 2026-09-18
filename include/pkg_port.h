/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#ifndef PKG_PORT_H
#define PKG_PORT_H

/* A verb runner: the same function the command line uses. Returns 0 on
 * success, 1 on a refusal, 2 on a usage error. */
typedef int (*pkg_run_fn)(int argc, char **argv);

/* Serve the ARexx port `name` until QUIT or Ctrl-C. 0 on a clean close. */
int pkg_port_serve(const char *name, pkg_run_fn run);

#endif
