<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# PUSH

Send a channel to the portal.
```
pkg PUSH CHANNEL <dir> TO <url> [SIGN <keyfile>]
```

## What it does

Uploads what a local channel holds that the portal's channel at `TO` does
not yet: manifests, signatures, payloads and archives (unless `UPSTREAM`
says the archive lives elsewhere), then asks the portal to commit. The
portal checks every signature before it publishes anything; a refused key
is exit 14, a portal that cannot be reached exit 17. Publish locally with
`PUBLISH`, then `PUSH`.

The portal knows who pushes in one of two ways:

- **To an `https://` address, with the portal's key**, which its maintainers
  gave you: it comes from `PKG_PUSHKEY`, never from the command line, and is
  sent only over `https`.
- **With no portal key, or to an `http://` address, by signing.** Each
  request is signed with your own publisher key (`SIGN <keyfile>`, or
  `PKG_SIGNKEY`), and the portal knows you by its public half, which you
  send its maintainers once (`pkg KEYINFO FILE <keyfile>`). Nothing secret
  travels, which is how a publisher who has no portal key pushes, and the
  only way to push to a plain `http://` address. The signature covers the
  address, the body and a number that only grows, so a request cannot be
  changed or sent again by someone else. What plain `http` does not give is
  secrecy (the files are public anyway) or proof that the answer came from
  the portal: `pkg SHOW CHANNEL <the same address>` afterwards shows what
  was really published.

  AROS pushes either way: it speaks `https` like every other system.

## Examples

```console
$ pkg PUSH CHANNEL channel TO https://aros-pkg.azurewebsites.net/mychannel    # exits 14
pkg push: no key to push with. A portal takes pushes only from publishers its maintainers have registered, and nobody can register themselves yet: ask them, as https://aros-pkg.azurewebsites.net/publishers explains. Once registered, sign the push with SIGN <keyfile> (or PKG_SIGNKEY), the key you registered, or over https with PKG_PUSHKEY, the key the portal gave you. Never make a key up
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
```

## Records (`MACHINE`)

`result:`, `uploaded:`, `uploaded-bytes:`, the portal's own answer lines
(`published:`, `refused:`), `summary:`.

## Related

[PUBLISH](publish.md), [Channels](../channels.md#the-portal),
[Publishing packages](../publishing.md#upload-to-the-portal).
