<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# KEYGEN

A signing key, readable by you alone.
```
pkg KEYGEN FILE <keyfile>
```

## What it does

Makes an Ed25519 key pair and writes it to `<keyfile>` with permissions for
its owner alone. The public key is printed; the secret stays in the file.
An existing file is never overwritten (exit 15). Every version you publish
is signed with this key, and every machine that installs your package pins
it: losing the key means a new one that every machine must accept by hand,
so keep the file with your secrets and back it up. `SIGN <keyfile>` on
`PUBLISH`, or `PKG_SIGNKEY`, names it.

The 32 bytes the key is made from come from the system's random source, on
AROS as anywhere else: AROS gathers entropy in `entropy.resource`, and Pkg
reads it through `getentropy`.

On an AROS old enough to have neither, there is no source at all, and the
clock, free memory and the task's address can all be guessed, so Pkg asks
the person at the Shell window to press keys at random and hashes the moment
of each one, read from the CPU's cycle counter and the system clock, with
what was typed. A key press counts for 4 bits (2 without a cycle counter), a
repeated one for none, and typing goes on until 256 bits are counted: 64
keys. Without a window (output redirected, `MACHINE`, the ARexx port)
`KEYGEN` refuses with exit 17 and writes nothing; Ctrl-C while typing does
the same.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key 186cae2ad3d9f560b46e143af6be6b4b634646662872f3e2a7e29c25a97c5a74
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ pkg KEYGEN FILE my.key    # exits 15
pkg keygen: "my.key" already exists; a key is never overwritten
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
```

## Records (`MACHINE`)

`result: created`, `file:`, `public:`.

## Related

[KEYINFO](keyinfo.md), [SIGN](sign.md), [PUBLISH](publish.md),
[Publishing packages](../publishing.md#your-key).
