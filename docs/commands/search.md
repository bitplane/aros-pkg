<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# SEARCH

The packages a channel offers that every word matches.
```
pkg SEARCH <word>... [CHANNEL <channel>] [ROOT <root>] [ARCH cpu]
```

## What it does

Looks for packages whose name, `Short`, `Tags`, `Category`, `Description` or
`Provides` hold every word given, whatever the case. Only the newest version
of each package counts, and a version its publisher withdrew is not offered.
With `ARCH cpu`, or a root that has recorded a CPU, only the versions built
for that CPU and the `generic` ones are shown.

Without `CHANNEL` it searches the channels the root lists
([CHANNEL](channel.md)); the channel is then a column of the table and a
field of each record.

Finding nothing is an answer, not a refusal: exit 0, `count: 0`.

## How a channel is read

A channel on a disk, or on a plain web server, is read as it stands: its
index, then the manifest of each package's newest version, which the network
layer caches.

A portal channel is asked first at its own `/api/search`, one request per
word, the answers intersected by name so that every word still has to match.
Reading two hundred manifests over the network to answer one question is
minutes of waiting, and the portal answers the same question itself in one
round trip. If the portal has no such API, or answers something Pkg cannot
read, the channel is read the long way instead and nothing is said about it:
the answer is the same, only slower.

## Examples

```console
$ pkg SEARCH media CHANNEL channel
Package  Version  Arch     Short
sdl2     2.30     aarch64  Simple DirectMedia Layer
1 package matches media
$ pkg SEARCH hello CHANNEL channel MACHINE
result: shown
package: hellolib 1.0 generic -
package: helloworld 1.1 generic -
count: 2
summary: 2 packages match hello
$ pkg SEARCH simple directmedia CHANNEL channel
Package  Version  Arch     Short
sdl2     2.30     aarch64  Simple DirectMedia Layer
1 package matches simple directmedia
$ pkg SEARCH nothinglikethis CHANNEL channel MACHINE
result: shown
count: 0
summary: nothing matches nothinglikethis
hint: every word must match, in the name, the short description, the tags, the category, the description or what the package provides; fewer words match more
```

## Records (`MACHINE`)

`result: shown`, one `package: <name> <version> <arch> <short>` line per
package (`package: <name> <version> <arch> <channel> <short>` when several
channels are searched, the short description last because it is the only
field with spaces), `count:` and `summary:`. Exit 0 whether or not anything
matched.

## Refusals

| Exit | When |
|---|---|
| 11 | a channel that cannot be read |
| 20 | no words, no channel and no root, or a root that lists no channel |

## Related

[SHOW](show.md) for everything a channel holds, [CHANNEL](channel.md),
[INSTALL](install.md).
