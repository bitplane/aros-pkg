#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Cross-build the Pkg client for AROS aarch64, hosted or native, with the
# recipe the AFS+ handler's own C commands use. Output: build/aros/Pkg.
#
# The host layer is the POSIX one: AROS links against its posixc library, so
# opendir, stat, rename and mkdir are there. Whether their path semantics match
# what the client assumes is what the hosted run exists to establish.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
aros_build=${AROS_BUILD:-"$HOME/aros-build"}
aros_crosstools=${AROS_CROSSTOOLS:-"$HOME/aros-crosstools"}
sdk=${PKG_AROS_SDK_ROOT:-"$aros_build/bin/darwin-aarch64"}
build_tools=${PKG_AROS_BUILD_TOOLS_ROOT:-"$sdk/tools"}
aros_target=${PKG_AROS_TARGET:-aarch64-unknown-aros}
aros_arch_flags=${PKG_AROS_ARCH_FLAGS:--mcmodel=large -ffixed-x18}
aros_clang="$aros_crosstools/bin/clang"
aros_cross_lib=${PKG_AROS_CROSS_LIB:-"$aros_crosstools/lib/generic"}
developer="$sdk/AROS/Developer"
out="$repo_root/build/aros"

for need in "$aros_clang" "$developer/lib/startup.o" "$sdk/gen/config/target.cfg"; do
    [ -e "$need" ] || { echo "build-aros: missing $need" >&2; exit 69; }
done

mkdir -p "$out"
cd "$repo_root"

# shellcheck disable=SC2086 -- the platform profile supplies separate flags.
COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
    "$aros_clang" --target="$aros_target" $aros_arch_flags \
    -O2 -std=gnu11 \
    -Wall -Wextra -Werror \
    -isystem "$developer/include" \
    -isystem "$sdk/gen/include" \
    -isystem "$sdk/gen/include/aros/posixc" \
    -isystem "$developer/include/aros/stdc" \
    -nostartfiles -nodefaultlibs \
    -L "$developer/lib" -L "$aros_cross_lib" \
    -I include -I third_party/bzip2 \
    "$developer/lib/startup.o" \
    src/pkg_main.c src/pkg_lib.c src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c \
    src/pkg_ed25519.c src/pkg_manifest.c src/pkg_image.c src/pkg_ameta.c src/pkg_archive.c src/pkg_bzip2.c src/pkg_fs_posix.c src/pkg_out.c src/pkg_port.c \
    -o "$out/Pkg" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lrexxsyslib -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport \
    -Wl,--end-group -lclang_rt.builtins-aarch64
chmod 755 "$out/Pkg"
echo "build-aros: $out/Pkg"

# The test probe, built against the AFS+ client library, read-only.
afsplus=${AFSPLUS_ROOT:-"$repo_root/../afsplus"}
if [ -f "$afsplus/native/aros/client/afsplus_client.c" ]; then
    # shellcheck disable=SC2086
    COMPILER_PATH="$build_tools:$aros_crosstools/bin" \
        "$aros_clang" --target="$aros_target" $aros_arch_flags \
        -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-pointer-sign \
        -isystem "$developer/include" \
        -isystem "$sdk/gen/include" \
        -isystem "$sdk/gen/include/aros/posixc" \
        -isystem "$developer/include/aros/stdc" \
        -nostartfiles -nodefaultlibs \
        -L "$developer/lib" -L "$aros_cross_lib" \
        -I "$afsplus/api" -I "$afsplus/native/aros/client" \
        "$developer/lib/startup.o" \
        tools/aros/pkg_handler_rev.c "$afsplus/native/aros/client/afsplus_client.c" \
        -o "$out/PkgHandlerRev" \
        -Wl,--allow-multiple-definition -Wl,--start-group \
        -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros \
        -lautoinit -llibinit -lutility -lamiga -larossupport \
        -Wl,--end-group -lclang_rt.builtins-aarch64
    chmod 755 "$out/PkgHandlerRev"
    echo "build-aros: $out/PkgHandlerRev"
else
    echo "build-aros: no AFS+ tree at $afsplus; PkgHandlerRev skipped"
fi
