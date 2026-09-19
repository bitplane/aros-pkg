<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# SIGN

A detached signature for any file.
```
pkg SIGN <file> KEY <keyfile> OUT <sigfile>
```

## What it does

Signs any file with a Pkg key and writes the signature to `<sigfile>`, in
the form the channel uses for manifests (`Signer:` and the Ed25519
signature). For a checksum list, a release note, or anything you want a
machine to verify against your public key outside a package.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key b01be86b74a9c904391b65bf96038f70d71e95f02054397ac4c273207e7b0eb6
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ printf 'release notes\n' > notes.txt
$ pkg SIGN notes.txt KEY my.key OUT notes.txt.sig
signed notes.txt with b01be86b74a9c904
$ cat notes.txt.sig
Signer: b01be86b74a9c904391b65bf96038f70d71e95f02054397ac4c273207e7b0eb6
Signature: e4ff69200b3150baadb65f5f1250906ce132dae5364bdd6734d836c1434f01c8f53fe119dbd6583671d0e642f7a757b754221aabf02d565078af95938ee5c605
```

## Records (`MACHINE`)

`result: signed`, `file:`, `signer:`.

## Related

[KEYGEN](keygen.md), [KEYINFO](keyinfo.md).
