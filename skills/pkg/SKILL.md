---
name: pkg
description: Drive Pkg, the AROS package tool, on behalf of a person - publish a program or a system component into a channel, install, upgrade, roll back, verify and remove it in a root, inspect a channel, mount an application image, on macOS, Linux, Windows or AROS. Use when asked to package, ship, install, update or remove AROS software, or to explain a Pkg refusal.
---

# Pkg, for agents

Pkg moves AROS software from where it is built to where it runs, signed at
every step. Most people will never type its commands; you will. Decisions that belong to whoever requested the work (called the requester
below: a person, or a supervising agent that holds the authority) are never
taken by the tool, and must not be taken by you. The tool is
built so that the safe path is the easy one: every refusal tells you what to
do next, every changing command can be tried first with `DRYRUN`, and nothing
it prints hands you a command that overrides a safeguard. Follow what it says.

## Getting the tool

`pkg` is one executable (`Pkg` on AROS). If it is not on `PATH`, build it
from its repository with `make` (`build/pkg`); for AROS, `sh
tools/build-aros.sh` (`build/aros/Pkg`). A `build/pkg` older than the
sources is rebuilt by `make`. `pkg HELP` lists every verb and keyword.
Programs that link Pkg instead of running it use `include/pkg.h`, which
lists every field each operation answers; its `item` callback gives the
multi-field records with their fields apart; `examples/basic.c` and
`examples/browse.c` show both uses.

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
| `ask-requester` | Take the `reason:` to whoever requested this: the person, or the agent that gave you the task, if it holds the authority. Wait for their answer. It is their decision: a new publisher key, going back a version, a file of theirs where the package installs (move it, or choose another root), a file they edited, removing something others need |
| `fix-command` | Your command is wrong; `pkg HELP` has the spelling. Keywords have no dashes: `ROOT <dir>`, not `--root` |
| `check-name` | The name is not there. Near names come as `suggest:` fields; `pkg SHOW CHANNEL <dir>` lists a channel, `pkg LIST ROOT <dir>` a root |
| `use-upgrade` | Another version is installed; `UPGRADE` moves it, if that is what the person asked for |
| `use-install` | It is not installed; `INSTALL` it, if that is what the person asked for |
| `report` | Report the `reason:` |

A refusal reads `result: refused`, `class:`, `code:`, `reason:`, `next:`. A
key refusal also gives the keys involved (`pinned:` or `first-signer:`, and
`signer:`): show them to the person. The key that signed a package's first
version in a channel is presumed its publisher's, so a first install, or a
publish, signed by another key is refused too. If the person confirms with
the publisher, by another route than the channel, that the other key is
theirs, repeat the command with `ACCEPTKEY <that key, in full>`. Never
before.

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
   `architecture:`, `depends:`, `file:` and `signer:`; compare them with the
   versions already published (`SHOW <name> CHANNEL <dir>`). When unsure,
   publish to a scratch channel first.
2. **Never pass `ACCEPTKEY` or `DOWNGRADE` on your own.** Pkg never suggests
   them; only the person's answer does.
3. **Never change `.pkg/` in a root, or `objects/` in a channel, by hand**;
   reading them does no harm, but `LIST`, `SHOW` and `TRACE` say the same
   more clearly. After everything is removed a root still holds the keys
   pinned for each package: that is on purpose.
4. **`REMOVE ORPHANS` removes packages.** Run it when the person asked to
   clean up what is left; otherwise show them the `orphan:` lines `REMOVE`
   printed.
5. **One key per publisher, kept.** Every later version must be signed with
   the same key; Pkg refuses to publish it otherwise. When `PKG_SIGNKEY` is
   not set, find the publisher's existing key, never make a new one: `pkg
   KEYINFO FILE <keyfile>` names the public key a file holds, and `SHOW`
   names the signer of each published version. Keep it where the person
   keeps secrets; never print, copy or commit it.
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
pkg SHOW     [<name>] CHANNEL <channel> [ROOT <root>] MACHINE
pkg KEYINFO  FILE <keyfile> MACHINE
```

Every one that changes something takes `DRYRUN`. Without `VERSION`, the
highest version. INSTALL settles every dependency before placing a file
(`dependency:` lines) and places nothing if anything is refused; installing
what is already installed answers `unchanged` and succeeds, so a retry is
safe. ROLLBACK goes back one step, to the version installed before the last
change. SHOW checks every entry of a channel as INSTALL would, without
installing, and exits 12 or 13 if any is damaged: use it before trusting a
channel, mirror or copy. Its `entry:` lines read `name version kind
architecture status signer`, status `ok` or a class name, and with `ROOT
<dir>` a last field, `installed`, `other-version` or `no`; `depends:` lines
read `package version needs [>= min]`, and a `warning:` names a package
signed by more than one key. `LIST`'s `package:` lines read `name version
kind files explicit|dependency`.

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

## When a published version is broken

Nothing published is ever deleted; three things are possible instead.

- **It works badly** (crashes, wrong behaviour): its publisher withdraws it.
  `pkg WITHDRAW <name> VERSION <v> CHANNEL <dir> MACHINE`, signed with the
  key that signed that version (`DRYRUN` first). It stays in the channel,
  but INSTALL and UPGRADE no longer pick it, asking for it by version is
  refused, and SHOW marks it `withdrawn`. Machines that already have it
  ROLLBACK, or UPGRADE once a fixed version is out. Withdrawing is the
  requester's decision.
- **Its files in the channel are damaged** (SHOW says `integrity` or
  `signature`): its publisher publishes the same drawer again, same name and
  version, same key. Pkg writes the damaged objects again and answers
  `repaired`. Without the original drawer and key it cannot be repaired:
  say so, and WITHDRAW it if the key is at hand.
- **Meanwhile**, anyone can install an intact version by asking for it
  with `VERSION`. A withdrawn version is skipped on its own; a damaged one
  is not: Pkg refuses rather than quietly installing something older.

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
`Path GURU:C ADD`. If the program then cannot open a library its package
brought (seen on a native system booted from CD, where a RAM: directory added
to `LIBS:` is not searched), `CD <root>` before running it: the library loader
also looks in `libs/` under the current directory. Before an upgrade or
rollback replaces the image, `Eject
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
