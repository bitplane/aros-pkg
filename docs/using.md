# Using pkg

This guide is for keeping software on an AROS system: finding it, installing
it, keeping it current, checking it, repairing it and removing it.

Every command names a **root**, the system it works on, and most name a
**channel**, where packages come from. On AROS the root is usually `SYS:`;
the examples below run on a Mac or a PC, where the root is a directory,
`aros`, and the channel a directory, `channel`, with a few example packages.
A channel can also be a web address, such as
`https://aros-pkg.azurewebsites.net/contrib-nightly`; nothing else changes,
on AROS as anywhere else. Plain `http://` is read too, for a server you run
yourself.

Words in capitals are keywords; you can type them in any case, as in any
AmigaDOS command.

## Say where packages come from, once

A root keeps its own list of channels, so you do not type `CHANNEL` on
every command:

```console
$ pkg CHANNEL ADD channel ROOT aros
added channel to aros, in place 1: it offers 4 packages
  hint: INSTALL, UPGRADE, STATUS, SHOW, REPAIR, ROLLBACK and SEARCH now read this channel when CHANNEL is left out; CHANNEL <dir|url> on the line still means that channel alone
$ pkg CHANNEL LIST ROOT aros
In  Channel
1   channel
1 channel in aros, asked in this order
```

From then on `INSTALL`, `UPGRADE`, `STATUS`, `SHOW`, `SEARCH`, `REPAIR`
and `ROLLBACK` read that list when no `CHANNEL` is given, asking the
channels in the order they were added and taking the newest version any of
them offers. `CHANNEL <channel>` on the line still means that channel and
no other, and `CHANNEL REMOVE` takes one off the list without removing
anything from the root.

Two listed channels offering the same package under different keys is
refused, naming both, until you have installed it once from the channel
whose key is the publisher's. Adding a channel is never a way for someone
to replace another publisher's package quietly. See
[CHANNEL](commands/channel.md).

## Find a package

`SEARCH` gives the packages whose name, short description, tags, category,
description or libraries hold every word:

```console
$ pkg SEARCH media ROOT aros
Package  Version  Arch     Short
sdl2     2.30     aarch64  Simple DirectMedia Layer
1 package matches media
$ pkg SEARCH hello ROOT aros
Package     Version  Arch     Short
hellolib    1.0      generic  -
helloworld  1.1      generic  -
2 packages match hello
```

Finding nothing is an answer, not a refusal: it exits 0. A portal channel
answers through its own search interface, so one question is one round
trip rather than a few hundred; see [SEARCH](commands/search.md).

## See what a channel offers

```console
$ pkg SHOW CHANNEL channel
Package     Version  Kind         Arch     Status  Signer
hellolib    1.0      library      generic  ok      5ff18d3fe14e383e
helloworld  1.0      application  generic  ok      5ff18d3fe14e383e
helloworld  1.1      application  generic  ok      5ff18d3fe14e383e
notes       1.0      image        generic  ok      5ff18d3fe14e383e
sdl2        2.30     library      aarch64  ok      5ff18d3fe14e383e
```

Each line is a package version: its name, version, kind, the CPU it is built
for, whether it checks out, and the key that signed it. Name a package to see
only its versions:

```console
$ pkg SHOW helloworld CHANNEL channel
Package     Version  Kind         Arch     Status  Signer
helloworld  1.0      application  generic  ok      5ff18d3fe14e383e
helloworld  1.1      application  generic  ok      5ff18d3fe14e383e
```

## Install

```console
$ pkg INSTALL helloworld VERSION 1.0 ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
```

Without `VERSION`, pkg installs the newest version. `helloworld` needs the
library `hellolib`, so pkg installs that first; everything is fetched and
checked before a single file is placed, so a failure anywhere leaves your
system as it was.

The first time you install a package, pkg remembers the key that signed it.
A later version signed by another key is refused (exit 14) until you
decide, with `ACCEPTKEY` and the new key in full, that the change is
legitimate.

Several names on one line install in turn:

```console
$ pkg INSTALL notes hellolib ROOT aros CHANNEL channel
  notes    installed 1.0: 1 file, signed by 5ff18d3fe14e383e
  hellolib 1.0 was installed as a dependency; it is now kept for itself
installed 2 packages into aros
```

It goes as far as it can: a name it cannot install is reported with its
reason and the rest are installed anyway. The exit code is the worst class
any of them refused with.

### Where the files come from

Some channels, the AROS contrib channel among them, publish packages whose
files stay inside one large archive the AROS build makes. pkg downloads that
archive once into its cache and says where it put it. The first install that
reads it writes a **block map** beside it, so every later install
decompresses only the blocks holding its own files instead of the whole
archive. Each install says, in one line, which it used and where it is:

- `the archive is kept at <path>` when it was just downloaded,
- `reading the archive already in the cache at <path>`,
- `reading only the blocks its files lie in, out of <path>` with the map,
- `reading the unpacked archive at <path>`,

and, with it, the cache directory, which `PKG_CACHE` moves elsewhere.

If you would rather unpack the archive yourself, `UNPACKED <dir>` reads the
files from a directory holding what the archive holds
(`<dir>/<prefix>/<path>`). Unpack it into the cache and pkg finds it without
being told:

```
mkdir -p ~/.cache/pkg/upstream/<sha256>/<archive>.d
tar xjf <archive> -C ~/.cache/pkg/upstream/<sha256>/<archive>.d
```

Every file is weighed and hashed against the signed manifest before anything
is written, whichever of these it came from; one that differs is refused
(exit 12) by name. See [Channels](channels.md).

See what is installed:

```console
$ pkg LIST ROOT aros
Package     Version  Kind         Files
hellolib    1.0      library      1 file
helloworld  1.0      application  2 files
notes       1.0      image        1 file
```

## Keep it current

Ask what can be updated:

```console
$ pkg STATUS ROOT aros CHANNEL channel
Package     Installed  State
hellolib    1.0        current
helloworld  1.0        upgradable to 1.1
notes       1.0        current
1 of 3 packages in aros can be updated from channel
  hint: UPGRADE ALL ROOT aros CHANNEL channel upgrades every one of them, a package before what depends on it; with DRYRUN it only says what it would do
```

Update one package, or all of them:

```console
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
upgraded helloworld from 1.0 to 1.1 in aros: 2 placed, 0 removed
$ pkg UPGRADE ALL ROOT aros CHANNEL channel
nothing needs an update: 3 packages, none with a newer version in the channel
```

`UPGRADE ALL` updates every package that has a newer version, each before
the packages that need it, and goes as far as it can: a package it cannot
update is named with the reason, and the others are updated anyway. Its
exit code is the worst class of what it refused, not the first, which is
what every pkg command given several things to do does. It never downgrades
and never accepts a new key; those are decisions you make one package at a
time. With several channels listed, `STATUS` and `UPGRADE ALL` name the
channel each new version comes from.

`STATUS` and `UPGRADE ALL` never ask a question and never wait for input,
so a startup script or a scheduler can run them. Add `DRYRUN` to see what
would happen without changing anything:

```console
$ pkg UPGRADE ALL ROOT aros CHANNEL channel DRYRUN
nothing needs an update: 3 packages, none with a newer version in the channel
```

### Files you edit

Some files are meant to be edited: preferences, startup scripts. A package
declares them, and pkg never overwrites your version. When a new version of
such a file arrives, it is set down beside yours with `.pkgnew` added to its
name, and you merge what you want:

```console
$ pkg ROLLBACK helloworld ROOT aros CHANNEL channel
rolled back helloworld from 1.1 to 1.0 in aros: 2 placed, 0 removed
$ echo "Greeting=Hi" > aros/S/HelloWorld.prefs
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
upgraded helloworld from 1.0 to 1.1 in aros: 1 placed, 0 removed, 1 kept
$ cat aros/S/HelloWorld.prefs.pkgnew
Greeting=Hello
Colour=Blue
```

Any other file you changed stops the update of that package (exit 15),
because pkg does not know whether the change is damage or your work.

### Go back

`ROLLBACK` returns a package to the version installed before its last
change, taken again from the channel:

```console
$ pkg ROLLBACK helloworld ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.0's version is beside it as S/HelloWorld.prefs.pkgnew)
rolled back helloworld from 1.1 to 1.0 in aros: 1 placed, 0 removed, 1 kept
$ pkg LIST ROOT aros
Package     Version  Kind         Files
hellolib    1.0      library      1 file
helloworld  1.0      application  2 files
notes       1.0      image        1 file
```

To install an older version on purpose, name it and add `DOWNGRADE`:
without it, an older version is refused (exit 18).

## Check and repair

`VERIFY` compares every installed file with what its package holds:

```console
$ pkg VERIFY ALL ROOT aros
Package     Version  Files    State
hellolib    1.0      1 file   intact
helloworld  1.0      2 files  intact, configuration edited
  edited   S/HelloWorld.prefs (a configuration file)
notes       1.0      1 file   intact
3 packages, 4 files, all intact; configuration files edited, as people do
```

Delete a file by accident, and `VERIFY` names it; `REPAIR` puts it back
from the channel:

```console
$ rm aros/C/HelloWorld
$ pkg VERIFY ALL ROOT aros    # exits 12
Package     Version  Files    State
hellolib    1.0      1 file   intact
helloworld  1.0      2 files  1 missing
  missing  C/HelloWorld
  edited   S/HelloWorld.prefs (a configuration file)
notes       1.0      1 file   intact
1 of 3 packages damaged
  hint: REPAIR ALL ROOT <dir> CHANNEL <dir> puts missing and changed files back from the channel. VERIFY <name> also says which missing files were moved by hand
$ pkg REPAIR ALL ROOT aros CHANNEL channel
  restored C/HelloWorld
  helloworld 1 file put back
1 file put back in 1 package
```

A file that was changed rather than deleted is kept beside the repaired one
with `.pkgold` added to its name, so nothing you wrote is lost. Files you
edit on purpose, the declared ones above, are left as they are.

## Remove

```console
$ pkg REMOVE helloworld ROOT aros
  kept     S/HelloWorld.prefs (changed since install, so it is yours now)
removed helloworld 1.0 from aros: 1 file removed, 1 kept
```

pkg removes only files that are as it installed them; a file you changed
stays, and pkg says so. A library installed only because something needed
it becomes an *orphan* when nothing needs it any more; remove orphans in
one go:

```console
$ pkg REMOVE ORPHANS ROOT aros
nothing to remove: every installed package is wanted or needed by one that is
```

pkg refuses to remove a package that another installed package needs, and
names which. To take pkg itself off the system, or to move to a pkg that
cannot read this one's database, see [Removing pkg](removing.md).

## Programs as images

A package of kind `image` installs as one file, a disk image holding the
program, which you mount like a disk. Nothing is spread over your system,
and removing the package removes the program entirely.

```console
$ pkg INSTALL notes ROOT aros CHANNEL channel
notes 1.0 is already installed in aros
$ pkg MOUNTLIST notes ROOT aros OUT aros/Devs/DOSDrivers/NOTES
wrote aros/Devs/DOSDrivers/NOTES, the mount entry for notes (32 blocks)
  On AROS, the device is named after the mountlist file:
    MakeDir RAM:fdsk
    Assign FDSK: RAM:fdsk
    MakeLink RAM:fdsk/Unit20 aros/notes.hdf
    Protect aros/notes.hdf w SUB
    Mount aros/Devs/DOSDrivers/NOTES
  hint: no FFS handler is installed in this root, so the entry relies on the system's. Native AROS has one; hosted AROS built on macOS has none: there, install one into the root as a device package, or name one with HANDLER <path>
```

`MOUNTLIST` writes the entry AmigaDOS mounts the image with, and lists the
steps. On AROS, with the root `SYS:`, they are:

```amigados
MakeDir RAM:fdsk
Assign FDSK: RAM:fdsk
MakeLink RAM:fdsk/Unit20 SYS:notes.hdf
Protect SYS:notes.hdf w SUB
Mount DEVS:DOSDrivers/NOTES
NOTES:Notes
```

## When a program opens the wrong library

A program that does not start often opens an old or missing library. See
which copy AROS gives it, and why, with `RESOLVE`:

```console
$ pkg INSTALL sdl2 ROOT aros CHANNEL channel
installed sdl2 2.30 into aros: 1 file, payload e6f50f1bd01d, signed by 5ff18d3fe14e383e
$ pkg RESOLVE SDL2.library ROOT aros FROM Game
Where                File                       Version  Package             Verdict
PROGDIR:             Game/SDL2.library          -        -                   no file
PROGDIR:libs/        Game/libs/SDL2.library     2.0      not from a package  taken, and hides the newer aros/Libs/SDL2.library 2.30
LIBS: (SYS:Libs)     aros/Libs/SDL2.library     2.30     sdl2 2.30           not reached: found earlier
LIBS: (SYS:Classes)  aros/Classes/SDL2.library  -        -                   no file
  PROGDIR:libs/: remove Game/libs/SDL2.library: aros/Libs/SDL2.library 2.30 is newer and would then be taken
SDL2.library resolves to Game/libs/SDL2.library 2.0
```

[Libraries](libraries.md) explains the order AROS searches in, and what
each verdict means.

## When pkg refuses

A refusal says what happened, and the line after it what to do. The exit
code names the kind of problem:

| Code | Kind | What it usually means |
|---|---|---|
| 11 | not found | no such package or version in the channel |
| 12 | integrity | a file does not match what the signed package says |
| 13 | signature | no signature, or one that does not verify |
| 14 | key | signed by another key than the one you accepted before |
| 15 | conflict | something is in the way, such as a file you edited |
| 16 | dependency | something a package needs is missing, or too old |
| 17 | io | the file system refused, or the network failed |
| 18 | policy | a downgrade you did not ask for |
| 20 | usage | the command is mistyped |

```console
$ pkg INSTALL hellowrld ROOT aros CHANNEL channel    # exits 11
pkg install: hellowrld is not in the channel channel; did you mean helloworld
  next: check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, pkg LIST ROOT <dir> what a root holds
```

Codes 12, 13 and 14 protect you: never work around them. On AROS every
refusal is 10 or more, so a script catches all of them with `If ERROR`,
and `$RC` holds the exact code.

For a script or a program that reads pkg's answers, add `MACHINE`: every
answer is then `key: value` lines, the same on every system.

```console
$ pkg LIST ROOT aros MACHINE
result: listed
package: hellolib 1.0 library 1 explicit
package: notes 1.0 image 1 explicit
package: sdl2 2.30 library 1 explicit
count: 3
```

The [reference](reference.md) lists every line a command can answer. To
find out why something happened, add `TRACE <file>`: pkg writes every step
it took, every file it touched and every check it made.
