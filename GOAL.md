<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Goal: Pkg delivers the platform to itself, and an agent watches

Set 2026-09-18. One sequence that passes or fails as a whole:

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
| M4 | **The agent.** The `PKG` ARexx port, and one script that drives steps 1 to 4 | The whole goal sequence passes |

Gates touched: `[PKG1]`, `[PKG6]`, `[PKG10]`, `[PKG11]`, `[PKG17]`, `[PKG18]`,
`[PKG19]`, `[PKG21]`, `[PKG22]`, `[PKG23]`, and the first component of
`[PKG9]`, all in the planning repository's `docs/features/packaging/spec.md`.
