<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# PORT

Serve every verb on an ARexx port (AROS).
```
Pkg PORT [<portname>]
```

## What it does

On AROS, keeps running and serves every Pkg command on an ARexx port, `PKG`
by default. A script sends the command line as a message; on success
`RESULT` holds what the command printed, on refusal `RC` is the exit class
and `LASTERROR` the refusal text. Nothing else needs ARexx: the same verbs
work from AmigaDOS scripts with `If ERROR` and `$RC`.

```amigados
Run >NIL: Pkg PORT
```

```rexx
/* install and read the answer */
ADDRESS PKG
OPTIONS RESULTS
'INSTALL helloworld ROOT SYS: CHANNEL DEPOT:channel MACHINE'
IF RC ~= 0 THEN SAY 'refused, class' RC
ELSE SAY RESULT
```

## Related

[Reference: the ARexx port](../reference.md#the-arexx-port),
[Pkg with an AI assistant](../agents.md).
