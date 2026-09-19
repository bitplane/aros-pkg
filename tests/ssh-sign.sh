#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# SIGN ... SSH NAMESPACE and KEYINFO ... SSH: Pkg's Ed25519 key in OpenSSH's
# formats, checked by the real `ssh-keygen -Y verify`, which is what the
# portal's installers run before Pkg exists on the machine. A valid signature
# verifies; a changed byte, another namespace and another key each fail.
# Needs OpenSSH 8.1 or later (ssh-keygen -Y).

set -u
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG="$repo_root/build/pkg"
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-ssh.XXXXXX")
trap 'rm -rf "$T"' EXIT
checks=0; fails=0
ok() { checks=$((checks + 1)); if [ "$1" -eq 0 ]; then echo "  ok   $2"; else fails=$((fails + 1)); echo "  FAIL $2"; fi; }
command -v ssh-keygen > /dev/null || { echo "ssh-sign: no ssh-keygen" >&2; exit 69; }
cd "$T" || exit 1

$PKG KEYGEN FILE jkn.key > /dev/null
$PKG KEYGEN FILE other.key > /dev/null
line=$($PKG KEYINFO FILE jkn.key SSH)
case $line in "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI"*" jkn") r=0 ;; *) r=1 ;; esac
ok $r "KEYINFO SSH prints an ssh-ed25519 line named after the key file"
printf '%s\n' "$line" > jkn.pub
ssh-keygen -l -f jkn.pub > /dev/null 2>&1;          ok $? "ssh-keygen reads that line as a public key"
hex=$($PKG KEYINFO FILE jkn.key MACHINE | awk '/^public: /{print $2}')
b64hex=$(printf '%s' "$line" | cut -d' ' -f2 | base64 -d 2>/dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n')
[ "$b64hex" = "$hex" ];                              ok $? "it holds the same 32 bytes as Pkg's public key"
$PKG KEYINFO FILE jkn.key SSH MACHINE | grep -q '^ssh: ssh-ed25519 ';  ok $? "MACHINE gives it as ssh:"

echo "jkn $(cut -d' ' -f1,2 jkn.pub)" > allowed
printf 'e3b0  Bootstrap/x86_64/Pkg\n' > SUMS
$PKG SIGN SUMS KEY jkn.key OUT SUMS.sig SSH NAMESPACE aros-pkg-bootstrap > /dev/null
ok $? "SIGN SSH NAMESPACE signs"
head -1 SUMS.sig | grep -q '^-----BEGIN SSH SIGNATURE-----$';   ok $? "armored as ssh-keygen writes it"
ssh-keygen -Y verify -f allowed -I jkn -n aros-pkg-bootstrap -s SUMS.sig < SUMS > /dev/null 2>&1
ok $? "ssh-keygen -Y verify accepts it"
printf 'e3b1  Bootstrap/x86_64/Pkg\n' > SUMS2
! ssh-keygen -Y verify -f allowed -I jkn -n aros-pkg-bootstrap -s SUMS.sig < SUMS2 > /dev/null 2>&1
ok $? "one byte changed: refused"
! ssh-keygen -Y verify -f allowed -I jkn -n other -s SUMS.sig < SUMS > /dev/null 2>&1
ok $? "another namespace: refused"
echo "jkn $($PKG KEYINFO FILE other.key SSH | cut -d' ' -f1,2)" > allowed-other
! ssh-keygen -Y verify -f allowed-other -I jkn -n aros-pkg-bootstrap -s SUMS.sig < SUMS > /dev/null 2>&1
ok $? "another key in allowed_signers: refused"
$PKG SIGN SUMS KEY jkn.key OUT again.sig SSH NAMESPACE aros-pkg-bootstrap > /dev/null
cmp -s SUMS.sig again.sig;                           ok $? "the same file and key give the same signature (Ed25519 is deterministic)"

$PKG SIGN SUMS KEY jkn.key OUT x.sig SSH > /dev/null 2>&1
[ $? -eq 20 ] && [ ! -e x.sig ];                     ok $? "SSH without NAMESPACE: 20, nothing written"
$PKG SIGN SUMS KEY jkn.key OUT x.sig NAMESPACE n > /dev/null 2>&1
[ $? -eq 20 ] && [ ! -e x.sig ];                     ok $? "NAMESPACE without SSH: 20, nothing written"
$PKG SIGN SUMS KEY jkn.key OUT plain.sig > /dev/null
grep -q '^Signer: ' plain.sig;                       ok $? "without SSH, SIGN still writes Pkg's own signature"

echo
echo "ssh-sign: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
