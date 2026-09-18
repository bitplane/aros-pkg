/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * PkgHandlerRev <volume>: print the interface revision of the AFS+ handler
 * that serves <volume>, as that handler reports it through the AFS+ extension
 * transport. A test tool: it tells which handler binary AROS actually loaded,
 * which is what an upgrade that takes effect at restart has to demonstrate.
 *
 * Built against the AFS+ client library, which it uses read-only; AFS+ itself
 * is not modified.
 */

#include <proto/dos.h>
#include <dos/dos.h>

#include "afsplus_client.h"

int main(int argc, char **argv)
{
    struct DevProc *process;
    uint32_t revision = 0;
    uint64_t groups = 0;
    LONG error;

    if (argc != 2) {
        Printf("usage: PkgHandlerRev <volume>\n");
        return RETURN_ERROR;
    }
    process = GetDeviceProc((CONST_STRPTR)argv[1], NULL);
    if (process == NULL) {
        Printf("PkgHandlerRev: %s: error %ld\n", argv[1], IoErr());
        return RETURN_ERROR;
    }
    error = afsplus_client_interface(process->dvp_Port, &revision, NULL, &groups);
    FreeDeviceProc(process);
    if (error != 0) {
        Printf("PkgHandlerRev: %s: error %ld\n", argv[1], error);
        return RETURN_ERROR;
    }
    Printf("revision %lu\n", (unsigned long)revision);
    return RETURN_OK;
}
