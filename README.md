<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Pkg

The AROS package tool. Design and gate ladder live in the planning repository,
at `docs/features/packaging/`: `README.md` for the feature and its five `[PKG0]`
decisions, `tool.md` for this tool, `spec.md` for `[PKG0]` through `[PKG24]`.

Portable C99, no dependencies. One codebase in three roles: a **client** that
resolves, installs, verifies and rolls back; a **publisher** that derives a
manifest, signs it and pushes it to a channel; and a **repository** that is a
directory of signed index snapshots and content-addressed objects.

## State

Goals and milestones: [GOAL.md](GOAL.md). What remains: [OPEN.md](OPEN.md). **Goal 1 is met**: all four milestones, and the whole sequence passes as one run, `tests/goal.sh`, 25 checks. **Goal 2 is met** within its agreed line: an application arrives with its dependencies and runs, checked from outside, with no ARexx anywhere (`tests/goal2.sh`, 51 checks), the contract is identical on AROS, macOS and through the ARexx port (`tests/aros-contract.sh`, 152 checks), and the Windows, macOS and Linux kit is ready; its Windows run is the owner's.

| Piece | State |
|---|---|
| `.pkg` container, reader and writer | Built, with its test |
| Byte-order discipline and its checks | Built |
| SHA-256 | Built, checked against the NIST vectors |
| Text manifest | Built, strict parser, with its test |
| Host filesystem layer | POSIX, used on macOS, Linux and AROS through its posixc library |
| `MANIFEST`, `PUBLISH`, `INSTALL ROOT`, `LIST`, `VERIFY`, `REMOVE` | Built, end-to-end test on macOS |
| SHA-512 and Ed25519 | Built, checked against FIPS and the RFC 8032 vectors |
| `KEYGEN`, `SIGN`, signed `PUBLISH`, key pinned per package in the root | Built, end-to-end test |
| `UPGRADE`, `ROLLBACK`, `DOWNGRADE`, `EXACT` and `COMPATIBLE` selection | Built, end-to-end test |
| AROS client | Built with `tools/build-aros.sh`; `make check-aros` and `tests/aros-handler.sh` on hosted AROS |
| ARexx port `PKG` | Built: `Pkg PORT` on AROS, every verb, RESULT on success, RC the class code and `LASTERROR` on refusal |
| Machine contract, `MACHINE` or `PKG_OUTPUT=machine` | Built: `tests/e2e.sh` on macOS, `tests/aros-contract.sh` compares macOS and hosted AROS line for line |
| Image route, `KIND image` and `IMAGE` | Built: FFS images, validated by amitools in `tests/image.sh`, mounted by the AROS FFS handler in `tests/goal2.sh` |
| `Depends`, resolution, orphans, `REMOVE ORPHANS` | Built: `tests/deps.sh` on macOS, `tests/goal2.sh` on hosted AROS |

## On hosted AROS

`sh tools/build-aros.sh` cross-builds `Pkg` for aarch64 AROS, and `PkgHandlerRev`,
a test probe that asks a mounted AFS+ volume which handler revision serves it.

`make check-aros` boots hosted AROS once: a package published and signed on
macOS is installed through the `MacRW:` share, verified, listed, removed, and a
tampered payload is refused with AmigaDOS seeing the error.

`tests/aros-handler.sh` is M3. It installs the real AFS+ handler with Pkg, over
four boots, so that a restart is a restart:

| Boot | What happens | What the handler itself reports |
|---|---|---|
| 1 | Install revision 14, mount; upgrade to 15 while mounted | 14, then still 14: the loaded handler runs on until restart |
| 2 | Restart | 15 |
| 3 | Upgrade to a 16 that cannot load; it fails to start, and the startup sequence runs `Pkg ROLLBACK` | the start fails, AmigaDOS sees it |
| 4 | Restart after the fallback | 15 |

It needs two handler packages built by AFS+'s own `tools/package-aros-alpha0.sh`,
at interface revisions 14 and 15:

```sh
PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/aros-handler.sh
```

Four things the hosted runs established, none of them guessed beforehand:

- **Output.** posixc's `stdout` reached nothing a shell redirection could see;
  output goes through `dos.library` to `Output()`, refusals included.
- **Exit codes.** A refusal is `RETURN_ERROR` (10), since `If ERROR` tests for
  10 and a POSIX 1 would pass as success.
- **errno.** posixc reports no `EEXIST` for an existing directory and no `ENOENT`
  from `opendir` on an absent one; existence is tested, never inferred.
- **Formats.** AFS+ changes its on-disk format without keeping legacy readers,
  so a revision 14 handler refuses an image written by revision 15 tools. For
  the tool this is the concrete case behind `[PKG22]` item 5: rolling a handler
  back across a format change leaves volumes unreadable, so a package that owns
  an on-disk format has to declare whether its state survives a downgrade, and
  ROLLBACK has to honour that declaration. Not built yet; recorded as the next
  piece of the version model.

## As a library

`include/pkg.h` is the interface; `src/pkg_lib.c` holds every operation;
`src/pkg_main.c`, the `pkg` command, only turns words into `pkg_options` and
prints what comes back. A graphical front end, an installer or an agent's tool
adapter links the library (`make build/libpkg.a`) and calls `pkg_install`,
`pkg_show` and the rest with a sink: structured, it receives the same fields
the command line prints with `MACHINE`; otherwise sentences for a person; and,
if it asks, a trace of every step. The library writes nothing to stdout and
reads no environment. `tests/test_api.c` drives it as a front end would.

## Examples

`examples/basic.c` is the shortest useful program on libpkg: a key, a
publish, an install, a listing, and a refusal read with its `next`.
`examples/browse.c` is a package browser in the shape a graphical front end
takes: items with their fields apart, whether each version is installed,
suggestions for a mistyped name, a Cancel button, the trace. It was first
written by an agent from `pkg.h` alone, as a test of that header, then
adopted. Both build against `build/libpkg.a` and run in `make test`
(`tests/examples.sh`).

## Diagnosing

`TRACE <file>` on any command, or `PKG_TRACE=<file>` (`-` for stderr), writes
the operation's own account: every file read, written, moved or deleted,
every check with what was expected and found, every choice with its reason,
and each refusal as it happens. It never mixes into stdout.

## For agents

`skills/pkg/SKILL.md` is written for the agent that drives Pkg for a person:
the contract, publishing, installing, mounting an image, AmigaDOS scripts,
and the rules an agent must keep (never accept a new key or downgrade on its
own, never work around codes 12 to 14).

## The contract an agent reads

The same on every host, and the same for a shell script, an AmigaDOS script,
an agent or the ARexx port. No verb needs ARexx.

The exit code names the class of a refusal. On AROS every refusal is at least
10, so `If ERROR` catches all of them, and `$RC` holds the exact number.

| Code | Class | Examples |
|---|---|---|
| 0 | ok | |
| 10 | refused | a refusal that fits no class below |
| 11 | not-found | no such package, no such version in the channel |
| 12 | integrity | a payload or file that does not match its digest, an unsafe path |
| 13 | signature | unsigned, or a signature that does not verify |
| 14 | key | signed by another key than the one pinned |
| 15 | conflict | already installed, a file already present, an edited file an upgrade would replace |
| 16 | dependency | reserved for dependency resolution |
| 17 | io | the filesystem refused |
| 18 | policy | a downgrade without `DOWNGRADE` |
| 20 | usage | an unknown verb, a missing keyword or value |

With `MACHINE` on the line, or `PKG_OUTPUT=machine` in the environment, stdout
carries only `key: value` lines, the manifest's syntax, and stderr stays empty.
Every answer has a `result:` line: `installed`, `upgraded`, `downgraded`,
`rolled-back`, `unchanged`, `listed`, `intact`, `damaged`, `removed`,
`published`, `created`, `signed` or `refused`. A refusal reads:

```
result: refused
class: key
code: 14
reason: hello is signed by a different key than the one pinned ...
```

`tests/aros-contract.sh` runs one sequence, with every class among its
refusals, on macOS and then from the AmigaDOS startup of hosted AROS against
the same channel. It checks each code both ways, `$RC` on AROS, and finds the
outputs identical line for line once the root and channel paths are
normalised. In the same boot Pkg installs Regina from a second channel and
the sequence runs a third time through the `PKG` port: the same RC and the
same records. 152 checks. A deliberately altered line shows the comparison can
fail.

## The goal sequence

`tests/goal.sh` runs the goal as one sequence that passes or fails:

1. On macOS, the AFS+ handler at interface revisions 14 and 15 is published into
   a directory channel, signed with a development key. An attacker publishes a
   16 into the same channel with another key, and a copy of the channel has one
   payload byte flipped.
2. On hosted AROS, one boot: `Pkg` bootstraps from a plain archive with no
   package manager present, installs itself as a signed package, and serves
   the `PKG` ARexx port from that managed copy.
3. One ARexx script, `tests/goal.rexx`, drives install 14, verify, upgrade 15,
   verify, rollback 14 through the port, checking RC and the database at every
   step, and exits 10 at the first disagreement.
4. Inside that script the tampered payload and the substituted key are refused
   with RC 12 and RC 14, their class codes, each for its own reason, read back
   with `LASTERROR`.

Afterwards AROS mounts the volume with the handler Pkg left in place and it
reports revision 14. A second boot runs the same script with one expectation
sabotaged, and must see it stop at that line with an error reaching AmigaDOS:
a sequence that cannot fail proves nothing.

What it took, beyond the tool:

- **An ARexx interpreter.** Hosted AROS ships `rexxsyslib.library` and no
  interpreter. `tools/build-aros-regina.sh` cross-builds Regina's static `rexx`
  from the AROS contrib sources, read-only. Regina resolves `ADDRESS <name>` to a
  public port and sends it `RXCOMM` messages, so no RexxMast is needed.
- **A Regina fix.** For an ARexx port Regina set RC to the command's RESULT
  string instead of the host's numeric `rm_Result1`, so every successful
  command read as a failure. `tools/aros/regina-arexx-rc.patch` gives RC the
  number and RESULT the string, as ARexx specifies; it is applied to a copy at
  build time and kept as a file to offer upstream.
- **A different bootstrap.** AROS's own `C:Unpack` reads the `.pkg` container
  this tool adopted and was the first choice. On hosted aarch64 AROS it does not
  load: the shell answers "file is not executable" for the shipped binary and
  for one rebuilt from its sources with `tools/build-aros-unpack.sh`. The
  bootstrap uses the `minigzip` AROS ships instead: `Pkg` is one file, so that
  file compressed is its plain archive. The `Unpack` defect is AROS's; see
  "AROS defects found along the way".

```sh
sh tools/build-aros.sh && sh tools/build-aros-regina.sh
PKG_HANDLER_V14=<dir> PKG_HANDLER_V15=<dir> sh tests/goal.sh
```

## Goal 2: an application and its dependency, with no ARexx

`tests/goal2.sh`, 51 checks, one boot, driven by the AmigaDOS startup and
nothing else. Hosted AROS installs no ARexx interpreter, and the test checks
that none is present and that the script names none.

1. On macOS, Guru, the alert decoder that ships with identify.library in the
   AROS sources, is published as a signed image at 2.0 and 2.1 (2.1 adds the
   `Function` tool), depending on `identify >= 37.1`, published as a library
   component. The AROS FFS handler is published as a device component, since
   the hosted build ships none. Every version is the one in the binary's
   `$VER`.
2. On hosted AROS: `Pkg` bootstraps and installs itself; installs the FFS
   handler; installs Guru 2.0, which brings identify in first. The image is
   write-protected, mounted through `fdsk.device`, and Guru runs from it. A
   control run first, before the root's `Libs` joins `LIBS:`, must fail with
   "Could not open version 37 or higher of library identify.library" and RC 20.
   Then upgrade to 2.1, rollback to 2.0, each image ejected, replaced and
   mounted again, Guru run and the volume listed each time.
3. `VERIFY` finds the image intact after three mounts: nothing wrote to it.
4. A tampered image is refused with 12, a badly signed dependency with 13 and
   nothing placed. `Pkg` removes itself and Guru still runs. Last, a
   bootstrapped `Pkg` refuses to remove identify while Guru needs it (16),
   removes Guru, reports identify as an orphan, and `REMOVE ORPHANS` takes it
   out; the FFS handler, installed by name, stays.

Guru's output is compared on the host with the strings in its own sources:
`exec/alerts.h`, the two catalogue descriptions and the table in `idalert.c`.

```sh
make && sh tools/build-aros.sh && sh tools/build-aros-extras.sh
sh tests/goal2.sh
```

### The image route

`KIND image` turns the drawer into a Fast File System volume, `DOS\3`, written
once in memory by `src/pkg_image.c`, and the package holds that one file,
`<name>.hdf`. `pkg IMAGE <drawer> OUT <file>` writes the same file without a
channel. Geometry is fixed (512-byte blocks, 32 per track, two reserved), so
the size in the signed manifest gives the mount entry. The same drawer always
gives the same bytes.

FFS rather than AFS+, which settles one of the open questions in the planning
repository's packaging README. An application image is read-only, written
once, and has to outlive handler revisions; AFS+ changes its on-disk format
without keeping legacy readers, and a revision 14 handler already refuses a
revision 15 image. Every AmigaOS, AROS and MorphOS FFS reads a `DOS\3`
volume, and UAE mounts it as a hardfile. Block compression, the other open
question, is not done: the image is stored whole.

The writer is judged by readers written apart from it: amitools validates and
unpacks every image in `tests/image.sh`, and the AROS FFS handler mounts them
in `tests/goal2.sh`.

### Dependencies

`Depends: <name>` or `Depends: <name> >= <version>` in the manifest, set with
`DEPENDS "a >= 1.0, b"` at publish. An install, upgrade or rollback plans the
whole graph first, fetching and verifying every package, and places nothing
until all of it is settled; then dependencies go in before what needs them.
A failure while placing takes the new dependencies back out. Refused with 16,
nothing applied: a dependency the channel lacks, a version it cannot meet, an
installed version too old (the refusal names `UPGRADE`), and a cycle, named
with its path. A package installed as a dependency is marked in `.pkg/auto`;
`REMOVE` refuses while something needs a package and names what, reports what
it leaves orphaned, and `REMOVE ORPHANS` takes those out, repeating until none
is left. `tests/deps.sh`, 44 checks.

## Native AROS in QEMU

`sh tools/build-aros-x86_64.sh` builds Pkg for native AROS on x86_64 with the
Homebrew LLVM (which knows the `x86_64-unknown-aros` triple but predefines none
of the AROS macros, so the script does) against the SDK of a nightly
linux-x86_64 system, linking with a `collect-aros` it builds for x86_64 from the
AROS sources.

`tests/native-x86_64.sh` runs goal 2 on an unmodified nightly pc-x86_64 AROS in
QEMU, nothing hosted and no host share: Guru, Function and identify.library are
taken out of the ISO itself, published on the host (the architecture read from
their ELF headers: x86_64), and the ISO is rebuilt without them, with the
channel, Pkg and the sequence in `S:User-Startup`. From its own startup AROS
installs Guru with its dependency, runs Guru from the image mounted through Pkg
MOUNTLIST with the system's own FFS (no handler component: the native system
has one), upgrades, rolls back, verifies, refuses to remove identify while Guru
needs it, and removes Guru and the orphan. Results leave through the second
serial port. 27 checks; a control shows Guru cannot open identify.library
before the root's libraries are reachable.

## Windows, macOS and Linux

`make build/pkg.exe` cross-builds Pkg for x86_64 Windows with mingw-w64;
`src/pkg_fs_win32.c` is the host layer. Paths stay UTF-8 inside Pkg and go
through the wide API, so names outside the ANSI code page work, and the
command line is read back as UTF-16 for the same reason. Replacement is
`MoveFileExW` with write-through, a signing key is created with a DACL for
its owner alone from the first instant, randomness comes from
`BCryptGenRandom`, and output is in binary mode so a newline stays one byte.
`make build/pkg-macos` builds a universal binary, and
`make build/pkg-linux-x86_64` or `-aarch64` a static Linux one with zig.

`sh tools/make-test-kit.sh` writes `build/pkg-test-kit.zip`: Pkg for the
four targets, the contract channel, `tests/contract-steps.txt` (the sequence
hosted AROS runs too), the reference answers, and `run.ps1` for PowerShell
5.1 on Windows or PowerShell 7 anywhere. It compares every exit code and
machine output byte for byte, then checks the key's permissions (ACL on
Windows, mode 0600 elsewhere), a root named outside ASCII, and an image
written on the host against the reference bytes, and writes `report.txt`.
Run here with PowerShell 7 on macOS: 87 checks, 0 failures, and one altered
expectation is caught.

Not run yet: Windows and Linux. No machine of either here, and Wine's
Homebrew casks were withdrawn on 2026-09-01. The owner runs the kit.

## AROS defects found along the way

Each observed on hosted aarch64 AROS built from `jonx/AROS`, branch
`aarch64-darwin-graft`. Two are fixed and proposed upstream, at the owner's
request (2026-09-18); the others are not reported.

| Where | What happens | Seen in | Worked around by |
|---|---|---|---|
| `C:Unpack` | Does not load: "file is not executable", for the shipped binary and for one rebuilt from its sources | goal 1 bootstrap; board thread 17 | Bootstrap through `minigzip` |
| identify.library, `IdAlert` | Every dead-end CPU alert decodes as "Unknown": `idalert.c` stores `ACPU_DivZero` and its neighbours with the dead-end bit (0x80000005) and searches with that bit masked off (`id & 0x7fffffff`), so the entry never matches | goal 2, `Guru 80000005` | The test decodes a recoverable alert, 04000001. Fixed: aros-development-team/AROS#1238, checked on hosted AROS (80000005 now reads Divide by zero, 84000001 Unknown gadget type) |
| posixc `stdout` | Output written through posixc reaches no shell redirection | first AROS runs | Output goes through `dos.library` `Output()` |
| posixc `errno` | No `EEXIST` for an existing directory, no `ENOENT` from `opendir` on an absent one | first AROS runs | Existence is tested, never inferred from errno |
| Regina, aros-contrib | For an ARexx port, RC is set to the RESULT string instead of the numeric `rm_Result1` | goal 1 | `tools/aros/regina-arexx-rc.patch`; proposed as aros-development-team/contrib#64 |
| The darwin hosted build | Ships no FFS handler at all, so no FFS volume can mount | goal 2 | `tools/build-aros-extras.sh` builds `rom/filesys/afs` |
| The shell, `$RC` | A command that cannot be loaded (file not found, volume not mounted) leaves `$RC` at its previous value, 0 or 10 alike, so a script reads success after it; not yet compared with AmigaOS | goal 2, then a four-case check | Scripts check each step's output, not only `$RC` |
| dos.library, `Lock()` on a multi-directory assign | A name found only in a later directory of the assign is not found by `Lock()` (`List`), while `Open()` (`Type`) finds it. Hosted: `Assign X: SYS:Libs`, `Assign X: RAM:L2 ADD`, `Echo x >RAM:L2/f`; `List X:f` fails, `Type X:f` works | a probe while testing native AROS | Nothing in Pkg depends on it |
| Library search, native pc-x86_64 booted from CD | A library in a RAM: directory added to `LIBS:` (`Assign LIBS: RAM:sys/Libs ADD`) is not found by OpenLibrary, nor by `Version identify.library`; the same assign works on hosted AROS | `tests/native-x86_64.sh` | The run changes to the root first: the loader also searches `libs/` under the current directory |

## Use, on macOS

```sh
make
./build/pkg KEYGEN FILE ~/.pkg-dev.key          # once
export PKG_SIGNKEY=~/.pkg-dev.key
./build/pkg PUBLISH ~/dev/MyTool CHANNEL ~/pkg-channel
./build/pkg INSTALL mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg LIST ROOT ~/aros-root
./build/pkg VERIFY mytool ROOT ~/aros-root
./build/pkg UPGRADE mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg ROLLBACK mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg REMOVE mytool ROOT ~/aros-root
```

**Every package is signed; there is no development mode.** A development key
is a real Ed25519 key, written readable by its owner alone. The signature covers
the manifest, and the manifest names the payload digest, so one signature covers
every byte installed. The first install of a package into a root pins the key
that signed it, in `.pkg/keys`; a later install or upgrade signed by another key
is refused with both keys printed, and goes through only with `ACCEPTKEY`
followed by the new key in full.

`UPGRADE` with no `VERSION` takes the highest published version; with `VERSION`
it takes exactly that one. An older version needs `DOWNGRADE`. `ROLLBACK` returns
to the version installed before the last change, fetched again from the channel,
which never changes a published version. An upgrade that would overwrite a file
the user edited is refused before anything moves.

Name and version come from the `$VER:` cookie when `NAME` and `VERSION` are
not given. Keywords are case-insensitive, AmigaDOS style.

A channel is a directory: `index` holds one `name version digest` line per
published version, and `objects/` holds each payload and its manifest under the
payload's SHA-256. A root keeps its own database in `.pkg/db`, so a machine can
hold several roots without interference.

Host metadata the Amiga side has no use for, `.DS_Store` and AppleDouble `._`
files, is left out of every package and counted in the publish report. Without
that, the same drawer would give different manifests on two Macs.

## Build and test

```sh
make test
```

`cc -std=c99 -Wall -Wextra -Werror`. `make check` runs everything: the
portability grep, the unit tests, the end-to-end run, all of them again under
`-fsanitize=undefined,address`, and a big-endian compile of the portable core.
`make test` removes the binaries before rebuilding, deliberately: see the
comment in the `Makefile`.

### What the end-to-end run proves, and how it was checked

`tests/e2e.sh` publishes a drawer, installs it into a root, lists, verifies,
edits a file, verifies again, removes, and then runs the refusals: a republished
version with different bytes, a payload with one byte flipped, a second install,
an overwrite of a file already in the root, an unknown package, an absent
version, and three channel entries **forged in Python** rather than by `pkg`: a
traversal in the manifest, a container disagreeing with its manifest, and a
payload aimed at the package database.

Its oracles are independent of the code: `shasum -a 256` for every digest, `cmp`
for installed bytes, and Python for the forged entries.

The suite was run against three deliberate defects, each built separately:

| Defect | Result |
|---|---|
| Whole-payload digest check disabled | 1 check fails, the one pinning that refusal's message. The tampered payload is **still refused**, by the per-file digests, so the two layers are independent and each is exercised |
| Unsafe-path refusal disabled | 4 checks fail, including "nothing written outside the root": with the guard gone, `../evil` **was** written outside it |
| macOS metadata filter disabled | 5 checks fail, exactly the metadata ones |
| Signature verification disabled | The altered signature installs, and its check fails. The unsigned refusal still holds, correctly, since it comes from the missing file |
| Key pinning disabled | 6 checks fail, all in the substituted-key section |
| Edited-file protection disabled | 3 checks fail, exactly the edited-file ones |

A first run of that last control reported 38 failures. The mutated binary had
been built without `-Werror` and its compiler output cut off, so the 38 measured
a broken build and said nothing about the filter. Rerun with the build verified
first, it gave the 5 above. Recorded because a control that fails for the wrong
reason looks exactly like one that works.

## The container

`src/pkg_container.c` implements the byte layout AROS documents in
`tools/package/FORMAT`, read out of the AROS tree rather than inferred:

```
package     = header, file*
file        = pathLength, path, dataLength, data
header      = 'P', 'K', 'G', version, packageSize
```

Lengths are big-endian 32-bit. `packageSize` is the whole stream, header
included.

Two things worth knowing, both found by reading the AROS sources beside the
document:

**The document describes the raw stream.** `workbench/c/Unpack` reads it through
bzip2, while `arch/riscv64-opensbi/kernel/kernel_elf.c` reads it uncompressed
from memory. Compression is a layer above this one and lives elsewhere.

**This reader is stricter than AROS's own.** `PKG_ReadHeader` accepts any
version byte and ignores `packageSize`; `FORMAT` says the version must be 1.
This reader requires version 1 and requires `packageSize` to agree with the
buffer it was handed, so a truncated or padded stream is refused instead of
being walked.

## Byte order

The container is big-endian, because AROS defined it that way. The tool runs on
little-endian hosts (macOS and Linux on arm64 and x86_64, AROS on aarch64 and
i386) and on big-endian ones (AROS on m68k and ppc). So the rule:

**Byte order is expressed in `pkg_be32_get` and `pkg_be32_put`, and nowhere
else.** Both read and write one byte at a time with explicit shifts, so they
describe the *stream's* order and never ask the host what it is. They compile
to the same behaviour everywhere, and they need no conditional, no host-order
conversion macro and no byte-swap builtin.

They also place **no alignment requirement** on the pointer, and that property
is load-bearing here. On a 68000 an unaligned 32-bit access raises an address
error, so any design that maps a packed struct over a byte stream is broken on
the oldest target this program serves. Byte-wise access sidesteps it, and the
same choice removes the padding and strict-aliasing questions a struct would
bring.

Everything else the tool defines is **text**: the manifest, the channel index,
the installed-package database, the lockfile. One binary format in the whole
tool, so byte order lives in one file behind two functions. That is also what
makes the Aminet `.readme` interop cheap, since a text manifest converts to a
text header with no second representation in between.

### Why big-endian here when AFS+ is little-endian

AFS+ is the newer format and it chose little-endian for its integer fields, so
the question comes up. AFS+ is worth reading closely before it is used as an
argument, because it did not pick *an* endianness at all. It picked per field,
by what the field is for: little-endian for values, and **big-endian for tree
keys, so that byte order is numeric order** and a key comparison is a byte
comparison. Its own design review reaches the same place this file does on the
other axis, that explicit byte-based decoding is what avoids the native
alignment hazards.

So the rule generalises past both formats: **byte order is chosen by access
frequency and by purpose.** A filesystem decodes integers on every read, so
matching the host on that path saves real work. A tree key must sort the way it
counts, so it is big-endian whatever the host is. A package container decodes
two integers per entry plus a header, so its order costs nothing measurable and
buys interoperability.

And here it is not a choice at all. `.pkg` is big-endian because AROS defined it
that way, and this project adopted that format precisely because it exists, is
documented, is used at boot and needs no port. Redefining it little-endian would
discard the one thing it was chosen for, that AROS's own `Unpack` and the
riscv64 loader read what we write, in exchange for saving four shifts per entry.

The durable protection is elsewhere and is already in place: this container is
the only binary format in the whole tool. If `.pkg` is ever replaced, the blast
radius is two functions and one file. The calculus would change if something
frequently decoded ever moved inside the container, which today holds paths and
opaque blobs.

Four checks hold the rule:

| Check | What it catches |
|---|---|
| `make check-portability` | Host-order conversion macros, endianness conditionals and byte-swap builtins anywhere in the tree |
| Accessor vectors in the test | A byte-swapped implementation. The vectors are asymmetric on purpose: `1` and `0x01000000` are each other's swap, so a suite built only from palindromes would pass while swapped |
| The misaligned-buffer test | A struct mapped over the stream. It reads the same package from offsets 1, 2 and 3 |
| `make test-ubsan` | The same, loudly, under `-fsanitize=undefined,address` |
| `make check-m68k` | That the code builds for a big-endian target, using the AROS m68k cross compiler when it is present |

`make check` runs all of them.

**What is not claimed:** none of this is a big-endian *run*. `check-m68k`
compiles, and executing the suite on a big-endian target is separate work,
waiting on a target to run it on.

## How the container is proven

The oracle is the worked example inside `tools/package/FORMAT`:

```
"PKG", 1, 28L, 3L, "foo", 0, 8L, "barbarba"
```

Twenty-eight bytes, written by someone else before this code existed, so it
checks the layout instead of confirming what the writer happens to do. The test
asserts the writer emits those exact bytes and that the reader reads them back.

Eight malformed streams are each refused with their own status: bad magic,
version 0, version 2, a `packageSize` disagreeing with the buffer, a truncated
entry, a path with no terminating NUL, a zero path length, and a `dataLength`
running past the end.

The suite was checked against a deliberate defect: with `be32_put` writing
little-endian, six checks fail, including the byte comparison against the
FORMAT example. A suite that cannot fail proves nothing.
