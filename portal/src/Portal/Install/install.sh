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
is_ours() { [ -x "$1" ] && "$1" HELP 2>/dev/null | head -1 | grep -q '^Pkg '; }
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
