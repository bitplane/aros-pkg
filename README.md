# Pkg

Pkg installs, updates and removes software on AROS, and publishes it. Every
package is signed, every file is checked before anything is written, and an
update you did not ask for never happens: Pkg refuses and tells you why. The
same program runs on AROS, where it manages your system, and on macOS, Linux
and Windows, where you build, publish and test packages.

## Install Pkg

**On AROS.** Get the drawer that carries Pkg for your machine (from the
package portal, or any volume or share that holds a Pkg channel) and run its
install script, naming the drawer:

```amigados
Execute Work:Pkg/Install-Pkg Work:Pkg
```

The script picks the build for your CPU and installs Pkg into `SYS:` as a
signed package, so later versions arrive with `Pkg UPGRADE pkg`.

**On macOS, Linux and Windows.** Build it from this repository with a C99
compiler:

```sh
make                    # build/pkg
make install            # into ~/.local: pkg, pkg.h, libpkg.a
make build/pkg.exe      # Windows, cross-built with mingw-w64
```

## Your first five minutes

The examples below run on a Mac or a PC, where a *root* is a directory that
stands for an AROS system, and the channel is read over the network. On
AROS, use `ROOT SYS:` instead of `ROOT aros`, and name a copy of the
channel on a volume, since Pkg does not read channels over the network
there yet ([Channels](docs/channels.md)).

See what a channel offers. A channel is where packages are published; this
one holds the contrib programs of the AROS nightly build:

```console
$ pkg SHOW lua CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly
lua                  0+20260918 application x86_64   ok         a974a917b19cfc46
```

Install it:

```console
$ pkg INSTALL lua ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly ARCH x86_64
installed lua 0+20260918 into aros: 57 files, from AROS-20260918-pc-x86_64-contrib.tar.bz2!/AROS-20260918-pc-x86_64-contrib, signed by a974a917b19cfc46
```

The contrib packages keep their files in the nightly's archive, which Pkg
downloads once from SourceForge and keeps in its cache; the next package
from the same nightly needs no download. `ARCH x86_64` says which machine
the root is for; on AROS, Pkg knows its own.

See what is installed, and check it:

```console
$ pkg LIST ROOT aros
lua                      0+20260918 application  57 files
$ pkg VERIFY lua ROOT aros
lua 0+20260918: 57 files, all intact
```

Ask whether anything can be updated, then update everything:

```console
$ pkg STATUS ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly
lua                      0+20260918   current
1 package in aros, 0 upgradable from https://aros-pkg.azurewebsites.net/contrib-nightly
$ pkg UPGRADE ALL ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly
nothing needs an update: 1 package, none with a newer version in the channel
```

Remove it:

```console
$ pkg REMOVE lua ROOT aros
removed lua 0+20260918 from aros: 57 files removed
```

`STATUS` and `UPGRADE ALL` never ask anything, so you can run them from
`S:User-Startup`, cron or any scheduler.

## When Pkg says no

A refusal says what happened and what to do next, and its exit code names
the kind of problem: 11 not found, 12 a file that does not match its
signature, 13 no valid signature, 14 a different publisher's key, 15
something in the way (such as a file you edited), 16 a dependency, 17 the
file system, 18 a downgrade you did not ask for, 20 a mistyped command. On
AROS every refusal is at least 10, so `If ERROR` catches all of them.

## Guides

- [Using Pkg](docs/using.md): installing, updating, checking, repairing,
  rolling back and removing software; images; what each refusal means.
- [Publishing packages](docs/publishing.md): keys, making a package,
  versions, dependencies, configuration files, withdrawing, uploading to
  the portal.
- [Channels](docs/channels.md): what a channel holds, serving one over
  HTTP, the portal.
- [Reference](docs/reference.md): every verb, keyword and environment
  variable; the machine-readable output; the ARexx port.

Pkg is written in C99, with no dependency beyond the C library (bzip2 is
included). It builds as a command and as a library, `libpkg`, for programs
that want to install software themselves. MIT licence.

How Pkg is built and tested, and its open questions:
[docs/development.md](docs/development.md).
