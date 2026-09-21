<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Checking a program's updates with libpkg

A program can use `libpkg` to compare its installed version with the highest compatible version
offered by its channels. The result includes the publisher's signed release notes, version, signing
key and package sizes. The program decides when to check and how to display the result.

The check preserves installed files, the package database and trusted keys. Network reads can
populate Pkg's download cache. An application serializes its libpkg calls; a graphical application
can perform the check on its worker thread and pass the result to its interface.

## Basic example

Build and run the local demonstration:

```sh
make build/pkg build/example-selfupdate
sh examples/selfupdate-demo.sh
```

The demonstration creates a temporary signed channel, installs `hello` 1.0, publishes 2.0 with
release notes, and checks through both code and configuration. It displays the offered version and
its changes, then shows that 1.0 is installed. Its temporary files are removed when it exits.

[examples/selfupdate.c](../examples/selfupdate.c) is the minimal client. It accepts `PACKAGE ROOT
CHANNEL`, or `--config FILE`.

## From code

Include `pkg.h` and link `build/libpkg.a`. Initialize both structures before using them:

```c
struct pkg_update update;
struct pkg_update_found found;
int state;
pkg_update_init(&update);
pkg_update_found_init(&found);
update.package = "hello";
update.root = "SYS:";
update.channel = "https://aros-pkg.azurewebsites.net/hello";
state = pkg_update_check(&update, &found);
if (state == PKG_UPDATE_AVAILABLE) {
    printf("Version %s is available.\n%s\n", found.offered, found.changes);
}
pkg_update_found_free(&found);
pkg_update_free(&update);
```

The package name and installation root are required. The installed version comes from that root's
database. `channel = NULL` selects the root's configured channels. An explicit channel selects that
source for this check; its signer is compared with the root's pinned key.

Strings assigned directly to `update` belong to the application and must survive the call. Result
strings belong to `found`; they survive until `pkg_update_found_free` or the next check using that
structure. A repeated check clears the previous result and refreshes each remote channel index.

## From a configuration file

The application names the file explicitly:

```c
char error[512];
int rc = pkg_update_from_file(&update, "PROGDIR:hello.pkgupdate",
                              error, sizeof error);
```

Its contents use `Key: value` lines:

```text
Format: pkg-update 1
Package: hello
Root: SYS:
Channel: https://aros-pkg.azurewebsites.net/hello
```

`Format`, `Package` and `Root` are required. `Channel` is optional. Relative root and local channel
paths are resolved beside the configuration file. A file can live beside the executable or at an
application-selected path.

Unknown keys are ignored. Duplicate known keys, empty required values, malformed lines and control
characters are refused. A failed load preserves the previous configuration. `pkg_update_free`
releases the parsed strings. The `Every` field from the installer design is advisory extension data;
the check API gives scheduling to the application.

## Results

`pkg_update_check` returns a state and stores it in `found.state`:

| State | Meaning |
|---|---|
| `PKG_UPDATE_NONE` | The channel offers no newer compatible version. |
| `PKG_UPDATE_AVAILABLE` | A newer version is signed by the pinned publisher. |
| `PKG_UPDATE_NOT_MANAGED` | The named package has no database entry in this root. |
| `PKG_UPDATE_UNREACHABLE` | The channel or required network metadata could not be read. |
| `PKG_UPDATE_WITHDRAWN` | The installed version has a verified withdrawal, with no newer offer. |
| `PKG_UPDATE_KEY_CHANGED` | The selected offer uses a different signing key. |
| `PKG_UPDATE_NOT_OFFERED` | The channels offer no compatible active version. |
| `PKG_UPDATE_ERROR` | Invalid arguments, damaged metadata, invalid signature or another error. |

`found.code` gives the `PKG_RC_*` error class and `found.error` its diagnostic. A failed check
represents an unknown update state. The application decides whether to report the failure or offer a
retry.

`installed_withdrawn` also reports a withdrawal when a newer version exists. `newer` compares
offered and installed versions. `installed`, `offered`, `changes`, `signer`, `channel`, `homepage`
and `short_desc` supply display text. Fields without a verified offer can be NULL; missing optional
text on a verified offer is an empty string. The manifest digest identifies that exact offer.

`changes` contains the offered version's release notes, preserving their paragraphs.
`installed_bytes` is the signed sum of that offer's file sizes. `download_bytes` is meaningful when
`download_size_known` is set, as for an upstream archive whose signed manifest gives its size. A
payload's compressed download size requires separate information; the check fetches metadata.

Selection uses the root's recorded architecture, AROS's native architecture when applicable, or the
installed package's architecture. It skips signed withdrawals and compares versions with Pkg's
version ordering. Among several channels it selects the highest compatible version, then checks its
signer. Channel order breaks a version tie. A changed key is exposed to the caller. Its signature
verifies under the reported key; accepting that publisher key requires a separate decision. Upgrade
performs dependency and installed-file checks when installation is requested.

## Installer design

An installer is a separate operation from this check. Its design requires explicit user acceptance,
the ordinary UPGRADE integrity and dependency checks, pinned-key enforcement, staging and rollback
support. Its progress can use [the activity interface](activity.md). A key change belongs to the
requester's explicit decision in Pkg. Scheduling and restarting the program belong to the
application.

The proposed `pkg_update_apply` and `pkg_update_finish` calls require an implementation and platform
tests. UNVERIFIED: replacement of the calling program while it runs on AROS, macOS and Linux, and a
staged replacement at exit on Windows. Establishing those paths supplies the installation phase. The
proposed installer result distinguishes `PKG_UPDATE_RESTART` after replacement from
`PKG_UPDATE_AT_EXIT` for a staged replacement. It preserves the selected channel and refuses
downgrades. Files in use, including libraries and devices, require a restart policy and platform
verification.

Root discovery from an executable, accumulated release notes across versions, a conventional
configuration-file location, and `STATUS FROM` on the command line are separate extensions.

Scripts can inspect installations with `STATUS ... MACHINE` and request an upgrade with `UPGRADE`.
The C example and the config loader provide the check-only integration described here.
