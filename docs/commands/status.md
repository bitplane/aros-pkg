<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# STATUS

What is installed, and what has a newer version.
```
pkg STATUS [<name>] ROOT <root> [CHANNEL <channel>] [ARCH cpu]
```

## What it does

Compares every installed package (or one) with the channel and gives each a
state. It writes nothing and asks nothing, so it runs from a script or a
scheduler; the exit code is 0 whether or not updates exist.

| State | Meaning |
|---|---|
| `current` | nothing newer is offered |
| `upgradable to v` | `UPGRADE` would take `v` |
| `withdrawn by its publisher` | the installed version was withdrawn; what the channel offers instead is named |
| `no longer offered by the channel` | the package is not in the channel any more |
| `files edited since install` | `VERIFY` names them; an upgrade would keep them |
| `published for <cpu>, not yet for this root's CPU` | a newer version exists for another machine |

Without `CHANNEL` it reads the channels the root lists ([CHANNEL](channel.md)), in order; `CHANNEL <channel>` means that channel alone.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel VERSION 1.0
  added    hellolib 1.0, a dependency
installed helloworld 1.0 into aros: 2 files, payload 7959be27f4ef, signed by 5ff18d3fe14e383e
$ pkg STATUS ROOT aros CHANNEL channel
Package     Installed  State
hellolib    1.0        current
helloworld  1.0        upgradable to 1.1
1 of 2 packages in aros can be updated from channel
  hint: UPGRADE ALL ROOT aros CHANNEL channel upgrades every one of them, a package before what depends on it; with DRYRUN it only says what it would do
$ pkg STATUS helloworld ROOT aros CHANNEL channel MACHINE
result: shown
package: helloworld 1.0 1.1 upgradable
count: 1
upgradable: 1
summary: 1 of 1 package can be updated
hint: UPGRADE ALL ROOT aros CHANNEL channel upgrades every one of them, a package before what depends on it; with DRYRUN it only says what it would do
```

## Records (`MACHINE`)

`result: shown`, one `package: name installed available state` line per
package, `count:`, `upgradable:`, `summary:`. `STATUS <name>` for a package
not installed: exit 11, `next: use-install`.

## Related

[UPGRADE](upgrade.md) and `UPGRADE ALL`, [VERIFY](verify.md),
[Using pkg](../using.md#keep-it-current).
