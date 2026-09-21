/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "pkg.h"
#include <stdio.h>
#include <string.h>

/* A persistent client lets the integration test change a remote channel
 * between checks in the same process. */
int main(int argc, char **argv)
{
    struct pkg_update u;
    struct pkg_update_found f;
    char command[32];
    if (argc != 4) return 20;
    pkg_update_init(&u);
    pkg_update_found_init(&f);
    u.package = argv[1]; u.root = argv[2];
    u.channel = strcmp(argv[3], "-") ? argv[3] : NULL;
    while (fgets(command, sizeof command, stdin)) {
        pkg_update_check(&u, &f);
        printf("state: %d\ncode: %d\ninstalled: %s\noffered: %s\nnewer: %d\nwithdrawn: %d\n"
               "bytes: %llu\ndownload-known: %d\nchanges: %s\nerror: %s\nEND\n",
               f.state, f.code, f.installed ? f.installed : "", f.offered ? f.offered : "",
               f.newer, f.installed_withdrawn, f.installed_bytes, f.download_size_known,
               f.changes ? f.changes : "", f.error);
        fflush(stdout);
    }
    pkg_update_found_free(&f);
    pkg_update_free(&u);
    return 0;
}
