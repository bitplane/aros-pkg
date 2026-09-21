<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# A program that keeps itself up to date: specification

Status: specification, not built. It says what a program that links libpkg
can do to find out that a newer version of itself exists, tell its user, and
install it when the user says yes.

## What it is for

A program ships as a package. Its author wants it to say "version 2.1 is
out" and to update itself at a click, without writing any of the
networking, the signature checks or the file replacement: those are pkg's
and stay pkg's. The program decides only when to ask and how to show the
answer.

The user decides whether to update. Nothing here ever installs anything on
its own.

## What the program provides

Three facts, in code or in a file beside the program:

| | |
|---|---|
| its package name | `hello` |
| where newer versions are published | a channel: a URL or a directory |
| the root it is installed in | `SYS:` on AROS; found by the library when not given |

Its own version is NOT one of them: the library reads what is installed
from the root's database, which is the truth, and not what the program
believes it is.

### In a file

`<program>.pkgupdate`, beside the executable or named explicitly, in the
same `Key: value` form as a manifest:

```
Format: pkg-update 1
Package: hello
Channel: https://aros-pkg.azurewebsites.net/hello
Every: 7 days
```

`Every` is advice to the program about how often to ask, never a schedule:
pkg runs nothing in the background. Unknown keys are ignored.

### In code

```c
struct pkg_update u;
pkg_update_init(&u);
u.package = "hello";
u.channel = "https://aros-pkg.azurewebsites.net/hello";
u.root    = NULL;                      /* found from the running program */
```

or `pkg_update_from_file(&u, "PROGDIR:hello.pkgupdate")`.

## The three calls

```c
/* 1. Is there something newer? Reads the channel's index; changes nothing. */
int pkg_update_check(const struct pkg_update *u, struct pkg_update_found *found);

/* 2. What would change: the versions, the size, what the publisher says
 *    changed, and whether the key is the one this machine trusts. */
/*    (filled in `found` by the call above) */

/* 3. Do it, once the user has said yes. */
int pkg_update_apply(const struct pkg_update *u, const struct pkg_update_found *found,
                     const struct pkg_sink *progress);
```

`pkg_update_check` returns:

| | |
|---|---|
| `PKG_UPDATE_NONE` | installed is the newest offered |
| `PKG_UPDATE_AVAILABLE` | `found` names the version, its size, its `Changes` text and its signer |
| `PKG_UPDATE_NOT_MANAGED` | the program is not installed by pkg in that root: there is nothing to compare with, and nothing will be updated |
| `PKG_UPDATE_UNREACHABLE` | the channel could not be read; say nothing to the user, try again later |
| `PKG_UPDATE_WITHDRAWN` | the installed version was withdrawn by its publisher: worth telling the user even with nothing newer |
| `PKG_UPDATE_KEY_CHANGED` | a newer version exists but is signed by another key than the pinned one: never applied by this API, the user must use pkg itself |

`found` holds what a dialog needs: `installed`, `offered`, `bytes`,
`changes` (the publisher's text for that version), `signer`, `channel`.

`pkg_update_apply` is `UPGRADE <package>` with everything that implies:
signature, every file against the manifest, the pinned key, staging, and the
previous version kept for `ROLLBACK`. Its `progress` sink receives the
activity lines of [docs/activity.md](activity.md), so the program draws
them its own way (a gauge in a window, not a terminal line).

## Replacing a program that is running

The program is updating the file it was loaded from.

- **AROS, macOS, Linux:** the file is replaced on disk while the old image
  keeps running; the new version starts at the next launch. `apply` returns
  `PKG_UPDATE_RESTART` to say so. The program offers to restart; it is not
  restarted for it.
- **Windows:** a running `.exe` cannot be replaced. `apply` stages the new
  version and returns `PKG_UPDATE_AT_EXIT`; the program calls
  `pkg_update_finish()` as its last act, or pkg completes it at the next
  launch.
- A library or device in use follows pkg's existing rule for files in use
  (replaced at restart).

## What it never does

- Never installs without the program calling `apply`, and the program only
  calls it on the user's word.
- Never runs on a timer or in the background: no scheduler, as the roadmap
  has it.
- Never accepts a new publisher key. That decision is made in pkg, by a
  person, with both keys shown.
- Never downgrades, never crosses to another channel than the one given.
- Never reports the check to anyone: reading a channel's index sends what
  any pkg request sends (the `User-Agent` of [docs/channels.md](channels.md))
  and nothing about the program or the user.

## The command line, for a script instead of a program

The same, with no code:

```
pkg STATUS hello ROOT SYS: CHANNEL <url> MACHINE      is there something newer
pkg UPGRADE hello ROOT SYS: CHANNEL <url>              do it
```

and with the file: `pkg STATUS FROM PROGDIR:hello.pkgupdate`.

## A sample

`examples/selfupdate.c`: a program that reads its `.pkgupdate`, checks,
prints what it found with the publisher's changes, asks `y/n`, applies, and
says whether to restart. It is the reference for anyone integrating this,
and it runs against a local channel in `tests/selfupdate.sh`, which covers
every return value above, the refused key change, and a program not
installed by pkg.

## Open questions

1. Where the `.pkgupdate` file lives by default on AROS: `PROGDIR:` beside
   the executable is the obvious place, and it would be shipped inside the
   package itself, so the package says where its own updates come from.
2. Whether `found.changes` gives only the offered version's text or every
   version between installed and offered. The second is more useful and
   costs a few more manifest reads.
3. Whether a program may ask for a different channel than the one the
   package was installed from. Safer default: it may not, and the channel in
   the file is only used when the root records none.
