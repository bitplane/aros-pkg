<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# `.pkginfo`: what a contrib port says about the package it is

Packaging the contrib nightly meant writing by hand, for 104 components,
what no file of theirs says: a short line, a category, an author, a licence,
a home page, and above all which installed paths belong to which component
([FINDINGS.md](FINDINGS.md), `about.txt`, the split tables). A port that
carries a `.pkginfo` file needs none of that: the information lives with the
port, is reviewed with it, and travels in the nightly archive.

The first one is `development/libs/mbedtls/mbedtls.pkginfo` in AROS contrib
(the Mbed TLS port Pkg's own TLS comes from).

## The file

Text, `Key: value` lines, as in an Aminet `.readme` and in Pkg's manifest, so
there is no second syntax. `<port>.pkginfo`, beside the port's `mmakefile.src`,
installed by the port with its documentation (`Developer/Docs/<name>/` for a
library, the program's own drawer otherwise).

| Key | Meaning |
|---|---|
| `Format: pkginfo 1` | first line |
| `Name`, `Version`, `Kind` | as `PUBLISH` takes them: the version of the software, not of the port; a static library has no `$VER` to read it from |
| `Short`, `Category`, `Tags`, `Author`, `Homepage`, `Repository`, `License`, `Distribution` | the catalogue fields, with the manifest's rules (Short at most 40 characters, an Aminet category, an SPDX licence expression) |
| `Description`, `Changes` | repeatable, one line each, an empty one is a paragraph break |
| `Depends` | packages this one needs, as `PUBLISH DEPENDS` |
| `Files` | repeatable: an installed path that belongs to the package, relative to the system root; a drawer means all of it. This is what the split tables guess today |
| `Config` | repeatable: a path of `Files` a person may edit (`PUBLISH CONFIG`) |
| `Upstream-Archive`, `Upstream-SHA256` | where the port's source came from |
| `Port`, `Port-Maintainer` | where the port lives in contrib, and who to ask |

Unknown keys are ignored, as in a manifest.

## What Pkg does with it today

Nothing yet: the fields of `mbedtls.pkginfo` were checked by giving them to
`PUBLISH` as keywords, which accepts them all. To do, in this order:

1. `PUBLISH ... INFO <file>`: read name, version, kind, the catalogue fields,
   `Depends`, `Config` and `Files` from a `.pkginfo`; keywords still win.
2. `tools/contrib/publish-nightly.sh`: for every `*.pkginfo` found in the
   archive, publish that component from it and leave it out of the guessed
   split; the hand-written tables shrink as ports gain their file.
3. Propose the file to the AROS developers with the contrib findings, so
   that new ports carry one from the start.
