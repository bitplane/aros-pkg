<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signatures and trust

What is signed, by whom, what is checked where, what a key change means for
you, and how to check a package yourself without Pkg.

## What is signed

A package version is described by its **manifest**: name, version, CPU,
kind, dependencies, and every file with its size and SHA-256. The publisher
signs the manifest with an Ed25519 key; the signature is a small file beside
it, `<digest>.sig`:

```
Signer: a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7
Signature: a596b3131e51a2ba0e06b04a39483dbad54a5ffbb39c722908cfc4a88ab0f88b...
```

The files themselves are not signed one by one; they do not need to be.
The manifest lists each file's SHA-256, the signature covers the manifest,
and Pkg refuses any file whose bytes do not hash to what the manifest says.
So a valid signature on the manifest vouches for every byte of the package.

The channel's `index` names each version by the SHA-256 of its manifest, and
the payload container by the SHA-256 of its bytes. Nothing in a channel is
ever rewritten except the index: a version, once published, is immutable.

## Who signs

**The publisher, and nobody else.** A publisher makes a key once
([`KEYGEN`](commands/keygen.md)), keeps the secret half with their secrets,
and signs every version they publish with it. The public half, 64 hex
digits, is the publisher's identity: it is what `SHOW` prints under *Signer*
and what the portal shows on a package page.

The portal holds no signing key. It cannot sign a package, alter one, or
sign a replacement: it can only refuse to publish. The same is true of
anyone who copies or mirrors a channel: the files are what the publisher
signed, or Pkg refuses them.

Nobody vouches for who a publisher *is*. The key is the identity; a name
beside it on the portal is a label its maintainers attached when they gave
that publisher a push key. If it matters to you that `pkg` on the portal is
signed by this project, compare the key with the one published here:

```
a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7
```

## What is checked, and where

**On your machine, by Pkg, at every install, upgrade, verify and repair:**

- the signature of the manifest, under the key the `.sig` names (refused
  with exit 13 when absent or invalid);
- every file of the package against the size and SHA-256 in the manifest
  (exit 12 when one differs), before anything is written;
- that the signer is the key **pinned** for this package on this machine
  (exit 14 when it is not; see below).

**On the portal, before a push is published:** the portal runs Pkg itself
on the uploaded channel (`SHOW ... METADATA`), so the same signature and
digest checks apply; then two rules of its own: a package keeps the key of
its first version (a push signed by another key is refused), and a
published file never changes (a push that would replace one is refused).

**On the way:** nothing. A channel may travel over plain `http`, a USB
stick or a shared drawer; the connection proves nothing and does not have
to. This is why AROS machines without TLS are as safe as any other.

## Key pinning, and what a key change means for you

The first version of a package you install pins its signer's key in your
root, for that package. From then on:

- a version signed by the same key installs, upgrades and repairs as usual;
- a version signed by **another** key is refused (exit 14), whoever
  published it and wherever it comes from, and Pkg prints both keys.

That refusal is the point of the system: it is what stops a channel or a
mirror from replacing a program under you. It is also what you see when a
publisher legitimately changes their key. Only you can tell the two apart,
by asking the publisher through a channel you trust (their web page, their
repository, a person you know). If the new key is theirs, accept it once:

```
pkg UPGRADE hello ROOT SYS: CHANNEL DEPOT:channel ACCEPTKEY <the new key>
```

Pkg never accepts a new key by itself, and never prints a command with the
new key filled in for you to paste; an assistant that drives Pkg is told
the same ([Pkg with an AI assistant](agents.md)).

## When a publisher loses a key

There is no recovery: a lost secret key cannot sign again, and the portal
cannot re-sign anything. The publisher makes a new key, announces its
public half where their users can see it, and publishes the next version
with it; the portal's maintainers replace the old key with the new one in
the push permissions of that publisher's channels (the first-key rule is
applied per package, so this needs the maintainers, not a trick). Every
machine that has the package sees exit 14 once and accepts the new key
with `ACCEPTKEY`. Nothing already installed is affected.

Back your key up. `KEYGEN` writes one file, readable by you alone; a copy
in a password manager or on an encrypted disk is enough.

## Checking a package by hand

Pkg does this at every install, and `SHOW` does it for a whole channel:

```console
$ pkg SHOW CHANNEL https://aros-pkg.azurewebsites.net/pkg
Package  Version  Kind         Arch     Status  Signer
pkg      0.3      application  aarch64  ok      a974a917b19cfc46
pkg      0.3      application  x86_64   ok      a974a917b19cfc46
pkg      0.4      application  aarch64  ok      a974a917b19cfc46
pkg      0.4      application  x86_64   ok      a974a917b19cfc46
```

To check without trusting Pkg at all, three files and one script suffice.
`tools/verify-manifest.py` in the repository is plain Python 3 with no
module beyond the standard library; it implements the Ed25519 check from
RFC 8032 in sixty readable lines, so you can read what it does:

```sh
B=https://aros-pkg.azurewebsites.net/pkg
curl -fsSO $B/index
d=$(awk '$1=="pkg" && $2=="0.4" && $3=="x86_64" {print $4}' index)   # the manifest's digest
curl -fsSO $B/objects/$d.manifest
curl -fsSO $B/objects/$d.sig
python3 tools/verify-manifest.py $d.manifest $d.sig a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7
```

```
digest:    the manifest is the one its name says, sha256 6b31aa973a476b59
signature: valid, made by a974a917b19cfc46eb462510fa21f95013932bfbda7c8f343e06a3e988f7bde7
signer:    the expected key
result:    the manifest is what its publisher signed
```

Change one byte of the manifest and it says `do not trust this package`.
From there, the manifest's `File:` lines give the SHA-256 of every file;
`shasum -a 256` on an installed file, or on a file taken out of the `.pkg`
container, completes the check.

## Related

[`KEYGEN`](commands/keygen.md), [`KEYINFO`](commands/keyinfo.md),
[`SIGN`](commands/sign.md) for a detached signature on any file,
[Publishing packages](publishing.md#your-key), [Channels](channels.md),
[the container](container.md) for the byte layout of a `.pkg`.
