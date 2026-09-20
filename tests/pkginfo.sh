#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# INFO <file>: PUBLISH and MANIFEST read what a package is from the .pkginfo
# a port carries. The drawer below is the Mbed TLS port's install, and the
# file is the one in AROS contrib, word for word.

set -u
PKG=${PKG:-./build/pkg}
PKG=$(cd "$(dirname "$PKG")" && pwd)/$(basename "$PKG")
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-pkginfo.XXXXXX")
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
checks=0
fails=0
ok() { checks=$((checks + 1)); [ "$1" -eq 0 ] || { fails=$((fails + 1)); echo "  FAIL $2"; }; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
man() { f=$(awk -v v="$1" '$1=="mbedtls" && $2==v {print $4}' ch/index); cat "ch/objects/$f.manifest"; }

$PKG KEYGEN FILE key > /dev/null
export PKG_SIGNKEY="$T/key"

# the port as it installs: three static libraries, two header drawers, one
# header, its documentation, and one file no Files line names
mkdir -p d/Developer/lib d/Developer/include/mbedtls d/Developer/include/psa \
         d/Developer/Docs/mbedtls
for f in libmbedtls libmbedx509 libmbedcrypto; do printf 'ar\n' > "d/Developer/lib/$f.a"; done
printf 'x\n' > d/Developer/lib/libunrelated.a
printf 'h\n' > d/Developer/include/mbedtls/ssl.h
printf 'h\n' > d/Developer/include/psa/crypto.h
printf 'h\n' > d/Developer/include/mbedtls_user_config_aros.h
printf 'doc\n' > d/Developer/Docs/mbedtls/ChangeLog

info=d/Developer/Docs/mbedtls/mbedtls.pkginfo
cat > "$info" <<'PKGINFO'
Format: pkginfo 1
Name: mbedtls
Version: 3.6.7
Kind: sdk
Short: Small TLS library to link into programs
Category: dev/lib
Tags: tls, ssl, https, crypto, x509, library
Author: The Mbed TLS Contributors
Homepage: https://www.trustedfirmware.org/projects/mbed-tls/
Repository: https://github.com/Mbed-TLS/mbedtls
License: Apache-2.0 OR GPL-2.0-or-later
Distribution: open-source
Upstream-Archive: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
Upstream-SHA256: a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
Port: development/libs/mbedtls
Port-Maintainer: The AROS Development Team
Depends:
Files: Developer/lib/libmbedtls.a
Files: Developer/lib/libmbedx509.a
Files: Developer/lib/libmbedcrypto.a
Files: Developer/include/mbedtls
Files: Developer/include/psa
Files: Developer/include/mbedtls_user_config_aros.h
Files: Developer/Docs/mbedtls
Description: Mbed TLS is a C library that implements TLS 1.2 and 1.3, X.509 certificates and the cryptography they need. It is small and made to be linked into a program: a TLS client that verifies certificates adds about 0.7 MB.
Description:
Description: This is the long-term support branch, 3.6. The AROS port is static libraries and headers. It leaves out Mbed TLS's own socket layer, since bsdsocket.library sockets are not file descriptors: a program gives the library its send and receive functions with mbedtls_ssl_set_bio(). Randomness comes from getentropy(), which AROS answers from entropy.resource (AROS of July 2026 or later).
Description:
Description: Compile with -DMBEDTLS_USER_CONFIG_FILE='<mbedtls_user_config_aros.h>' and link -lmbedtls -lmbedx509 -lmbedcrypto.
Changes: First AROS port.
PKGINFO

echo "a port that carries one"
$PKG PUBLISH d CHANNEL ch INFO "$info" MACHINE > o1 2>&1
[ $? -eq 0 ] && has o1 '^name: mbedtls$' && has o1 '^version: 3.6.7$'
                                                      ok $? "PUBLISH with INFO alone needs no NAME, VERSION or KIND"
man 3.6.7 > m1
has m1 '^Kind: sdk$' && has m1 '^Short: Small TLS library to link into programs$' \
  && has m1 '^Category: dev/lib$' && has m1 '^Tags: tls, ssl, https, crypto, x509, library$' \
  && has m1 '^Author: The Mbed TLS Contributors$' \
  && has m1 '^Homepage: https://www.trustedfirmware.org/projects/mbed-tls/$' \
  && has m1 '^Repository: https://github.com/Mbed-TLS/mbedtls$' \
  && has m1 '^License: Apache-2.0 OR GPL-2.0-or-later$' && has m1 '^Distribution: open-source$' \
  && has m1 '^Changes: First AROS port.$' && [ "$(grep -c '^Description: ' m1)" = 5 ]
                                                      ok $? "the kind and every catalogue field come from the file, paragraph breaks kept"
! has m1 '^Upstream-Archive' && ! has m1 '^Port-Maintainer' && ! has m1 '^Port:'
                                                      ok $? "the keys Pkg does not know are ignored, not carried into the manifest"
grep '^File: ' m1 | awk '{print $4}' | sort > got
cat > want <<'FILES'
Developer/Docs/mbedtls/ChangeLog
Developer/Docs/mbedtls/mbedtls.pkginfo
Developer/include/mbedtls/ssl.h
Developer/include/mbedtls_user_config_aros.h
Developer/include/psa/crypto.h
Developer/lib/libmbedcrypto.a
Developer/lib/libmbedtls.a
Developer/lib/libmbedx509.a
FILES
cmp -s got want;                                      ok $? "Files selects exactly the paths it lists, and not the library beside them"
has o1 '^info-from: ' && has o1 '^info-fields: name, version, kind, files$' \
  && has o1 "^about-from: $info\$"
                                                      ok $? "PUBLISH says which fields came from the file, and where it was"
$PKG SHOW mbedtls CHANNEL ch > o2 2>&1
has o2 'mbedtls 3.6.7: Small TLS library to link into programs' && has o2 'dev/lib' \
  && has o2 'Apache-2.0 OR GPL-2.0-or-later' && has o2 'mbedtls_ssl_set_bio'
                                                      ok $? "SHOW prints them back for a person"

echo "the command wins"
$PKG PUBLISH d CHANNEL ch2 INFO "$info" SHORT "Mine" VERSION 3.6.8 MACHINE > o3 2>&1
f=$(awk '{print $4}' ch2/index)
[ $? -eq 0 ] && grep -q '^Short: Mine$' "ch2/objects/$f.manifest" \
  && grep -q '^Version: 3.6.8$' "ch2/objects/$f.manifest" \
  && grep -q '^Category: dev/lib$' "ch2/objects/$f.manifest"
                                                      ok $? "a keyword replaces that one field of the file and leaves the rest"
$PKG PUBLISH d CHANNEL ch3 INFO "$info" FILES Developer/lib MACHINE > o4 2>&1
f=$(awk '{print $4}' ch3/index)
[ $? -eq 0 ] && [ "$(grep -c '^File: ' "ch3/objects/$f.manifest")" = 4 ]
                                                      ok $? "FILES on the line replaces the file's Files, libunrelated.a among them"

echo "MANIFEST"
$PKG MANIFEST d INFO "$info" > m2 2>&1
[ $? -eq 0 ] && sed '/^Payload: /d' m1 > m1n && sed '/^Payload: /d' m2 > m2n && cmp -s m1n m2n
                                                      ok $? "MANIFEST with INFO shows the manifest PUBLISH signed, Payload apart"

echo "refusals"
bad() {  # bad <what> <exit code> <pattern> <sed program>
    what=$1 code=$2 pat=$3 prog=$4
    sed "$prog" "$info" > b.pkginfo
    $PKG MANIFEST d INFO b.pkginfo MACHINE > o5 2>&1
    [ $? -eq "$code" ] && has o5 "$pat"
    ok $? "$what"
}
bad "a Format number this Pkg does not read is refused, saying Pkg is older" \
    12 'line 1: it is pkginfo 2' 's/^Format: pkginfo 1/Format: pkginfo 2/'
bad "a Short over 40 characters is refused, naming its line" \
    12 'line 5: Short is 65 characters' 's|^Short: .*|Short: Small TLS library to link into programs that speak to the network|'
bad "a category that is no Aminet type is refused" \
    12 'line 6: Category is an Aminet type' 's|^Category: .*|Category: crypto/lib|'
bad "a Files path that climbs out of the drawer is refused" \
    12 'the path contains' 's|^Files: Developer/lib/libmbedtls.a|Files: ../../etc/passwd|'
bad "a Files path the drawer does not hold is refused, naming it" \
    11 'names "Developer/lib/libmbedssl.a" in Files' 's|^Files: Developer/lib/libmbedtls.a|Files: Developer/lib/libmbedssl.a|'
bad "a licence that is no SPDX expression is refused" \
    12 'line 11: License is an SPDX expression' 's|^License: .*|License: Apache-2.0/GPL-2.0|'
bad "a line that is no Key: value is refused, naming it" \
    12 'is not a "Key: value" line' 's|^Port: .*|this line has no colon|'
$PKG MANIFEST d INFO nowhere.pkginfo MACHINE > o6 2>&1
[ $? -eq 11 ] && has o6 'no .pkginfo at "nowhere.pkginfo"'
                                                      ok $? "an INFO file that is not there is refused with 11"

echo "negative control"
# The check above must be able to fail: the same drawer with the file's own
# Files honoured holds no libunrelated.a, and looking for it must fail.
grep -q '^File: .*libunrelated.a$' m1
[ $? -ne 0 ];                                         ok $? "the file list check can fail: libunrelated.a is absent, and a test for it reports so"
printf 'Format: pkginfo 1\nName: mbedtls\nVersion: 3.6.7\nKind: sdk\nFiles: Developer/lib/libunrelated.a\n' > c.pkginfo
$PKG MANIFEST d INFO c.pkginfo > m3 2>&1
[ $? -eq 0 ] && grep -q '^File: .*libunrelated.a$' m3
                                                      ok $? "and the same check passes when a .pkginfo does name it"

echo
echo "pkginfo: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
