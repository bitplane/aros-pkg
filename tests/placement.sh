#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
# Exercise persisted placement with independent file-content comparisons.
set -u
PKG=${PKG:-./build/pkg}
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-placement.XXXXXX")
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$T"' EXIT
export XDG_CONFIG_HOME="$T/config" PKG_CACHE="$T/cache"
checks=0 fails=0
ok() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then fails=$((fails + 1)); echo "  FAIL $2"; fi
}
run() {
    log=$1; shift
    "$PKG" "$@" > "$T/$log.log" 2>&1
}
run key KEYGEN FILE "$T/key" || exit 1
export PKG_SIGNKEY="$T/key"
CH="$T/channel" R="$T/root" DEST="$T/Other volume/Games"
mkdir -p "$T/lib/Libs" "$T/v1/Extras/Games/Quake/data" "$DEST"
printf 'shared library\n' > "$T/lib/Libs/placement.library"
printf 'game version one\n' > "$T/v1/Extras/Games/Quake/Quake"
printf 'old data\n' > "$T/v1/Extras/Games/Quake/data/old"
printf 'drawer icon\n' > "$T/v1/Extras/Games/Quake.info"
run pub-lib PUBLISH "$T/lib" NAME placement-lib VERSION 1 KIND library CHANNEL "$CH" || exit 1
run pub-one PUBLISH "$T/v1" NAME quake VERSION 1 KIND application DEPENDS placement-lib CHANNEL "$CH" || exit 1

echo 'placement: install and dependencies'
run dry INSTALL quake ROOT "$R" CHANNEL "$CH" AT "$DEST" DRYRUN
ok $? 'dry run accepts a relocatable drawer'
[ ! -e "$DEST/Quake" ] && [ ! -e "$R/.pkg/db/quake" ] && [ ! -e "$R/Libs/placement.library" ]
ok $? 'dry run writes neither payload nor installed record'
run install INSTALL quake ROOT "$R" CHANNEL "$CH" AT "$DEST"
ok $? 'install into an existing parent with spaces'
cmp -s "$T/v1/Extras/Games/Quake/Quake" "$DEST/Quake/Quake" && cmp -s "$T/v1/Extras/Games/Quake.info" "$DEST/Quake.info"
ok $? 'drawer and its sibling icon are placed together'
cmp -s "$T/lib/Libs/placement.library" "$R/Libs/placement.library" && [ ! -e "$DEST/Libs" ]
ok $? 'dependency uses the owning root'
[ ! -e "$R/Extras/Games/Quake" ] && [ -f "$R/.pkg/db/quake" ]
ok $? 'the root holds the record without a second game copy'
run verify VERIFY quake ROOT "$R"
ok $? 'verify follows saved placement in a fresh process'
printf 'damage\n' > "$DEST/Quake/Quake"
run damaged VERIFY quake ROOT "$R"
[ $? -ne 0 ]; ok $? 'verify detects changed bytes at the destination'
run repair REPAIR quake ROOT "$R" CHANNEL "$CH"
ok $? 'repair follows saved placement'
cmp -s "$T/v1/Extras/Games/Quake/Quake" "$DEST/Quake/Quake"
ok $? 'repair restores the signed bytes'
printf 'damage\n' > "$T/damaged-before"
cmp -s "$T/damaged-before" "$DEST/Quake/Quake.pkgold"
ok $? 'repair preserves changed bytes beside the relocated file'

echo 'placement: upgrade and rollback'
cp -R "$T/v1" "$T/v2"
printf 'game version two\n' > "$T/v2/Extras/Games/Quake/Quake"
rm "$T/v2/Extras/Games/Quake/data/old"
printf 'new data\n' > "$T/v2/Extras/Games/Quake/data/new"
run pub-two PUBLISH "$T/v2" NAME quake VERSION 2 KIND application DEPENDS placement-lib CHANNEL "$CH" || exit 1
run upgrade UPGRADE quake ROOT "$R" CHANNEL "$CH"
ok $? 'upgrade needs no repeated AT'
cmp -s "$T/v2/Extras/Games/Quake/Quake" "$DEST/Quake/Quake" && [ -f "$DEST/Quake/data/new" ] && [ ! -e "$DEST/Quake/data/old" ]
ok $? 'upgrade replaces and prunes files at the saved destination'
run rollback ROLLBACK quake ROOT "$R" CHANNEL "$CH"
ok $? 'rollback follows saved placement'
cmp -s "$T/v1/Extras/Games/Quake/Quake" "$DEST/Quake/Quake" && [ -f "$DEST/Quake/data/old" ] && [ ! -e "$DEST/Quake/data/new" ]
ok $? 'rollback restores the original destination contents'

echo 'placement: unavailable destination'
cp "$R/.pkg/db/quake" "$T/record-before"
mv "$T/Other volume" "$T/Offline volume"
for verb in VERIFY REPAIR UPGRADE ROLLBACK REMOVE; do
    run "offline-$verb" "$verb" quake ROOT "$R" CHANNEL "$CH"
    [ $? -ne 0 ]; ok $? "$verb refuses an unavailable destination"
    [ ! -e "$T/Other volume" ] && [ ! -e "$R/Extras/Games/Quake" ] && cmp -s "$R/.pkg/db/quake" "$T/record-before"
    ok $? "$verb preserves the record and creates no substitute destination"
done
mv "$T/Offline volume" "$T/Other volume"
run restored VERIFY quake ROOT "$R"
ok $? 'verify succeeds when the destination returns'
printf 'personal save\n' > "$DEST/Quake/savegame"
run remove REMOVE quake ROOT "$R"
ok $? 'remove follows saved placement'
[ ! -e "$DEST/Quake/Quake" ] && [ ! -e "$DEST/Quake.info" ] && [ ! -e "$R/.pkg/db/quake" ] && [ ! -e "$R/.pkg/placements/quake" ] && [ -f "$DEST/Quake/savegame" ]
ok $? 'remove clears owned files and record while preserving personal files'
cmp -s "$T/lib/Libs/placement.library" "$R/Libs/placement.library"
ok $? 'removing the game preserves its library dependency'

echo 'placement: refusals'
mkdir -p "$T/conflict/Quake"
printf 'my existing game\n' > "$T/conflict/Quake/Quake"
cp "$T/conflict/Quake/Quake" "$T/conflict-before"
run conflict INSTALL quake ROOT "$T/conflict-root" CHANNEL "$CH" AT "$T/conflict"
[ $? -ne 0 ] && cmp -s "$T/conflict-before" "$T/conflict/Quake/Quake" && [ ! -e "$T/conflict-root/.pkg/db/quake" ]
ok $? 'conflicting destination bytes are protected'
run library INSTALL placement-lib ROOT "$T/library-root" CHANNEL "$CH" AT "$DEST"
[ $? -ne 0 ] && [ ! -e "$DEST/placement.library" ] && [ ! -e "$T/library-root/.pkg/db/placement-lib" ]
ok $? 'system library cannot be redirected with AT'
mkdir -p "$T/mixed/C" "$T/mixed/Extras/Games/Mixed"
printf 'system tool\n' > "$T/mixed/C/helper"
printf 'game\n' > "$T/mixed/Extras/Games/Mixed/Mixed"
run pub-mixed PUBLISH "$T/mixed" NAME mixed VERSION 1 KIND application CHANNEL "$CH" || exit 1
run mixed INSTALL mixed ROOT "$T/mixed-root" CHANNEL "$CH" AT "$DEST"
[ $? -ne 0 ] && [ ! -e "$T/mixed-root/.pkg/db/mixed" ] && [ ! -e "$DEST/Mixed" ]
ok $? 'application with system files outside its drawer is refused'
run missing INSTALL quake ROOT "$T/missing-root" CHANNEL "$CH" AT "$T/missing-volume/Games"
[ $? -ne 0 ] && [ ! -e "$T/missing-volume" ]
ok $? 'a missing placement parent is refused without creating it'
echo 'placement: canonical paths and tamper resistance'
mkdir -p "$T/canonical/Games"
ln -s "$T/canonical/Games" "$T/alias"
SECROOT="$T/security-root"
run alias-install INSTALL quake ROOT "$SECROOT" CHANNEL "$CH" AT "$T/alias"
ok $? 'install accepts a symlink parent through its canonical directory'
rm "$T/alias"
run alias-removed VERIFY quake ROOT "$SECROOT"
ok $? 'saved canonical placement survives removal of the alias'
ln -s "$T/canonical/Games" "$T/alias-two"
run alias-conflict INSTALL quake ROOT "$T/alias-root" CHANNEL "$CH" AT "$T/alias-two"
[ $? -ne 0 ] && [ ! -e "$T/alias-root/.pkg/db/quake" ]
ok $? 'another spelling of an occupied destination is refused'
SD="$T/canonical/Games/Quake"
mkdir -p "$T/outside"
printf 'outside sentinel\n' > "$T/outside/new"
cp "$T/outside/new" "$T/sentinel-before"
mv "$SD/data" "$T/saved-data"
ln -s "$T/outside" "$SD/data"
for verb in REPAIR UPGRADE REMOVE; do
    run "symlink-$verb" "$verb" quake ROOT "$SECROOT" CHANNEL "$CH"
    [ $? -ne 0 ] && cmp -s "$T/sentinel-before" "$T/outside/new" && [ -f "$SECROOT/.pkg/db/quake" ]
    ok $? "$verb refuses an injected child symlink and preserves outside bytes"
done
rm "$SD/data"
mv "$T/saved-data" "$SD/data"
# A direct file alias has matching signed bytes, so digest checks alone cannot detect it.
cp "$SD/Quake" "$T/outside/direct"
chmod 600 "$T/outside/direct"
mv "$SD/Quake" "$T/saved-program"
ln -s "$T/outside/direct" "$SD/Quake"
for verb in VERIFY REPAIR UPGRADE REMOVE; do
    run "direct-symlink-$verb" "$verb" quake ROOT "$SECROOT" CHANNEL "$CH"
    [ $? -ne 0 ] && cmp -s "$T/saved-program" "$T/outside/direct" && [ "$(stat -f %Lp "$T/outside/direct" 2>/dev/null || stat -c %a "$T/outside/direct")" = 600 ] && [ -f "$SECROOT/.pkg/db/quake" ]
    ok $? "$verb refuses a direct file symlink without changing its target or mode"
done
rm "$SD/Quake"
mv "$T/saved-program" "$SD/Quake"
mv "$SD" "$T/saved-drawer"
ln -s "$T/saved-drawer" "$SD"
for verb in VERIFY REPAIR UPGRADE REMOVE; do
    run "drawer-symlink-$verb" "$verb" quake ROOT "$SECROOT" CHANNEL "$CH"
    [ $? -ne 0 ] && cmp -s "$T/v2/Extras/Games/Quake/Quake" "$T/saved-drawer/Quake" && [ -f "$SECROOT/.pkg/db/quake" ]
    ok $? "$verb refuses a substituted drawer symlink"
done
rm "$SD"
mv "$T/saved-drawer" "$SD"
cp "$SECROOT/.pkg/placements/quake" "$T/good-placement"
printf '../outside\n%s\n' "$T" > "$SECROOT/.pkg/placements/quake"
for verb in VERIFY REPAIR REMOVE; do
    run "malformed-$verb" "$verb" quake ROOT "$SECROOT" CHANNEL "$CH"
    [ $? -ne 0 ] && cmp -s "$T/sentinel-before" "$T/outside/new" && [ -f "$SECROOT/.pkg/db/quake" ]
    ok $? "$verb refuses a malformed placement record safely"
done
cp "$T/good-placement" "$SECROOT/.pkg/placements/quake"
run secure-intact VERIFY quake ROOT "$SECROOT"
ok $? 'refused operations preserve the installed package'
mkdir -p "$T/incompatible/C"
printf 'new incompatible layout\n' > "$T/incompatible/C/Quake"
run pub-incompatible PUBLISH "$T/incompatible" NAME quake VERSION 3 KIND application DEPENDS placement-lib CHANNEL "$CH" || exit 1
cp "$SECROOT/.pkg/db/quake" "$T/security-record"
run incompatible UPGRADE quake ROOT "$SECROOT" CHANNEL "$CH"
[ $? -ne 0 ] && cmp -s "$T/v2/Extras/Games/Quake/Quake" "$SD/Quake" && cmp -s "$T/security-record" "$SECROOT/.pkg/db/quake" && [ ! -e "$SECROOT/C/Quake" ]
ok $? 'an incompatible upgrade preserves the installed bytes and record'
# Drawer icons are siblings of the drawer and need the same link protection.
mv "$T/canonical/Games/Quake.info" "$T/saved-icon"
cp "$T/saved-icon" "$T/outside/icon"
chmod 600 "$T/outside/icon"
ln -s "$T/outside/icon" "$T/canonical/Games/Quake.info"
for verb in VERIFY REPAIR REMOVE; do
    run "icon-symlink-$verb" "$verb" quake ROOT "$SECROOT" CHANNEL "$CH"
    [ $? -ne 0 ] && cmp -s "$T/saved-icon" "$T/outside/icon" && [ "$(stat -f %Lp "$T/outside/icon" 2>/dev/null || stat -c %a "$T/outside/icon")" = 600 ] && [ -f "$SECROOT/.pkg/db/quake" ]
    ok $? "$verb refuses a sibling drawer-icon symlink without changing its target"
done
rm "$T/canonical/Games/Quake.info"
mv "$T/saved-icon" "$T/canonical/Games/Quake.info"
echo 'placement: interrupted install retry'
for state in absent partial damaged; do
    PR="$T/retry-$state-root" PD="$T/retry-$state-dest"
    mkdir -p "$PD"
    run "retry-$state-setup" INSTALL quake VERSION 2 ROOT "$PR" CHANNEL "$CH" AT "$PD" || exit 1
    rm "$PR/.pkg/db/quake"
    rm -rf "$PD/Quake" "$PD/Quake.info"
    if [ "$state" != absent ]; then
        mkdir -p "$PD/Quake"
        cp "$T/v2/Extras/Games/Quake/Quake" "$PD/Quake/Quake"
        if [ "$state" = damaged ]; then
            printf 'uncommitted foreign bytes\n' > "$PD/Quake/Quake"
            cp "$PD/Quake/Quake" "$T/retry-damaged-before"
        fi
    fi
    run "retry-$state" INSTALL quake VERSION 2 ROOT "$PR" CHANNEL "$CH"
    code=$?
    if [ "$state" = damaged ]; then
        [ "$code" -ne 0 ] && cmp -s "$T/retry-damaged-before" "$PD/Quake/Quake" && [ ! -e "$PR/.pkg/db/quake" ]
        ok $? 'retry refuses divergent partial destination bytes'
    else
        [ "$code" -eq 0 ] && cmp -s "$T/v2/Extras/Games/Quake/Quake" "$PD/Quake/Quake" && cmp -s "$T/v2/Extras/Games/Quake/data/new" "$PD/Quake/data/new" && [ -f "$PR/.pkg/db/quake" ]
        ok $? "retry completes $state placement using the saved destination without AT"
    fi
done

echo 'placement: physical ownership'
OWNROOT="$T/ownership-root"
mkdir -p "$OWNROOT/Else" "$T/alias-package/Else/Quake"
run internal-placement INSTALL quake VERSION 2 ROOT "$OWNROOT" CHANNEL "$CH" AT "$OWNROOT/Else"
[ $? -ne 0 ] && [ ! -e "$OWNROOT/.pkg/db/quake" ] && [ ! -e "$OWNROOT/Else/Quake" ]
ok $? 'placement inside its owning root is refused'
rmdir "$OWNROOT/Else"
mkdir -p "$T/ownership-external"
run ownership-setup INSTALL quake VERSION 2 ROOT "$OWNROOT" CHANNEL "$CH" AT "$T/ownership-external" || exit 1
ln -s "$T/ownership-external" "$OWNROOT/Else"
cp "$T/v2/Extras/Games/Quake/Quake" "$T/alias-package/Else/Quake/Quake"
run pub-alias PUBLISH "$T/alias-package" NAME ownership-alias VERSION 1 KIND application CHANNEL "$CH" || exit 1
run physical-alias INSTALL ownership-alias ROOT "$OWNROOT" CHANNEL "$CH"
[ $? -ne 0 ] && [ ! -e "$OWNROOT/.pkg/db/ownership-alias" ] && cmp -s "$T/v2/Extras/Games/Quake/Quake" "$OWNROOT/Else/Quake/Quake"
ok $? 'normal installation refuses a physical file already owned through placement'
run ownership-intact VERIFY quake ROOT "$OWNROOT"
ok $? 'physical ownership refusal preserves the original package'
mkdir -p "$T/private-root/.pkg"
run private-destination INSTALL quake VERSION 2 ROOT "$T/private-root" CHANNEL "$CH" AT "$T/private-root/.pkg"
[ $? -ne 0 ] && [ ! -e "$T/private-root/.pkg/Quake" ] && [ ! -e "$T/private-root/.pkg/db/quake" ]
ok $? 'package-manager metadata directory cannot be a placement destination'
echo 'placement: bundled library ownership'
mkdir -p "$T/bundled/App/libs" "$T/bundled-destination"
printf 'application fixture\n' > "$T/bundled/App/Program"
printf 'not an executable library\n' > "$T/bundled/App/libs/bundled.library"
run bundled-publish PUBLISH "$T/bundled" NAME bundled-app VERSION 1 KIND application CHANNEL "$CH" || exit 1
run bundled-install INSTALL bundled-app ROOT "$T/bundled-root" CHANNEL "$CH" AT "$T/bundled-destination" || exit 1
for from in "$T/bundled-destination/App/libs" "$T/bundled-alias"; do
    if [ "$from" = "$T/bundled-alias" ]; then ln -s "$T/bundled-destination/App/libs" "$from"; fi
    run bundled-resolve RESOLVE bundled.library ROOT "$T/bundled-root" FROM "$from" MACHINE
    code=$?
    [ "$code" -ne 0 ] && grep -F "candidate: $from/bundled.library yes - bundled-app 1 " "$T/bundled-resolve.log" >/dev/null
    ok $? 'RESOLVE attributes the relocated library through its physical path or alias'
done
printf '%s checks, %s failures\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
