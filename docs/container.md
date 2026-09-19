<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# The .pkg container

The byte layout of a package, its byte-order discipline and how both are
proven. Normative text: what a reader and a writer must do.

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
