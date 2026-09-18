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

Goal and milestones: [GOAL.md](GOAL.md). **M1, the macOS loop, is done.**

| Piece | State |
|---|---|
| `.pkg` container, reader and writer | Built, with its test |
| Byte-order discipline and its checks | Built |
| SHA-256 | Built, checked against the NIST vectors |
| Text manifest | Built, strict parser, with its test |
| Host filesystem layer | POSIX (macOS, Linux). The AROS layer comes with M3 |
| `MANIFEST`, `PUBLISH`, `INSTALL ROOT`, `LIST`, `VERIFY`, `REMOVE` | Built, end-to-end test on macOS |
| Ed25519 signatures, key pinning | M2 |
| `UPGRADE`, `ROLLBACK`, version selection beyond highest and exact | M2 |
| AROS client | M3 |
| ARexx port | M4 |

## Use, on macOS

```sh
make
./build/pkg PUBLISH ~/dev/MyTool CHANNEL ~/pkg-channel
./build/pkg INSTALL mytool ROOT ~/aros-root CHANNEL ~/pkg-channel
./build/pkg LIST ROOT ~/aros-root
./build/pkg VERIFY mytool ROOT ~/aros-root
./build/pkg REMOVE mytool ROOT ~/aros-root
```

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
