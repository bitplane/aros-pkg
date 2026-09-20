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
| `Short`, `Category`, `Tags`, `Homepage`, `Repository`, `License`, `Distribution` | the catalogue fields, with the manifest's rules (Short at most 40 characters, an Aminet category, an SPDX licence expression) |
| `Author` | repeatable, one person or team each |
| `Description`, `Changes` | repeatable, one line each, an empty one is a paragraph break |
| `Depends` | packages this one needs, as `PUBLISH DEPENDS` |
| `Files` | repeatable: an installed path that belongs to the package, relative to the system root; a drawer means all of it. This is what the split tables guess today |
| `Config` | repeatable: a path of `Files` a person may edit (`PUBLISH CONFIG`) |
| `Upstream-Archive`, `Upstream-SHA256` | where the port's source came from |
| `Port`, `Port-Maintainer` | where the port lives in contrib, and who to ask |

Unknown keys are ignored, as in a manifest. So are an empty line and a line
starting with `#`. A bad value is refused, naming the file, the line and
the key.

## What Pkg does with it today

`PUBLISH <drawer> INFO <file>` and `MANIFEST <drawer> INFO <file>` read it:
the name, the version, the kind, the catalogue fields, `Depends`, `Config`
and `Files` are used for whatever the command does not give. A keyword on
the line wins over the file; the file wins over an Aminet `README` and over
what the last version published carried. `Files` selects what is published,
as `FILES` does, and a path it names that the drawer does not hold is
refused (exit 11) rather than skipped. With a drawer inside an archive,
`INFO "!/<path>"` reads the file out of that archive.

`tools/contrib/publish-nightly.sh` extracts every `*.pkginfo` of the
nightly in one pass, publishes each component from its own file, and leaves
the paths it claims out of the guessed split, so nothing is published
twice. The hand-written tables shrink as ports gain their file.

Still to do: propose the file to the AROS developers with the contrib
findings, so that new ports carry one from the start.
