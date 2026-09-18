<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Open items

What is known to remain, in one place, to fix when the time comes. Each item
says where it was seen and what would close it. Written 2026-09-18, after
goal 2.

## AROS defects not fixed

Details in the README table "AROS defects found along the way".

| Item | Seen in | What would close it |
|---|---|---|
| `C:Unpack` does not load on hosted aarch64 ("file is not executable"), shipped or rebuilt | goal 1 bootstrap; board thread 17 | Find why the loader refuses it (hunk or ELF flags, relocation); then bootstrap through `Unpack` as first designed |
| posixc `stdout` reaches no shell redirection | first AROS runs | Fix in posixc, or document that CLI tools must write through `Output()` |
| posixc reports no `EEXIST` or `ENOENT` where POSIX does | first AROS runs | Fix the errno mapping in posixc |
| A command that cannot load leaves `$RC` unchanged | goal 2, four-case check | Compare with AmigaOS 3.x first; if AmigaOS sets a failure code, set it in the AROS shell too |
| The darwin hosted build ships no FFS handler | goal 2 | Add `kernel-fs-afs` to the hosted build, or say why it is left out |

## Proposed upstream, waiting

| PR | Fix | Local branch |
|---|---|---|
| aros-development-team/AROS#1238 | identify.library decodes dead-end alerts | `~/aros-pr-identify`, `fix/identify-deadend-alerts` on jonx/AROS |
| aros-development-team/contrib#64 | Regina gives RC the port's number, RESULT its string | `~/aros-pr-regina`, `fix/regina-arexx-rc` on jonx/contrib |

When both are merged: drop `tools/aros/regina-arexx-rc.patch` and the copy
step in `tools/build-aros-regina.sh`, decode a dead-end alert in
`tests/goal2.sh` again, and remove the two worktrees.

## Native AROS runs

- **pc-x86_64 in QEMU**, started 2026-09-18 and paused to put the library
  in place first. Downloaded to `~/aros-native`: the nightly boot ISO and the
  linux-x86_64 system as SDK (MD5 checked). The native ISO carries its FFS
  handler, `SER:`, `DEBUG:`, and identify.library with Guru: the test removes
  identify.library from a remastered ISO (xorriso installed) so the
  dependency can only come from Pkg. Pkg for x86_64 AROS still needs a
  compiler: the AROS crosstools clang targets aarch64 only; Homebrew's LLVM
  has x86, and `collect-aros` links with `ld.lld` and LLVM tools, which do.
- **m68k**, later, at the owner's request: an Amiga emulator (UAE) with the
  AROS m68k build in `~/aros-m68k-build`; QEMU does not emulate an Amiga.

## Pkg, not built yet

- **Runs on Windows and Linux.** `build/pkg-test-kit.zip` is ready; only macOS
  has run it. The owner runs it on Windows.
- **A version rule for formats.** A package that owns an on-disk format must
  declare whether its state survives a downgrade, and ROLLBACK must honour it
  (`[PKG22]` item 5; README, "On hosted AROS").
- **Block-compressed images.** The image is stored whole; chunk size and codec
  are still open in the planning repository's packaging README.
- **Images over about 49 MB.** Bitmap extension blocks are not written, and
  such an image is refused.
- **Mounting help.** A person or agent writes the Mountlist from the manifest's
  image size by hand (`skills/pkg/SKILL.md`). Pkg could print the entry.
- **An upgrade that is refused at dependency level only names `UPGRADE`**; it
  does not offer to upgrade the dependency in the same run.
- **Network channels, publishing from a GitHub link, Aminet and AmigaOS
  interoperability, WHDLoad, paid packages, licences listing**: designed in
  the planning repository, none built.
- **No `[PKG*]` gate is claimed.** Goals 1 and 2 touch many gates, but none has
  its `pkg-*.json` verdict artifact; `STATUS.md` in aros-next still says spec.
