/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * The goal sequence, driven through the PKG ARexx port and nothing else.
 * Every step checks RC and the facts it depends on, and the script exits with
 * 10 at the first disagreement, printing what it expected and what it got.
 */

options results
parse arg mode .

root = 'MacRW:sys'
ch   = 'MacRW:channel'

/* The port is opened by `Run Pkg PORT`, which may still be starting. */
do i = 1 to 60 while ~show('P', 'PKG')
    address command 'C:Wait 1'
end
if ~show('P', 'PKG') then call fail 'the PKG port never opened'
say 'port PKG is open'

address PKG

'INSTALL afsplus-handler VERSION 14 ROOT' root 'CHANNEL' ch
call expect_ok 'install 14', 'installed afsplus-handler 14'

'VERIFY afsplus-handler ROOT' root
call expect_ok 'verify 14', 'all intact'
/* SABOTAGE is the control run: the script expects the wrong version here and
 * must stop, and the caller must see an error. A gate that cannot fail proves
 * nothing, and this is the line that shows this one can. */
if mode = 'SABOTAGE' then call expect_version 13
call expect_version 14

'UPGRADE afsplus-handler VERSION 15 ROOT' root 'CHANNEL' ch
call expect_ok 'upgrade to 15', 'from 14 to 15'

'VERIFY afsplus-handler ROOT' root
call expect_ok 'verify 15', 'all intact'
call expect_version 15

'ROLLBACK afsplus-handler ROOT' root 'CHANNEL' ch
call expect_ok 'rollback', 'from 15 to 14'
call expect_version 14

/* Negative controls, inside the same sequence. */
'INSTALL afsplus-handler VERSION 15 ROOT MacRW:other CHANNEL MacRW:tampered'
call expect_refused 'a tampered payload', 12, 'does not match its signed manifest'

'UPGRADE afsplus-handler VERSION 16 ROOT' root 'CHANNEL' ch
call expect_refused 'a substituted publisher key', 14, 'different key'
call expect_version 14

'QUIT'
say 'GOAL PASS'
exit 0

expect_ok: procedure expose rc result
    parse arg what, needle
    if rc ~= 0 then do
        'LASTERROR'
        call fail what 'returned' rc':' result
    end
    if needle ~= '' & pos(needle, result) = 0 then
        call fail what 'answered "'result'", which lacks "'needle'"'
    say 'ok' what
    return

expect_refused: procedure expose rc result
    parse arg what, code, reason
    if rc = 0 then call fail what 'was accepted'
    /* RC is the class code, the number the command line exits with. */
    if rc ~= code then call fail what 'was refused with RC' rc', not' code
    'LASTERROR'
    if pos(reason, result) = 0 then
        call fail what 'was refused for another reason: "'result'"'
    say 'ok' what 'refused with RC' code':' reason
    return

expect_version: procedure expose rc result root
    parse arg want
    'LIST ROOT' root
    if rc ~= 0 then call fail 'list returned' rc
    /* One line per package; blanks and newlines both separate words here. */
    flat = ' 'space(translate(result, ' ', '0a'x))' '
    if pos(' afsplus-handler' want 'device ', flat) = 0 then
        call fail 'expected afsplus-handler' want', the database says "'strip(flat)'"'
    say 'ok the database says afsplus-handler' want
    return

fail:
    parse arg why
    say 'GOAL FAIL:' why
    /* Only a port that exists is told to quit: an ADDRESS naming no port falls
     * through to the shell, which is how an early failure once reported success. */
    if show('P', 'PKG') then address PKG 'QUIT'
    exit 10
