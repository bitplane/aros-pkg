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

| Piece | State |
|---|---|
| `.pkg` container, reader and writer | Built, with its test |
| Manifest | Not started |
| Digests (SHA-256) and signatures (Ed25519) | Not started |
| Channel index, resolution, install engine | Not started |

## Build and test

```sh
make test
```

`cc -std=c99 -Wall -Wextra -Werror`. `make test` removes the binary before
rebuilding, deliberately: see the comment in the `Makefile`.

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
