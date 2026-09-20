# Channels

A channel is where packages are published and where pkg finds them: a
directory, or the same directory served over HTTP. It holds only signed
files, so it can live anywhere, a disk, a share, a web server, without
being trusted: pkg checks every signature and every file itself.

## Where a channel can be

- **A directory** on a disk, a share or an image: `CHANNEL Work:channel` on
  AROS, `CHANNEL channel` on a Mac or a PC. This is how AROS reads channels.
- **A web address**: `CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly`,
  or `http://`. On macOS, Linux and Windows, pkg reads a channel over the
  network exactly as it reads a directory, and keeps what it downloaded in
  a cache (`PKG_CACHE`, else `~/.cache/pkg`, or `%LOCALAPPDATA%\pkg-cache`
  on Windows). On AROS it is the same over `http://`, once the machine's
  network is started (AROSTCP, or a hosted AROS's own sockets); the cache
  is `SYS:.pkg/cache`. AROS reads `https://` as well: the AROS builds carry
  a TLS library (Mbed TLS) and a bundle of certificate authorities, and check the
  certificate against the address, with no way to turn that off. A machine
  behind its own authority names its bundle in `PKG_CAFILE`.

## A root's list of channels

A root keeps the channels it reads in `<root>/.pkg/channels`, one per line,
in the order they were added; on AROS that is `SYS:.pkg/channels`. The list
is written and read with [`CHANNEL ADD`, `CHANNEL LIST` and `CHANNEL
REMOVE`](commands/channel.md), and it is what `INSTALL`, `UPGRADE`,
`STATUS`, `SHOW`, `SEARCH`, `REPAIR`, `ROLLBACK` and `RESOLVE` use when no
`CHANNEL` is given. Each channel is asked in turn, the newest version wins,
and the order breaks a tie.

The list adds no trust. A channel is still checked file by file, and a
package two listed channels offer under different keys is refused (exit 14)
until the root has pinned a key for it, so that adding a channel cannot
replace another publisher's package. See
[Signatures and trust](signing.md).

## The cache

`PKG_CACHE` names it; without it, `~/.cache/pkg`, `%LOCALAPPDATA%\pkg-cache`
on Windows, `SYS:.pkg/cache` on AROS. It holds what was downloaded, and for
a channel whose packages live in someone else's archive:

- `upstream/<sha256>/<archive>`: the archive itself, downloaded once for
  every package that comes out of it and kept under the SHA-256 the signed
  manifest gives it.
- `upstream/<sha256>/<archive>.pkgmap`: its block map, written by the first
  install that read the archive whole. It records where each member of the
  archive begins and which bzip2 block holds that point, so a later install
  decompresses only the blocks its own files lie in instead of the whole
  archive. The map is tied to the archive's size, time and SHA-256; a map
  that does not answer to all three, or one a read cannot use, is thrown
  away and made again. Deleting it costs one slow install, nothing else.
- `upstream/<sha256>/<archive>.d/`: a directory you unpacked the archive
  into yourself (`tar xjf <archive> -C <that directory>`). pkg reads it
  without being told, and `UNPACKED <dir>` names one anywhere else. Every
  file is still weighed and hashed against the signed manifest.

Every install says, once, which of these it read and where it is.

## What is in one

```console
$ ls channel channel/objects | head -8
channel:
index
objects

channel/objects:
0542ac0ba511cb6ad4a11ec54a84ebe0432713e122ce612c2f7b26510ead9dcc.pkg
26e291fd5946c38dd9e50b6031ce726092032889bcb649d52b1af6cfe2d88f3c.manifest
26e291fd5946c38dd9e50b6031ce726092032889bcb649d52b1af6cfe2d88f3c.sig
$ cat channel/index
hellolib 1.0 generic 81b11e885ecfedf6db7a9c576fa4a9e9ce6f0995cde713215cd714be07b22e7e
helloworld 1.0 generic bf939246395d4a7673ec349d5942960e76a5c805360f1503ffd6d03aa1e4dfff
helloworld 1.1 generic ec120097005e1766f6d10b012f1788d177e059bf6d255f6e511456079443393b
notes 1.0 generic 74f7af32c6e37a1e836aaebc088e705239e2dca2764ece28cfdbb8cce013cf12
sdl2 2.30 aarch64 07a2f17b1001fdf631e75e8b562f3a621da1f678cb8f6c3737f5cd383a0c9243
```

- `index`: one line per published version and CPU: name, version, CPU, and
  the SHA-256 of that version's description, its *manifest*.
- A manifest is `key: value` lines, `Format: pkg-manifest 1` first. Keys
  pkg knows are read strictly; a key it does not know is kept in the signed
  text, never acted on, and reported by `SHOW` as `ignored`, so a package
  can carry lines for other distribution systems, and a misspelt key
  (`Categroy:`) is seen rather than silently lost. A `Format` pkg does not
  know is refused: a change pkg must understand gets a new format number.
- `objects/<digest>.manifest`: the manifest, named by its own SHA-256, and
  `<digest>.sig`, its signature. The manifest lists every file of the
  package with its size and SHA-256.
- `objects/<digest>.pkg`: the files themselves, in one container named by
  its SHA-256, which the manifest gives.
- `objects/<digest>.withdrawn` and `.withdrawn.sig`, for a withdrawn version.
- `archives/<name>`: the archive a package's files stay in, for packages
  published from someone else's archive, unless its makers publish it
  themselves (`UPSTREAM`).
- `withdrawals`, in a channel served over the network: which versions have a
  withdrawal, so that a reader asks for one only where there is one. See
  below.

Nothing in a channel is ever rewritten except `index` and `withdrawals`: a
published version stays as it is, so a copy of a channel made at any moment is
consistent.

## The list of withdrawn versions

A withdrawal is `objects/<digest>.withdrawn`, signed by the key that signed
the version. In a directory that costs nothing to find. Over the network it
does: without a list, a reader asking whether anything is withdrawn asks once
per entry, and in the contrib nightly channel, 208 versions of which none is
withdrawn today, all 208 of those requests answer nothing.

A channel served by the portal therefore holds one more file beside the index:

```console
$ curl -s https://aros-pkg.azurewebsites.net/contrib-nightly/withdrawals
Format: pkg-withdrawals 1
```

The header first, then the digests that have a withdrawal, one per line, in
order; that channel has none, so a reader that has read this file asks for no
`.withdrawn` file at all. The file is written by the server from the signed
withdrawals it holds; a push may not send one. Every channel the portal serves
has it, empty when nothing is withdrawn, so a reader that finds it knows it
need ask for nothing else.

It is a hint, never authority. What the list names is still the signed
`.withdrawn` file, read and checked as before; a list naming a version whose
withdrawal does not check costs one request and changes nothing. A list that
leaves a version out hides its withdrawal — exactly as deleting the signed
file from that copy of the channel would, which anyone serving a channel could
always do. A withdrawal has never been a way to keep a version out of reach of
whoever serves it; it is how a publisher tells the readers who ask that this
version is not to be used.

A channel without the file — a directory, a copy someone made, a server that
does not write one — is read exactly as before, one request per entry.

## Check a channel

`SHOW` checks every signature, every description and every file of every
package, and says which entries are sound:

```console
$ pkg SHOW CHANNEL channel
Package     Version  Kind         Arch     Status  Signer
hellolib    1.0      library      generic  ok      5ff18d3fe14e383e
helloworld  1.0      application  generic  ok      5ff18d3fe14e383e
helloworld  1.1      application  generic  ok      5ff18d3fe14e383e
notes       1.0      image        generic  ok      5ff18d3fe14e383e
sdl2        2.30     library      aarch64  ok      5ff18d3fe14e383e
$ pkg SHOW CHANNEL channel MACHINE | tail -2
count: 5
bad: 0
```

On a channel with large archives, `METADATA` checks everything but the
archives, in a moment, and `ARCHIVE <name>` checks the packages of one
archive.

## Serve a channel

Any web server that serves files serves a channel: put the directory where
it is published, and give people its address. For a quick test on your own
machine:

```console
$ python3 -m http.server --bind 127.0.0.1 --directory channel 8765 > /dev/null 2>&1 &
$ sleep 1; pkg SHOW notes CHANNEL http://127.0.0.1:8765
Package  Version  Kind   Arch     Status  Signer
notes    1.0      image  generic  ok      5ff18d3fe14e383e
$ kill %1
```

pkg follows redirects and understands chunked replies. For `https` it uses
the system's `curl` on macOS, Linux and Windows, and its own client over
Mbed TLS on AROS.

That client asks for one file at a time, but not over one connection each:
it keeps the connection to a channel's server open and sends the next
request down it, which on AROS saves a TCP connection and a full TLS
handshake per file. A server that closes after every answer is served as
well, and so is one that closes a connection Pkg was keeping: the request is
sent again on a new one. A channel that is only read, rather than installed
from, asks for the index and then, for each version it reports on, that
version's manifest, its signature and whether it was withdrawn; the versions
a command does not look at cost nothing.

## The portal

The package portal at `https://aros-pkg.azurewebsites.net` serves channels
this way, over `https` and over plain `http`, and shows each package on a
web page: its versions, dependencies, files,
signer and catalogue. **Downloads** is where a newcomer starts (pkg for
every CPU and host), **Statistics** shows what each channel holds, and
**Documentation** is these guides.

What the portal holds and enforces:

- Descriptions, signatures and the files publishers upload. The archives
  of packages published from someone else's archive (the AROS nightly) are
  not on the portal: only their `Archive:` line, and machines download them
  from where they are published.
- A push needs a key given by the portal's maintainers, in `PKG_PUSHKEY`,
  and goes over `https` only (`http` is refused, 403).
- A package keeps the key of its first version: a push signed by another
  key is refused (14). A published file never changes (15), and a push
  never removes anything.
- Every push is checked by the portal running pkg itself (`SHOW ... METADATA`)
  before anything is published. What that proves and what it cannot:
  [Signatures and trust](signing.md).
- A portal can name the oldest pkg it still works with (`Portal:Policy:MinPkg`).
  An older pkg is refused when it pushes, and when it reads if the portal
  says so, with the version to update to; the channel pkg itself comes
  from, its bootstraps and the installers stay open to every version. pkg
  names its version in `User-Agent` since 1.5.
- A channel can be **unlisted**: served like any other to whoever has its
  address, and shown nowhere on the site (home page, search, statistics,
  feeds, publishers). The portal's maintainers switch it; it is for
  previews and private rounds, not a secret: the address is the only key.
- Its rules are readable: `https://aros-pkg.azurewebsites.net/api/policy`
  says whether pushes are open, which keys may upload files (the others
  publish by link to an `https` archive), which hosts links may point to,
  whether new channels may be created, and how to reach the maintainers;
  `/trust` shows the same. A refusal a rule causes carries a `policy:` line
  naming it. Another portal instance chooses its own rules.
- What the installers hand out is signed: `Bootstrap/SHA256SUMS` lists the
  SHA-256 of every host build, the AROS `Pkg` binaries, `Install-Pkg` and
  `ReadMe`, and `SHA256SUMS.sig` is that list signed by the channel owner's
  key in OpenSSH's format, so `ssh-keygen -Y verify` checks it before pkg
  exists on the machine ([Signatures and trust](signing.md#the-installers)).

Publishers upload with `PUSH`; see
[Publishing](publishing.md#upload-to-the-portal).

### What pkg tells a channel, and what the portal keeps

Every request pkg makes names pkg's version, the system and the CPU it was
built for, and nothing else:

```
User-Agent: Pkg/1.7.0+20260920 (aros; aarch64)
```

No name, no key, no machine identifier, no list of what is installed. The
portal keeps one tally per day from those three words, for each kind of
request: `day, version, system, cpu, kind, count`, where the kinds are
channel reads, packages taken and pushes. A line says "on this day, this
many channel reads came from this build", and that is the whole of it: no
addresses, no identifiers, no record of a single request, and no count of
people, since a tally cannot tell one machine asking a hundred times from
a hundred machines asking once. Anything that is not pkg counts as
`other`, and so does the rest of a day once a few hundred different builds
have been seen, which is what stops invented names from growing the file.

The portal shows it on its statistics page and hands over everything it has
at `/api/usage`; its `/privacy` page says the same in full, including what
its hosting platform logs. A channel you serve yourself sees the same header
and does with it what its server does.

## Host your own portal

The portal is in this repository, `portal/`, and anyone can run one: for a
club, a company, a distribution of your own, or a mirror. It is an ASP.NET
Core application (.NET 10) with no database; the channel directories are
its only state, so a channel you already serve from a directory becomes a
portal channel by being placed under its data directory.

To try it on your machine:

```sh
git clone https://github.com/jonx/aros-pkg && cd aros-pkg
make                                   # build/pkg: the portal checks pushes with it
cd portal/src/Portal && dotnet run     # http://localhost:5000, data in portal/data
```

Put a channel under `portal/data/<name>` (or push one: see below) and
`pkg SHOW CHANNEL http://localhost:5000/<name>` reads it like any channel.

To let publishers push, make a key per publisher and put its hash in the
settings; the key itself goes to the publisher once and is never stored:

```sh
dotnet Portal.dll key jane mychannel         # prints the key, and the Portal:Keys line
```

Settings, in `appsettings.json` or as environment variables with `__`
(`Portal__PublicUrl`):

| Setting | Meaning |
|---|---|
| `Portal:DataDir` | where channels, staging and state live |
| `Portal:PkgPath` | the pkg binary that checks pushes |
| `Portal:Keys` | `name:sha256-of-key:channel,channel;...`, one entry per publisher |
| `Portal:PublicUrl` | the address shown in the commands on the pages |
| `Portal:Pinned` | packages shown first on the home page, `channel/name` |
| `Portal:AllowLoopbackHttpPush` | pushes over `http` from `127.0.0.1`, for a local instance |

Deploying: any host that runs .NET 10 and gives the app a writable data
directory serves it; `portal/tools/deploy-azure.sh` is the script that puts
the public portal on an Azure Linux web app, to copy or adapt. The push
protocol, what the portal checks and its tests are in
[portal/README.md](../portal/README.md).
