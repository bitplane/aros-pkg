<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# REMOVE

Take a package out, or the packages nothing needs any more.
```
pkg REMOVE <name>  ROOT <root> [DRYRUN]
pkg REMOVE ORPHANS ROOT <root> [DRYRUN]
```

## What it does

`REMOVE <name>` deletes the package's files and its database entry. A file
changed since install is kept (`kept`, it is the person's now); a file
already gone is counted, not an error. A package another installed package
depends on is not removed (exit 16). Afterwards, packages that were
installed only as dependencies of this one and are needed by nothing else
are named: removing them is a separate, explicit act.

`REMOVE ORPHANS` removes exactly those: every package installed as a
dependency that nothing needs any more. Packages the person installed
explicitly are never orphans.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg REMOVE hellolib ROOT aros    # exits 16
pkg remove: hellolib is needed by helloworld 1.1; nothing was removed. Removing it would break it
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
$ pkg REMOVE helloworld ROOT aros
removed helloworld 1.1 from aros: 2 files removed
  hellolib 1.0 is no longer needed by anything; REMOVE ORPHANS takes it out
$ pkg REMOVE ORPHANS ROOT aros
  hellolib 1.0 removed, which nothing needed (1 file)
removed 1 package nothing needed any more
$ pkg LIST ROOT aros
nothing installed in aros
```

## Records (`MACHINE`)

`result:` (`removed`, `would-remove`), `name:`, `version:`, `root:`,
`removed:`, `kept:`, `gone:`, `note:` naming what is now an orphan.
`ORPHANS`: one `package: name version` per package removed, `count:`,
`summary:`.

## Refusals

| Exit | When |
|---|---|
| 11 | not installed |
| 16 | another package needs it |
| 17 | a file could not be deleted |

## Related

[INSTALL](install.md), [Using Pkg](../using.md#remove).
