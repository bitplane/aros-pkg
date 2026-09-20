# Publishing packages

This guide is for making software available through Pkg: your own programs,
or the components of someone else's archive. You publish on any system Pkg
runs on, into a channel (a directory); then you copy that directory to
where people read it, or upload it to the portal.

The examples use a drawer `MyTool` holding a program, `C/MyTool`, and a
newer build of it in `MyTool-1.1`.

## Your key

What the key proves, how machines pin it and what happens when it is lost:
[Signatures and trust](signing.md).

Every package is signed; there is no unsigned mode. Make a key once:

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key 9f4314e9f88233c4cf8c4c47420b2ac35d526cb713dc3f26d97cde75b3ff3fc8
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
```

The file holds your private key, readable by you alone. Keep it with your
other secrets and back it up: every later version of what you publish must
be signed with it, because people who installed your package accept a new
version only from the same key. Tell Pkg where it is:

```console
$ export PKG_SIGNKEY=my.key
```

or name it on each command with `SIGN my.key`. On AROS, `SetEnv
PKG_SIGNKEY` does the same. `KEYINFO` shows the public key, the part you
can give to anyone:

```console
$ pkg KEYINFO FILE my.key
my.key holds the public key d0186fe935f2b7fdd2a929478a9f484d00456722774450dfb50bf4b5c7985506
```

## See what the package would be

```console
$ pkg MANIFEST MyTool KIND application
Format: pkg-manifest 1
Name: mytool
Version: 1.0
Architecture: x86_64
Kind: application
Payload: bf9bc6edcb45a4a786709554952e6988649c6e6cc405595d86f54e3d1d8c87d4
File: c93731985b65bf43460090b575902f0a6eca3ad030914fb0e58c0700569b2d85 94 C/MyTool
Protect: 0x00000002 C/MyTool
```

`MANIFEST` shows the description Pkg would sign, and writes nothing. The
name and version come from the program's `$VER:` string (`$VER: mytool 1.0
(19.9.2026)`), and the CPU from its executable header; give `NAME`,
`VERSION` or `ARCH` to say otherwise.

## Publish

```console
$ pkg PUBLISH MyTool CHANNEL mychannel KIND application
published mytool 1.0 to mychannel: 1 file, payload bf9bc6edcb45, signed by 2f36386096f1ff0b
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  hint: the channel mychannel did not exist and was created
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
```

The first `PUBLISH` into a directory makes it a channel. `KIND` says what
the package is, and is needed only for the first version:

| Kind | For |
|---|---|
| `image` | a program people run, installed as one disk image to mount |
| `application` | a program installed as loose files |
| `library`, `device`, `class`, `font`, `catalog` | what other programs use, in `Libs`, `Devs` or `L`, `Classes`, `Fonts`, `Locale` (a handler is a `device`) |
| `startup`, `boot` | what the system runs as it starts |
| `data`, `sdk`, `slave` | files, development kits, WHDLoad slaves |

A new version takes the kind, the dependencies and the configuration files
of the last one published, so you only name what changes:

```console
$ pkg PUBLISH MyTool-1.1 CHANNEL mychannel CONFIG S/MyTool.prefs
published mytool 1.1 to mychannel: 2 files, payload 4606917aa7ca, signed by 2f36386096f1ff0b
  architecture x86_64, read from C/MyTool
  name and version taken from $VER: in C/MyTool
  kind and dependencies from mytool 1.0, published before
  hint: the package is in the local channel mychannel: any machine that can read that directory installs from it with INSTALL mytool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL mychannel TO <portal channel> sends it to a portal
$ pkg SHOW CHANNEL mychannel
Package  Version  Kind         Arch    Status  Signer
mytool   1.0      application  x86_64  ok      9f4314e9f88233c4
mytool   1.1      application  x86_64  ok      9f4314e9f88233c4
```

A version, once published, never changes: publishing different files under
the same version is refused. Raise the version in `$VER:`.

## Describe your package

What people read about your package, on the portal and in `SHOW`, comes
from fields you publish with it, signed like the rest:

```console
$ printf 'MyTool renames files by pattern.\n\nIt runs from the Shell or from an icon.\n' > about.txt
$ printf 'First release.\n' > changes.txt
$ pkg MANIFEST MyTool KIND application SHORT "Renames files by pattern" DESCRIPTION about.txt CATEGORY util/misc TAGS "files, rename" AUTHOR "Jane Roe" LICENSE MIT DISTRIBUTION open-source HOMEPAGE https://example.org/mytool CHANGES changes.txt
Format: pkg-manifest 1
Name: mytool
Version: 1.0
Architecture: x86_64
Kind: application
Short: Renames files by pattern
Description: MyTool renames files by pattern.
Description: 
Description: It runs from the Shell or from an icon.
Category: util/misc
Tags: files, rename
Author: Jane Roe
Homepage: https://example.org/mytool
License: MIT
Distribution: open-source
Changes: First release.
Payload: bf9bc6edcb45a4a786709554952e6988649c6e6cc405595d86f54e3d1d8c87d4
File: c93731985b65bf43460090b575902f0a6eca3ad030914fb0e58c0700569b2d85 94 C/MyTool
Protect: 0x00000002 C/MyTool
```

| Keyword | Field | Rules |
|---|---|---|
| `SHORT` | one line on cards and lists | at most 40 characters |
| `DESCRIPTION <file>` | the text of the package page | a text file; an empty line starts a paragraph |
| `CATEGORY` | where it is listed | an Aminet type and its sub-directory: `util/misc`, `game/think`, `dev/lang` |
| `TAGS` | words people search for | comma-separated, lowercase, 16 at most |
| `AUTHOR` | who wrote the program | comma-separated; the key that signs is the packager |
| `HOMEPAGE`, `REPOSITORY` | links | `http://` or `https://` |
| `LICENSE` | the licence of the source | an SPDX expression: `MIT`, `GPL-2.0-or-later` |
| `DISTRIBUTION` | the terms | `open-source`, `freeware`, `shareware`, `public-domain`, `commercial`, `demo` or `other` |
| `CHANGES <file>` | what this version changes | a text file |
| `ICON`, `SCREENSHOT` | pictures | paths of files in the package |

A new version keeps every field of the last one except `CHANGES`, which
belongs to one version; give a keyword again to change a field, or `none`
to drop it. If your program comes with an Aminet `.readme`, `README
<file>` takes its `Short:`, `Author:`, `Type:` and text for the fields you
do not give.

## A port that carries a `.pkginfo`

A port of someone else's program has no `$VER` to read a version from and
no `.readme` to describe itself, and which installed paths belong to it is
known only to whoever wrote the port. A port that carries a `.pkginfo`
beside its files says all of it, and `INFO <file>` reads it:

```console
$ printf 'Format: pkginfo 1\nName: mytool\nVersion: 1.0\nKind: application\nShort: Renames files by pattern\nCategory: util/misc\nAuthor: Jane Roe\nLicense: MIT\nDistribution: open-source\nFiles: C/MyTool\nDescription: MyTool renames files by pattern.\n' > MyTool/mytool.pkginfo
$ pkg MANIFEST MyTool INFO MyTool/mytool.pkginfo
Format: pkg-manifest 1
Name: mytool
Version: 1.0
Architecture: x86_64
Kind: application
Short: Renames files by pattern
Description: MyTool renames files by pattern.
Category: util/misc
Author: Jane Roe
License: MIT
Distribution: open-source
Payload: bf9bc6edcb45a4a786709554952e6988649c6e6cc405595d86f54e3d1d8c87d4
File: c93731985b65bf43460090b575902f0a6eca3ad030914fb0e58c0700569b2d85 94 C/MyTool
Protect: 0x00000002 C/MyTool
```

Nothing else is needed on the line: the name, the version, the kind, the
catalogue fields, `Depends`, `Config` and the paths the package is made of
all come from the file. `Files:` picks them as `FILES` does, one path per
line, a drawer meaning all of it, and a path the drawer does not hold is
refused rather than quietly skipped. A keyword you type wins over the file,
the file wins over a `README` and over the last version published, and a
key Pkg does not know (`Upstream-Archive`, `Port-Maintainer`) is ignored,
so a port may keep its own lines in the same file.
[`tools/contrib/PKGINFO.md`](../tools/contrib/PKGINFO.md) is the format in
full, for whoever writes the port.

## Dependencies

```console
$ pkg MANIFEST MyTool-1.1 KIND application DEPENDS "hellolib >= 1.0"
Format: pkg-manifest 1
Name: mytool
Version: 1.1
Architecture: x86_64
Kind: application
Depends: hellolib >= 1.0
Payload: 4606917aa7ca32a6d1d5b8bfe2fd8e8b82a94c65d105caa8e4229c282311dedf
File: 37a22af64bb210dc1f16587e4bae09026f437614b322ad681bfb46dd4288e757 94 C/MyTool
File: 70c860de45698205204d4d766d861a795cf2a8e6252b29596aa1c8be449b2214 15 S/MyTool.prefs
Protect: 0x00000002 C/MyTool
Protect: 0x00000002 S/MyTool.prefs
```

`DEPENDS` lists the packages yours needs, each with the lowest version that
will do, and goes into the signed description as a `Depends:` line. Publish
them into the same channel. Installing your package installs them first;
removing it leaves them as orphans for `REMOVE ORPHANS`. `DEPENDS none`
publishes a version that needs nothing, when the last one did.

Libraries work the same way. A package that ships `Libs/SDL2.library` gets
a `Provides: SDL2.library` line, and a program that opens a library no
dependency provides gets a warning when you publish it, naming the library.
[Libraries](libraries.md) explains why a shared library is a package of its
own.

## Files people edit

If your package carries files people change, such as preferences or a
startup script, name them with `CONFIG`, one by one or by drawer:

```console
$ pkg MANIFEST MyTool-1.1 KIND application CONFIG S/MyTool.prefs
Format: pkg-manifest 1
Name: mytool
Version: 1.1
Architecture: x86_64
Kind: application
Payload: 4606917aa7ca32a6d1d5b8bfe2fd8e8b82a94c65d105caa8e4229c282311dedf
File: 37a22af64bb210dc1f16587e4bae09026f437614b322ad681bfb46dd4288e757 94 C/MyTool
File: 70c860de45698205204d4d766d861a795cf2a8e6252b29596aa1c8be449b2214 15 S/MyTool.prefs
Protect: 0x00000002 C/MyTool
Protect: 0x00000002 S/MyTool.prefs
Config: S/MyTool.prefs
```

When a person has edited one of those files, an update keeps their version
and sets yours beside it as `<file>.pkgnew`; `VERIFY` reports the edit
without calling it damage; `REPAIR` leaves it alone. A name that is no file
or drawer of the package is refused, saying so. Later versions keep the
list; publish with `CONFIG` again to change it.

## Several CPUs

Publish each build into the same channel; Pkg reads the CPU from the
executables and a machine installs the build it can run. How that works,
`ARCH`, `WITHDRAW` of one build and Pkg's own channel:
[Distributing builds for several platforms](distributing.md).

## Withdraw a version

A version with a serious fault can be withdrawn. It stays in the channel,
so nothing that refers to it breaks, but nobody can install it any more,
and `STATUS` tells the people who have it:

```console
$ pkg WITHDRAW mytool VERSION 1.1 CHANNEL mychannel
withdrew mytool 1.1 from mychannel: it stays in the channel, and nothing installs it any more
$ pkg SHOW mytool CHANNEL mychannel
Package  Version  Kind         Arch    Status     Signer
mytool   1.0      application  x86_64  ok         9f4314e9f88233c4
mytool   1.1      application  x86_64  withdrawn  9f4314e9f88233c4
```

## Packages from someone else's archive

To publish the components of an archive you did not make, such as the
nightly contrib archive of AROS, name the archive and the path inside it.
Pkg reads the files straight out of the archive; nothing is unpacked or
copied:

```console
$ mkdir -p nightly/archives && cp nightly.tar.bz2 nightly/archives/
$ pkg PUBLISH "nightly/archives/nightly.tar.bz2!/Top" FILES Extras/Tool CHANNEL nightly NAME tool BUILD 20260918 KIND application UPSTREAM https://example.org/nightly.tar.bz2
published tool 2.1+20260918 to nightly: 1 file, from nightly.tar.bz2!/Top, signed by 2f36386096f1ff0b
  architecture x86_64, read from Extras/Tool/Tool
  version taken from $VER: in Extras/Tool/Tool
  hint: the package is in the local channel nightly: any machine that can read that directory installs from it with INSTALL tool ROOT <root> CHANNEL <that directory, as the machine names it>, and PUSH CHANNEL nightly TO <portal channel> sends it to a portal
```

`FILES` picks the paths that make up this package. `BUILD` adds the date of
the nightly to the version (`2.1+20260918`), so each nightly is a newer
version, and a nightly whose files did not change publishes nothing.
`UPSTREAM` records where the archive is published: people's Pkg downloads
it from there, once, checks it against the size and SHA-256 you signed,
and the channel never carries the archive.

## Upload to the portal

**The portal takes pushes only from publishers its maintainer has
registered, and for now the only way to be registered is to ask:** see
<https://aros-pkg.azurewebsites.net/publishers>. Send your public key
(`pkg KEYINFO FILE <your key>`, not secret) and the channel you want; you
are told when it is done. There is no sign-up form yet.

Once registered, publish into a local channel and send it to the portal,
signing the push with the key you registered:

```sh
pkg PUSH CHANNEL mychannel TO https://aros-pkg.azurewebsites.net/mychannel SIGN my.key
```

Or, if the maintainer gave you an upload key instead, over https with the
key in `PKG_PUSHKEY`; never on the command line:

```sh
export PKG_PUSHKEY="the key the portal gave you"
pkg PUSH CHANNEL mychannel TO https://aros-pkg.azurewebsites.net/mychannel
```

`PUSH` sends only the files the portal does not have yet, large ones in
parts that resume after a failure, and never an archive published with
`UPSTREAM`. The portal checks every signature before it publishes anything.

From AROS, or from any machine without a portal key, push to the `http://`
address instead: Pkg signs each request with your publisher key, and the
portal knows you by its public half ([PUSH](commands/push.md)).

**What your key may send.** A portal decides per key whether it takes your
files. On the AROS portal a key publishes *by link* unless its maintainers
gave it the *files* right: your push carries the signed manifests and
signatures, and the files stay where you publish them, an `https` address
such as a GitHub release. Put the release archive there and publish from
it, naming the address:

```sh
pkg PUBLISH "release/mytool-1.1.tar.bz2!/MyTool" CHANNEL mychannel KIND application UPSTREAM https://github.com/you/mytool/releases/download/v1.1/mytool-1.1.tar.bz2
pkg PUSH CHANNEL mychannel TO https://aros-pkg.azurewebsites.net/mychannel
```

A push with a payload from a link-only key is refused (20) and the refusal
names the portal's rule; every portal states its rules at
`/api/policy` and on its trust page. See [Channels](channels.md) for
serving a channel yourself.
