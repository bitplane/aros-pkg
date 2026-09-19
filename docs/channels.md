# Channels

A channel is where packages are published and where Pkg finds them: a
directory, or the same directory served over HTTP. It holds only signed
files, so it can live anywhere, a disk, a share, a web server, without
being trusted: Pkg checks every signature and every file itself.

## Where a channel can be

- **A directory** on a disk, a share or an image: `CHANNEL Work:channel` on
  AROS, `CHANNEL channel` on a Mac or a PC. This is how AROS reads channels.
- **A web address**: `CHANNEL https://aros-pkg.azurewebsites.net/contrib-nightly`,
  or `http://`. On macOS, Linux and Windows, Pkg reads a channel over the
  network exactly as it reads a directory, and keeps what it downloaded in
  a cache (`PKG_CACHE`, else `~/.cache/pkg`, or `%LOCALAPPDATA%\pkg-cache`
  on Windows). On AROS it is the same over `http://`, once the machine's
  network is started (AROSTCP, or a hosted AROS's own sockets); the cache
  is `SYS:.pkg/cache`. AROS has no TLS, so `https://` is refused there
  with that reason; nothing is lost, since Pkg checks every signature and
  every file itself whatever the connection.

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
  Pkg knows are read strictly; a key it does not know is kept in the signed
  text, never acted on, and reported by `SHOW` as `ignored`, so a package
  can carry lines for other distribution systems, and a misspelt key
  (`Categroy:`) is seen rather than silently lost. A `Format` Pkg does not
  know is refused: a change Pkg must understand gets a new format number.
- `objects/<digest>.manifest`: the manifest, named by its own SHA-256, and
  `<digest>.sig`, its signature. The manifest lists every file of the
  package with its size and SHA-256.
- `objects/<digest>.pkg`: the files themselves, in one container named by
  its SHA-256, which the manifest gives.
- `objects/<digest>.withdrawn` and `.withdrawn.sig`, for a withdrawn version.
- `archives/<name>`: the archive a package's files stay in, for packages
  published from someone else's archive, unless its makers publish it
  themselves (`UPSTREAM`).

Nothing in a channel is ever rewritten except `index`: a published version
stays as it is, so a copy of a channel made at any moment is consistent.

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

Pkg follows redirects and understands chunked replies; for `https`, it uses
the system's `curl`.

## The portal

The package portal at `https://aros-pkg.azurewebsites.net` serves channels
this way, over `https` and over plain `http` for machines without TLS, and
shows each package on a web page: its versions, dependencies, files,
signer and catalogue. **Downloads** is where a newcomer starts (Pkg for
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
- Every push is checked by the portal running Pkg itself (`SHOW ... METADATA`)
  before anything is published. What that proves and what it cannot:
  [Signatures and trust](signing.md).
- Its rules are readable: `https://aros-pkg.azurewebsites.net/api/policy`
  says whether pushes are open, which keys may upload files (the others
  publish by link to an `https` archive), which hosts links may point to,
  whether new channels may be created, and how to reach the maintainers;
  `/trust` shows the same. A refusal a rule causes carries a `policy:` line
  naming it. Another portal instance chooses its own rules.
- What the installers hand out is signed: `Bootstrap/SHA256SUMS` lists the
  SHA-256 of every host build, the AROS `Pkg` binaries, `Install-Pkg` and
  `ReadMe`, and `SHA256SUMS.sig` is that list signed by the channel owner's
  key in OpenSSH's format, so `ssh-keygen -Y verify` checks it before Pkg
  exists on the machine ([Signatures and trust](signing.md#the-installers)).

Publishers upload with `PUSH`; see
[Publishing](publishing.md#upload-to-the-portal).

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
| `Portal:PkgPath` | the Pkg binary that checks pushes |
| `Portal:Keys` | `name:sha256-of-key:channel,channel;...`, one entry per publisher |
| `Portal:PublicUrl` | the address shown in the commands on the pages |
| `Portal:Pinned` | packages shown first on the home page, `channel/name` |
| `Portal:AllowLoopbackHttpPush` | pushes over `http` from `127.0.0.1`, for a local instance |

Deploying: any host that runs .NET 10 and gives the app a writable data
directory serves it; `portal/tools/deploy-azure.sh` is the script that puts
the public portal on an Azure Linux web app, to copy or adapt. The push
protocol, what the portal checks and its tests are in
[portal/README.md](../portal/README.md).
