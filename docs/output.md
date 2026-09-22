<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# What pkg says, and how it looks

Every line pkg prints has a role, and the role decides how it is drawn. The
library never draws anything: it hands the front end a line with its role
(`pkg_sink.line`), and `src/pkg_style.c` is the only place that turns a role
into ink. `examples/lines.c` draws one line of every kind, so a change to
the palette is judged in a glance:

```
make build/example-lines && ./build/example-lines
```

## The roles

| | |
|---|---|
| result | what the command did: `installed hello 1.2 into SYS:` |
| problem | a result that is bad news: `2 of 3 packages damaged` |
| detail | the figures under a result: `3 files, signed by 26ffb2bc` |
| item | one file or package under a result, its first word saying what happened to it |
| note | a remark worth reading, in passing |
| hint | what usually comes next |
| warning | it happened, and something in it deserves a second look |
| question | a question waiting for an answer, on the same line |
| refusal | why the command was refused, and nothing changed |
| next | the step after a refusal |
| head, row, end | a table, its columns sized once the rows are all in |
| progress | the activity line, drawn in place: [docs/activity.md](activity.md) |

## The palette

Four meanings carry a colour. Everything else is the terminal's own ink, so
that a person reads an answer instead of decoding a rainbow.

| | | |
|---|---|---|
| done | the project's purple | a result, and the words in an item or a table cell that say the thing worked |
| refused | red | a refusal, a problem, and the words that say something is missing or changed |
| watch | yellow | a warning, and the words that say something was left alone |
| quiet | dim | details, notes, hints and next steps |
| asked | bold | a question: the one line a person has to read before typing |

A question is the only line with no newline of its own: the answer is typed
after it.

On the AROS console there are no colours to give, only the screen's pens,
and only pens 0 to 3 differ on the standard palette. There the same order of
attention is kept in the ink that console has: bold for a result and a
question, italic for what is quiet, inverse video for a refusal.

## Where the colour goes

- Piped, redirected or captured output is plain: no marks, `warning:` and
  `next:` spelled out, columns kept, so a script can read it.
- `PKG_COLOR=always` or `never` decides it outright. `NO_COLOR` and
  `TERM=dumb` turn it off.
- `MACHINE` output has no roles at all: it is `key: value` records, and a
  question is the record `question`, asked of nobody.
- A `LOG` file gets the plain text of every line, never the escapes.

## The methods

A front end says things of its own, outside the library's lines: which root
it chose, a question, a path. Those go through the same roles, through one
method each, so that what a question looks like is decided in one place:

```c
say_asked("Which root is this operation for? Its number:");
say_quiet("1. aros: /Users/you/AROS/System (personal configuration)");
say_noted("no environment is registered");
say_done("environment configuration saved: %s", path);
```

A program that links libpkg does the same with its own `pkg_sink.line`: it
receives the role and draws it its own way, a gauge in a window where pkg
draws a line in a terminal.
