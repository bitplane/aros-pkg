/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * pkgtest.library, the smallest library the AROS loader accepts, built at
 * the version VER.REV given when compiling: two builds of it at different
 * versions show which copy OpenLibrary() took (tests/aros-resolve.sh). It
 * has the four standard vectors and nothing else, and never expunges. */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <aros/libcall.h>
#include <aros/asmcall.h>

#define STR2(x) #x
#define STR(x) STR2(x)

/* Run as a program by mistake: return at once. First in the file. */
int pkgtest_entry(void)
{
    return -1;
}

static const char libname[] = "pkgtest.library";
static const char idstring[] = "pkgtest.library " STR(VER) "." STR(REV) " (19.9.2026)";
const char pkgtest_version[] = "$VER: pkgtest.library " STR(VER) "." STR(REV) " (19.9.2026)";

AROS_LH1(struct Library *, Open, AROS_LHA(ULONG, version, D0), struct Library *, base, 1, Pkgtest)
{
    AROS_LIBFUNC_INIT
    (void)version;
    base->lib_OpenCnt++;
    base->lib_Flags &= ~LIBF_DELEXP;
    return base;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(BPTR, Close, struct Library *, base, 2, Pkgtest)
{
    AROS_LIBFUNC_INIT
    base->lib_OpenCnt--;
    return BNULL;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(BPTR, Expunge, struct Library *, base, 3, Pkgtest)
{
    AROS_LIBFUNC_INIT
    (void)base;
    return BNULL;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(IPTR, Null, struct Library *, base, 4, Pkgtest)
{
    AROS_LIBFUNC_INIT
    (void)base;
    return 0;
    AROS_LIBFUNC_EXIT
}

AROS_UFH3(struct Library *, LibInit,
          AROS_UFHA(struct Library *, base, D0),
          AROS_UFHA(BPTR, seglist, A0),
          AROS_UFHA(struct ExecBase *, sysbase, A6))
{
    AROS_USERFUNC_INIT
    (void)seglist; (void)sysbase;
    base->lib_Node.ln_Type = NT_LIBRARY;
    base->lib_Node.ln_Name = (char *)libname;
    base->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib_Version = VER;
    base->lib_Revision = REV;
    base->lib_IdString = (APTR)idstring;
    return base;
    AROS_USERFUNC_EXIT
}

static const APTR functions[] = {
    (APTR)AROS_SLIB_ENTRY(Open, Pkgtest, 1),
    (APTR)AROS_SLIB_ENTRY(Close, Pkgtest, 2),
    (APTR)AROS_SLIB_ENTRY(Expunge, Pkgtest, 3),
    (APTR)AROS_SLIB_ENTRY(Null, Pkgtest, 4),
    (APTR)-1
};

static const IPTR inittable[4] = {
    sizeof(struct Library), (IPTR)functions, 0, (IPTR)LibInit
};

const struct Resident pkgtest_romtag = {
    RTC_MATCHWORD, (struct Resident *)&pkgtest_romtag, (APTR)(&pkgtest_romtag + 1),
    RTF_AUTOINIT, VER, NT_LIBRARY, 0, (CONST_STRPTR)libname, (CONST_STRPTR)idstring,
    (APTR)inittable, REV, NULL
};
