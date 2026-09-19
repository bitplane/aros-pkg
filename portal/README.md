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

## The maintainers' API

`/_admin/…`, HTTPS only, with an admin key (never a push key):

- `POST /_admin/channels/<channel>/remove` with lines `<name> *`,
  `<name> <version>`, `<name> <version> <arch>` or index lines; `?dryrun=1`
  says what would go. The versions leave the index; their files, first-seen
  dates and download counts move to `state/removed/<stash>/`, nothing is
  destroyed, and a file another version still needs stays.
- `POST /_admin/restore/<stash>` puts a removal back.
- `POST /_admin/channels/<channel>/unlist` serves a channel to whoever has its address and shows it nowhere (home page, search, statistics, feeds, publishers); `.../list` shows it again. `Portal:Unlisted` (`a;b`) does the same from the settings. `sh portal/tools/admin.sh <portal> unlist <channel>`.
- `GET /_admin/log` lists every admin action.

`tools/admin.sh` calls them from a shell.

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
| `Portal:Keys` | `name:sha256-of-key:channel,channel[:files];…`, one entry per publisher. Without `files` a key publishes by link only: signed manifests and signatures, whose files stay in an archive on an https server named by an `Archive:` line; payloads, archives and bootstrap programs are refused |
| `Portal:MaxStagingBytes`, `Portal:MaxLinkOnlyStagingBytes` | what one key may hold in staging at once (2 GiB, 16 MiB) |
| `Portal:PublicUrl` | the address shown in commands |
| `Portal:Publishers` | publisher profiles, `key|name|url|contact;…` (url and contact optional); they win over SignerNames |
| `Portal:Owners` | packages moved to another key by the maintainers, `channel/name=key;…`; from then on a push of that package must be signed by that key |
| `Portal:SignerNames` | names for signing keys, `hex=Name;…`; keys are otherwise named after the publisher whose push first used them |
| `Portal:Pinned` | packages shown first, `channel/name` separated by commas; `pkg/pkg` by default |
| `Portal:AdminKeys` | maintainers' keys for `/_admin`, `name:sha256-of-key;…`; `dotnet Portal.dll adminkey <name>` makes one |
| `Portal:Policy:Push` | uploads at all (`true`); `false` makes a read-only mirror |
| `Portal:Policy:Binaries` | `keys`: binaries from keys with the `files` right; `off`: from nobody, every publisher links |
| `Portal:Policy:LinkHosts` | hosts an `Archive:` address may name, comma-separated; empty: any https host (subdomains match) |
| `Portal:Policy:NewChannels` | whether a push may create a channel (`true`) |
| `Portal:Policy:Admin` | the maintainers' API (`true`) |
| `Portal:Policy:PlainHttp` | channels also over plain http (`true`); `false` sends every http request to https |
| `Portal:Policy:Note`, `Portal:Policy:Contact` | the operators' sentence and contact, added to every refusal a rule causes and shown on `/trust` |
| `Portal:BootstrapKey` | the OpenSSH public key (`ssh-ed25519 …`) that signs the pkg channel's `Bootstrap/SHA256SUMS`; set, `/install` and `/install.ps1` check what they download with `ssh-keygen -Y verify` before installing it |
| `Portal:AllowLoopbackHttpPush` | pushes over http from 127.0.0.1, for a local instance only |

A key: `dotnet Portal.dll key <publisher> <channel,channel|*> [files]` prints the
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
