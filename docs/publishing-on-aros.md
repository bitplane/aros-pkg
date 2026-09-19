# Publishing from an AROS machine, from zero

This guide takes an AROS machine with nothing on it to a package of your
own program, installed and updated through Pkg, in the order you do it:
put Pkg on the machine, make your key, lay out the program, publish it,
try it as a user would, publish a new version. Every command is shown as
you type it in the Shell, with what it prints. [Publishing packages](publishing.md)
is the same subject on a Mac or a PC, with more on descriptions,
dependencies and archives; this guide points there rather than repeat it.

The example is `Greet`, a small program that says hello. Two drawers hold
its builds: `Work:src/Greet` and, later, `Work:src/Greet-1.1`.

## 1. Put Pkg on the machine

Pkg is not on a stock AROS yet. There are three ways to put it there.

**With a network and `wget`** (AROS One, Icaros, or the nightly with its
contrib), paste these two lines in the Shell; the script fetches the Pkg
for your CPU, which then installs the signed `pkg` package from the portal:

```amigados
wget -q -O RAM:Get-Pkg http://aros-pkg.azurewebsites.net/Get-Pkg
Execute RAM:Get-Pkg
```

If `wget` ends at once without a word, it wants its settings file first:
`Echo "" >ENV:wgetcfg` (and the same to `ENVARC:` to keep it).

**On Macaros, or any AROS hosted on your computer**, run
`curl -fsSL https://aros-pkg.azurewebsites.net/install | sh` in the
computer's terminal. Besides Pkg for the computer, it puts the AROS drawer
in the folder AROS shares (`~/AROS/Shared`), checked against the portal's
signed checksums, and prints the line to paste in the AROS Shell:

```amigados
Execute MacRW:Pkg-aarch64/Install-Pkg MacRW:Pkg-aarch64
```

**Without a network**, carry the drawer over:

1. On a Mac or PC, open the portal's [Downloads](https://aros-pkg.azurewebsites.net/downloads)
   page and take the zip for your CPU: `pkg-aarch64.zip` for an ARM
   machine, `pkg-x86_64.zip` for a PC. Unpack it: a drawer `Pkg-aarch64`
   (or `Pkg-x86_64`).
2. Copy the drawer to the AROS machine: a USB stick, a shared drawer, a
   disk image. Below it is at `Work:Pkg-aarch64`.
3. In the Shell:

```amigados
Execute Work:Pkg-aarch64/Install-Pkg Work:Pkg-aarch64
```

The script checks that the bare Pkg in the drawer runs on this machine,
then that Pkg installs the signed `pkg` package from the drawer into
`SYS:`, checking its signature and every file as it does for any package.
What you have afterwards is `SYS:C/Pkg`, the copy from the signed package:

```
This machine runs the aarch64 build.
installed pkg 1.3 into SYS:
Pkg is in SYS:C. Try: Pkg HELP.
```

Add a root after the drawer to install elsewhere than `SYS:`
(`Execute Work:Pkg-aarch64/Install-Pkg Work:Pkg-aarch64 Work:aros/`).

If `Execute` itself fails (*object not found*, *error while creating
temporary file*), the machine has no `T:` assign, which AmigaDOS needs for
any script that takes arguments: `Assign T: RAM:` and run it again. If the
script says *None of the Pkg builds in the channel's Bootstrap drawer runs on
this machine*, the drawer is for another CPU: it names the CPUs it holds.

`Pkg HELP` lists every verb and keyword; [Reference](reference.md) has each in full.
Words in capitals are keywords, in any case, in any order, as in any
AmigaDOS command.

## 2. Make your key

Every package is signed; there is no unsigned package. The key is a file,
made once, kept where the machine keeps your secrets and backed up: every
later version of what you publish must be signed with the same key,
because people who installed your package accept a new version only from
it ([Signatures and trust](signing.md) says why).

```amigados
MakeDir Work:keys
Pkg KEYGEN FILE Work:keys/my.key
```

AROS has no random source, and a key made from the clock could be guessed.
So Pkg makes it from the moments you press keys, read from the CPU's
cycle counter: type anything, at random, until the count reaches 0 (64
keys; a held key does not count). What you type is not kept.

```
AROS has no random source, so the key is made from the moments you press keys.
Type anything, at random, until the count reaches 0. What you type is not kept.
   64
key written to Work:keys/my.key, readable by you alone
  public key d0e87172f204b4e6663aa1d58fb79022bb59a83ba4d8a69acf19e4ff830c776e
```

This needs a Shell window: from a script with its output redirected, or
through the ARexx port, `KEYGEN` refuses (exit 17) rather than make a weak
key. A key made on a Mac or a PC with `pkg KEYGEN FILE my.key` is the same
kind of file, if you would rather carry one over.

Tell Pkg where it is, once for this session and once for every boot:

```amigados
SetEnv PKG_SIGNKEY Work:keys/my.key
Copy ENV:PKG_SIGNKEY ENVARC:
```

`SIGN Work:keys/my.key` on a command does the same for that command. The
public part, the only part you give anyone, is what `KEYINFO` prints;
`SHOW` prints its first sixteen digits under *Signer*:

```amigados
Pkg KEYINFO FILE Work:keys/my.key
```

```
Work:keys/my.key holds the public key d0e87172f204b4e6663aa1d58fb79022bb59a83ba4d8a69acf19e4ff830c776e
```

Never put the key file in a channel, a package or a drawer you copy
around.

## 3. Lay out the program as it will be installed

A package is a drawer laid out like `SYS:`: what goes in `C` is under `C`,
what goes in `S` under `S`, `Libs`, `Devs`, `Fonts`, `Locale` likewise.
For a program run from the Shell that is one file:

```
Work:src/Greet/
    C/Greet
```

Pkg takes the package's name and version from the program's `$VER:`
string, the one `Version` prints, and its CPU from the executable itself.
`Greet` carries `$VER: Greet 1.0 (19.9.2026)`, so the package is `greet`
1.0 for `aarch64`, with nothing to type. A program without `$VER:`, or a
package of data, scripts or fonts, gets them on the command: `NAME greet
VERSION 1.0`, and `ARCH generic` when nothing in it is a program.

Two files beside the drawer describe the package to people; they are
plain text, written with any editor:

```
Work:src/about.txt      what the program does, a paragraph or two
Work:src/changes.txt    what this version changes
```

## 4. Read the description before publishing

`MANIFEST` shows what `PUBLISH` would sign, and writes nothing:

```amigados
Pkg MANIFEST Work:src/Greet KIND application
```

```
Format: pkg-manifest 1
Name: greet
Version: 1.0
Architecture: aarch64
Kind: application
Payload: 618a890b085b01250bc6f749917a97f8c196851024c61142434bb4998336f8fd
File: 54ab65f5594306bc0841adaf7f7c23ead7d3e914e4306bebfb60f5158986bc0e 27592 C/Greet
Protect: 0x00008800 C/Greet
```

Name, version and CPU are the ones expected, and `File:` lists exactly
what will be installed: if a backup file or an icon you did not mean is in
the drawer, it shows here.

`KIND` says what the package is: `application` for a program installed as
loose files, `library`, `device`, `font`, `data`, ... ([Publishing
packages](publishing.md) has the table). It is needed for the first
version only.

## 5. Publish

A channel is a drawer that Pkg fills; the first `PUBLISH` into it creates
it. Keep the channel apart from the sources.

```amigados
Pkg PUBLISH Work:src/Greet CHANNEL Work:mychannel KIND application SHORT "Says hello" DESCRIPTION Work:src/about.txt CATEGORY util/misc TAGS "shell, example" AUTHOR "Jane Roe" LICENSE MIT DISTRIBUTION open-source CHANGES Work:src/changes.txt
```

```
published greet 1.0 to Work:mychannel: 1 file, payload 618a890b085b, signed by d0e87172f204b4e6
  architecture aarch64, read from C/Greet
  name and version taken from $VER: in C/Greet
  hint: the channel Work:mychannel did not exist and was created: machines install from it with INSTALL greet ROOT <root> CHANNEL <this channel, as the machine names it>
```

`SHORT` is the one line people see in lists, `DESCRIPTION` the file with
the long text, `CHANGES` the file for this version; `CATEGORY`, `TAGS`,
`AUTHOR`, `LICENSE`, `DISTRIBUTION` and `HOMEPAGE` are what the portal
shows and searches. All are optional and all are signed with the rest.

```amigados
Pkg SHOW CHANNEL Work:mychannel
```

```
Package  Version  Kind         Arch     Status  Signer
greet    1.0      application  aarch64  ok      d0e87172f204b4e6
```

`ok` means Pkg checked the signature and the files of that entry, as it
will on every machine that installs it.

## 6. Try it as a user would

Install it into a root that is not your system, then run it from there:

```amigados
MakeDir RAM:try
Pkg INSTALL greet ROOT RAM:try CHANNEL Work:mychannel
RAM:try/C/Greet NAME Jane
Pkg LIST ROOT RAM:try
Pkg VERIFY greet ROOT RAM:try
```

```
installed greet 1.0 into RAM:try: 1 file, payload 618a890b085b, signed by d0e87172f204b4e6
Hello, Jane!
Package  Version  Kind         Files
greet    1.0      application  1 file
greet 1.0: 1 file, all intact
```

`INSTALL` checks the signature and every file before it places anything,
and pins your key for `greet` in that root. When it is right, install it
for real: `Pkg INSTALL greet ROOT SYS: CHANNEL Work:mychannel`.

## 7. Publish a new version

Raise the version in `$VER:` and build again; a version, once published,
never changes, and publishing different files under the same version is
refused (exit 15). Greet 1.1 adds a preference file people may edit, so
it is named with `CONFIG`: an update keeps the person's copy and puts the
new one beside it as `.pkgnew`.

```
Work:src/Greet-1.1/
    C/Greet
    S/Greet.prefs
```

```amigados
Pkg PUBLISH Work:src/Greet-1.1 CHANNEL Work:mychannel CONFIG S/Greet.prefs CHANGES Work:src/changes-1.1.txt
```

```
published greet 1.1 to Work:mychannel: 2 files, payload 0b094c1fe04e, signed by d0e87172f204b4e6
  architecture aarch64, read from C/Greet
  name and version taken from $VER: in C/Greet
  kind and dependencies from greet 1.0, published before
```

The kind, the description and the dependencies come from the last version
published; name only what changes. Then what a person with 1.0 sees and
does:

```amigados
Pkg STATUS ROOT RAM:try CHANNEL Work:mychannel
Pkg UPGRADE greet ROOT RAM:try CHANNEL Work:mychannel
```

```
Package  Installed  State
greet    1.0        upgradable to 1.1
1 of 1 package in RAM:try can be updated from Work:mychannel
upgraded greet from 1.0 to 1.1 in RAM:try: 2 placed, 0 removed
```

`ROLLBACK greet ROOT RAM:try CHANNEL Work:mychannel` goes back to 1.0;
`REMOVE greet ROOT RAM:try` takes it out.

## 8. Give it to people

The channel is the drawer `Work:mychannel`, and only that: copy it to a
stick, a share or a disk image, and anyone installs from it with
`Pkg INSTALL greet ROOT SYS: CHANNEL <the drawer as they see it>`. Pkg
checks the signatures on their machine, so the way the drawer travels
does not matter.

People with a network install straight from a web address, so a channel
served over plain `http` reaches every AROS machine:
`Pkg INSTALL greet ROOT SYS: CHANNEL http://example.org/mychannel`
([Channels](channels.md) shows how to serve one).

The portal is where people look first, and `PUSH` sends a channel to it
from AROS too, over plain `http`, signed with your key instead of a secret
from the portal. Send the portal's maintainers your public key once
(`Pkg KEYINFO FILE Work:keys/my.key`); when they have added it:

```amigados
Pkg PUSH CHANNEL Work:mychannel TO http://aros-pkg.azurewebsites.net/mychannel
```

Whether your push carries the files or only links to them is the portal's
rule for your key; its rules are on its
[trust page](https://aros-pkg.azurewebsites.net/trust), and
[PUSH](commands/push.md) says how the signing works.

## When something is refused

Pkg refuses rather than guess, says why in one line, and its exit code is
the class of the refusal; a Shell script tests it with `If WARN` or
`FailAt`. The ones you meet when publishing:

| It says | Exit | What to do |
|---|---|---|
| *no NAME given and no $VER: cookie found* | 20 | give `NAME` and `VERSION` |
| *greet 1.0 is already published with a different payload* | 15 | raise the version in `$VER:` |
| *no signing key: give SIGN <keyfile>, or set PKG_SIGNKEY* | 14 | `SetEnv PKG_SIGNKEY` (section 2) |
| *greet 1.0, the first version in ..., is signed by ...* | 14 | sign with the key that made the channel, or publish into a channel of your own |
| *CONFIG names "S/x.prefs", which is no file or folder of this package* | 20 | name a path as the package installs it |
| *this system has no random source* | 17 | run `KEYGEN` in a Shell window, not from a script (section 2) |
| *Software Failure* on `MANIFEST` or `PUBLISH` | | that is Pkg 1.1: take the current Pkg from the Downloads page, or type `Stack 1000000` first |
| *no executable in the drawer* (a warning, still published) | 0 | the drawer holds a script or a placeholder, not the build; or say `ARCH generic` on purpose |

`Pkg <command> DRYRUN` runs every check and writes nothing; `TRACE
RAM:trace.txt` writes every step Pkg took, for when the one line is not
enough. [Reference](reference.md) lists every verb and exit code.
