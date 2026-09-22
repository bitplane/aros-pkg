/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Every kind of line pkg says, one after the other, so that a change to the
 * palette can be judged in a glance instead of by hunting for a command that
 * happens to produce the line. Nothing here touches the network or the
 * filesystem: the texts are made up, the drawing is the real one.
 *
 *     make build/example-lines && ./build/example-lines
 *
 * docs/output.md says what each kind is for.
 */

#define _POSIX_C_SOURCE 200809L

#include "pkg.h"
#include "pkg_style.h"

#include <stdio.h>
#include <string.h>

static void write_styled(int is_error, const char *styled, const char *plain)
{
    (void)plain;
    fputs(styled, is_error ? stderr : stdout);
    fflush(is_error ? stderr : stdout);
}

static void line(int kind, const char *text)
{
    pkg_style_line(write_styled, kind, 0, text);
}

int main(void)
{
    pkg_style_init(1, 1, 0);

    printf("\nWhat a command says when it worked\n\n");
    line(PKG_LINE_RESULT, "installed hello 1.2 into SYS:");
    line(PKG_LINE_DETAIL, "3 files, 41 KB, signed by 26ffb2bc9a07");
    line(PKG_LINE_ITEM, "adopted\t2 files already there, identical to hello 1.2's");
    line(PKG_LINE_ITEM, "kept\tS/Startup-Sequence (edited; hello ships it unchanged)");
    line(PKG_LINE_HINT, "to run it: SYS:Hello");

    printf("\nWhat it says while working, and in passing\n\n");
    line(PKG_LINE_NOTE, "read from https://aros-pkg.azurewebsites.net/pkg, cached in RAM:pkg-cache");
    line(PKG_LINE_WARNING, "no executable in the drawer: kind application is usually a program, "
                           "and pkg found no ELF or hunk header, so it is published as generic");

    printf("\nWhat it asks\n\n");
    line(PKG_LINE_QUESTION, "administrator access is required. Run this same executable with sudo? [y/N]");
    printf("y\n");

    printf("\nWhat it says when it refused\n\n");
    line(PKG_LINE_REFUSAL, "nothere is not in the channel https://aros-pkg.azurewebsites.net/pkg");
    line(PKG_LINE_NEXT, "check the name; pkg SEARCH <words> looks through every channel this root knows");
    line(PKG_LINE_PROBLEM, "2 of 3 packages damaged");

    printf("\nA table\n\n");
    line(PKG_LINE_HEAD, "Package\tVersion\tKind\tStatus");
    line(PKG_LINE_ROW, "hello\t1.2\tapplication\tok");
    line(PKG_LINE_ROW, "sdl2.library\t53.11\tlibrary\tupgradable to 53.12");
    line(PKG_LINE_ROW, "zip\t3.0\tapplication\tfiles edited");
    line(PKG_LINE_END, "");
    pkg_style_flush(write_styled);

    printf("\n");
    return 0;
}
