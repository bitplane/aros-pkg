<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# RESOLVE

Which library a program would get from here, and why.
```
pkg RESOLVE <name.library|name.device> [VERSION n] [FROM <dir>] [ROOT <root>] [CHANNEL <channel>] [ARCH cpu]
pkg RESOLVE <program>                    [FROM <dir>] [ROOT <root>] [CHANNEL <channel>] [ARCH cpu]
```

## What it does

Walks the places AROS looks for a library, in the loader's order (the
current directory and its `libs/`, the program's directory and its
`libs/`, then each directory of `LIBS:`), and gives every candidate a
verdict: `taken`, `no file`, `not a library`, `wrong CPU`, `too old`, or
`shadowed` by one found earlier; on AROS, a copy already in memory comes
first and wins. Each failing row ends with what to do. `RESOLVE <program>`
does it for every library the program names, from the program's own
directory, in one table. With `CHANNEL`, a missing or too old library that
a package in the channel would provide is named. Exit 0 when the winner
satisfies, 11 when nothing resolves, 18 when the winner is too old.

The rules it applies, with the loader's source as the reference, and worked
examples: [Libraries](../libraries.md).

## Example

The example channel's `hello.library` is a stand-in with no code in it, and
RESOLVE says exactly that: the loader would pass it over.

```console
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg RESOLVE hello.library ROOT aros FROM aros/C    # exits 11
Where                File                        Version  Package       Verdict
PROGDIR:             aros/C/hello.library        -        -             no file
PROGDIR:libs/        aros/C/libs/hello.library   -        -             no file
LIBS: (SYS:Libs)     aros/Libs/hello.library     1.0      hellolib 1.0  not a program file: LoadSeg fails, passed over
LIBS: (SYS:Classes)  aros/Classes/hello.library  -        -             no file
  LIBS: (SYS:Libs): remove aros/Libs/hello.library: it is no library, and hides nothing now, but confuses whoever looks
  LIBS: (SYS:Classes): install hello.library into SYS:Libs
pkg resolve: hello.library is found nowhere: none of the 4 places the loader looks holds a library it can use
  next: check the name; pkg SHOW CHANNEL <dir> lists what a channel offers, pkg LIST ROOT <dir> what a root holds
```

Real libraries, a shadowing private copy and a loaded copy are walked
through in [Libraries](../libraries.md#resolve).

## Records (`MACHINE`)

`candidate: path exists version package chosen` per place, each followed by
`verdict:` and `next-step:`; `loaded:` on AROS; `winner:`, `satisfies:`,
`summary:`; `program:` and `library:` lines in program mode.

## Related

[Libraries](../libraries.md), [PUBLISH](publish.md) for `Provides` and the
unprovided-library warning, [INSTALL](install.md).
