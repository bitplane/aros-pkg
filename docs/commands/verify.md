<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# VERIFY

Every installed file against its signed manifest.
```
pkg VERIFY <name> ROOT <root>
pkg VERIFY ALL    ROOT <root>
```

## What it does

Reads each installed file and compares it with the size and SHA-256 its
signed manifest gave. A file is `missing`, `changed`, or, if the package
declared it a configuration file, `edited`, which is not damage. `VERIFY
<name>` also looks for missing files elsewhere under the root and reports
them as `moved` when it finds them intact: moving a drawer is the person's
right. Exit 0 when everything is intact or edited; 12 when anything is
missing or changed.

`VERIFY ALL` gives one row per package and lists the damaged files under
their package. Pkg overwrites no changed file by itself: `REPAIR` does, on
request.

## Examples

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg VERIFY helloworld ROOT aros
helloworld 1.1: 2 files, all intact
$ rm aros/C/HelloWorld
$ printf 'Greeting=Hi\n' > aros/S/HelloWorld.prefs
$ pkg VERIFY ALL ROOT aros    # exits 12
Package     Version  Files    State
hellolib    1.0      1 file   intact
helloworld  1.1      2 files  1 missing
  missing  C/HelloWorld
  edited   S/HelloWorld.prefs (a configuration file)
1 of 2 packages damaged
  hint: REPAIR ALL ROOT <dir> CHANNEL <dir> puts missing and changed files back from the channel. VERIFY <name> also says which missing files were moved by hand
$ pkg VERIFY helloworld ROOT aros MACHINE    # exits 12
name: helloworld
version: 1.1
files: 2
edited: S/HelloWorld.prefs
missing: C/HelloWorld
result: damaged
class: integrity
code: 12
```

## Records (`MACHINE`)

`result:` (`intact`, `damaged`, `moved`), `name:`, `version:`, `files:`,
one `missing:`, `changed:`, `edited:` or `moved: from to` per file,
`edited-config:`; `ALL`: `package: name version intact|damaged changed missing`
per package, `packages:`, `summary:`, and `class:`/`code:` when damaged.

## Related

[REPAIR](repair.md), [Using Pkg](../using.md#check-and-repair).
