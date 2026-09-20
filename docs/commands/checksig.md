<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# CHECKSIG

Whether a signature made by `SIGN` is good, and whose it is.
```
pkg CHECKSIG <file> FILE <sigfile> [KEY <public key>]
```

## What it does

Checks the detached signature `<sigfile>` (the `Signer:` and `Signature:`
lines `SIGN` writes) over `<file>`, and says which public key signed it. With
`KEY`, the signer must be that key (exit 14 when it is another). A missing or
bad signature is exit 13. It needs nothing but pkg, so it works on AROS,
where there is no `ssh-keygen`; the portal checks signed pushes with it.

## Examples

```console
$ pkg KEYGEN FILE my.key
key written to my.key, readable by you alone
  public key 0cd991a390aec90403a9f38d21578cca68ca52d880fb0d272b18db9cd5d9c893
  hint: every later version of what this key publishes must be signed with it: keep the file with the person's secrets, outside any channel or repository, and back it up. Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared
$ printf 'release notes\n' > notes.txt
$ pkg SIGN notes.txt KEY my.key OUT notes.txt.sig
signed notes.txt with 0cd991a390aec904
$ pkg CHECKSIG notes.txt FILE notes.txt.sig
notes.txt is signed by 0cd991a390aec90403a9f38d21578cca68ca52d880fb0d272b18db9cd5d9c893
$ printf 'changed\n' > notes.txt
$ pkg CHECKSIG notes.txt FILE notes.txt.sig    # exits 13
pkg checksig: the signature does not check: notes.txt or notes.txt.sig was changed after signing, or the signature is for another file
  next: stop here: the bytes or signatures are not what was published, and no keyword or other channel makes that safe
```

## Records (`MACHINE`)

`result: good`, `file:`, `signer:`.

## Related

[SIGN](sign.md), [KEYINFO](keyinfo.md), [Signatures and trust](../signing.md).
