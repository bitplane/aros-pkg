#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Cross-build Pkg for native AROS on x86_64 (pc-x86_64), the build that runs
# under QEMU. Output: build/aros-x86_64/Pkg.
#
#   AROS_X86_64_SDK   the Developer directory of an x86_64 AROS system, from
#                     the nightly AROS-<date>-linux-x86_64-system archive:
#                     same ABI for programs as pc-x86_64. Default:
#                     ~/aros-native/AROS-*-linux-x86_64-system/Developer.
#   LLVM              a clang that knows the x86_64-unknown-aros triple; the
#                     Homebrew LLVM does. Default: /opt/homebrew/opt/llvm.
#
# Linking needs collect-aros, AROS's linker wrapper, built for x86_64: the one
# a hosted aarch64 build makes is fixed to aarch64. It is built here from the
# AROS sources, with the hosted build's own environment (the paths to ld.lld
# and the LLVM tools, which handle every architecture) and the CPU switched.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_src=${AROS_SRC:-"$repo_root/../aros-upstream"}
hosted_env=${AROS_HOSTED_ENV:-"$HOME/aros-build/bin/darwin-aarch64/gen/tools/collect-aros/env.h"}
llvm=${LLVM:-/opt/homebrew/opt/llvm}
sdk=${AROS_X86_64_SDK:-$(ls -d "$HOME"/aros-native/AROS-*-linux-x86_64-system/Developer 2>/dev/null | tail -1)}
openssl=${PKG_X86_64_OPENSSL:-"$HOME/aros-native/openssl-x86_64"}
out="$repo_root/build/aros-x86_64"
tools="$out/tools"

for need in "$llvm/bin/clang" "$sdk/lib/startup.o" "$hosted_env" "$aros_src/tools/collect-aros/collect-aros.c"; do
    [ -e "$need" ] || { echo "build-aros-x86_64: missing $need" >&2; exit 69; }
done
for need in "$openssl/lib/libssl.a" "$openssl/lib/libcrypto.a" "$openssl/include/openssl/ssl.h"; do
    [ -e "$need" ] || { echo "build-aros-x86_64: missing $need, which https needs. AROS contrib" >&2
                        echo "builds it: take Developer/lib/lib{ssl,crypto}.a and Developer/include/openssl" >&2
                        echo "out of a nightly AROS-<date>-pc-x86_64-contrib archive into" >&2
                        echo "  $openssl/{lib,include}" >&2
                        echo "or point PKG_X86_64_OPENSSL at another copy." >&2
                        exit 69; }
done
mkdir -p "$out/obj" "$tools"

# The certificate authorities Pkg carries.
sh "$repo_root/tools/ca-bundle-c.sh" "$repo_root/third_party/cacert/cacert.pem" > "$out/pkg_cabundle.c"

# collect-aros for x86_64.
sed 's/#define TARGET_CPU_aarch64/#define TARGET_CPU_x86_64/' "$hosted_env" > "$tools/env.h"
grep -q 'TARGET_CPU_x86_64' "$tools/env.h" || { echo "build-aros-x86_64: env.h not switched" >&2; exit 1; }
(cd "$aros_src/tools/collect-aros" && cc -O2 -w -I "$tools" -I . -DOBJLIBDIR=\""$sdk/lib"\" \
    collect-aros.c misc.c backend-generic.c docommand-exec.c gensets.c -o "$tools/collect-aros")

cd "$repo_root"
for f in src/pkg_main.c src/pkg_lib.c src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c \
         src/pkg_ed25519.c src/pkg_manifest.c src/pkg_image.c src/pkg_ameta.c src/pkg_archive.c src/pkg_bzip2.c src/pkg_fs_posix.c src/pkg_out.c \
         src/pkg_port.c src/pkg_style.c src/pkg_tls_aros.c "$out/pkg_cabundle.c"; do
    # The AROS clang predefines these; the stock one, which knows the triple
    # but not the platform, does not, and without __AROS__ Pkg would take its
    # POSIX output path, which reaches no AmigaDOS redirection.
    "$llvm/bin/clang" --target=x86_64-unknown-aros -mcmodel=large -O2 -std=gnu11 \
        -D__AROS__=1 -D__AROS=1 -DAROS=1 -DAMIGA=1 -D_AMIGA=1 \
        -Wall -Wextra -Werror \
        -isystem "$sdk/include" -isystem "$sdk/include/aros/posixc" -isystem "$sdk/include/aros/stdc" \
        -isystem "$openssl/include" \
        -I include -I third_party/bzip2 -c "$f" -o "$out/obj/$(basename "$f" .c).o"
done
"$tools/collect-aros" -o "$out/Pkg" "$sdk/lib/startup.o" "$out"/obj/*.o -L"$sdk/lib" \
    --allow-multiple-definition --start-group \
    "$openssl/lib/libssl.a" "$openssl/lib/libcrypto.a" \
    -lrexxsyslib -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport --end-group
chmod 755 "$out/Pkg"
echo "build-aros-x86_64: $out/Pkg"
