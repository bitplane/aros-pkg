<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Pkg with an AI assistant

If you use Claude Code, Cursor, Codex or another coding agent, you do not
have to learn Pkg's commands: the agent runs them for you. Pkg ships a
*skill*, a document written for the agent, that teaches it the commands, the
habits (try with `DRYRUN` first, read the machine output, never work around
a refusal) and the decisions it must leave to you. Give the agent that
document and ask in your own words.

## Where the skill is

`skills/pkg/SKILL.md` in this repository. `make install` copies it to
`~/.local/share/pkg/skills/pkg/SKILL.md` beside the tool.

## Loading it

**Claude Code.** Skills are loaded from `.claude/skills/<name>/SKILL.md` in
a project, or from `~/.claude/skills/<name>/SKILL.md` for every project.
Copy the directory:

```sh
mkdir -p ~/.claude/skills
cp -R skills/pkg ~/.claude/skills/pkg
```

From then on any request about packaging, publishing, installing or updating
AROS software triggers it, or you name it: `/pkg publish ~/dev/MyTool to my
channel`.

**Any other agent.** Paste the content of `SKILL.md` at the start of the
conversation, or point the agent at the file. It is plain Markdown with no
tool-specific syntax.

## What to ask

Plain requests work; the skill supplies the commands and the checks:

- "Publish `~/dev/MyTool` to my channel at `~/channel` and tell me the
  install command for an AROS machine."
- "Install `lua` from `https://aros-pkg.azurewebsites.net/contrib-nightly`
  into `~/aros-root` for x86_64, then verify it."
- "Check `~/aros-root` for updates and update everything, dry run first."
- "Why did this INSTALL refuse?" (paste the output; the exit code and the
  `next:` line tell the agent what happened)
- "Why does Game not find `SDL2.library`?" (the agent runs `RESOLVE` on the
  program and reads the verdicts)
- "Put Pkg itself on my AROS machine" (the agent builds the channel with
  `make aros-channel` and tells you the `Execute ... Install-Pkg` line)

## What the agent will not do

The skill forbids the agent to take four decisions that are yours:

- accept a new publisher key (`ACCEPTKEY`) after a key refusal (exit 14);
- downgrade (`DOWNGRADE`) unless you asked for that version;
- work around an integrity or signature refusal (exit 12, 13);
- copy a `Pkg` binary onto an AROS machine by hand instead of installing it
  through a channel.

When one of those comes up, the agent stops and asks. Pkg helps: its
refusals never print a command that overrides a safeguard, and its machine
output (`MACHINE`) gives the agent `result:`, `class:`, `code:` and `next:`
instead of prose to guess from ([reference](reference.md#machine-readable-output)).

## For programs that embed Pkg

An agent's tool adapter, an installer or a graphical front end links
`libpkg` instead of running the command: `include/pkg.h` lists every
operation and every field it answers. See
[Developing Pkg](development.md#as-a-library).
