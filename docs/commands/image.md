<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# IMAGE

An FFS volume image of a drawer.
```
pkg IMAGE <drawer> OUT <file> [NAME <volume>]
```

## What it does

Writes the drawer as one Fast File System volume image, the form a package
of kind `image` ships: a program with everything it needs on one disk,
mounted on AROS with the entry `MOUNTLIST` writes. The volume name is the
drawer's name unless `NAME` says otherwise. `PUBLISH ... KIND image` does
this itself; `IMAGE` is for making the image alone.

## Examples

```console
$ pkg IMAGE MyTool OUT mytool.hdf NAME MyTool
wrote mytool.hdf: volume MyTool, 32 blocks of 512 bytes
$ pkg IMAGE nosuch OUT other.hdf    # exits 20
pkg image: "nosuch" is not a directory
  next: fix the command; pkg HELP lists the verbs and keywords
```

## Records (`MACHINE`)

`result: written`, `file:`, `volume:`, `blocks:`.

## Related

[MOUNTLIST](mountlist.md), [PUBLISH](publish.md),
[Using Pkg](../using.md#programs-as-images).
