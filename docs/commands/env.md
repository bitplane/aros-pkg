<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# ENV

Roots registered under a name, so a command can leave `ROOT` out.
```
pkg ENV ADD <name> ROOT <root> [SYSTEM]
pkg ENV LIST [SYSTEM]
pkg ENV REMOVE <name> [SYSTEM]
pkg ENV DEFAULT <name> [SYSTEM]
```

## What it does

An environment is a name for a root. `ENV ADD` registers one, `ENV DEFAULT`
makes it the root a command uses when neither `ROOT` nor `ENVIRONMENT <name>`
says which, `ENV REMOVE` forgets one, and `ENV LIST`, or `ENV` alone, shows
them. The root must be an existing directory, given by its absolute path;
removing an environment leaves the root and what is installed in it as they
are.

The names are kept in a configuration file, not in the root: your own
(`~/.config/aros-pkg/environments.conf`, or `XDG_CONFIG_HOME`'s), or, with
`SYSTEM`, the machine's (`/etc/aros-pkg/environments.conf`). On AROS there is
one, `ENVARC:pkg/environments.conf`. `ROOT` on a command always wins over
every environment. [Environments and updating pkg](../environments.md) is the
guide.

## Examples

```console
$ mkdir aros
$ pkg ENV ADD test ROOT "$PWD/aros"
  environment configuration: /home/you/.config/aros-pkg/environments.conf
  this file names roots; a package's records stay in that root's .pkg drawer
environment configuration saved: /home/you/.config/aros-pkg/environments.conf
$ pkg ENV DEFAULT test
  environment configuration: /home/you/.config/aros-pkg/environments.conf
  this file names roots; a package's records stay in that root's .pkg drawer
environment configuration saved: /home/you/.config/aros-pkg/environments.conf
$ pkg LIST
Environment: test
Root: /home/you/aros
Selected from: /home/you/.config/aros-pkg/environments.conf
nothing installed in /home/you/aros
$ pkg ENV REMOVE test
  environment configuration: /home/you/.config/aros-pkg/environments.conf
  this file names roots; a package's records stay in that root's .pkg drawer
environment configuration saved: /home/you/.config/aros-pkg/environments.conf
```

## Records (`MACHINE`)

`configuration:` the file written, `result: configured`. `ENV LIST`:
`system-config:`, `user-config:`, one `environment:` record per root with its
name and where it was registered, `default-environment:`.

## Refusals

| Exit | When |
|---|---|
| 20 | a name with characters it may not have, a root that is not an absolute path to an existing directory, a name already registered or not registered, `ROOT` on another word than `ADD` |

## Related

[CHANNEL](channel.md), [Environments and updating pkg](../environments.md),
[Using pkg](../using.md).
