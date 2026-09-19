<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# INSTALL

Install a package and what it depends on.
```
pkg INSTALL <name> ROOT <root> CHANNEL <channel> [VERSION v] [ARCH cpu] [ACCEPTKEY <key>] [DRYRUN]
```

## What it does

`INSTALL` takes the newest version of `<name>` the channel offers for the
root's CPU (or `VERSION v`), checks its signature and every file's digest,
installs the packages it depends on first, then places its files under the
root and records the package in the root's database (`.pkg/`).

- A file already in place and byte for byte the package's is **adopted**:
  nothing is written, and the package is recorded as installed. This is how
  a system copied by hand comes under Pkg afterwards.
- A file already in place with other content, belonging to no package, is a
  conflict (exit 15): Pkg overwrites nothing it did not install.
- A package installed only as a dependency is marked so; `REMOVE ORPHANS`
  takes it out when nothing needs it any more. Installing it explicitly
  later keeps it for itself.
- The first version installed pins the publisher's key for that package;
  a later version signed by another key is refused (exit 14) until
  `ACCEPTKEY <public key>` names the new one. That decision is the person's.
- Installing the version already installed succeeds and says so
  (`result: unchanged`); a newer one asks for `UPGRADE` (exit 15).
- A withdrawn version is not installed unless `VERSION` names it (exit 18).

## Keywords

| Keyword | Meaning |
|---|---|
| `ROOT <dir>` | the system installed into: `SYS:` on AROS, a directory elsewhere |
| `CHANNEL <dir\|url>` | where the package is published |
| `VERSION v` | this version instead of the newest |
| `ARCH cpu` | the root's CPU, when the root has never been told and Pkg cannot know it |
| `ACCEPTKEY <key>` | accept a publisher key other than the one pinned |
| `DRYRUN` | every check, no write; the result reads `would install` |

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
helloworld 1.1 is already installed in aros
$ pkg INSTALL notes ROOT aros CHANNEL channel VERSION 1.0 DRYRUN
would install notes 1.0 into aros: 1 file, payload cfac33cc9d05, signed by 5ff18d3fe14e383e
  image    notes.hdf, 32 blocks
  hint: to run it, mount the image: MOUNTLIST notes ROOT aros OUT <file> writes the mount entry and lists the steps
$ pkg INSTALL nosuch ROOT aros CHANNEL channel    # exits 11
pkg install: nosuch is not in the channel channel
  next: check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, pkg LIST ROOT <dir> what a root holds
$ pkg INSTALL helloworld ROOT aros CHANNEL channel MACHINE
result: unchanged
name: helloworld
version: 1.1
```

## Records (`MACHINE`)

`result:` (`installed`, `unchanged`, `would-install`, `refused`), `name:`,
`version:`, `root:`, `files:`, `payload:` or `source:`, `signer:`;
`dependency:` for each package brought along; `adopted:`,
`unchanged-files:`, `config-kept:`, `config-new:` when files were already
there; `image:` and `blocks:` for an image; `first-signer:` and `pinned:`
around a key refusal.

## Refusals

| Exit | When |
|---|---|
| 11 | no such package or version in the channel; a dependency the channel does not offer |
| 12 | a manifest, payload or archive that does not match what was signed |
| 13 | unsigned, or a signature that does not verify |
| 14 | signed by another key than the one pinned; `next: ask-requester` |
| 15 | a file in the way; the version installed is newer (`next: use-upgrade`) |
| 16 | a dependency too old, missing, or in a cycle |
| 17 | the file system refused |
| 18 | the version is withdrawn and `VERSION` did not name it |

## Related

[STATUS](status.md), [UPGRADE](upgrade.md), [REMOVE](remove.md),
[MOUNTLIST](mountlist.md) for images, [Using Pkg](../using.md#install),
[Distributing builds](../distributing.md) for `ARCH`.
