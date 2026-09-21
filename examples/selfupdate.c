/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Check the installed version of your program and display its update.
 *   ./build/example-selfupdate hello /path/to/root /path/to/channel
 *   ./build/example-selfupdate --config /path/to/hello.pkgupdate
 *
 * The configuration contains:
 *   Format: pkg-update 1
 *   Package: hello
 *   Root: /path/to/root
 *   Channel: /path/to/channel
 * Omitting Channel selects the root's configured channels.
 */
#include "pkg.h"
#include <stdio.h>
#include <string.h>

static const char *state_name(int state)
{
    switch (state) {
    case PKG_UPDATE_NONE: return "Up to date";
    case PKG_UPDATE_AVAILABLE: return "Update available";
    case PKG_UPDATE_NOT_MANAGED: return "Program is not installed by Pkg";
    case PKG_UPDATE_UNREACHABLE: return "Channel is unreachable";
    case PKG_UPDATE_WITHDRAWN: return "Installed version has been withdrawn";
    case PKG_UPDATE_KEY_CHANGED: return "Publisher key has changed";
    case PKG_UPDATE_NOT_OFFERED: return "No compatible version is offered";
    default: return "Update check failed";
    }
}

int main(int argc, char **argv)
{
    struct pkg_update update;
    struct pkg_update_found found;
    char error[512];
    int state, rc;
    pkg_update_init(&update);
    pkg_update_found_init(&found);
    if (argc == 3 && !strcmp(argv[1], "--config")) {
        rc = pkg_update_from_file(&update, argv[2], error, sizeof error);
        if (rc) { fprintf(stderr, "%s\n", error); return rc; }
    } else if (argc == 4 && strcmp(argv[1], "--config")) {
        /* These strings belong to the application. */
        update.package = argv[1];
        update.root = argv[2];
        update.channel = argv[3];
    } else {
        fprintf(stderr, "usage: %s PACKAGE ROOT CHANNEL\n       %s --config FILE\n", argv[0], argv[0]);
        return PKG_RC_USAGE;
    }
    printf("Checking for updates to %s...\n", update.package);
    fflush(stdout);
    state = pkg_update_check(&update, &found);
    printf("%s\n", state_name(state));
    if (found.installed) printf("Installed: %s\n", found.installed);
    if (found.offered) printf("Offered: %s\n", found.offered);
    if (found.installed_withdrawn) puts("The installed version has been withdrawn by its publisher.");
    if (found.changes && *found.changes) printf("What's new:\n%s\n", found.changes);
    if (found.signer) printf("Publisher key: %s\n", found.signer);
    if (*found.error) fprintf(stderr, "%s\n", found.error);
    rc = state == PKG_UPDATE_ERROR || state == PKG_UPDATE_UNREACHABLE
        ? (found.code ? found.code : PKG_RC_REFUSED) : 0;
    pkg_update_found_free(&found);
    pkg_update_free(&update);
    return rc;
}
