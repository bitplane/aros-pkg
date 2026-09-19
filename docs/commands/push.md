<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# PUSH

Send a channel to the portal.
```
pkg PUSH CHANNEL <dir> TO <https url>
```

## What it does

Uploads what a local channel holds that the portal's channel at `TO` does
not yet: manifests, signatures, payloads and archives (unless `UPSTREAM`
says the archive lives elsewhere), then asks the portal to commit. The
portal's key comes from `PKG_PUSHKEY`, never from the command line, and
goes only over `https`. The portal checks every signature before it
publishes anything; a refused key is exit 14, a portal that cannot be
reached exit 17. Publish locally with `PUBLISH`, then `PUSH`.

## Examples

```console
$ pkg PUSH CHANNEL channel TO https://aros-pkg.azurewebsites.net/mychannel    # exits 14
pkg push: no portal key: set PKG_PUSHKEY to the key the portal gave the publisher. Ask whoever requested this for it; never make one up
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
```

## Records (`MACHINE`)

`result:`, `uploaded:`, `uploaded-bytes:`, the portal's own answer lines
(`published:`, `refused:`), `summary:`.

## Related

[PUBLISH](publish.md), [Channels](../channels.md#the-portal),
[Publishing packages](../publishing.md#upload-to-the-portal).
