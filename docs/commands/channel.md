<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# CHANNEL

The channels a root reads when `CHANNEL` is left out.
```
pkg CHANNEL ADD <channel> [NAME <name>] ROOT <root> [DRYRUN]
pkg CHANNEL LIST ROOT <root>
pkg CHANNEL REMOVE <channel|name> ROOT <root> [DRYRUN]
```

## What it does

A machine that installs from the same places every day should not have to
name them on every command. `CHANNEL ADD` writes a channel into the root's
own list, `<root>/.pkg/channels`, one channel per line in the order they
were added, beside `.pkg/arch` and `.pkg/keys`. The list belongs to the
root, so an AROS system keeps its own in `SYS:.pkg` and a test root keeps
another.

From then on `INSTALL`, `UPGRADE` (including `UPGRADE ALL`), `STATUS`,
`SHOW`, `SEARCH`, `REPAIR`, `ROLLBACK` and `RESOLVE` read that list when no
`CHANNEL` is given, asking the channels in order. `CHANNEL <channel>` on the
line still means that channel and no other.

`ADD` checks the channel can be read before writing it down, and says how
many packages it offers; with `DRYRUN` it checks nothing and writes nothing.
A channel already listed is refused (exit 15). `REMOVE` takes a channel off
the list and changes nothing else: what was installed from it stays
installed.

## The name a channel answers to

`https://aros-pkg.azurewebsites.net/contrib-nightly` is a lot to type when
what you mean is contrib-nightly. So a channel in a root's list has a short
name, and `CHANNEL <name>` says exactly what `CHANNEL <address>` says.

A channel names itself: the last part of its address, which is what it is
called anyway. `NAME <name>` chooses another. `CHANNEL LIST` shows the names,
and `REMOVE` takes either the name or the address.

A name is letters, digits, a dash, an underscore or a dot, up to 63 of them,
and nothing that could be read as a path or a URL instead. A word on the
line is looked up as a name only when it is neither an address nor a
directory that is there, so a channel in a directory always wins over a name
that looks like it. A name another channel in that root already answers to
is refused (exit 15) rather than made unique quietly; when a channel names
itself into a name already taken, it simply stays unnamed.

The name belongs to the root, as the list does. A machine with no root
selected has no list to look a name up in, and the address is then the only
way to say which channel is meant.

## Several channels

A package is looked for in each channel in turn, and the newest version
wins; when two channels offer the same version, the one listed first wins.
Dependencies are resolved across the list the same way.

Two channels offering the same package name under **different keys** is
refused (exit 14, `next: ask-requester`), naming both channels and both
keys, as long as this root has pinned no key for that name. Adding a channel
is never a way to replace someone's package quietly, and which key is the
publisher's is not pkg's decision. Install once from the channel whose key
is right, with `CHANNEL <that one>`: the root pins that key, and from then
on only the channels whose chosen version carries it count for that name.

## Examples

```console
$ pkg CHANNEL ADD channel ROOT aros
added channel to aros, in place 1: it offers 4 packages
  hint: INSTALL, UPGRADE, STATUS, SHOW, REPAIR, ROLLBACK and SEARCH now read this channel when CHANNEL is left out; CHANNEL <dir|url> on the line still means that channel alone
$ pkg CHANNEL ADD https://aros-pkg.azurewebsites.net/contrib-nightly NAME contrib ROOT aros
added https://aros-pkg.azurewebsites.net/contrib-nightly to aros, in place 2: it offers 1 package
  hint: this root now knows it as contrib: CHANNEL contrib says the same as CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly
$ pkg CHANNEL LIST ROOT aros
In  Name     Channel
1   channel  channel
2   contrib  https://aros-pkg.azurewebsites.net/contrib-nightly
2 channels in aros, asked in this order
$ pkg INSTALL helloworld ROOT aros
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg CHANNEL LIST ROOT aros MACHINE
result: shown
channel: channel
channel-name: channel
channel: https://aros-pkg.azurewebsites.net/contrib-nightly
channel-name: contrib
count: 2
root: aros
summary: 2 channels, asked in this order
$ pkg CHANNEL ADD channel ROOT aros    # exits 15
pkg channel: aros already lists the channel channel, in place 1; nothing was changed. CHANNEL LIST ROOT aros shows them
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
$ pkg CHANNEL REMOVE contrib ROOT aros
removed https://aros-pkg.azurewebsites.net/contrib-nightly from aros; 1 channel left
  what was installed from it stays installed; nothing was removed from this root
```

## Records (`MACHINE`)

`ADD`: `result: added` (`would-add` under `DRYRUN`), `channel:`, `name:` when
it has one, `root:`, `position:`, `packages:`. `LIST`: `result: shown`, one
`channel: <channel>` per channel in order, each followed by
`channel-name: <name>` when it has one and carrying that name as a field for
a library caller, `count:`, `root:`, `summary:`. `REMOVE`: `result: removed`
(`would-remove`), `channel:` (the address, whichever was typed), `root:`,
`count:`.

## Refusals

| Exit | When |
|---|---|
| 11 | `ADD` of a directory that holds no channel, or `REMOVE` of a channel the root does not list |
| 12 | the list file is malformed |
| 15 | the channel is already listed, or `NAME` is a name another channel here answers to |
| 17 | the list cannot be written |
| 20 | no `ROOT`, no channel, a word that is not `ADD`, `LIST` or `REMOVE`, `NAME` outside `ADD`, or a name with characters it may not have |

## Related

[SEARCH](search.md), [INSTALL](install.md), [STATUS](status.md),
[Channels](../channels.md), [Using pkg](../using.md).
