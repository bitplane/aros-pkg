<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Removing pkg

Run this to remove the pkg executable you launched:

```text
pkg REMOVE
```

Use `pkg REMOVE DRYRUN` to preview the removal. pkg reports the executable
path before acting. A managed installation also removes its own package
record. Other installed programs, their records and environment configuration
remain available when you reinstall pkg from [Downloads](https://aros-pkg.azurewebsites.net/downloads).

On Windows, pkg moves its running executable out of the installed path. It
schedules deletion at reboot when permitted, or prints the remaining file to
delete after the command exits.

For a normal update, use `pkg UPGRADE` or `pkg U`. Reinstallation is available
when you need a fresh executable.

The commands below describe explicit package removal and optional cleanup.
On a host, `aros` names the AROS root directory; on AROS the examples use `SYS:`.

## On AROS

### Remove pkg, keep what it installed

```amigados
Pkg REMOVE pkg ROOT SYS:
```

`C:Pkg` is deleted and the `pkg` record leaves the database. pkg removes its
own file while it is running: on AROS that is allowed, and the command ends
with `$RC` 0. Everything else stays: the packages you installed, their files
and their records in `SYS:.pkg`. Install pkg again later, with
`Install-Pkg`, and `Pkg LIST ROOT SYS:` names them all again.

### Remove every trace

```amigados
Pkg REMOVE pkg ROOT SYS:
Delete SYS:.pkg ALL FORCE
```

`SYS:.pkg` holds the database, the pinned keys, the staging area and the
download cache (`SYS:.pkg/cache`), so the one `Delete` takes all of it. Two
places lie outside it: `RAM:pkg-cache`, used when the system volume cannot be
written, and whatever `PKG_CACHE` names when it is set.

What this leaves is the software itself. Every file pkg ever placed stays
where it is and keeps working; what is gone is the record of which package
put it there, which version it was and what it should look like. Nothing is
deleted from the packages.

### Remove the software first

If the system should go back to what it was before pkg, take the packages out
before pkg itself, most recent first, and then the libraries nothing needs any
more:

```amigados
Pkg REMOVE helloworld ROOT SYS:
Pkg REMOVE ORPHANS ROOT SYS:
Pkg LIST ROOT SYS:
Pkg REMOVE pkg ROOT SYS:
Delete SYS:.pkg ALL FORCE
```

There is no `REMOVE ALL`: removing software is one decision per package.
`ALL` is refused as a word `REMOVE` does not take, before anything is looked
at:

```console
$ pkg REMOVE ALL ROOT aros    # exits 20
pkg remove: ALL is not a word REMOVE takes; it takes ORPHANS, ROOT, DRYRUN
  next: fix the command; pkg HELP lists the verbs and keywords
```

`Pkg LIST ROOT SYS:` after the orphans have gone says what is still there;
repeat `REMOVE` until only `pkg` is left.

A file you changed since it was installed is kept, and pkg says so
([REMOVE](commands/remove.md)); delete those by hand if you want them gone.

## Moving to a pkg that cannot read this one

A new pkg that changed its database format, or that is signed by another key,
needs the old state out of the way. The software on disk does not: the new
pkg takes it over again, file by file, without downloading or rewriting
anything.

**Keep** everything the packages put on the system. **Delete** `SYS:.pkg`,
the state the old pkg wrote. Then install the new pkg from its drawer and
install the packages again: each file already in place and identical to the
package's is *adopted*, so nothing is fetched twice and nothing you are using
is overwritten.

```amigados
Pkg REMOVE pkg ROOT SYS:
Delete SYS:.pkg ALL FORCE
Execute Work:pkg-x86_64/Install-Pkg Work:pkg-x86_64
Pkg INSTALL helloworld ROOT SYS: CHANNEL Work:channel
```

Here is the same on a Mac or a PC, with the state deleted and the files left
alone:

```console
$ pkg INSTALL pkg ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/pkg ARCH aarch64
installed pkg 1.7.0 into aros: 1 file, payload 54abf82ec01e, signed by 5ff18d3fe14e383e
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 2 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg REMOVE pkg ROOT aros
removed pkg 1.7.0 from aros: 1 file removed
  hint: pkg is gone, but aros/.pkg still holds what is installed, the keys pinned for it and the downloads; a later pkg takes over from there, and the guide to removing pkg says what to delete when nothing should stay
$ rm -rf aros/.pkg
$ pkg INSTALL pkg ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/pkg ARCH aarch64
installed pkg 1.7.0 into aros: 1 file, payload 54abf82ec01e, signed by 5ff18d3fe14e383e
$ pkg INSTALL helloworld ROOT aros CHANNEL channel
  adopted  1 file already there, identical to hellolib 1.0's
  adopted  2 files already there, identical to helloworld 1.1's
  added    hellolib 1.0, a dependency
installed helloworld 1.1 into aros: 0 files, payload 0542ac0ba511, signed by 5ff18d3fe14e383e
$ pkg VERIFY ALL ROOT aros
Package     Version  Files    State
hellolib    1.0      1 file   intact
helloworld  1.1      2 files  intact
pkg         1.7.0    1 file   intact
3 packages, 4 files, all intact
```

`adopted` is the point: the files were already right, so the second install
placed none of them and the new database describes what is on the disk.
[Moving to pkg](migrating.md) explains adoption at length.

### When the new pkg is signed by another key

The key that signed a package is pinned when it is first installed, and the
pin outlives the package: `REMOVE` deletes the files and the record, not the
pin, because a pin is what stops someone else's build from arriving under a
name you trust. So a pkg signed by a new key is refused, even after the old
pkg has been removed:

```console
$ pkg REMOVE pkg ROOT aros
removed pkg 1.7.0 from aros: 1 file removed
  hint: pkg is gone, but aros/.pkg still holds what is installed, the keys pinned for it and the downloads; a later pkg takes over from there, and the guide to removing pkg says what to delete when nothing should stay
$ pkg INSTALL pkg ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/pkg-next    # exits 14
pkg install: pkg is signed by a different key from the one pinned in aros.
pkg install:   pinned 5ff18d3fe14e383e95e905adb17a66dc17864a4588ac585ff550d1436399e111
pkg install:   signer 4f6b8dcf459b471d2a31847aef8967782200a8187fde3e6da5d31f628996f650
pkg install: Nothing was changed. Either the publisher changed keys or someone else signed this; only whoever requested this can tell, by asking the publisher by another route than this channel. If they confirm the new key, it is accepted with ACCEPTKEY and the key in full
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
```

There are two ways past it, and both are your decision, not a step to take
lightly. Ask the publisher by some route other than the channel itself
whether the key really changed.

- **Accept the new key by name.** Add `ACCEPTKEY` and the new key in full, as
  the refusal prints it; pkg says what changed and pins the new one. This is
  the one to use when you are keeping the rest of the database.
- **Delete the pin.** When `.pkg` goes altogether, the pin goes with it and
  the new key is pinned as a first install would pin it. To drop only this
  one, delete `SYS:.pkg/keys/pkg` (`aros/.pkg/keys/pkg` on a host):

```console
$ rm aros/.pkg/keys/pkg
$ pkg INSTALL pkg ROOT aros CHANNEL https://aros-pkg.azurewebsites.net/pkg-next
installed pkg 2.0 into aros: 1 file, payload 4917add02b19, signed by 4f6b8dcf459b471d
```

## On macOS, Linux and Windows

There, pkg is one program on your `PATH`, put there by the installer from the
downloads page. Undoing it means the program, the `PATH` line and the cache.

**macOS and Linux.** `pkg` is in `/usr/local/bin`, in `~/.local/bin`, or
wherever `PKG_INSTALL_DIR` named; `command -v pkg` says which. The installer
adds one line to your shell's start-up file only when that directory was not
already on your `PATH`, and it says so when it does.

```sh
sudo rm -f "$(command -v pkg)"           # or rm -f, for ~/.local/bin
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/pkg"
rm -rf "$HOME/AROS/Shared/Pkg-aarch64" "$HOME/AROS/Shared/Pkg-x86_64"
```

Then delete the `PATH` line the installer added, if it added one: it sits
under the comment `# pkg, the AROS package tool` in `~/.zshrc`,
`~/.bash_profile`, `~/.bashrc`, `~/.profile` or
`~/.config/fish/config.fish`. Open a new terminal afterwards.

The third line is for a hosted AROS on the same computer: the installer also
puts the AROS drawer in the folder AROS sees as a volume, and that drawer is
only a copy of the download. Deleting it removes nothing from AROS itself;
the AROS system is removed with the AmigaDOS lines above.

**Windows.** In PowerShell:

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\pkg"
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\pkg-cache"
$p = [Environment]::GetEnvironmentVariable('Path','User') -split ';' |
    Where-Object { $_ -ne "$env:LOCALAPPDATA\Programs\pkg" -and $_ -ne '' }
[Environment]::SetEnvironmentVariable('Path', ($p -join ';'), 'User')
```

The installer writes nothing else: no service, no registry key of its own, no
file outside those two folders and that one `Path` entry.

## What is left

After `REMOVE pkg`, on the root it managed:

| Path | What it holds | Safe to delete |
|---|---|---|
| `SYS:.pkg/db` | one record per installed package: version, files, digests | yes, and a later pkg then knows nothing about them |
| `SYS:.pkg/keys` | the key pinned for each package, `pkg` among them | yes, and the next key seen is pinned instead |
| `SYS:.pkg/auto` | which packages came in as dependencies | yes; without it, `REMOVE ORPHANS` finds nothing |
| `SYS:.pkg/arch` | the CPU this root is for | yes; name it again with `ARCH` |
| `SYS:.pkg/staging` | space for a download being checked; empty between commands | yes |
| `SYS:.pkg/cache` | packages already downloaded | yes; they are fetched again when needed |
| `RAM:pkg-cache` | the same, when the system volume is read-only | yes |
| the installed files | the software itself, in `C:`, `Libs:` and the rest | only if you want that software gone; `REMOVE <name>` does it properly |

On a Mac or a PC the cache is `~/.cache/pkg` (`$XDG_CACHE_HOME/pkg` when that
is set, `%LOCALAPPDATA%\pkg-cache` on Windows, `PKG_CACHE` over all of them),
and the roots you manage are directories you made yourself.

## Related

[Using pkg](using.md), [REMOVE](commands/remove.md),
[INSTALL](commands/install.md), [Moving to pkg](migrating.md),
[Signatures and trust](signing.md).
