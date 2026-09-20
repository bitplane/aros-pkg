<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# MOUNTLIST

The Mount entry for an installed image.
```
pkg MOUNTLIST <name> ROOT <root> [OUT <file>] [UNIT n] [HANDLER <path>]
```

## What it does

A package of kind `image` installs as one file, a disk image. `MOUNTLIST`
writes the DOSDriver entry AmigaDOS mounts it with (its size read from the
image), names the `fdsk.device` unit (`UNIT`, 20 by default), and prints the
steps to mount it: the `FDSK:` assign, the link from the unit to the image,
`Protect`, `Mount`. Without `OUT` it prints the entry instead of writing it.
`HANDLER` names an FFS handler when the system has none of its own (hosted
AROS built on macOS); native AROS needs none.

## Examples

```console
$ pkg INSTALL notes ROOT aros CHANNEL channel
installed notes 1.0 into aros: 1 file, payload cfac33cc9d05, signed by 5ff18d3fe14e383e
  image    notes.hdf, 32 blocks
  hint: to run it, mount the image: MOUNTLIST notes ROOT aros OUT <file> writes the mount entry and lists the steps
$ pkg MOUNTLIST notes ROOT aros OUT aros/Devs/DOSDrivers/NOTES
wrote aros/Devs/DOSDrivers/NOTES, the mount entry for notes (32 blocks)
  On AROS, the device is named after the mountlist file:
    MakeDir RAM:fdsk
    Assign FDSK: RAM:fdsk
    MakeLink RAM:fdsk/Unit20 aros/notes.hdf
    Protect aros/notes.hdf w SUB
    Mount aros/Devs/DOSDrivers/NOTES
  hint: no FFS handler is installed in this root, so the entry relies on the system's. Native AROS has one; hosted AROS built on macOS has none: there, install one into the root as a device package, or name one with HANDLER <path>
$ cat aros/Devs/DOSDrivers/NOTES
Device          = fdsk.device
Unit            = 20
Flags           = 0
Surfaces        = 1
BlocksPerTrack  = 32
LowCyl          = 0
HighCyl         = 0
Reserved        = 2
BlockSize       = 512
Buffers         = 20
BufMemType      = 1
Mask            = 0
StackSize       = 16384
Priority        = 5
GlobVec         = -1
DosType         = 0x444F5303
Activate        = 1
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg MOUNTLIST helloworld ROOT aros    # exits 20
pkg mountlist: helloworld is a application, not an image; only an image is mounted
  next: fix the command; pkg HELP lists the verbs and keywords
```

## Records (`MACHINE`)

`result: written` (or `shown`), `name:`, `image:`, `blocks:`, `highcyl:`,
`unit:`, `file:`, `handler:`, one `step:` per AmigaDOS command, and the
image's catalogue fields.

## Related

[Using pkg](../using.md#programs-as-images), [IMAGE](image.md),
[INSTALL](install.md).
