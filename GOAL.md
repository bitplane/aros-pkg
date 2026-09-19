<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Goal 1: Pkg delivers the platform to itself, and an agent watches

Set 2026-09-18. **Met 2026-09-18**: `tests/goal.sh`, 25 checks, 0 failures, including a sabotaged control run that must fail and does. One sequence that passes or fails as a whole:

1. **On macOS**, the AFS+ handler is cross-built at two versions, and
   `Pkg Publish` puts both into a channel that is a directory, signed with a
   development key.
2. **On hosted AROS**, `Pkg` bootstraps itself from a plain archive with no
   package manager present. Then `Install afsplus-handler` gives v1,
   `Verify` passes, `Upgrade` gives v2, and `Rollback` returns v1.
3. **One ARexx script** drives all of it through the `PKG` port and exits
   non-zero at the first disagreement.
4. **Two negative controls run inside that same sequence**: a tampered payload
   and a substituted publisher key, both refused.

Every hard property appears once, on a real component: the cross-host loop,
the container, the manifest, SHA-256, Ed25519 and key pinning, versions, a
handler replaced at restart with fallback, rollback, bootstrap, and an agent
in control. It is also the component copied by hand every day at the time of
writing, so the tool pays for itself the day the sequence passes.

Out of scope for this goal: AFS+ reflinks, network and hosting, contrib
breadth, WHDLoad, entitlements, AmigaOS interop.

## Milestones, macOS first

The owner develops on macOS, so the tool becomes useful there first and AROS
follows.

| | Milestone | Done when |
|---|---|---|
| **M1 ✓** | **macOS loop.** Done 2026-09-18. Manifest, publish into a directory channel, install into a root, list, verify, remove, with every payload digest-checked | `make check` passes, including an end-to-end run on macOS with its negative controls |
| **M2 ✓** | **Trust and versions.** Done 2026-09-18. Ed25519 signatures, publisher key pinned on first use, `Upgrade` and `Rollback`, `EXACT` and `COMPATIBLE` selection | A tampered payload and a substituted key are refused on macOS |
| **M3 ✓** | **The AROS client.** Done 2026-09-18. `Pkg` built for aarch64-aros, bootstrapped on hosted AROS, installs the AFS+ handler with replacement at restart and fallback | Steps 1 and 2 of the goal pass on hosted AROS |
| **M4 ✓** | **The agent.** Done 2026-09-18. The `PKG` ARexx port, and one script that drives steps 1 to 4 | The whole goal sequence passes |

Gates touched: `[PKG1]`, `[PKG6]`, `[PKG10]`, `[PKG11]`, `[PKG17]`, `[PKG18]`,
`[PKG19]`, `[PKG21]`, `[PKG22]`, `[PKG23]`, and the first component of
`[PKG9]`, all in the planning repository's `docs/features/packaging/spec.md`.

# Goal 2: an application arrives, its dependencies arrive by Pkg, it runs, and it is checked from outside, with no ARexx anywhere

Set 2026-09-18. ARexx is never a requirement: hosted AROS installs none by
default, and macOS and Windows have none at all. **Met 2026-09-18 within its agreed line**: steps 1,
2, 4, 5 and 6, and step 3 on AROS and macOS. The Windows run was set outside the finish line from the start; the kit
waits for it.

1. **macOS publishes** a real AROS application as a signed mountable image,
   with `Depends` on a system library published as a component.
2. **On hosted AROS with no ARexx installed**, an AmigaDOS script installs it
   with dependency resolution, places and mounts the image, runs the
   application from it with its output compared on the host, upgrades, rolls
   back, and removes the library once it is an orphan.
3. **One command-line contract**, machine-readable output and exit codes by
   class of failure, is identical on AROS, macOS and Windows.
4. **A tampered image and a badly signed dependency are refused**, and the
   application still runs once Pkg is removed.
5. Where Regina is installed by Pkg, the same sequence through the `PKG` port
   gives identical results.
6. **A skill for the agents who drive Pkg**, `skills/pkg/SKILL.md`: few people
   will run these commands by hand, so the agent acting for them needs the
   contract, the everyday tasks, what to do with each refusal class, and the
   decisions that stay with the person (a new key, a downgrade, removing
   orphans). Added 2026-09-18 at the owner's request.

| | Milestone | Done when |
|---|---|---|
| **M1 ✓** | **The contract.** Done 2026-09-18. Exit codes by class, `MACHINE` output, the port's RC equal to the class code | `tests/e2e.sh` on macOS, `tests/aros-contract.sh` identical on macOS and hosted AROS, `tests/goal.sh` with RC 12 and 14 |
| **M2 ✓** | **Image route and dependencies on macOS.** Done 2026-09-18. An `image` kind writing FFS volumes, `Depends` resolution `[PKG2]`, orphans and `REMOVE ORPHANS` | `tests/deps.sh`, 44 checks; `tests/image.sh`, 12 checks against amitools |
| **M3 ✓** | **The sequence on hosted AROS, without ARexx.** Done 2026-09-18. Guru and identify.library from the AROS sources, the FFS handler as a component | `tests/goal2.sh`, 51 checks, steps 1, 2 and 4, and step 3 on AROS and macOS |
| M4 | **Windows, and ARexx equivalence.** Built 2026-09-18: the Win32 host layer, `pkg.exe` with mingw-w64, and `build/pkg-test-kit.zip` for Windows, macOS and Linux (87 checks with PowerShell 7 on macOS). Step 5 done: `tests/aros-contract.sh` runs the sequence through the `PKG` port with Regina installed by Pkg, 152 checks. Rerun 2026-09-19 with Pkg 1.1: `tests/goal2.sh` 54 checks, `tests/aros-contract.sh` 174, `tests/goal.sh` 25, the kit 99 with PowerShell 7 on macOS, all without failure. Waiting: the kit's run on Windows | Step 3 on Windows when the owner runs the kit |
