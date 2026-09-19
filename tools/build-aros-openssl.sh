#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Build OpenSSL for AROS aarch64 and install libssl.a, libcrypto.a and the
# headers where tools/build-aros.sh looks for them. AROS contrib builds the
# same library for the architectures it ships; this script is the aarch64
# hosted build, which no nightly carries, made with the same recipe: contrib's
# Configure target and its patch, applied to the release openssl.org ships.
#
# Output (PKG_AROS_OPENSSL, default ~/aros-native/openssl-aarch64):
#   lib/libssl.a  lib/libcrypto.a  include/openssl/*.h
#
# The build tree is large and is removed once the libraries are installed;
# KEEP_BUILD=1 leaves it. Nothing is written inside a repository.

set -eu

version=4.0.1
sha256=2db3f3a0d6ea4b59e1f094ace2c8cd536dffb87cdc39084c5afa1e6f7f37dd09
url="https://www.openssl.org/source/openssl-$version.tar.gz"

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
contrib=${AROS_CONTRIB:-"$repo_root/../aros-contrib"}
patch_file="$contrib/development/libs/openssl/openssl-$version-aros.diff"
aros_build=${AROS_BUILD:-"$HOME/aros-build"}
aros_crosstools=${AROS_CROSSTOOLS:-"$HOME/aros-crosstools"}
sdk=${PKG_AROS_SDK_ROOT:-"$aros_build/bin/darwin-aarch64"}
developer="$sdk/AROS/Developer"
aros_target=${PKG_AROS_TARGET:-aarch64-unknown-aros}
aros_arch_flags=${PKG_AROS_ARCH_FLAGS:--mcmodel=large -ffixed-x18}
prefix=${PKG_AROS_OPENSSL:-"$HOME/aros-native/openssl-aarch64"}
cache=${PKG_OPENSSL_CACHE:-"$HOME/aros-native/openssl-src"}
work=${PKG_OPENSSL_WORK:-"$HOME/aros-native/openssl-build-aarch64"}
jobs=${JOBS:-$( (sysctl -n hw.ncpu || nproc || echo 4) 2>/dev/null )}

for need in "$aros_crosstools/bin/clang" "$sdk/gen/include/aros/posixc/unistd.h" "$patch_file"; do
    [ -e "$need" ] || { echo "build-aros-openssl: missing $need" >&2; exit 69; }
done
command -v perl >/dev/null || { echo "build-aros-openssl: no perl, which Configure needs" >&2; exit 69; }

# The release, checked before it is unpacked.
mkdir -p "$cache"
tarball="$cache/openssl-$version.tar.gz"
[ -f "$tarball" ] || curl -fsSL -o "$tarball" "$url"
have=$(shasum -a 256 "$tarball" 2>/dev/null || sha256sum "$tarball")
case "$have" in
    "$sha256 "*) ;;
    *) echo "build-aros-openssl: $tarball is not the release: $have" >&2; exit 1 ;;
esac

rm -rf "$work"
mkdir -p "$work"
tar -xzf "$tarball" -C "$work"
src="$work/openssl-$version"
(cd "$src" && patch -p1 --forward --silent < "$patch_file")
[ -f "$src/Configurations/15-aros.conf" ] || { echo "build-aros-openssl: the AROS target did not patch in" >&2; exit 1; }

# The compiler build-aros.sh uses, with the AROS headers: OpenSSL's own build
# calls it as one word, so the flags travel with it.
cc="$aros_crosstools/bin/clang --target=$aros_target $aros_arch_flags"
cflags="-O2 -isystem $developer/include -isystem $sdk/gen/include -isystem $sdk/gen/include/aros/posixc -isystem $developer/include/aros/stdc"
cflags="$cflags -Wno-everything"

cd "$src"
env -i PATH="$PATH" HOME="$HOME" \
    AROS_CC="$cc" AROS_CFLAGS="$cflags" \
    AROS_RANLIB="$aros_crosstools/bin/llvm-ranlib" AR="$aros_crosstools/bin/llvm-ar" \
    ./Configure aros-aarch64-buildsys threads no-ssl3 no-asm no-shared no-tests \
    --prefix="$prefix" --openssldir="$prefix/etc/ssl"

# Only the libraries and the headers: no apps, which would need a whole
# AROS link and are not what Pkg uses.
env -i PATH="$PATH" HOME="$HOME" make -j"$jobs" build_libs
mkdir -p "$prefix/lib" "$prefix/include"
cp libssl.a libcrypto.a "$prefix/lib/"
rm -rf "$prefix/include/openssl"
cp -R include/openssl "$prefix/include/openssl"
cp -R "$src/include/openssl/"*.h "$prefix/include/openssl/" 2>/dev/null || true

cd "$repo_root"
[ "${KEEP_BUILD:-0}" = 1 ] || rm -rf "$work"
echo "build-aros-openssl: $prefix/lib/libssl.a $prefix/lib/libcrypto.a"
ls -l "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a"
