<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# KEYINFO

The public key a key file holds.
```
pkg KEYINFO FILE <keyfile> [SSH]
```

## What it does

Prints the public key of a key file, the part you may share: it is what
`SHOW` prints as the signer and what `ACCEPTKEY` takes. Checks that the file
is a Pkg key and that its public half matches its secret (exit 12 when it
is damaged).

With `SSH`, prints the same key as OpenSSH writes one, `ssh-ed25519`, the
key, and the key file's name as its comment: the line an `allowed_signers`
file takes for [`SIGN ... SSH`](sign.md) signatures.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key e7b8fc13ae67ec39af29ad1e2ee75e2396109320d24d4c8e6f0662f1f4e429f8
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ pkg KEYINFO FILE my.key
my.key holds the public key e7b8fc13ae67ec39af29ad1e2ee75e2396109320d24d4c8e6f0662f1f4e429f8
$ pkg KEYINFO FILE my.key SSH
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIN13/qqhgko5gmpxaYSg2wSae9eRdbnvwaTf2I2m/1z7 my
$ pkg KEYINFO FILE channel/index    # exits 12
pkg keyinfo: "channel/index" is not a Pkg key file
  next: stop here: the bytes or signatures are not what was published, and no keyword or other channel makes that safe
```

## Records (`MACHINE`)

`result: shown`, `file:`, `public:`, and with `SSH` `ssh:`.

## Related

[KEYGEN](keygen.md), [Publishing packages](../publishing.md#your-key).
