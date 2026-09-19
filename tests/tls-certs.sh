#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Throwaway certificates for the tests that make Pkg on AROS speak https:
# an authority of our own, a certificate that is right, and three that are
# wrong in one way each. Everything lands in the directory given and lives
# as long as the test does.
#
#   ca.pem        the authority, which the test gives Pkg in PKG_CAFILE
#   good.pem      the address, signed by ca.pem: the one that must work
#   other.pem     the address, signed by an authority Pkg is not given
#   wrongname.pem signed by ca.pem, but made out to another name
#   expired.pem   the address, signed by ca.pem, out of date since last year
#
# The address is 127.0.0.1 unless one is given: a machine in an emulator
# sees the host at another one.
#   good.pfx      good.pem again, for a server that wants PKCS#12 (Kestrel);
#                 its password is "pkg"
#
# Each <name>.pem holds the certificate and its key, as python3's ssl wants.
# OPENSSL= names the openssl to use; it must be one that takes -not_after
# (OpenSSL 3.2 and newer), since an expired certificate cannot be asked for
# in days.

set -eu
dir=${1:?usage: tls-certs.sh <dir> [address]}
addr=${2:-127.0.0.1}
ssl=${OPENSSL:-}
if [ -z "$ssl" ]; then
    for c in openssl /opt/homebrew/opt/openssl@3/bin/openssl /opt/homebrew/opt/openssl/bin/openssl \
             /usr/local/opt/openssl@3/bin/openssl; do
        command -v "$c" > /dev/null 2>&1 || continue
        "$c" x509 -help 2>&1 | grep -q -- '-not_after' && { ssl=$c; break; }
    done
fi
[ -n "$ssl" ] || { echo "tls-certs: no openssl that takes -not_after; set OPENSSL=" >&2; exit 69; }

mkdir -p "$dir"
cd "$dir"
year=$(date -u +%Y)
past_from=$((year - 2))
past_to=$((year - 1))

authority() {                                   # authority <stem> <name>
    "$ssl" req -x509 -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.pem" \
        -days 3 -subj "/CN=$2" -addext "basicConstraints=critical,CA:TRUE" 2> /dev/null
}
leaf() {                                        # leaf <stem> <ca stem> <SAN> <dates...>
    stem=$1; ca=$2; san=$3
    shift 3
    printf 'subjectAltName = %s\nextendedKeyUsage = serverAuth\n' "$san" > "$stem.ext"
    "$ssl" req -newkey rsa:2048 -nodes -keyout "$stem.key" -out "$stem.csr" \
        -subj "/CN=pkg test server" 2> /dev/null
    "$ssl" x509 -req -in "$stem.csr" -CA "$ca.pem" -CAkey "$ca.key" -CAcreateserial \
        -extfile "$stem.ext" -out "$stem.crt" "$@" 2> /dev/null
    cat "$stem.crt" "$stem.key" > "$stem.pem"
}

authority ca "Pkg test authority"
authority other-ca "Another authority"

leaf good      ca       "IP:$addr"                      -days 2
leaf other     other-ca "IP:$addr"                      -days 2
leaf wrongname ca       "DNS:not-this-machine.invalid"  -days 2
leaf expired   ca       "IP:$addr" \
    -not_before "${past_from}0101000000Z" -not_after "${past_to}0101000000Z"

"$ssl" pkcs12 -export -out good.pfx -inkey good.key -in good.crt -passout pass:pkg 2> /dev/null

echo "$dir/ca.pem"
