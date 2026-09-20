<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# WITHDRAW

A version nothing installs any more.
```
pkg WITHDRAW <name> VERSION v CHANNEL <dir> [ARCH cpu] [SIGN <keyfile>] [DRYRUN]
```

## What it does

Marks one published version as withdrawn, with a signature by the key that
published it (another key is refused, exit 14). The version stays in the
channel, so machines that have it can still verify and repair it, but
`INSTALL` and `UPGRADE` no longer choose it: `STATUS` on a machine that has
it says so and names what the channel offers instead. A version built for
several CPUs needs `ARCH` to withdraw one build. Withdrawing a withdrawn
version says so and changes nothing.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key b261704c924d5d1cc52f66f4963761b3f689ad482772f54bd043d6e522d25929
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ export PKG_SIGNKEY=my.key
$ pkg PUBLISH MyTool CHANNEL mychannel KIND application
packaged mytool 1.0 into mychannel: 1 file, payload bf9bc6edcb45, signed by 738f1f023802d578
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  hint: the channel mychannel did not exist and was created
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg PUBLISH MyTool-1.1 CHANNEL mychannel
packaged mytool 1.1 into mychannel: 2 files, payload 4606917aa7ca, signed by 738f1f023802d578
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  kind and dependencies from mytool 1.0, published before
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg WITHDRAW mytool VERSION 1.1 CHANNEL mychannel
withdrew mytool 1.1 from mychannel: it stays in the channel, and nothing installs it any more
$ pkg SHOW mytool CHANNEL mychannel
Package  Version  Kind         Arch    Status     Signer
mytool   1.0      application  x86_64  ok         b261704c924d5d1c
mytool   1.1      application  x86_64  withdrawn  b261704c924d5d1c
```

## Records (`MACHINE`)

`result: withdrawn` (or `would-withdraw`, `unchanged`), `name:`, `version:`,
`channel:`, `signer:`.

## Related

[PUBLISH](publish.md), [STATUS](status.md),
[Publishing packages](../publishing.md#withdraw-a-version).
