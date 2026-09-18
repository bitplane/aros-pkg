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

`pkg` is one executable (`Pkg` on AROS). On a development machine, in its
repository, `make install` puts `pkg` in `~/.local/bin`, with `pkg.h`,
`libpkg.a` and this skill beside it (`include/`, `lib/`, `share/pkg/`);
`make install PREFIX=<dir>` puts them under another directory, for a person
who wants nothing system-wide. Then `<prefix>/bin` must be on `PATH`; say so
to the person rather than editing their shell profile. Without installing,
`make` builds `build/pkg`. `pkg HELP` names the version and lists every verb
and keyword.

To put Pkg on an AROS machine: build it for each CPU (`sh tools/build-aros.sh`
for aarch64, `sh tools/build-aros-x86_64.sh` for x86_64; `make` alone builds
only the host's `pkg`), then `make aros-channel CHANNEL=<dir>` with
`PKG_SIGNKEY` set. On AROS the directory has another name, the one that
machine gives it (a shared volume `DEPOT:`, a drawer `Work:channel`); the
person, or a startup script, runs `Execute <ch>/Install-Pkg <ch>` with that
name, `Execute DEPOT:Install-Pkg DEPOT:` for a volume root. Never tell the
person a host path to type on AROS. Never copy a Pkg binary into C: by hand:
installed through its channel, it is verified and can upgrade itself.
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
3. **On a refusal, do what `next:` says. On a success, read the `hint:`** lines:
   they say what usually comes next (keep the new key, check the channel,
   mount the image), and anything worth telling the person.
   `warning:` lines are things to check before going on; `note:` lines are
   facts worth passing on (a newer version published for another CPU).

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

**Kinds.** Every publish names one; Pkg refuses to guess.

| `KIND` | For | Installed as |
|---|---|---|
| `image` | a program people run (a tool, an application, a game) | one read-only volume, `<name>.hdf`, mounted to run |
| `application` | a program installed as loose files into the root, when mounting is not wanted | its files, in place |
| `library`, `device`, `class`, `font`, `catalog` | system components other programs use: `Libs/`, `Devs/` or `L/` (a handler, a filesystem is `device`), `Classes/`, `Fonts/`, `Locale/` | their files, in their fixed place |
| `startup`, `boot` | pieces the system runs at startup | their files |
| `data`, `sdk`, `slave` | documents and data; headers and link libraries; WHDLoad slaves | their files |

When unsure between `image` and `application`, it is `image`; the
requester decides if they want otherwise. Keep a package's kind from one
version to the next: Pkg warns when it changes.

**Several CPUs.** One version may be published for several CPUs (aarch64
for hosted AROS, x86_64 for native, m68k); the architecture is read from
the binaries. When installing from a host into a root for an AROS machine,
pass `ARCH <cpu>` the first time; the root remembers it. A refusal "offered
for several CPUs" means exactly that. Publish each CPU's build separately,
same name and version.

**Two routes.** A system component (library, device or handler, class, font) is
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
5. **One key per publisher, kept.** Every later version of a package must be
   signed with the key that signed its first version; Pkg refuses to publish
   it otherwise. So: if the package already exists in the channel (`SHOW
   <name> CHANNEL <dir>` names its signer), find that key and use it, never
   make a new one (`pkg KEYINFO FILE <keyfile>` names the public key a file
   holds). Make a key with KEYGEN only for someone who has never published,
   into a channel where their packages do not exist yet. In a team, each
   package belongs to whoever published it first; whether the team shares
   one key or each person keeps their own is the requester's call, and
   worth asking once. Keep a key where the person keeps secrets (outside the
   channel and any repository), readable by them alone, backed up; never
   print, copy or commit it. The `public:` line is what may be shared.
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

PUBLISH creates the channel directory if it does not exist (`hint:` says
so). A program shows `architecture: generic` only when Pkg found no
executable header in it: for a real AROS binary that means the drawer holds
something else than the build (a script, a copy, the wrong file); check.

An application:

```sh
pkg PUBLISH <drawer> CHANNEL <channel> NAME guru VERSION 2.0 KIND image \
            DEPENDS "identify.library >= 37.1" DRYRUN MACHINE
```

The volume's top level is the drawer, laid out as the program expects to
find itself when mounted; its own helpers, icons and documents go inside,
and only what other programs share goes into `DEPENDS`, by package name.
The dry run lists the files that go into the image as `content:` lines.
On macOS the filesystem ignores case: never create `GURU` beside `Guru`.

**A new version** starts from nothing: `KIND`, `DEPENDS` and `ARCH` are not
carried over from the previous one; pass them again. Before publishing it,
check that the drawer holds the new build: Pkg warns when `VERSION`
contradicts the program's own `$VER` cookie (often the old build copied by
mistake), when a dependency of the previous version is missing, and when
the kind changes. Compare the dry run's `file:`/`content:` sizes with the
previous version's (`SHOW`, or `MANIFEST` on the old drawer) when in doubt,
and ask rather than publish a version whose program did not change.

**One CPU ahead of another** (an x86_64 fix, the aarch64 build not ready):
publish the new version for the CPU that has it, and nothing for the other.
Roots of the other CPU stay on their version; UPGRADE there answers
`unchanged` with a `note:` that the newer version exists for another CPU,
and asking for it there is refused as "published for x86_64 only". Publish
the other build later, same name and version, when it is ready.

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
| KEYGEN, KEYINFO | `created`; `shown` |
| MANIFEST | `shown`, then the manifest's own fields |
| PUBLISH | `published`; `unchanged` for the exact content already there; `repaired` when that content's objects were damaged |
| WITHDRAW | `withdrawn`, or `unchanged` |
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
rollback replaces the image, `Eject GURU:`; afterwards run MOUNTLIST again,
since the size may change. The FFS handler is the system's; native AROS has
one. Hosted AROS built on macOS has none: build one on the host
(`sh tools/build-aros-extras.sh` writes `build/aros/afs-handler`), publish
it as `KIND device` with its file at `L/afs-handler` in the drawer, install
it into the root, and MOUNTLIST names it; or pass `HANDLER <path>`. The
`hint:` lines of MOUNTLIST say when no handler is installed.

**The whole flow**, for a person who will type on AROS, with `SYS:` as the
root (one database, libraries straight into `LIBS:`; another root such as
`Work:Apps` keeps the system drawer untouched, at the cost of an `Assign
LIBS: <root>/Libs ADD`, which MOUNTLIST adds to the steps):

```
Execute DEPOT:Install-Pkg DEPOT:                          ; once: Pkg itself
Pkg INSTALL guru ROOT SYS: CHANNEL DEPOT:                 ; guru and what it depends on
Pkg MOUNTLIST guru ROOT SYS: OUT RAM:GURU                 ; prints the steps
MakeDir RAM:fdsk
Assign FDSK: RAM:fdsk
MakeLink RAM:fdsk/Unit20 SYS:guru.hdf
Protect SYS:guru.hdf w SUB
Mount RAM:GURU
GURU:C/Guru 04000001
```

The step lines are the ones MOUNTLIST printed; copy them from its output
rather than from here, since unit and paths follow the arguments. They can
go into a script the person runs after each boot, since mounts do not
survive one; INSTALL answers `unchanged` when run again. Before giving a
person such a script, run the same lines against a scratch root on the host
(`ROOT <tmp> ARCH <cpu>`), and say which lines were checked on AROS and
which only on the host.

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
