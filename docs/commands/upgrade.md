<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# UPGRADE

Move a package, or every package, to a newer version.
```
pkg UPGRADE <name> ROOT <root> CHANNEL <channel> [VERSION v] [ARCH cpu] [DOWNGRADE] [ACCEPTKEY <key>] [DRYRUN]
pkg UPGRADE ALL    ROOT <root> CHANNEL <channel> [ARCH cpu] [DRYRUN]
```

## What it does

`UPGRADE <name>` replaces the installed version with the newest the channel
offers for the root's CPU, or with `VERSION v`. Files the person edited
(`CONFIG` files, or any file whose content changed since install) are kept:
the new one is put beside as `<file>.pkgnew`, and the result says `kept`.
The previous version is recorded so `ROLLBACK` can return to it. An image
package is replaced whole; a machine that has it mounted must eject it
first.

An older version is refused (exit 18) unless `DOWNGRADE` says so. A version
signed by another key is refused (exit 14) unless `ACCEPTKEY` names it.

`UPGRADE ALL` upgrades every package the channel has a newer version for, a
package before what depends on it. It never downgrades and never accepts a
new key. It goes as far as it can: a package it cannot upgrade is named
with its reason, what depends on it is skipped and named, and the rest are
upgraded. Exit 0 when everything offered was taken, otherwise the class of
the first refusal.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel VERSION 1.0
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
$ printf 'Greeting=Hi\n' > aros/S/HelloWorld.prefs
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
upgraded helloworld from 1.0 to 1.1 in aros: 1 placed, 0 removed, 1 kept
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel VERSION 1.0    # exits 18
pkg upgrade: helloworld 1.0 is older than the installed 1.1; nothing was changed. Going back a version is the requester's decision
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
$ pkg UPGRADE helloworld ROOT aros CHANNEL channel VERSION 1.0 DOWNGRADE
  kept     S/HelloWorld.prefs (edited; helloworld 1.0's version is beside it as S/HelloWorld.prefs.pkgnew)
downgraded helloworld from 1.1 to 1.0 in aros: 1 placed, 0 removed, 1 kept
$ pkg UPGRADE ALL ROOT aros CHANNEL channel DRYRUN
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
  helloworld would upgrade from 1.0 to 1.1: 1 placed, 0 removed, 1 kept
would update 1 package
$ pkg UPGRADE ALL ROOT aros CHANNEL channel
  kept     S/HelloWorld.prefs (edited; helloworld 1.1's version is beside it as S/HelloWorld.prefs.pkgnew)
  helloworld upgraded from 1.0 to 1.1: 1 placed, 0 removed, 1 kept
updated 1 package
```

## Records (`MACHINE`)

One package: `result:` (`upgraded`, `downgraded`, `unchanged`, `would-upgrade`),
`name:`, `version:`, `from:`, `placed:`, `removed:`, `kept:`,
`config-kept:`, `config-new:`, `image:`. `ALL`: one `package: name from to`
per package upgraded, `not-upgraded:` and `skipped:` with reasons,
`count:`, `upgraded:`, `summary:`.

## Refusals

| Exit | When |
|---|---|
| 11 | the package is not installed (`next: use-install`); no newer version |
| 14 | a new publisher key; `next: ask-requester` |
| 15 | a file the upgrade would replace was edited and is not a `CONFIG` file |
| 16 | a dependency the new version needs cannot be met |
| 18 | an older version without `DOWNGRADE`; the installed version was withdrawn and nothing newer is offered |

## Related

[ROLLBACK](rollback.md), [STATUS](status.md), [Using Pkg](../using.md#keep-it-current).
