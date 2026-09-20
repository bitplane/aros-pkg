<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Commands

One page per command: what it does, its keywords, examples that run, what it
prints and records, and its refusals. The one-line summary of all of them is
in the [reference](../reference.md#verbs); `pkg HELP` prints it.

## Installing and keeping software

- [`INSTALL`](install.md): install a package and what it depends on
- [`STATUS`](status.md): what is installed, and what has a newer version
- [`UPGRADE`](upgrade.md): move a package, or every package, to a newer version
- [`ROLLBACK`](rollback.md): return a package to the version installed before
- [`LIST`](list.md): what a root holds
- [`VERIFY`](verify.md): every installed file against its signed manifest
- [`REPAIR`](repair.md): put missing and changed files back from the channel
- [`REMOVE`](remove.md): take a package out, or the packages nothing needs any more
- [`SHOW`](show.md): what a channel offers, each entry checked
- [`SEARCH`](search.md): the packages every word matches
- [`CHANNEL`](channel.md): the channels a root reads when `CHANNEL` is left out
- [`MOUNTLIST`](mountlist.md): the Mount entry for an installed image
- [`RESOLVE`](resolve.md): which library a program would get from here, and why

## Publishing

- [`KEYGEN`](keygen.md): a signing key, readable by you alone
- [`KEYINFO`](keyinfo.md): the public key a key file holds
- [`MANIFEST`](manifest.md): the manifest PUBLISH would sign, to read before publishing
- [`PUBLISH`](publish.md), `PACKAGE`: publish a drawer as a version of a package
- [`WITHDRAW`](withdraw.md): a version nothing installs any more
- [`SIGN`](sign.md): a detached signature for any file
- [`CHECKSIG`](checksig.md): whether such a signature is good, and whose it is
- [`IMAGE`](image.md): an FFS volume image of a drawer
- [`PUSH`](push.md): send a channel to the portal

## On AROS

- [`PORT`](port.md): serve every verb on an ARexx port (AROS)
