---
name: pkg
description: Drive Pkg, the AROS package tool, on behalf of a person - publish a program or a system component into a channel, install, upgrade, roll back, verify and remove it in a root, inspect a channel, mount an application image, on macOS, Linux, Windows or AROS. Use when asked to package, ship, install, update or remove AROS software, or to explain a Pkg refusal.
---

# Pkg, for agents

Pkg moves AROS software from where it is built to where it runs, signed at
every step. Most people will never type its commands; you will. The tool is
built so that the safe path is the easy one: every refusal tells you what to
do next, every changing command can be tried first with `DRYRUN`, and nothing
it prints hands you a command that overrides a safeguard. Follow what it says.

## Getting the tool

`pkg` is one executable (`Pkg` on AROS). If it is not on `PATH`, build it
from its repository with `make` (`build/pkg`); for AROS, `sh
tools/build-aros.sh` (`build/aros/Pkg`). A `build/pkg` older than the
sources is rebuilt by `make`. `pkg HELP` lists every verb and keyword.
Programs that link Pkg instead of running it use `include/pkg.h`.

## The three habits

1. **Add `MACHINE`** to every command. stdout then holds only `key: value`
   lines, with a `result:` field in every answer; stderr stays empty.
2. **Read the exit code straight away** (on AROS, `$RC`): 0 is done, 10 to 18
   a refusal whose number is its class, 20 a wrong command. `$?` after a pipe
   is the pipe's.
3. **On a refusal, do what `next:` says.**

| `next:` | What you do |
|---|---|
| `stop` | Stop. Report the `reason:` to the person as it is. The bytes or signatures are not what was published; no keyword, copy or other channel makes that safe |
| `ask-person` | Show the person the `reason:` and wait. It is their decision: a new publisher key, going back a version, a file they edited, removing something others need |
| `fix-command` | Your command is wrong; `pkg HELP` has the spelling. Keywords have no dashes: `ROOT <dir>`, not `--root` |
| `check-name` | The name is not there. The `reason:` suggests near names; `pkg SHOW CHANNEL <dir>` lists a channel, `pkg LIST ROOT <dir>` a root |
| `use-upgrade` | Another version is installed; `UPGRADE` moves it, if that is what the person asked for |
| `use-install` | It is not installed; `INSTALL` it, if that is what the person asked for |
| `report` | Report the `reason:` |

A refusal reads `result: refused`, `class:`, `code:`, `reason:`, `next:`. A
key refusal also gives `pinned:` and `signer:`: show both to the person. If
they confirm with the publisher, by another route than the channel, that the
new key is theirs, repeat the command with `ACCEPTKEY <signer, in full>`.
Never before.

When something behaves unexpectedly, add `TRACE <file>` (or set
`PKG_TRACE=<file>`, `-` for stderr): the file then tells every step, every
file touched, every check with what was expected and found, and every
choice with its reason. Read it before guessing.

## Words

- **Drawer**: a directory laid out as it will be installed: `Libs/foo.library`
  for a library, `L/foo-handler` for a handler, `C/Tool` and `Tool.info` for
  a program. Flat files are laid out into these first.
- **Channel**: a directory with `index` and `objects/`; published into,
  installed from. Copy, sync or serve it as it is.
- **Root**: where packages are installed; its database is `<root>/.pkg/`.
- **Key**: the publisher's Ed25519 key file, readable by its owner alone.
- **Package name**: lower case, `a-z 0-9 + . _ -`. Read from a `$VER`
  cookie it is lower-cased (`Guru` becomes `guru`, `identify.library`
  stays). Other commands refer to it by exactly that name.

**Two routes.** A system component (library, handler, class, font) is
published with its kind and installed into its fixed place. An application
is published with `KIND image`: it travels as one read-only volume, mounted
to run, and names the components it needs with `DEPENDS`.

## Rules

1. **Publishing is permanent.** A channel has no unpublish. Run the command
   with `DRYRUN` first and read `name:`, `version:`, `kind:`,
   `architecture:`, `depends:` and `file:`; when unsure, publish to a
   scratch channel first.
2. **Never pass `ACCEPTKEY` or `DOWNGRADE` on your own.** Pkg never suggests
   them; only the person's answer does.
3. **Never touch `.pkg/` in a root, or `objects/` in a channel, by hand.**
   After everything is removed a root still holds the keys pinned for each
   package: that is on purpose.
4. **`REMOVE ORPHANS` removes packages.** Run it when the person asked to
   clean up what is left; otherwise show them the `orphan:` lines `REMOVE`
   printed.
5. **One key per publisher, kept.** Every later version must be signed with
   the same key, or every machine that installed the earlier one refuses it
   with 14. Keep it where the person keeps secrets; never print, copy or
   commit it. The `public:` line is what may be shared.
6. **Check the result, not only the code.** On AROS a command that cannot
   even load leaves `$RC` as it was. A step has succeeded when its output
   has the `result:` you expect.

## Publishing

```sh
pkg KEYGEN FILE <keyfile> MACHINE          # once per publisher; never overwrites
export PKG_SIGNKEY=<keyfile>
pkg PUBLISH <drawer> CHANNEL <channel> KIND library DRYRUN MACHINE
pkg PUBLISH <drawer> CHANNEL <channel> KIND library MACHINE
```

Name and version come from the drawer's `$VER` cookie when it has one; Pkg
refuses with 20, listing them, when cookies name different programs. Then
pass `NAME`, and `VERSION` follows from that program's cookie; if the only
cookie has another name (`afs.handler` packaged as `afs-handler`), its
version still counts. The architecture comes from the executables' own
headers (`aarch64`, `x86_64`, `m68k`, ...), `generic` when there are none;
mixed CPUs are refused. Host metadata is left out and each file named in a
`left-out:` line: names starting with `.` (`.DS_Store`, `.git`,
`.backdrop`), `Icon\r`, `Thumbs.db`, `desktop.ini`. `Name.info` icons stay.

An application:

```sh
pkg PUBLISH <drawer> CHANNEL <channel> NAME guru VERSION 2.0 KIND image \
            DEPENDS "identify.library >= 37.1" DRYRUN MACHINE
```

The volume's top level is the drawer, laid out as the program expects to
find itself when mounted; its own helpers, icons and documents go inside,
and only what other programs share goes into `DEPENDS`, by package name.
On macOS the filesystem ignores case: never create `GURU` beside `Guru`.

## Installing and changing

```sh
pkg INSTALL  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg UPGRADE  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg ROLLBACK <name> ROOT <root> CHANNEL <channel> MACHINE
pkg VERIFY   <name> ROOT <root> MACHINE
pkg LIST     ROOT <root> MACHINE
pkg REMOVE   <name> ROOT <root> MACHINE
pkg REMOVE   ORPHANS ROOT <root> MACHINE
pkg SHOW     [<name>] CHANNEL <channel> MACHINE
```

Every one that changes something takes `DRYRUN`. Without `VERSION`, the
highest version. INSTALL settles every dependency before placing a file
(`dependency:` lines) and places nothing if anything is refused; installing
what is already installed answers `unchanged` and succeeds, so a retry is
safe. SHOW checks every entry of a channel as INSTALL would, without
installing: kind, architecture, signer, dependencies, and exit 12 or 13 if
any is damaged. Use it before trusting a channel, mirror or copy.

| Verb | `result:` on success |
|---|---|
| KEYGEN | `created` |
| MANIFEST | `shown`, then the manifest's own fields |
| PUBLISH | `published`, or `unchanged` for the exact version already there |
| INSTALL | `installed`, `unchanged`, or `kept` (a dependency now kept for itself) |
| UPGRADE, ROLLBACK | `upgraded`, `downgraded`, `rolled-back`, or `unchanged` |
| VERIFY | `intact`; `damaged` with exit 12 and `changed:`/`missing:` lines |
| LIST | `listed`, `package:` lines, `count:` |
| REMOVE | `removed`, `orphan:` lines for what nothing needs any more |
| REMOVE ORPHANS | `removed`, a `package:` line each, `count:` |
| SHOW | `shown`, `entry:` lines, `count:`, `bad:` |
| IMAGE, MOUNTLIST | `created`, or `shown` without `OUT` |

Under `DRYRUN` the results read `would-publish`, `would-install`,
`would-upgrade`, `would-remove` and so on. The `result:` field is not always
the first line: look for it by key.

## Running an application image on AROS

INSTALL of an image answers `image:` (the file, at the top of the root) and
`blocks:`. Then, on AROS:

```
Pkg MOUNTLIST guru ROOT SYS:Apps OUT RAM:GURU MACHINE
```

writes the mount entry with the image's own geometry, and its `step:` lines
are the commands to run, in order: make a directory for `FDSK:`, link the
image there as `UnitN`, write-protect it, add the root's `Libs` to `LIBS:`
when its dependencies live there, and `Mount RAM:GURU`. The device is named
after the mountlist file: `GURU:`. Run the program as `GURU:C/Guru`, or
`Path GURU:C ADD`. Before an upgrade or rollback replaces the image, `Eject
GURU:`; afterwards run MOUNTLIST again, since the size may change. The FFS
handler is the system's; where the system has none (hosted AROS), install
one into the root as a component and MOUNTLIST finds it, or pass `HANDLER`.

## On AROS without ARexx

An AmigaDOS script is enough:

```
C:FailAt 21
Pkg INSTALL guru ROOT SYS:Apps CHANNEL Work:channel MACHINE >T:pkg.out
C:Echo "$RC" >T:pkg.rc
```

Every refusal is at least 10, so `If ERROR` catches them all. A root other
than `SYS:` becomes visible by adding its directories: `Assign LIBS:
<root>/Libs ADD`, likewise `Devs:` and `L:`, and `Path <root>/C ADD`.

## The ARexx port, where ARexx is installed

`Run Pkg PORT` opens the port `PKG`. `ADDRESS PKG` takes the same commands;
with `options results`, RESULT is the output and RC the class code, and
`LASTERROR` returns the last refusal, as a record when the command had
`MACHINE`. `QUIT` closes the port. Nothing requires ARexx.

## Checking a new host

`tools/make-test-kit.sh` writes `build/pkg-test-kit.zip`, which runs the
contract sequence on Windows, macOS or Linux with `run.ps1` and writes
`report.txt`. Ask the person to run it on a host Pkg has not met before.
