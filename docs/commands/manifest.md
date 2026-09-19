<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# MANIFEST

The manifest PUBLISH would sign, to read before publishing.
```
pkg MANIFEST <drawer> [NAME n] [VERSION v] [ARCH cpu] [KIND k] [DEPENDS "a >= 1, b"] [CONFIG "f, g"] [FILES "a, b"] [BUILD <date>] [README <file>] [SHORT ..] [DESCRIPTION <file>] [CATEGORY ..] [TAGS ..] [AUTHOR ..] [HOMEPAGE ..] [REPOSITORY ..] [LICENSE ..] [DISTRIBUTION ..] [CHANGES <file>] [ICON ..] [SCREENSHOT ..]
```

## What it does

Reads a drawer as `PUBLISH` would and prints the manifest, unsigned:
name and version from the `$VER:` string of the program (or `NAME` and
`VERSION`), the CPU from the executables' headers, the kind, dependencies,
configuration files, every file with its size and SHA-256, the protection
bits and comments from `.ameta`, the catalogue fields, and `Provides:` for
each `.library` or `.device` the drawer ships. Host metadata no Amiga uses
(`.DS_Store`, AppleDouble files, `.git`) is left out and listed. It also
warns: an executable that opens a library no dependency provides, a `$VER:`
that contradicts `VERSION`, no executable in an application. Nothing is
written.

## Examples

```console
$ pkg MANIFEST MyTool KIND application
Format: pkg-manifest 1
Name: mytool
Version: 1.0
Architecture: x86_64
Kind: application
Payload: bf9bc6edcb45a4a786709554952e6988649c6e6cc405595d86f54e3d1d8c87d4
File: c93731985b65bf43460090b575902f0a6eca3ad030914fb0e58c0700569b2d85 94 C/MyTool
Protect: 0x00000002 C/MyTool
$ pkg MANIFEST MyTool-1.1 KIND application DEPENDS "hellolib >= 1.0" CONFIG S/MyTool.prefs
Format: pkg-manifest 1
Name: mytool
Version: 1.1
Architecture: x86_64
Kind: application
Depends: hellolib >= 1.0
Payload: 4606917aa7ca32a6d1d5b8bfe2fd8e8b82a94c65d105caa8e4229c282311dedf
File: 37a22af64bb210dc1f16587e4bae09026f437614b322ad681bfb46dd4288e757 94 C/MyTool
File: 70c860de45698205204d4d766d861a795cf2a8e6252b29596aa1c8be449b2214 15 S/MyTool.prefs
Protect: 0x00000002 C/MyTool
Protect: 0x00000002 S/MyTool.prefs
Config: S/MyTool.prefs
$ pkg MANIFEST channel    # exits 20
pkg manifest: channel needs a value
  next: fix the command; pkg HELP lists the verbs and keywords
```

## Refusals

| Exit | When |
|---|---|
| 11 | a `DESCRIPTION`, `CHANGES` or `README` file cannot be read |
| 20 | no drawer; executables for two CPUs in one drawer; `CONFIG` or `FILES` naming what is not there; `DEPENDS` malformed; `.ameta` naming a file the drawer lacks |

## Related

[PUBLISH](publish.md), [Publishing packages](../publishing.md#see-what-the-package-would-be).
