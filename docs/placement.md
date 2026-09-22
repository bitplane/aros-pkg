<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Application placement

`INSTALL <name> AT <directory>` places an application's drawer in an existing
absolute directory outside the selected root. The root records its location
and owns its package record, publisher key and dependencies.

For an AROS environment whose root and channels are configured:

```text
Pkg INSTALL xinvaders3d AT Work:Games
Pkg VERIFY xinvaders3d
Pkg UPGRADE xinvaders3d
Pkg REMOVE xinvaders3d
```

`Work:Games` must exist. The application's drawer is placed below it, for example
`Work:Games/XInvaders3D`. With explicit root selection, the installation command
is `Pkg INSTALL xinvaders3d ROOT SYS: AT Work:Games`.

On macOS, Linux or Windows, use a host absolute directory that the target AROS
installation can access. A macOS example is `/Users/aros/AROS/Shared/Games`;
a Windows example is `D:\AROS\Games`. Host installation does not configure AROS
volume mappings or make an AROS executable runnable by the host operating system.

## Layout and ownership

Placement supports an application contained in one identifiable drawer,
including its drawer icon. Packages containing system files or ambiguous drawer
layouts are refused. Libraries, devices, classes, fonts and image packages use
their normal installation paths.

`AT` applies to one explicitly requested application. Its dependencies use the
selected root and their own recorded locations. Each installation keeps the
signed manifest's original paths; a separate placement record in
`.pkg/placements/` maps the application drawer to its destination.

The destination parent must be outside the selected root, including when an
alias refers to it. The destination drawer must be unused. Pkg refuses a
conflicting destination before placing application files. `DRYRUN` checks the proposed installation
without writing files or recording placement.

An interrupted first installation keeps its placement record. Retry `INSTALL`
with the same root and package name to complete it at the saved destination.
Files whose contents were changed after the interruption are refused.

## Later operations

`UPGRADE`, `VERIFY`, `REPAIR`, `ROLLBACK` and `REMOVE` read the recorded destination.
Use the same root or environment as installation. An upgrade whose drawer layout
is incompatible with the recorded placement is refused.

The destination must be available when operating on the application. An absent
volume causes an error. Reconnect it before retrying. Pkg stages relocated files
on the destination volume so placing them uses same-volume renames.

`AT` is an installation option. Moving an installed application to another
destination requires removing it and installing it at the new location. Preserve
personal data before removal. Keep the selected root's `.pkg` directory with its
placement records when backing up the installation.
