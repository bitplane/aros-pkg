<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Environments and updating pkg

Once your environment is configured, installing a program takes one command:

```text
pkg INSTALL xinvaders3d
```

Pkg remembers the root and channels and announces the selected root before
working. The setup below is done once for each environment.

## Quick start on AROS

For your running AROS system, register `SYS:` and add the contrib channel:

```text
Pkg ENV ADD native ROOT SYS:
Pkg ENV DEFAULT native
Pkg CHANNEL ADD https://aros-pkg.azurewebsites.net/contrib-nightly
Pkg INSTALL xinvaders3d
```

If the installer registered `native`, start with the channel command. If the
channel is already listed by `Pkg CHANNEL LIST`, proceed to installation.
AROS supplies its CPU architecture automatically.

After setup, `Pkg SEARCH paint`, `Pkg UPGRADE xinvaders3d` and `Pkg LIST`
use the same environment.

## Quick start from macOS, Linux or Windows

Choose the existing directory containing the AROS system you want to manage.
For example, substitute its absolute path for `/path/to/aros` below. On Windows,
use an absolute Windows path such as `C:\AROS`.

```text
pkg ENV ADD aros ROOT /path/to/aros
pkg ENV DEFAULT aros
pkg CHANNEL ADD https://aros-pkg.azurewebsites.net/contrib-nightly
pkg INSTALL xinvaders3d ARCH x86_64
```

The first installation names the **target AROS architecture**. This example
uses `x86_64`; use the architecture of your AROS system. Pkg records it in the
root's `.pkg/arch`, so subsequent commands are short:

```text
pkg INSTALL xinvaders3d
pkg UPGRADE xinvaders3d
pkg SEARCH paint
```

An environment gives a name to a package root. Its root's `.pkg` database
records installed packages, trust and channels. The optional environment
configuration records names, root paths and a default selection. Commands with
an explicit `ROOT` work without this configuration.

## Choose a root

`ROOT <path>` always chooses the target, including when `ENVIRONMENT` is also
present. Without `ROOT`, `ENVIRONMENT <name>` selects a registered environment.
Otherwise pkg uses the configured default, or the only registered environment.
When several environments need a choice, an interactive command asks the user.
An unattended command reports the missing choice and asks for `ROOT` or
`ENVIRONMENT` on the command line.

Before operating, pkg reports the chosen root and where the choice came from.
This includes explicit command-line roots and roots selected from configuration.
Ordinary package commands do not create an environment configuration.

These are command forms; replace the names and paths with your installation:

```text
pkg ENV ADD native ROOT <absolute-root-path>
pkg ENV DEFAULT native
pkg ENV LIST
pkg UPGRADE lunapaint ENVIRONMENT native
pkg UPGRADE lunapaint ROOT <root-path>
pkg ENV REMOVE native
```

`ENV ADD` requires an existing directory and an absolute path. The directory
can be an empty package root. Removing an environment entry leaves the root
and its installed packages intact.

## Personal and machine configuration

| Platform | Personal file | Machine file |
|---|---|---|
| macOS / Linux | `$XDG_CONFIG_HOME/aros-pkg/environments.conf`, or `~/.config/aros-pkg/environments.conf` | `/etc/aros-pkg/environments.conf` |
| Windows | `%APPDATA%\aros-pkg\environments.conf` | `%PROGRAMDATA%\aros-pkg\environments.conf` |
| AROS | Uses the machine configuration | `ENVARC:pkg/environments.conf` |

Personal configuration supplies the user's default when one is set. Otherwise
the machine default applies. If the same environment name refers to different
roots in the two files, pkg asks for an explicit choice. Machine entries are shared
by users with access to their roots. Root filesystem permissions govern package
operations; recording a root grants no extra permission to modify it.

`ENV ADD`, `ENV REMOVE` and `ENV DEFAULT` write personal configuration on host
systems. Add `SYSTEM` to manage the machine file explicitly. `ENV LIST SYSTEM`
shows machine configuration. AROS uses its machine file for both forms.

The command reports the configuration file being written. Changing a machine
file requires the corresponding filesystem permissions.

## Installer setup

The macOS, Linux and Windows installers offer environment registration at an
interactive terminal. They display the configuration path and its purpose.
The suggested name is `aros`; Enter accepts each suggestion. The suggested
root is `~/AROS/System` on macOS and Linux, and `%USERPROFILE%\AROS\System`
on Windows. The AROS installer uses `SYS:` as its default installation root.

Host installers create a missing root directory. For a directory containing
files, they explain that registration preserves its contents and ask before
using it. You can choose another directory or cancel setup while keeping pkg
installed. Invalid names, paths and failed registrations allow another attempt.
The installer also offers to select the environment by default.

For unattended installation, set `PKG_ENV_NAME` and `PKG_ENV_ROOT` together to
request registration. Set `PKG_ENV_DEFAULT=1` to make that environment the
default. A nonempty root requires `PKG_ENV_REUSE=1` to confirm reuse.
`PKG_NO_ENV=1` disables the interactive offer. Without explicit
registration variables, unattended installers create no environment entry.
Installer registration writes personal configuration; use `ENV ... SYSTEM`
separately for a machine-wide entry.

The AROS `Install-Pkg` script prints the configuration location and registration
commands. Its explicit `REGISTER` switch registers the supplied root as
`native` and selects it by default:

```text
Execute Work:Pkg-x86_64/Install-Pkg Work:Pkg-x86_64 SYS: REGISTER
```

## Update the running pkg

```text
pkg upgrade
pkg u
```

Both commands update the running pkg executable. They announce the target and
verify its downloaded build against the publisher's signature before replacing
it. A shared executable serves every user who launches that path; a personal
executable serves its owner. Directory permissions apply to replacement.

To update a package inside a root, provide its name and the desired root or
environment. See [UPGRADE](commands/upgrade.md). Applications embedding libpkg
can [check their own package updates](self-update.md).
