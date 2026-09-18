#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Build the portal for Linux with the static Pkg beside it, and deploy it to
# the App Service web app. Settings (keys, paths) live in the web app, not here.
#
#   sh portal/tools/deploy-azure.sh [<web app>] [<resource group>]

set -eu
app=${1:-aros-pkg}
rg=${2:-rg-aros}
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
dotnet=${DOTNET:-dotnet}
out="$here/publish"

make -C "$repo" build/pkg-linux-x86_64
rm -rf "$out"
"$dotnet" publish "$here/src/Portal/Portal.csproj" -c Release -r linux-x64 --self-contained false -o "$out/app"
cp "$repo/build/pkg-linux-x86_64" "$out/app/pkg"
( cd "$out/app" && rm -f ../portal.zip && zip -qr ../portal.zip . )
echo "deploy-azure: $(du -h "$out/portal.zip" | cut -f1) to $app in $rg"
az webapp deploy -g "$rg" -n "$app" --src-path "$out/portal.zip" --type zip --restart true -o none
echo "deploy-azure: deployed; health: $(curl -s --max-time 60 "https://$app.azurewebsites.net/health")"
