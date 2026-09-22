<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# PUBLISH, PACKAGE

Publish a drawer as a version of a package.
```
pkg PUBLISH <drawer> CHANNEL <dir> KIND k [SIGN <keyfile>] [NAME n] [VERSION v] [ARCH cpu] [DEPENDS "a >= 1, b"] [CONFIG "f, g"] [FILES "a, b"] [BUILD <date>] [ACCEPTKEY <key>] [UPSTREAM <url>] [README <file>] [INFO <file>] [SHORT ..] [DESCRIPTION <file>] [CATEGORY ..] [TAGS ..] [AUTHOR ..] [HOMEPAGE ..] [REPOSITORY ..] [LICENSE ..] [DISTRIBUTION ..] [CHANGES <file>] [ICON ..] [SCREENSHOT ..] [DRYRUN]
```

`PACKAGE` is the same verb under another name, everywhere the verb is
taken: on the command line, on the ARexx port and in `HELP`. `PUBLISH` was
read by more than one person as "it is online now"; it is not. A channel on
a disk or a share is a real channel, and a machine that can read that
directory installs from it; what sends a package to a portal is
[`PUSH`](push.md). Every successful publish into a local channel says so in
a hint.

## What it does

Builds the manifest `MANIFEST` shows, signs it with `SIGN <keyfile>` (or
`PKG_SIGNKEY`), packs the files into one container named by its SHA-256,
and adds the version to the channel's index. A channel that does not exist
is created. A later version of the same package keeps the kind,
dependencies, configuration list and catalogue of the version before unless
the keywords say otherwise (`none` drops a field). A version already
published with the same content publishes nothing and succeeds; with
different content it is refused (exit 15): a version is immutable, and a
changed build needs a new version number. A channel whose first
version of this package was signed by another key refuses this one (exit
14) unless `ACCEPTKEY` names the new key.

`<drawer>` may be a path inside an archive, `archive.tar.bz2!/Top`, with
`FILES` picking the paths of this package: the files stay in the archive,
which the channel serves from `archives/`, or, with `UPSTREAM <url>`,
downloads from where its makers publish it. `BUILD <date>` appends the date
to the version for nightlies. A changed source archive, upstream URL or archive
digest produces a new build even when the package files are unchanged.
The signed metadata must identify an archive that remains downloadable.

`INFO <file>` reads a `.pkginfo` the port carries: its `Name`, `Version`,
`Kind`, catalogue fields, `Depends`, `Files` and `Config` are used for what
the command does not give, so packaging a port that carries one needs no
keywords at all. A keyword on the line wins over the file, and the file
wins over an Aminet `README` and over the last version published. With a
drawer inside an archive, `INFO "!/<path>"` reads the file out of that
archive. `Files` selects what is published, as `FILES` does, and a path it
names that is not there is refused (exit 11).

`KIND`: `image` (a program on one volume to mount), `application` (loose
files), `library`, `device`, `class`, `font`, `catalog`, `startup`, `boot`,
`data`, `sdk`, `slave`.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key e37f352e46ab9d18978bb45b3c9c2ccfd811b77b20d73d34512ea6f16235335e
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ export PKG_SIGNKEY=my.key
$ pkg PUBLISH MyTool CHANNEL mychannel KIND application
packaged mytool 1.0 into mychannel: 1 file, payload bf9bc6edcb45, signed by b5682a63dfb34066
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  hint: the channel mychannel did not exist and was created
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg PUBLISH MyTool-1.1 CHANNEL mychannel CONFIG S/MyTool.prefs SHORT "Renames files by pattern" AUTHOR "Jane Roe" LICENSE MIT
packaged mytool 1.1 into mychannel: 2 files, payload 4606917aa7ca, signed by b5682a63dfb34066
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  kind and dependencies from mytool 1.0, published before
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ printf 'Window=800x600\n' > MyTool-1.1/S/MyTool.prefs
$ pkg PUBLISH MyTool-1.1 CHANNEL mychannel    # exits 15
pkg publish: mytool 1.1 is already published with a different payload, and 1.1 came from the $VER cookie in C/MyTool: either this is a new build whose $VER was not raised (raise it, or give VERSION), or it is the old build, changed; check which before publishing
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
$ pkg SHOW mytool CHANNEL mychannel
Package  Version  Kind         Arch    Status  Signer
mytool   1.0      application  x86_64  ok      e37f352e46ab9d18
mytool   1.1      application  x86_64  ok      e37f352e46ab9d18
  mytool 1.1: Renames files by pattern
  author     Jane Roe
  license    MIT
$ mkdir -p nightly/archives && cp nightly.tar.bz2 nightly/archives/
$ pkg PUBLISH "nightly/archives/nightly.tar.bz2!/Top" FILES Extras/Tool CHANNEL nightly NAME tool BUILD 20260918 KIND application UPSTREAM https://example.org/nightly.tar.bz2
packaged tool 2.1+20260918 into nightly: 1 file, from nightly.tar.bz2!/Top, signed by b5682a63dfb34066
  architecture x86_64, read from Extras/Tool/Tool
  version taken from $VER: in Extras/Tool/Tool
  hint: the package is in the local channel nightly: any machine that can read that directory installs from it with INSTALL tool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL nightly TO <portal channel> sends it to a portal
```

## Records (`MACHINE`)

`result:` (`published`, `unchanged`, `would-publish`), `name:`, `version:`,
`kind:`, `architecture:`, `channel:`, `files:`, `payload:` or `source:` and
`upstream:`, `manifest:`, `signer:`, `depends:`, `config-files:`, `left-out:`,
where each field came from (`name-from:`, `version-from:`, `arch-from:`,
`kind-from:`, `depends-from:`, `config-from:`, `about-from:`, `info-from:`
and `info-fields:`), `same-as:`
when nothing changed, `first-signer:` around a key refusal.

## Refusals

| Exit | When |
|---|---|
| 14 | no signing key; a channel whose first version was signed by another key |
| 15 | this version is already published with other content |
| 17 | the channel cannot be written |
| 20 | no `KIND` for a first version; a channel URL (publish locally, then `PUSH`); a mixed-CPU drawer; malformed keywords |

## Related

[MANIFEST](manifest.md), [WITHDRAW](withdraw.md), [PUSH](push.md),
[Publishing packages](../publishing.md), [Distributing builds](../distributing.md),
[Moving an existing distribution](../migrating.md).
