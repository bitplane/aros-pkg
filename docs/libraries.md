# How AROS finds a library

When a program calls `OpenLibrary("SDL2.library", 2)`, AROS decides which
copy of SDL2 it gets. Knowing how it decides explains most "my program does
not start" and "it opens the wrong version" problems, and shows where a
library package must put its files. `pkg RESOLVE` does the same search and
tells you, step by step, what AROS would find and why.

## The rules

1. **A library already in memory wins.** If any program has opened
   `SDL2.library` since the machine started, and it has not been flushed
   from memory, every program gets that copy. No disk is searched, whatever
   newer file lies there. If that copy is older than the version asked for,
   the open fails.
2. **Otherwise the loader searches, in this order**, for a name without a
   colon:
   1. the current directory of the program that asks: `SDL2.library`, then
      `libs/SDL2.library`;
   2. the program's own directory, `PROGDIR:`: `SDL2.library`, then
      `libs/SDL2.library`;
   3. `LIBS:`, the assign the Startup-Sequence makes of `SYS:Libs` and
      `SYS:Classes`, in that order.

   A device is looked for the same way, with `devs/` and `DEVS:`. A name
   with a colon (`PROGDIR:libs/SDL2.library`, `Work:Libs/SDL2.library`) is
   loaded from that path only.
3. **The first file the loader can use is taken, whatever its version.** It
   passes over a file it cannot load: one built for another CPU, or not a
   program file at all. The AROS built from the fork this project uses also
   passes over a file with no resident tag, which is not a library; the
   AROS of the nightlies stops at such a file, and the open fails. A file it
   takes and that turns out older than the version asked for makes the open
   fail: the search does not go on to a newer copy.
4. **One name, one copy in memory.** The first copy loaded is the one every
   program gets until it is flushed (`Avail FLUSH`, which frees libraries
   nobody has open) or the machine restarts.

## What this means for packages

- **A shared library is a package of its own, in `SYS:Libs`**, with a
  version that only ever goes up. Every program that needs it depends on
  that package. Since everybody gets the same copy, a newer version must
  work for the programs built against an older one.
- **`PROGDIR:libs` is for a private copy**: a library you changed for one
  program, under its own name. A private copy of a shared library, under the
  shared name, is a trap: it is found before `SYS:Libs`, so the program gets
  the old one even after the system one is updated; and if another program
  loaded the shared one first, your program gets that one instead of its
  private copy.
- **`Depends` connects a program to the library package.** A library package
  lists the libraries it ships as `Provides:` lines, derived from its
  `Libs/` and `Devs/` when it is published. When you publish a program, pkg
  reads the library names in its executables and warns about each one that
  no dependency provides and that is not part of AROS.

## RESOLVE

The examples install a library package and use a game that carries an older
private copy of the same library in its `libs/`:

```console
$ pkg INSTALL sdl2 ROOT aros CHANNEL channel
installed sdl2 2.30 into aros: 1 file, payload e6f50f1bd01d, signed by 5ff18d3fe14e383e
$ pkg RESOLVE SDL2.library ROOT aros FROM Game
Where                File                       Version  Package             Verdict
PROGDIR:             Game/SDL2.library          -        -                   no file
PROGDIR:libs/        Game/libs/SDL2.library     2.0      not from a package  taken, and hides the newer aros/Libs/SDL2.library 2.30
LIBS: (SYS:Libs)     aros/Libs/SDL2.library     2.30     sdl2 2.30           not reached: found earlier
LIBS: (SYS:Classes)  aros/Classes/SDL2.library  -        -                   no file
  PROGDIR:libs/: remove Game/libs/SDL2.library: aros/Libs/SDL2.library 2.30 is newer and would then be taken
SDL2.library resolves to Game/libs/SDL2.library 2.0
```

Each row is a place the loader looks, in its order, with what it finds and
why. `FROM` names the program's directory, which stands for both the current
directory and `PROGDIR:`; `ROOT` stands for `SYS:`. On AROS you name neither:
pkg searches from where you are, and first asks the system whether the
library is in memory.

Ask for the version the program needs, and RESOLVE says whether it would
get it, and what to do:

```console
$ pkg RESOLVE SDL2.library VERSION 2.30 ROOT aros FROM Game    # exits 18
Where                File                       Version  Package             Verdict
PROGDIR:             Game/SDL2.library          -        -                   no file
PROGDIR:libs/        Game/libs/SDL2.library     2.0      not from a package  taken, too old (2.0, 2.30 needed): the open fails
LIBS: (SYS:Libs)     aros/Libs/SDL2.library     2.30     sdl2 2.30           not reached: found earlier
LIBS: (SYS:Classes)  aros/Classes/SDL2.library  -        -                   no file
  PROGDIR:libs/: remove Game/libs/SDL2.library: aros/Libs/SDL2.library 2.30 is newer and would be taken
pkg resolve: SDL2.library resolves to Game/libs/SDL2.library 2.0, and the open fails: taken, too old (2.0, 2.30 needed): the open fails
  next: ask whoever requested this (the person, or the agent that launched you); it is their decision, not a step to take for them
```

Name a program instead of a library, and RESOLVE checks every library the
program names, from the program's own directory:

```console
$ pkg RESOLVE Game/Game ROOT aros
Library       Taken from              Version  Verdict
SDL2.library  Game/libs/SDL2.library  2.0      taken, and hides the newer aros/Libs/SDL2.library 2.30
  SDL2.library: remove Game/libs/SDL2.library: aros/Libs/SDL2.library 2.30 is newer and would then be taken
dos.library   -                       -        part of AROS, not in this root
all 2 libraries Game/Game names resolve
```

Remove the private copy, and the system library is taken:

```console
$ rm Game/libs/SDL2.library
$ pkg RESOLVE SDL2.library VERSION 2.30 ROOT aros FROM Game
Where                File                       Version  Package    Verdict
PROGDIR:             Game/SDL2.library          -        -          no file
PROGDIR:libs/        Game/libs/SDL2.library     -        -          no file
LIBS: (SYS:Libs)     aros/Libs/SDL2.library     2.30     sdl2 2.30  taken
LIBS: (SYS:Classes)  aros/Classes/SDL2.library  -        -          no file
SDL2.library resolves to aros/Libs/SDL2.library 2.30
```

RESOLVE exits 0 when the library would open, 11 when it is found nowhere,
and 18 when the copy AROS takes is too old or cannot be used. With
`CHANNEL`, a library found nowhere is matched against what the channel's
packages provide, and the next step names the package to install. With
`MACHINE`, each place is a `candidate:` record with its `verdict` and
`next`; the [reference](reference.md) lists them.

The verdicts:

| Verdict | Meaning |
|---|---|
| taken | the loader uses this file |
| taken, too old | the loader uses it, and the open fails |
| taken, and hides the newer ... | a later copy is newer; this one is found first |
| no file | nothing there |
| wrong CPU | an executable for another processor: the loader passes over it |
| not a library (no resident tag) | a program or data file under a library name |
| not a program file | not an executable at all: the loader passes over it |
| not reached: found earlier | the search stopped before this place |
| not consulted: the copy in memory is used | on AROS, a copy is loaded |

## The loader's own account

The AROS source this project builds has a trace of every place the loader
tries, `[LDDiag]` lines in `rom/lddemon/lddemon.c`. It is a build-time
switch: set `__lddemon_trace` to 1 and rebuild the loader
(`make kernel-lddemon`); a running system cannot turn it on. With it on,
`C:TestLib <library> [<version>]` opens the library for real and prints the
version it got, and the trace shows each place the loader tried, next to
what RESOLVE predicted. `tests/aros-resolve.sh` makes that comparison on
hosted AROS without the trace: what RESOLVE predicts, then what `TestLib`
really gets.
