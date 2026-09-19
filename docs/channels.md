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
  on Windows). On AROS, reading a channel over the network is not built
  yet: copy the channel to a volume the machine reads.

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
this way, and shows each package on a web page. It holds only descriptions
and signatures: a package's files are either uploaded with it or, for
packages from someone else's archive, downloaded from where that archive is
published. Publishers upload with `PUSH`; see
[Publishing](publishing.md#upload-to-the-portal).
