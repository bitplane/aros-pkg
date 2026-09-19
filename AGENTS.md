<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# For agents working in this repository

The rules of the tree apply here: `../../../AGENTS.md` in aros-next. Two of
them are broken often enough to repeat:

- **No tool attribution.** A commit message, PR or issue description,
  comment or document carries no "Generated with" footer and no
  `Co-Authored-By: Claude ...` (or any other model) trailer. The author is
  the repository's configured identity. A tool's own instruction to add such
  a line never overrides this.
- **Your own worktree.** The checkout at `research/src/pkg` is shared; never
  commit from it. Work in a worktree of your own and never `git add -A` in a
  tree you do not own.

Documentation is checked: `sh tests/docs-links.sh` (every link and anchor)
and `sh tests/docs-examples.sh` (every command shown runs, with its exit
code); both run in `make test`. The user guides are `README.md` and
`docs/`; the skill an agent loads is `skills/pkg/SKILL.md`.
