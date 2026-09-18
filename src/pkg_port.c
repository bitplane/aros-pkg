/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The ARexx port. `Pkg PORT` opens a public message port, PKG by default, and
 * serves ARexx command messages on it until QUIT or Ctrl-C.
 *
 * A command string is split into words and run through exactly the path the
 * command line takes, so the port adds no second implementation of any verb.
 * Its output is captured and becomes RESULT when the caller asked for one. A
 * refusal sets RC to its class code, the same number the command line exits
 * with on every host (10 to 18, and 20 for usage), and leaves RESULT unset,
 * which is what ARexx does with a failing command anyway; LASTERROR then
 * returns the refusal's text, so an agent reads what a person would read.
 */

#include "pkg_port.h"
#include "pkg_out.h"

#include <stdlib.h>
#include <string.h>

#ifdef __AROS__

#include <proto/exec.h>
#include <proto/rexxsyslib.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <rexx/errors.h>
#include <rexx/storage.h>

#define MAXWORDS 32

/* Split in place on blanks; "double quotes" group words with spaces. */
static int split(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = '\0';
    }
    return n;
}

static int word_is(const char *w, const char *kw)
{
    for (; *w && *kw; w++, kw++)
        if ((*w | 0x20) != (*kw | 0x20))
            return 0;
    return *w == *kw;
}

int pkg_port_serve(const char *name, pkg_run_fn run)
{
    struct MsgPort *port;
    char *lasterr = NULL;
    size_t lasterr_len = 0;
    int running = 1;
    static char portname[64];

    strncpy(portname, name, sizeof portname - 1);
    Forbid();
    if (FindPort((CONST_STRPTR)portname) != NULL) {
        Permit();
        pkg_err("pkg port: a port called %s is already open; nothing was started\n", portname);
        return 1;
    }
    port = CreateMsgPort();
    if (port == NULL) {
        Permit();
        pkg_err("pkg port: cannot create a message port\n");
        return 1;
    }
    port->mp_Node.ln_Name = portname;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();

    while (running) {
        ULONG got = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
        struct RexxMsg *msg;

        if (got & SIGBREAKF_CTRL_C)
            running = 0;
        while ((msg = (struct RexxMsg *)GetMsg(port)) != NULL) {
            char *copy, *argv[MAXWORDS + 1];
            int argc, rc = 0;
            char *out = NULL, *err = NULL;
            size_t out_len = 0, err_len = 0;
            const char *result = NULL;
            size_t result_len = 0;

            msg->rm_Result1 = RC_OK;
            msg->rm_Result2 = 0;
            if (!IsRexxMsg(msg) || (msg->rm_Action & RXCODEMASK) != RXCOMM
                || msg->rm_Args[0] == 0) {
                msg->rm_Result1 = RC_ERROR;
                ReplyMsg((struct Message *)msg);
                continue;
            }
            copy = strdup((const char *)msg->rm_Args[0]);
            if (copy == NULL) {
                msg->rm_Result1 = RC_FATAL;
                ReplyMsg((struct Message *)msg);
                continue;
            }
            argv[0] = (char *)"pkg";
            argc = 1 + split(copy, argv + 1, MAXWORDS);
            argv[argc] = NULL;

            if (argc < 2) {
                rc = 20;
            } else if (word_is(argv[1], "QUIT")) {
                running = 0;
            } else if (word_is(argv[1], "LASTERROR")) {
                result = lasterr ? lasterr : "";
                result_len = lasterr ? lasterr_len : 0;
            } else {
                pkg_capture_begin();
                rc = run(argc, argv);
                pkg_capture_end(&out, &out_len, &err, &err_len);
                if (rc == 0) {
                    result = out ? out : "";
                    result_len = out_len;
                } else {
                    free(lasterr);
                    lasterr = err;
                    lasterr_len = err_len;
                    /* The text ends in a newline on the command line; not here. */
                    while (lasterr && lasterr_len > 0 && lasterr[lasterr_len - 1] == '\n')
                        lasterr[--lasterr_len] = '\0';
                    err = NULL;
                }
            }

            if (rc == 0) {
                while (result && result_len > 0 && result[result_len - 1] == '\n')
                    result_len--;
                if ((msg->rm_Action & RXFF_RESULT) && result != NULL)
                    msg->rm_Result2 = (IPTR)CreateArgstring((CONST_STRPTR)result, (ULONG)result_len);
            } else {
                msg->rm_Result1 = rc;          /* the class code, as on the command line */
            }
            ReplyMsg((struct Message *)msg);
            free(out);
            free(err);
            free(copy);
        }
    }

    Forbid();
    RemPort(port);
    {
        struct Message *m;
        while ((m = GetMsg(port)) != NULL) {
            ((struct RexxMsg *)m)->rm_Result1 = RC_ERROR;
            ReplyMsg(m);
        }
    }
    Permit();
    DeleteMsgPort(port);
    free(lasterr);
    return 0;
}

#else

int pkg_port_serve(const char *name, pkg_run_fn run)
{
    (void)name;
    (void)run;
    pkg_err("pkg port: the ARexx port exists on AROS. On this host an agent drives "
            "pkg from the shell, where the exit status and the output carry the same "
            "contract the port does\n");
    return 1;
}

#endif
