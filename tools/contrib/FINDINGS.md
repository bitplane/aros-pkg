<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# What the nightly contrib archive makes harder to package

Found while packaging the 2026-09-18 pc-x86_64 contrib archive, for
cleaning up the upstream build when the time comes. Pkg packages the
archive regardless: nothing here stops a publish or an install. Regenerate
the version lists with `tools/contrib/audit.py <archive>.pkgidx <table>`.

| Finding | What Pkg does meanwhile | What would fix it upstream |
|---|---|---|
| 42 components carry no `$VER` in any file: iperf3, sockperf, every demo under Extras/Demos, AmigaHunkParser, nasm, LBreakout2, Bomber, Spout, XInvaders3D, Inv, CXHextris, MadMatrix, AMP2 and its plugins, Radium, C-Ray, DBWRender, POVRay, CDXLPlay, WGet, Rexx, Text2PDF, the TrueType fonts, Curl | Version `0+<nightly date>`: ordered, but saying nothing of the program's own version | A `$VER` cookie in each program, as AmigaOS style asks |
| 18 components bundle several programs with their own `$VER`, none named like the component: Info-ZIP (zip 3.0, unzip 6.0), xadmaster, the SDK, BGUI, DOpus, Build, Lua, Kiel, the Misc, Aminet and Fish collections, Wazp3D, and the six MCC classes (TheBar, BWin, MailText, PopPH, Toolbar, Urltext) | Version `0+<date>` too, until Pkg knows each component's main file | A declared main file per component, from the build (the `contrib-*` target's own module), which Pkg can then read |
| 13 files installed outside any component's drawer, with no registration: six datatypes in Classes/DataTypes, GIFAnim in Devs/DataTypes, SmartTrash in WBStartup, two MUIBuilder catalogs in Locale, C/exe2arc | Being traced to their build targets (tools/contrib/owners.py) so the split follows the build | One owner per installed file, visible in the build metadata |
| The SYS/Packages registrations name their drawer through `Extras:` (Lua: `Extras:Developer/Lua`) | Contrib packages install at the archive's own paths, into SYS: | Nothing, if contrib always lives in SYS:Extras; otherwise registrations written at install time |
| The toolchain and SDK is one 2.4 GB tree under Developer | Published as one package, `sdk` | Split into compiler, libraries, headers and debug parts (to discuss with the AROS developers) |
| 100 of 104 components carry no short description of their own, and 99 no category: only FlexCat, MinAD, AmiChess, PlayCDDA and ShellPlayer ship an Aminet `.readme` (FlexCat's `Short:` is 65 characters, over Aminet's 40) | A Short line and an Aminet category written for each in `tools/contrib/about.txt`; the list of which is `tools/contrib/about.py`'s `missing.txt` | An Aminet `.readme` in each component's drawer (Short, Author, Type, Version, the text), which the build installs with it |
| 92 components have no description in any file, 93 name no author, 95 carry no licence file | Left out: the portal says "not stated". Descriptions quoted from a readme for 7 (`tools/contrib/about-stated.txt`, each with its file), licences read from the licence file for 9 | The same `.readme`, and a `COPYING` or `LICENSE` in each drawer, or an SPDX line in the readme |

Not defects, recorded because they looked like ones:

- `Prefs/Env-Archive/deficons.prefs` is a hunk file holding one data hunk
  and no code: the classic deficons format, loaded with LoadSeg. It is not
  a 68k program; Pkg now tells the two apart by the first hunk's type.
- MCC_TheBar has a SYS/Packages registration and no S/ startup script: its
  classes are found another way.
