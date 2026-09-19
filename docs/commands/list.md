<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# LIST

What a root holds.
```
pkg LIST ROOT <root>
```

## What it does

Lists every installed package with its version, kind and number of files,
and says which were installed only as dependencies. Reads the root's
database; touches nothing.

## Examples

```console
$ pkg LIST ROOT aros
nothing installed in aros
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg LIST ROOT aros
Package     Version  Kind         Files
hellolib    1.0      library      1 file, a dependency
helloworld  1.1      application  2 files
$ pkg LIST ROOT aros MACHINE
result: listed
package: hellolib 1.0 library 1 dependency
package: helloworld 1.1 application 2 explicit
count: 2
```

## Records (`MACHINE`)

`result: listed`, one `package: name version kind files explicit|dependency`
per package, `count:`.

## Related

[SHOW](show.md) for a channel, [VERIFY](verify.md), [STATUS](status.md).
