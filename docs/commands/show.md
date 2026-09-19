<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# SHOW

What a channel offers, each entry checked.
```
pkg SHOW [<name>] CHANNEL <channel> [ROOT <root>] [METADATA] [ARCHIVE <name>] [ARCH cpu]
```

## What it does

Reads the channel's index and, for every published version (or every
version of `<name>`), checks its signature, its manifest and its payload,
and prints one row: name, version, kind, CPU, status (`ok`, or what is
wrong), signer. With `ROOT`, marks the versions installed there. `SHOW
<name>` also prints the catalogue of the newest version: short description,
category, tags, author, licence, links. `METADATA` also checks the archives
that packages published from someone else's archive point to; `ARCHIVE
<name>` limits that to one archive. Exit 0 when every entry is sound;
otherwise the class of the first problem, and the count of bad entries.

## Examples

```console
$ pkg SHOW CHANNEL channel
Package     Version  Kind         Arch     Status  Signer
hellolib    1.0      library      generic  ok      5ff18d3fe14e383e
helloworld  1.0      application  generic  ok      5ff18d3fe14e383e
helloworld  1.1      application  generic  ok      5ff18d3fe14e383e
notes       1.0      image        generic  ok      5ff18d3fe14e383e
sdl2        2.30     library      aarch64  ok      5ff18d3fe14e383e
$ pkg SHOW helloworld CHANNEL channel
Package     Version  Kind         Arch     Status  Signer
helloworld  1.0      application  generic  ok      5ff18d3fe14e383e
helloworld  1.1      application  generic  ok      5ff18d3fe14e383e
$ pkg INSTALL hellolib ROOT aros CHANNEL channel
installed hellolib 1.0 into aros: 1 file, payload af76683f5719, signed by 5ff18d3fe14e383e
$ pkg SHOW CHANNEL channel ROOT aros MACHINE
result: shown
entry: hellolib 1.0 library generic ok 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111 installed
entry: helloworld 1.0 application generic ok 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111 no
depends: helloworld 1.0 hellolib
entry: helloworld 1.1 application generic ok 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111 no
depends: helloworld 1.1 hellolib
entry: notes 1.0 image generic ok 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111 no
entry: sdl2 2.30 library aarch64 ok 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111 no
count: 5
bad: 0
```

## Records (`MACHINE`)

`result: shown`, one `entry: name version kind arch status signer` per
version (and its fields apart for a program), `problem:` for a bad one,
`installed:` with `ROOT`, the catalogue fields (`short:`, `category:`,
`tag:`, `author:`, `homepage:`, `repository:`, `license:`,
`distribution:`, `description:`, `changes:`) for `SHOW <name>`, `count:`,
`bad:`, `warning:` when a package is signed by more than one key,
`ignored: name version key` for each manifest key Pkg does not know (for a
person: `ignored, for other systems: X-Aminet-Type` under the row).

## Related

[Channels](../channels.md), [INSTALL](install.md), [PUBLISH](publish.md).
