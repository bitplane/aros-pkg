<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# The Pkg portal

A small package site in the manner of nuget.org, for Pkg channels. It does two things:

- **It serves channels over HTTP**, file for file as a directory channel:
  `/{channel}/index`, `/{channel}/objects/…`, `/{channel}/archives/…`,
  `/{channel}/Bootstrap/<cpu>/Pkg`, `/{channel}/Install-Pkg` and `/{channel}/ReadMe`.
  `pkg … CHANNEL https://host/{channel}` reads it like a local channel. It
  answers over plain HTTP too, since 68k machines have no TLS and the
  signatures carry integrity. There is no HSTS, and Range requests work on
  archives.
- **It shows a catalogue** generated from the manifests: search (by name and
  by the files packages hold), a page per channel with each nightly's
  statistics, and a page per package with its versions, dependencies, the
  packages that use it, its files with protection bits and comments, its
  signer and its source archive.

There is no database. The channel directories are the only state, plus
`state/<channel>/archive-checks` and the push staging area.

## Publishing: plan, files, commit

Publishers sign on their own machine; the portal never holds a signing key.
A push has three steps, all under `/{channel}/_push/`, HTTPS only, with
`Authorization: Bearer <key>`:

1. `POST plan`, with one line per local file: `<path> <sha256> <size>`.
   The answer lists `need: <path>` for each file the portal lacks.
2. `PUT files/<path>` for each needed file. Files over 32 MiB go in parts
   with `Content-Range: bytes <a>-<b>/<size>`. A part that does not start
   where the portal stopped is answered with `received: <n>`, so an
   interrupted upload resumes from there.
3. `POST commit`, with the local index. The portal checks the new entries
   by running Pkg itself (`SHOW CHANNEL <staging> METADATA`). That covers
   signatures, digests and withdrawals. The portal enforces one more rule:
   a package keeps the key of its first version. It then places the files
   and swaps the index atomically. A published file never changes and a
   push never removes anything.

Each source archive is then checked once in the background
(`SHOW … ARCHIVE <name>`); until then its packages say "check pending".

Every answer is a Pkg record, with a plain `summary:` sentence and
statistics on what was sent and what was not sent again. `pkg PUSH` speaks
this protocol. Until it exists, `tools/push.sh` does the same with curl:

```sh
PKG_PUSHKEY=<key> sh portal/tools/push.sh <local channel dir> https://<host>/<channel>
```

## Running it

```sh
cd portal/src/Portal
dotnet run                      # Development: data in portal/data, pkg from ../../../build/pkg
```

Settings (environment variables use `__`, e.g. `Portal__DataDir`):

| Setting | Meaning |
|---|---|
| `Portal:DataDir` | channels, staging, state; `/home/data` on App Service |
| `Portal:PkgPath` | the Pkg that checks pushes; the deployment puts the static Linux build beside the app |
| `Portal:Keys` | `name:sha256-of-key:channel,channel;…`, one entry per publisher |
| `Portal:PublicUrl` | the address shown in commands |
| `Portal:AllowLoopbackHttpPush` | pushes over http from 127.0.0.1, for a local instance only |

A key: `dotnet Portal.dll key <publisher> <channel,channel|*>` prints the
key, which goes to the publisher once, and the line for `Portal:Keys`,
which holds only its SHA-256.

## Deploying

`sh portal/tools/deploy-azure.sh [app] [resource group]` builds the static
Pkg for Linux and the portal for linux-x64, then zip-deploys both to the web
app (`aros-pkg` in `rg-aros`, on the Linux B1 plan `asp-aros-linux`).

`global.json` pins the SDK to the 10.0.1xx band. The Razor compiler in SDK
10.0.401 refuses valid markup: `<text>`, `<details>` and `<section>` inside
a code block, and one-line `@if (…) { <tag> }` blocks.

## Tests

`dotnet test portal/tests/Portal.Tests` covers the version order (the same
as Pkg's), which paths a channel serves, manifest attributes, and the push
gate (HTTPS, key, channel scope, no indexing).
