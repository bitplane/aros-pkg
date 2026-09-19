# Using Pkg

This guide is for keeping software on an AROS system: finding it, installing
it, keeping it current, checking it, repairing it and removing it.

Every command names a **root**, the system it works on, and most name a
**channel**, where packages come from. On AROS the root is usually `SYS:`;
the examples below run on a Mac or a PC, where the root is a directory,
`aros`, and the channel a directory, `channel`, with a few example packages.
A channel can also be a web address, such as
`https://aros-pkg.azurewebsites.net/contrib-nightly`; nothing else changes.

Words in capitals are keywords; you can type them in any case, as in any
AmigaDOS command.

## See what a channel offers

```console
$ pkg SHOW CHANNEL channel
hellolib             1.0      library     generic  ok         5ff18d3fe14e383e
helloworld           1.0      application generic  ok         5ff18d3fe14e383e
helloworld           1.1      application generic  ok         5ff18d3fe14e383e
notes                1.0      image       generic  ok         5ff18d3fe14e383e
```

Each line is a package version: its name, version, kind, the CPU it is built
for, whether it checks out, and the key that signed it. Name a package to see
only its versions:

```console
$ pkg SHOW helloworld CHANNEL channel
helloworld           1.0      application generic  ok         5ff18d3fe14e383e
helloworld           1.1      application generic  ok         5ff18d3fe14e383e
```

## Install

```console
$ pkg INSTALL helloworld VERSION 1.0 ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
```

Without `VERSION`, Pkg installs the newest version. `helloworld` needs the
library `hellolib`, so Pkg installs that first; everything is fetched and
checked before a single file is placed, so a failure anywhere leaves your
system as it was.

The first time you install a package, Pkg remembers the key that signed it.
A later version signed by another key is refused (exit 14) until you
decide, with `ACCEPTKEY` and the new key in full, that the change is
legitimate.

See what is installed:

```console
$ pkg LIST ROOT aros
hellolib                 1.0        library      1 files, a dependency
helloworld               1.0        application  2 files
```

## Keep it current

Ask what can be updated:

```console
$ pkg STATUS ROOT aros CHANNEL channel
hellolib                 1.0          current
helloworld               1.0          upgradable to 1.1
2 packages in aros, 1 upgradable from channel
  hint: UPGRADE ALL ROOT aros CHANNEL channel upgrades every one of them, a package before what depends on it; with DRYRUN it only says what it would do
```

Update one package, or all of them:

```console
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
upgraded helloworld from 1.0 to 1.1 in aros: 2 placed, 0 removed
$ pkg UPGRADE ALL ROOT aros CHANNEL channel
nothing needs an update: 2 packages, none with a newer version in the channel
```

`UPGRADE ALL` updates every package that has a newer version, each before
the packages that need it, and goes as far as it can: a package it cannot
update is named with the reason, and the others are updated anyway. It
never downgrades and never accepts a new key; those are decisions you make
one package at a time.

`STATUS` and `UPGRADE ALL` never ask a question and never wait for input,
so a startup script or a scheduler can run them. Add `DRYRUN` to see what
would happen without changing anything:

```console
$ pkg UPGRADE ALL ROOT aros CHANNEL channel DRYRUN
nothing needs an update: 2 packages, none with a newer version in the channel
```

### Files you edit

Some files are meant to be edited: preferences, startup scripts. A package
declares them, and Pkg never overwrites your version. When a new version of
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
because Pkg does not know whether the change is damage or your work.

### Go back

`ROLLBACK` returns a package to the version installed before its last
change, taken again from the channel:

```console
$ pkg ROLLBACK helloworld ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.0's version is beside it as S/HelloWorld.prefs.pkgnew)
rolled back helloworld from 1.1 to 1.0 in aros: 1 placed, 0 removed, 1 kept
$ pkg LIST ROOT aros
hellolib                 1.0        library      1 files, a dependency
helloworld               1.0        application  2 files
```

To install an older version on purpose, name it and add `DOWNGRADE`:
without it, an older version is refused (exit 18).

## Check and repair

`VERIFY` compares every installed file with what its package holds:

```console
$ pkg VERIFY ALL ROOT aros
hellolib 1.0: 1 file, all intact
  edited   S/HelloWorld.prefs (helloworld, a configuration file)
helloworld 1.0: 2 files, all intact
2 packages, 3 files, all intact; configuration files edited, as people do
```

Delete a file by accident, and `VERIFY` names it; `REPAIR` puts it back
from the channel:

```console
$ rm aros/C/HelloWorld
$ pkg VERIFY ALL ROOT aros    # exits 12
hellolib 1.0: 1 file, all intact
  missing  C/HelloWorld (helloworld)
  edited   S/HelloWorld.prefs (helloworld, a configuration file)
helloworld 1.0: 0 changed, 1 missing, of 2 files
1 of 2 packages damaged, first helloworld (0 changed, 1 missing)
  hint: VERIFY <name> also says which missing files were moved by hand. Pkg overwrites no changed file: whether the change is damage or someone's work is the requester's call
$ pkg REPAIR ALL ROOT aros CHANNEL channel
  restored C/HelloWorld
helloworld: 1 file put back
1 file put back in 1 package
```

A file that was changed rather than deleted is kept beside the repaired one
with `.pkgold` added to its name, so nothing you wrote is lost. Files you
edit on purpose, the declared ones above, are left as they are.

## Remove

```console
$ pkg REMOVE helloworld ROOT aros
  kept     S/HelloWorld.prefs (changed since install, so it is yours now)
removed helloworld 1.0 from aros: 1 files removed, 1 kept
  hellolib 1.0 is no longer needed by anything; REMOVE ORPHANS takes it out
```

Pkg removes only files that are as it installed them; a file you changed
stays, and Pkg says so. A library installed only because something needed
it becomes an *orphan* when nothing needs it any more; remove orphans in
one go:

```console
$ pkg REMOVE ORPHANS ROOT aros
removed hellolib 1.0, which nothing needed: 1 files
removed 1 package nothing needed any more
```

Pkg refuses to remove a package that another installed package needs, and
names which.

## Programs as images

A package of kind `image` installs as one file, a disk image holding the
program, which you mount like a disk. Nothing is spread over your system,
and removing the package removes the program entirely.

```console
$ pkg INSTALL notes ROOT aros CHANNEL channel
installed notes 1.0 into aros: 1 files, payload cfac33cc9d05, signed by 5ff18d3fe14e383e
  image notes.hdf, 32 blocks
  hint: to run it, mount the image: MOUNTLIST notes ROOT aros OUT <file> writes the mount entry and lists the steps
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

## When Pkg refuses

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

For a script or a program that reads Pkg's answers, add `MACHINE`: every
answer is then `key: value` lines, the same on every system.

```console
$ pkg LIST ROOT aros MACHINE
result: listed
package: notes 1.0 image 1 explicit
count: 1
```

The [reference](reference.md) lists every line a command can answer. To
find out why something happened, add `TRACE <file>`: Pkg writes every step
it took, every file it touched and every check it made.
