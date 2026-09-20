<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Distributing builds for several platforms

AROS runs on several CPUs (aarch64, x86_64, m68k, and more), and a program
built for one does not run on another. pkg publishes one *version* of a
program with one *build per CPU*, and each machine installs the build it can
run. This guide is for a publisher who ships more than one build, and for
the person who runs a channel that serves several kinds of machine. For
publishing itself, read [Publishing packages](publishing.md) first.

## What pkg knows about a build

pkg reads the CPU from the executables themselves: an ELF header says
aarch64 or x86_64, a hunk header says m68k. A drawer whose executables agree
gets that architecture; a drawer with none (data, fonts, catalogs, scripts)
is `generic` and installs everywhere. A drawer that mixes CPUs is refused,
naming each CPU and a file that has it, because a package is one build.

```console
$ pkg MANIFEST MyTool-aarch64 KIND application
Format: pkg-manifest 1
Name: mytool
Version: 1.0
Architecture: aarch64
Kind: application
Payload: c943b57f1703c08b7ec46c29deb2a8fb485a43e8b6588a2eb24bde684ae53787
File: b4a900b2256d0d1a2308d492292a9f1929968d655c9430a6c59209af38360e46 94 C/MyTool
Protect: 0x00000002 C/MyTool
```

`ARCH <cpu>` at `PUBLISH` names the CPU when the files cannot say it: a
drawer with no executable that is still bound to one machine, or a `boot`
or `sdk` package, which legitimately carries files for other CPUs and is
accepted only when `ARCH` names the machine it is for. `ARCH` that
contradicts the executables of an ordinary package is refused.

## One version, several builds

Publish the same version once per build. The channel's index gets one line
per CPU, `mytool 1.1 aarch64 ...` and `mytool 1.1 x86_64 ...`, and `SHOW`
lists them side by side:

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key b4caa1c2c6c4df56ddfb76f394111d2e72d1d246254b672183acd8ae472bf3d1
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ export PKG_SIGNKEY=my.key
$ pkg PUBLISH MyTool-aarch64 CHANNEL mychannel KIND application
published mytool 1.0 to mychannel: 1 file, payload c943b57f1703, signed by b673dd18b5ca39ca
  architecture aarch64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  hint: the channel mychannel did not exist and was created
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg PUBLISH MyTool CHANNEL mychannel KIND application
published mytool 1.0 to mychannel: 1 file, payload bf9bc6edcb45, signed by b673dd18b5ca39ca
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  dependencies from mytool 1.0, published before
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg SHOW mytool CHANNEL mychannel
Package  Version  Kind         Arch     Status  Signer
mytool   1.0      application  aarch64  ok      b4caa1c2c6c4df56
mytool   1.0      application  x86_64   ok      b4caa1c2c6c4df56
```

The two drawers should be the same release: same `$VER:` string, same files
apart from the binaries. pkg checks the version and warns when a `$VER:`
contradicts `VERSION`; it does not compare the drawers' contents.

`WITHDRAW mytool VERSION 1.1 ARCH x86_64` withdraws one build; without
`ARCH`, every build of that version.

## How a machine picks its build

At `INSTALL`, the machine's CPU is, in this order: `ARCH <cpu>` on the
command; what the root recorded at its first CPU-specific install; on AROS,
the CPU pkg itself runs on. A root on a Mac or a PC that has never been told
gets its first CPU-specific package only with `ARCH`, and remembers it. From
then on dependencies, upgrades and rollbacks stay on that CPU, and a version
offered only for another CPU is reported as such by `STATUS`
(`not yet for this root's CPU`), never installed.

`generic` packages install on every root.

## pkg itself, for every AROS machine

pkg is distributed the same way, as a package. `make aros-channel
CHANNEL=<dir>` publishes each AROS build present under `build/` (aarch64,
x86_64, m68k when built) into a channel, with an `Install-Pkg` script that
picks the build for the machine it runs on and installs pkg into `SYS:` as a
signed package, so later versions arrive with `Pkg UPGRADE pkg`:

```sh
sh tools/build-aros.sh             # aarch64
sh tools/build-aros-x86_64.sh      # x86_64
PKG_SIGNKEY=~/.pkg-dev.key make aros-channel CHANNEL=~/depot
```

On the AROS machine, with the channel reachable as `DEPOT:`:

```amigados
Execute DEPOT:Install-Pkg DEPOT:
```

The same script also copies the host builds it finds (`build/pkg-macos`,
`build/pkg-linux-x86_64`, `build/pkg-linux-aarch64`, `build/pkg.exe`) into
the channel under `Bootstrap/`, which is what the portal's download page
serves.

## A channel for several kinds of machine

One channel serves them all: a machine reads the index, takes the lines for
its CPU and the `generic` ones, and ignores the rest. There is no reason to
split a channel by CPU. Split by *trust* instead: one channel per publisher
or per purpose, since the machines that install from it pin that
publisher's key per package ([Channels](channels.md)).

## Builds you did not make

The AROS nightly and other archives already contain builds per CPU. Publish
them as they are, from the archive, with `FILES` naming the paths of one
package and `UPSTREAM` where the archive lives; the CPU is read from the
files inside ([Packages from someone else's archive](publishing.md#packages-from-someone-elses-archive)).
