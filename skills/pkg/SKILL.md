---
name: pkg
description: Drive Pkg, the AROS package tool, on behalf of a person - publish a program or a system component into a channel, install, upgrade, roll back, verify and remove it in a root, mount an application image, on macOS, Linux, Windows or AROS. Use when asked to package, ship, install, update or remove AROS software, or to explain a Pkg refusal.
---

# Pkg, for agents

Pkg moves AROS software from where it is built to where it runs, signed at
every step. You drive it from a shell, from an AmigaDOS script, or through its
ARexx port; the contract is the same on every host. Most people will never
type these commands. You will, so follow this file exactly, and hand the
decisions it marks as the person's back to the person.

## Words

- **Drawer**: a directory laid out as it will be installed (`C/Tool`,
  `Libs/foo.library`, `Tool.info`).
- **Channel**: a directory holding `index` and `objects/`. Publishing writes
  to it; installing reads from it. It can be copied, synced or served as is.
- **Root**: the directory a package is installed into. Its database is
  `<root>/.pkg/`. Never edit anything under `.pkg/` by hand.
- **Key**: an Ed25519 signing key, a file readable by its owner alone.
  Every package is signed; there is no unsigned mode.
- **Kind**: `application`, `library`, `device`, `class`, `font`, `catalog`,
  `startup`, `data`, `boot`, `slave`, `sdk`, or `image`.

## Always ask for the machine contract

Add `MACHINE` to every command, or set `PKG_OUTPUT=machine`. Then:

- stdout holds only `key: value` lines, and stderr stays empty;
- every answer has a `result:` line;
- the exit code (on AROS, `$RC`) names the class of a refusal.

| Code | Class | What it means | What you do |
|---|---|---|---|
| 0 | ok | Done | Read `result:` and report it |
| 11 | not-found | No such package, version, or installed package | Check the name with `LIST`, or the channel's `index`; report |
| 12 | integrity | Bytes do not match their digest, an unsafe path, a damaged install | **Stop.** Report. Never work around it |
| 13 | signature | Unsigned, or the signature does not verify | **Stop.** Report. Never work around it |
| 14 | key | Signed by another key than the one pinned in this root | **Stop.** Ask the person (rule 1) |
| 15 | conflict | Already installed, a file already present, a file the person edited | Report what is in the way; ask before removing anything |
| 16 | dependency | A dependency is missing, too old, in a cycle, or still needed | Read `reason:`; it names the package and the way out |
| 17 | io | The filesystem refused | Check space and permissions; report |
| 18 | policy | Allowed only with an explicit keyword, such as `DOWNGRADE` | Ask the person before adding the keyword |
| 20 | usage | Unknown verb, missing keyword or value | Fix the command |
| 10 | refused | Any other refusal | Read `reason:` |

A refusal reads `result: refused`, `class:`, `code:`, `reason:`. Quote the
reason to the person as it is: it names the file, the package and the next
step.

## Rules

1. **Never pass `ACCEPTKEY` on your own.** A key change is either a
   publisher rotating keys or an attack, and only the person can tell which,
   by checking with the publisher outside this channel. Show them both keys
   from the refusal and wait.
2. **Never add `DOWNGRADE`** unless the person asked for that version.
   `ROLLBACK` returns to the previous version and needs no keyword.
3. **Never retry past codes 12, 13 or 14** with another channel, a copied
   file, or a hand-edited database. Those refusals are the tool working.
4. **A published version never changes.** To ship a fix, publish a new
   version. Code 15 on `PUBLISH` means that version already exists with
   other bytes.
5. **`REMOVE ORPHANS` removes packages.** Run `REMOVE` first, show the
   person the `orphan:` lines it prints, and take them out only when asked.
6. **Keys stay private.** Never print, copy or commit a key file. The public
   half is the `public:` line from `KEYGEN`, and only that is shared.
7. **Check the output, not only the code.** On AROS, a command that cannot
   even load leaves `$RC` unchanged, so a script can read 0 after a failure.
   A Pkg step has succeeded when its output has the `result:` you expect.

## Publishing

Once per publisher:

```sh
pkg KEYGEN FILE ~/.pkg-dev.key MACHINE        # result: created, public: <hex>
export PKG_SIGNKEY=~/.pkg-dev.key
```

A system component (a library, a handler, a class): lay out the drawer as it
installs, then

```sh
pkg PUBLISH <drawer> CHANNEL <channel> NAME foo VERSION 1.2 KIND library MACHINE
```

`NAME` and `VERSION` may be left out when a file in the drawer carries a
`$VER: name version` cookie; `version-from:` says which file was used. Host
metadata is never packaged: names starting with `.` (`.DS_Store`, `.git`,
`.backdrop`), `Icon\r`, `Thumbs.db`, `desktop.ini`. Each is named in a
`left-out:` line. Amiga icons, `Name.info`, are kept.

An application: publish it as an image, naming the system components it
needs.

```sh
pkg PUBLISH <drawer> CHANNEL <channel> KIND image DEPENDS "foo >= 1.2, bar" MACHINE
```

The package holds one file, `<name>.hdf`, a read-only FFS volume made from
the drawer. `pkg IMAGE <drawer> OUT <file> NAME <volume>` makes the same file
without a channel.

## Installing and changing

```sh
pkg INSTALL  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg UPGRADE  <name> ROOT <root> CHANNEL <channel> [VERSION v] MACHINE
pkg ROLLBACK <name> ROOT <root> CHANNEL <channel> MACHINE
pkg VERIFY   <name> ROOT <root> MACHINE
pkg LIST     ROOT <root> MACHINE
pkg REMOVE   <name> ROOT <root> MACHINE
pkg REMOVE   ORPHANS ROOT <root> MACHINE
```

- Without `VERSION`, the highest version; with it, exactly that one.
- `INSTALL` settles every dependency before placing a file. Each one brought
  in prints `dependency: <name> <version>`. Nothing is placed if anything is
  refused.
- `LIST` prints `package: <name> <version> <kind> <files> explicit|dependency`.
- `VERIFY` prints `changed:` and `missing:` lines and exits 12 when anything
  differs. An edited file is the person's; Pkg keeps it on upgrade and
  removal and says so with `kept:`.
- `INSTALL` of a package that came in as a dependency keeps it for itself
  (`result: kept`), so it is no longer an orphan candidate.

## On AROS without ARexx

An AmigaDOS script is enough. Set `FailAt 21` so refusals do not stop the
script, write each output to a file, and record `$RC` after each step:

```
C:FailAt 21
Pkg INSTALL guru ROOT SYS:Apps CHANNEL Work:channel MACHINE >T:pkg.out
C:Echo "$RC" >T:pkg.rc
```

Every refusal is at least 10, so `If ERROR` catches them all.

A root other than `SYS:` becomes visible to the system by adding its
directories: `Assign LIBS: <root>/Libs ADD`, likewise `Devs:` and `L:`, and
`Path <root>/C ADD` for commands.

## Mounting an application image

The image is an FFS volume with fixed geometry: 512-byte blocks, 32 per
track, 2 reserved, `DosType 0x444F5303`. Its size is on the image's `File:`
line in the signed manifest; `HighCyl` is size / 16384 - 1. With
`fdsk.device`, whose unit N opens the file `FDSK:UnitN`:

```
Assign FDSK: <dir>
C:MakeLink <dir>/Unit20 <root>/guru.hdf
C:Protect <root>/guru.hdf w SUB
C:Mount <mountlist>
```

with a mountlist naming `fdsk.device`, `Unit 20`, the geometry above, and the
FFS handler the system provides. Hosted AROS ships none; install the
`afs-handler` component and name `<root>/L/afs-handler`. Run
`C:Eject <device>:` before an upgrade or rollback replaces the image, then
mount the new one, with its own `HighCyl`.

## The ARexx port, where ARexx is installed

`Run Pkg PORT` opens the port `PKG`. `ADDRESS PKG` then takes the same
commands; with `options results`, RESULT is the output and RC is the class
code. `LASTERROR` returns the last refusal, as a record when the command had
`MACHINE`. `QUIT` closes the port. Nothing requires ARexx: prefer the shell.

## Checking a new host

`build/pkg-test-kit.zip` (from `tools/make-test-kit.sh`) runs the contract
sequence on Windows, macOS or Linux with `run.ps1` and writes `report.txt`.
Ask the person to run it on a host Pkg has not been used on before.
