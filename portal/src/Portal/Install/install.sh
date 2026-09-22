#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Installs Pkg, the AROS package tool, on macOS or Linux, where you can run it
# by name. Served by the package portal:
#
#   curl -fsSL @@PORTAL@@/install | sh
#
# What it does, in order:
#   1. picks the build for this computer (uname -s, uname -m);
#   2. downloads it from @@PORTAL@@/get/pkg/<platform> into a temporary file;
#   3. checks that it runs here ("pkg HELP");
#   4. puts it where you can run it: where an earlier Pkg already is, else
#      /usr/local/bin when you can write there (or with sudo, when you are at a
#      terminal to type the password), else ~/.local/bin;
#   5. when that directory is not on your PATH, adds one line to your shell's
#      start-up file (~/.zshrc, ~/.bashrc, ~/.bash_profile or fish's config) and
#      says which; PKG_NO_MODIFY_PATH=1 prints the line instead.
#   4. before that, checks it against Bootstrap/SHA256SUMS, whose signature
#      ssh-keygen -Y verify checks with the key below (when the portal has one);
# Running it again installs the newest Pkg over the old one.
# PKG_SKIP_VERIFY=1 installs without that check, saying so.
# PKG_INSTALL_DIR=<dir> chooses the directory yourself; PKG_SYSTEM_BIN names the
# system-wide one (/usr/local/bin by default, which macOS has on its PATH).

set -eu

portal="@@PORTAL@@"
# The key that signs the list of bootstrap files, as the portal publishes it (/trust).
sshkey="@@SSHKEY@@"
say() { printf '%s\n' "$*"; }
fail() { printf 'install: %s\n' "$*" >&2; exit 1; }

# 1. This computer.
os=$(uname -s)
cpu=$(uname -m)
case "$os/$cpu" in
    Darwin/arm64)                 platform=macos-arm64 ;;
    Darwin/x86_64)                platform=macos-x86_64 ;;
    Linux/x86_64 | Linux/amd64)   platform=linux-x86_64 ;;
    Linux/aarch64 | Linux/arm64)  platform=linux-arm64 ;;
    *) fail "there is no Pkg build for $os on $cpu yet; build it from its source: https://github.com/jonx/aros-pkg" ;;
esac

# 2. Download.
tmp=$(mktemp "${TMPDIR:-/tmp}/pkg.XXXXXX")
trap 'rm -f "$tmp"' EXIT INT TERM
url="$portal/get/pkg/$platform"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$tmp" "$url" || fail "could not download $url"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tmp" "$url" || fail "could not download $url"
else
    fail "neither curl nor wget is installed"
fi
chmod 755 "$tmp"

# 3. It is the file its publisher signed, and it runs here, before anything is replaced.
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
get() { if command -v curl >/dev/null 2>&1; then curl -fsSL -o "$2" "$1"; else wget -q -O "$2" "$1"; fi; }
if [ -n "${PKG_SKIP_VERIFY:-}" ]; then
    say "Warning: PKG_SKIP_VERIFY is set, so this Pkg is not checked against its signed checksum."
elif [ -z "$sshkey" ]; then
    say "Note: this portal publishes no signed checksum list yet; the download is protected by https alone."
else
    command -v ssh-keygen >/dev/null 2>&1 || fail "ssh-keygen is needed to check the download's signature (OpenSSH 8.1 or later); PKG_SKIP_VERIFY=1 installs without the check"
    sums="$tmp.sums"; sig="$tmp.sums.sig"; signers="$tmp.signers"
    trap 'rm -f "$tmp" "$sums" "$sig" "$signers"' EXIT INT TERM
    get "$portal/pkg/Bootstrap/SHA256SUMS" "$sums" || fail "could not download the checksum list"
    get "$portal/pkg/Bootstrap/SHA256SUMS.sig" "$sig" || fail "could not download the checksum list's signature"
    printf 'pkg namespaces="aros-pkg-bootstrap" %s\n' "$sshkey" > "$signers"
    ssh-keygen -Y verify -f "$signers" -I pkg -n aros-pkg-bootstrap -s "$sig" < "$sums" >/dev/null 2>&1 \
        || fail "the checksum list is not signed by the key this portal names; nothing was installed"
    want=$(awk -v p="Bootstrap/$platform/pkg" '$2 == p { print $1 }' "$sums")
    [ -n "$want" ] || fail "the signed checksum list names no Bootstrap/$platform/pkg"
    [ "$(sha "$tmp")" = "$want" ] || fail "the download does not match its signed checksum; nothing was installed"
    say "Checked: the download matches the checksum signed by the portal's bootstrap key."
fi
"$tmp" HELP >/dev/null 2>&1 || fail "the Pkg downloaded for $platform does not run on this computer"
version=$("$tmp" HELP 2>&1 | awk 'NR == 1 { print $2 }')

# 4. Where it goes. An earlier Pkg is upgraded where it is; another program
#    named pkg (a system's own package tool) is left alone.
is_ours() { [ -x "$1" ] && "$1" HELP 2>/dev/null | head -1 | grep -q '^[Pp]kg '; }
sys_bin=${PKG_SYSTEM_BIN:-/usr/local/bin}
sudo_cmd=
if [ -n "${PKG_INSTALL_DIR:-}" ]; then
    dir=$PKG_INSTALL_DIR
elif old=$(command -v pkg 2>/dev/null) && is_ours "$old" && [ -w "$(dirname "$old")" ]; then
    dir=$(dirname "$old")
elif is_ours "$HOME/.local/bin/pkg"; then
    dir=$HOME/.local/bin
elif [ -d "$sys_bin" ] && [ -w "$sys_bin" ]; then
    dir=$sys_bin
elif command -v sudo >/dev/null 2>&1 && [ -t 1 ] && (: < /dev/tty) 2>/dev/null; then
    # At a terminal: the system-wide directory, created if a fresh Mac lacks it.
    dir=$sys_bin
    sudo_cmd=sudo
    say "Pkg goes into $sys_bin, which needs your password (sudo)."
else
    dir=$HOME/.local/bin
fi
if [ -n "$sudo_cmd" ]; then
    # The script itself arrives on stdin (curl | sh): sudo asks for the password on the terminal.
    # shellcheck disable=SC2024
    { sudo mkdir -p "$dir" && sudo install -m 755 "$tmp" "$dir/pkg"; } < /dev/tty || fail "could not install into $dir"
else
    mkdir -p "$dir" || fail "could not create $dir"
    # Replace in one step, so a running Pkg is never half written.
    if ! { cp "$tmp" "$dir/.pkg.new" && chmod 755 "$dir/.pkg.new" && mv -f "$dir/.pkg.new" "$dir/pkg"; }; then
        rm -f "$dir/.pkg.new"
        fail "could not install into $dir"
    fi
fi

# 5. Reachable by name from a new shell.
case ":$PATH:" in
    *":$dir:"*) ;;
    *)
        shell=$(basename "${SHELL:-sh}")
        case "$shell" in
            zsh)  rc=$HOME/.zshrc;  line="export PATH=\"$dir:\$PATH\"" ;;
            bash) if [ "$os" = Darwin ]; then rc=$HOME/.bash_profile; else rc=$HOME/.bashrc; fi
                  line="export PATH=\"$dir:\$PATH\"" ;;
            fish) rc=$HOME/.config/fish/config.fish; line="fish_add_path \"$dir\"" ;;
            *)    rc=$HOME/.profile; line="export PATH=\"$dir:\$PATH\"" ;;
        esac
        if [ -n "${PKG_NO_MODIFY_PATH:-}" ]; then
            say "$dir is not on your PATH. Add this line to $rc:"
            say "  $line"
        elif [ -f "$rc" ] && grep -qF "$line" "$rc"; then
            say "$rc already adds $dir to PATH; open a new terminal to use it."
        else
            mkdir -p "$(dirname "$rc")"
            printf '\n# Pkg, the AROS package tool\n%s\n' "$line" >> "$rc"
            say "Added $dir to PATH in $rc; open a new terminal to use it."
        fi
        ;;
esac

# Another program called pkg earlier on PATH would be run instead, or be shadowed.
other=$(command -v pkg 2>/dev/null || true)
if [ -n "$other" ] && [ "$other" != "$dir/pkg" ] && ! is_ours "$other"; then
    say "Note: another program called pkg is at $other; in a new terminal, 'pkg' is whichever comes first on PATH."
fi

say "pkg $version installed in $dir; run: pkg HELP"

# 6. A hosted AROS on this computer (Macaros): its shared folder is a volume
#    inside AROS, so Pkg for AROS is put there too, checked like the above,
#    and one line in the AROS Shell finishes it. PKG_AROS_SHARED names another
#    folder; PKG_NO_AROS=1 skips this.
shared=${PKG_AROS_SHARED:-$HOME/AROS/Shared}
if [ -z "${PKG_NO_AROS:-}" ] && [ -d "$shared" ] && command -v unzip >/dev/null 2>&1; then
    case "$platform" in *arm64) acpu=aarch64 ;; *) acpu=x86_64 ;; esac
    zip="$tmp.zip"; stage="$tmp.aros"
    rm -rf "$stage"; mkdir -p "$stage"
    if get "$portal/get/pkg/pkg-$acpu.zip" "$zip" && unzip -q -o "$zip" -d "$stage"; then
        good=1
        if [ -z "${PKG_SKIP_VERIFY:-}" ] && [ -n "$sshkey" ]; then
            for f in "Bootstrap/$acpu/Pkg" Install-Pkg ReadMe; do
                want=$(awk -v p="$f" '$2 == p { print $1 }' "$sums")
                [ -n "$want" ] && [ "$(sha "$stage/Pkg-$acpu/$f")" = "$want" ] || good=0
            done
        fi
        if [ "$good" = 1 ]; then
            rm -rf "$shared/Pkg-$acpu"
            mv "$stage/Pkg-$acpu" "$shared/Pkg-$acpu"
            say ""
            say "AROS on this computer: Pkg for AROS ($acpu) is in $shared, checked the same way."
            say "In the AROS Shell, paste this line:"
            say "  Execute MacRW:Pkg-$acpu/Install-Pkg MacRW:Pkg-$acpu"
        else
            say "Note: the AROS drawer did not match the signed checksums and was not put in $shared."
        fi
    fi
    rm -rf "$zip" "$stage"
fi

# 7. Optional root registration. Empty answers accept the displayed defaults.
env_name=${PKG_ENV_NAME:-}
env_root=${PKG_ENV_ROOT:-}
env_default=${PKG_ENV_DEFAULT:-0}
env_interactive=0
suggested_root=$HOME/AROS/System
case "${XDG_CONFIG_HOME:-}" in
    /*) env_config=$XDG_CONFIG_HOME/aros-pkg/environments.conf ;;
    *) env_config=$HOME/.config/aros-pkg/environments.conf ;;
esac
ask_env() {
    printf '%s ' "$1" > /dev/tty
    if ! IFS= read -r answer < /dev/tty; then
        say "Environment setup cancelled. pkg is installed; run pkg ENV ADD later."
        exit 0
    fi
}
yes_no() {
    while :; do
        ask_env "$1"
        case "$answer" in
            '') answer=$2; return ;;
            y|Y|yes|YES) answer=y; return ;;
            n|N|no|NO) answer=n; return ;;
            *) say "Please answer y or n, or press Return for the default." ;;
        esac
    done
}
if [ -n "$env_name" ] || [ -n "$env_root" ]; then
    [ -n "$env_name" ] && [ -n "$env_root" ] || fail "set both PKG_ENV_NAME and PKG_ENV_ROOT to register an environment"
elif [ -z "${PKG_NO_ENV:-}" ] && [ -t 1 ] && (: < /dev/tty) 2>/dev/null; then
    say "Optional environments let pkg find a package root without ROOT on each command."
    say "Personal configuration: $env_config"
    yes_no 'Configure an environment? [y/N]' n
    [ "$answer" = y ] && env_interactive=1
fi
while [ "$env_interactive" = 1 ] || [ -n "$env_name" ]; do
    if [ "$env_interactive" = 1 ]; then
        say "Press Return to accept a suggestion; type cancel to finish setup later."
        ask_env "Environment name [${env_name:-aros}]:"
        [ "$answer" != cancel ] || break
        env_name=${answer:-${env_name:-aros}}
        case "$env_name" in
            *[!a-zA-Z0-9_.-]*|'') say "Use letters, digits, dots, underscores or hyphens."; env_name=; continue ;;
        esac
        if [ "${#env_name}" -gt 63 ]; then say "Use at most 63 characters."; env_name=; continue; fi
        ask_env "Package root [${env_root:-$suggested_root}]:"
        [ "$answer" != cancel ] || break
        env_root=${answer:-${env_root:-$suggested_root}}
    fi
    case "$env_root" in '~/'*) env_root=$HOME/${env_root#\~/} ;; esac
    case "$env_root" in
        /*) ;;
        *)
            if [ "$env_interactive" = 1 ]; then say "Enter an absolute path, for example $suggested_root."; env_root=; continue; fi
            fail "PKG_ENV_ROOT must be an absolute path" ;;
    esac
    if ! mkdir -p "$env_root"; then
        if [ "$env_interactive" = 1 ]; then say "Cannot create that directory. Choose a writable location."; env_root=; continue; fi
        fail "cannot create package root $env_root"
    fi
    nonempty=0
    for entry in "$env_root"/* "$env_root"/.[!.]* "$env_root"/..?*; do
        if [ -e "$entry" ] || [ -L "$entry" ]; then nonempty=1; break; fi
    done
    if [ "$nonempty" = 1 ]; then
        say "This directory already contains files: $env_root"
        say "Registration keeps those files. Future package operations will use this root and its .pkg database."
        if [ "$env_interactive" = 1 ]; then
            yes_no 'Use this existing directory? [y/N]' n
            if [ "$answer" != y ]; then env_root=; continue; fi
        elif [ "${PKG_ENV_REUSE:-0}" != 1 ]; then
            fail "confirm this existing root with PKG_ENV_REUSE=1, or choose an empty directory"
        fi
    fi
    say "Registering $env_name with root $env_root in $env_config"
    if ! "$dir/pkg" ENV ADD "$env_name" ROOT "$env_root"; then
        if [ "$env_interactive" = 1 ]; then say "Registration failed. Choose another name or root, or type cancel. pkg remains installed."; env_name=; continue; fi
        fail "pkg was installed; environment registration failed"
    fi
    if [ "$env_interactive" = 1 ]; then
        yes_no 'Use this environment by default? [Y/n]' y
        env_default=0
        [ "$answer" != y ] || env_default=1
    fi
    if [ "$env_default" = 1 ]; then
        "$dir/pkg" ENV DEFAULT "$env_name" || fail "environment registered; setting its default failed"
    fi
    break
done
