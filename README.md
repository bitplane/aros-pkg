# pkg

pkg installs, updates and removes software on AROS, and publishes it. Every
package is signed, every file is checked before anything is written, and an
update you did not ask for never happens: pkg refuses and tells you why. The
same program runs on AROS, where it manages your system, and on macOS, Linux
and Windows, where you build, publish and test packages.

pkg is being built to become part of the official AROS distribution, once it
has stabilised a little more. Until then it lives here, with its
portal, and changes as the work needs.

This page is for using pkg. If you are here to **publish your own
programs**, read [Publishing packages](docs/publishing.md); to **run your own
channel**, [Channels](docs/channels.md); to **ship builds for several
CPUs**, [Distributing builds](docs/distributing.md); to **move an existing
distribution, archive or package manager to pkg**,
[Moving to pkg](docs/migrating.md); to **take pkg itself off a system**,
[Removing pkg](docs/removing.md); and if **an AI assistant does the
typing for you**, [pkg with an AI assistant](docs/agents.md).

## Install pkg

**On AROS, with a network and `wget`** (AROS One, Icaros, or the nightly
with its contrib). Paste these two lines in the Shell:

```amigados
wget -q -O RAM:Get-Pkg http://aros-pkg.azurewebsites.net/Get-Pkg
Execute RAM:Get-Pkg
```

The script fetches the pkg for your CPU, and that pkg installs the signed
`pkg` package into `SYS:` from the portal's channel.

If `wget` ends at once without a word, it wants its settings file first:
`Echo "" >ENV:wgetcfg` (and the same to `ENVARC:` to keep it).

**On Macaros, or any AROS hosted on your computer.** Run the macOS and Linux
line below in the computer's terminal: it also puts the AROS drawer in the
folder AROS shares (`~/AROS/Shared`), checked against the signed checksums,
and prints the one line to paste in the AROS Shell:

```amigados
Execute MacRW:Pkg-aarch64/Install-Pkg MacRW:Pkg-aarch64
```

**On AROS without a network.** The [downloads page](https://aros-pkg.azurewebsites.net/downloads)
gives `pkg-<cpu>.zip` for each CPU, a small drawer that is itself a pkg
channel: unzip it on your Mac or PC, copy the drawer to the AROS machine (a
share, a USB stick, an image), and run its install script, naming the drawer
as the machine sees it:

```amigados
Execute Work:Pkg-x86_64/Install-Pkg Work:Pkg-x86_64
```

Either way pkg is installed as a signed package, so later versions arrive
with `Pkg UPGRADE pkg ROOT SYS: CHANNEL https://aros-pkg.azurewebsites.net/pkg`.

**On macOS and Linux.** One line downloads the build for your computer,
checks it runs, puts it where your shell finds it (`/usr/local/bin`, or
`~/.local/bin` with a PATH line added to your shell's start-up file) and
says where it went:

```sh
curl -fsSL https://aros-pkg.azurewebsites.net/install | sh
```

Open a new terminal, and `pkg HELP` answers. The script is plain `sh`,
[readable before you run it](https://aros-pkg.azurewebsites.net/install);
run it again to upgrade. To place the file yourself, the downloads page
has each build: `curl -fsSLo pkg https://aros-pkg.azurewebsites.net/get/pkg/macos-arm64
&& chmod +x pkg && sudo mv pkg /usr/local/bin/` (or `macos-x86_64`,
`linux-x86_64`, `linux-arm64`).

**On Windows.** In PowerShell:

```powershell
irm https://aros-pkg.azurewebsites.net/install.ps1 | iex
```

which puts `pkg.exe` under `%LOCALAPPDATA%\Programs\pkg` and adds it to
your `Path`; or download
`https://aros-pkg.azurewebsites.net/get/pkg/windows-x86_64` as `pkg.exe`
and put it on your `Path` yourself.

**From source**, on any of them, with a C99 compiler:

```sh
make                    # build/pkg
make install            # into ~/.local: pkg, pkg.h, libpkg.a
make build/pkg.exe      # Windows, cross-built with mingw-w64
```

## Embed update checks in your application

Link your C application with `libpkg` to check for a newer compatible build
and display its version and the publisher's signed release notes. Supply
the package name, installation root and channel in code or a `.pkgupdate`
configuration file. pkg reads the installed version from its database.

The check leaves the installation unchanged. Your application chooses when
to check and asks the user before requesting an ordinary pkg upgrade.
The [online integration guide](https://aros-pkg.azurewebsites.net/docs/self-update)
covers the API, configuration and error handling; its
[repository copy](docs/self-update.md) links the minimal C client and a
local HTTP demonstration comparing two 1.7.0 builds:

```sh
make build/pkg build/example-selfupdate
sh examples/selfupdate-demo.sh
```

## Your first five minutes

The examples below run on a Mac or a PC, where a *root* is a directory that
stands for an AROS system, and the channel is read over the network. On
AROS, use `ROOT SYS:` instead of `ROOT aros`; the addresses are the same,
since AROS reads channels over `https` too ([Channels](docs/channels.md)).
A copy of the channel on a volume works too, with no network at all.

See what a channel offers. A channel is where packages are published; this
one holds pkg itself, built for two CPUs:

```console
$ pkg SHOW CHANNEL https://aros-pkg.azurewebsites.net/pkg
Package  Version  Kind         Arch     Status  Signer
pkg      1.1      application  aarch64  ok      43c550967bc18dfe
pkg      1.1      application  x86_64   ok      43c550967bc18dfe
```

Every entry was checked on the way: its signature, its description, its
files. Install the newest into a root:

```console
$ pkg INSTALL pkg ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/pkg ARCH x86_64
installed pkg 1.1 into aros: 1 file, payload 08916a1ece2a, signed by 43c550967bc18dfe
```

`ARCH x86_64` says which machine the root is for, and the root remembers
it; on AROS, pkg knows its own. See what is installed, and check it:

```console
$ pkg LIST ROOT aros
Package  Version  Kind         Files
pkg      1.1      application  1 file
$ pkg VERIFY pkg ROOT aros
pkg 1.1: 1 file, all intact
```

Typing the channel on every command gets old. Put it in the root's own
list once, and leave `CHANNEL` off from then on:

```console
$ pkg CHANNEL ADD https://aros-pkg.azurewebsites.net/pkg ROOT aros
added https://aros-pkg.azurewebsites.net/pkg to aros, in place 1: it offers 1 package
  hint: INSTALL, UPGRADE, STATUS, SHOW, REPAIR, ROLLBACK and SEARCH now read this channel when CHANNEL is left out; CHANNEL <dir|url> on the line still means that channel alone
$ pkg SEARCH pkg ROOT aros
Package  Version  Arch            Short
pkg      1.1      aarch64,x86_64  Installs and updates AROS software
1 package matches pkg
```

Ask whether anything can be updated, then update everything:

```console
$ pkg STATUS ROOT aros
Package  Installed  State
pkg      1.1        current
1 package in aros, all up to date with https://aros-pkg.azurewebsites.net/pkg
$ pkg UPGRADE ALL ROOT aros
nothing needs an update: 1 package, none with a newer version in the channel
```

Remove it:

```console
$ pkg REMOVE pkg ROOT aros
removed pkg 1.1 from aros: 1 file removed
```

`STATUS` and `UPGRADE ALL` never ask anything, so you can run them from
`S:User-Startup`, cron or any scheduler. At a terminal the same lines come
with colour and marks; piped or logged they are the plain text above.

The portal's other channel, `contrib-nightly`, holds the hundred-odd
programs of the AROS nightly build (`lua`, `wget`, `xadmaster`, ...). Their
files stay in the nightly's own archive, which pkg downloads from
SourceForge once (about 640 MB for x86_64) and keeps in its cache for every
package from that nightly.

## When pkg says no

A refusal says what happened and what to do next
([Signatures and trust](docs/signing.md) explains the ones that protect
you), and its exit code names
the kind of problem: 11 not found, 12 a file that does not match its
signature, 13 no valid signature, 14 a different publisher's key, 15
something in the way (such as a file you edited), 16 a dependency, 17 the
file system, 18 a downgrade you did not ask for, 20 a mistyped command. On
AROS every refusal is at least 10, so `If ERROR` catches all of them.

## Guides

Using pkg

- [Using pkg](docs/using.md): installing, updating, checking, repairing,
  rolling back and removing software; images; what each refusal means.
- [Libraries](docs/libraries.md): how AROS finds a library, what that means
  for packages, and RESOLVE, which shows why a program gets the copy it gets.
- [Signatures and trust](docs/signing.md): what is signed and by whom, what
  is checked where, what a key change means for you, checking a package by
  hand.
- [Removing pkg](docs/removing.md): taking pkg itself off a system, what it
  leaves behind, and moving to a pkg that cannot read this one's database.
- [pkg with an AI assistant](docs/agents.md): the skill an agent loads, what
  to ask it, what it will not decide for you.

Publishing

- [Publishing packages](docs/publishing.md): keys, making a package,
  versions, dependencies, configuration files, withdrawing, uploading to
  the portal.
- [Publishing from an AROS machine, from zero](docs/publishing-on-aros.md):
  every step typed in the AROS Shell, from putting pkg on the machine to your
  program on the portal and its second version, with what each command prints.
- [Channels](docs/channels.md): what a channel holds, serving one over
  HTTP or a share, the portal.
- [Distributing builds for several platforms](docs/distributing.md): one
  version, one build per CPU, how a machine picks its build, pkg's own
  channel.
- [Moving an existing distribution to pkg](docs/migrating.md): archives,
  `.readme` files, Installer scripts, machines already set up, another
  package manager.

Every command

- [Commands](docs/commands/README.md): one page per command, with examples
  that run, what it prints and records, and its refusals.
- [Reference](docs/reference.md): every verb, keyword and environment
  variable on one page; the machine-readable output; the ARexx port.

pkg is written in C99, with no dependency beyond the C library: bzip2 is
included, and so is Mbed TLS (Apache-2.0), which the AROS builds use for
`https`. It builds as a command and as a library, `libpkg`, for programs
that want to install software themselves. Programs can also
[check their own updates](docs/self-update.md), with a C example and a
configuration-file interface. MIT licence; the included
libraries keep theirs, in `third_party/`.

## For contributors

[Developing pkg](docs/development.md) (build, test, the library),
[the portal's source](portal/README.md), [the container](docs/container.md),
[AROS defects found while building pkg](docs/aros-defects.md),
[how pkg was established](docs/history.md), [GOAL.md](GOAL.md) and
[OPEN.md](OPEN.md).
