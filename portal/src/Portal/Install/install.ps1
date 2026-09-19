# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Installs Pkg, the AROS package tool, on Windows, where you can run it by name.
# Served by the package portal:
#
#   irm @@PORTAL@@/install.ps1 | iex
#
# It downloads pkg.exe into %LOCALAPPDATA%\Programs\pkg, checks that it runs
# ("pkg HELP"), and adds that folder to your user Path when it is not there.
# Running it again installs the newest Pkg over the old one.

$ErrorActionPreference = 'Stop'
$portal = '@@PORTAL@@'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\pkg'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pkg-" + [guid]::NewGuid().ToString('N') + '.exe')

try {
    Invoke-WebRequest -UseBasicParsing -Uri "$portal/get/pkg/windows-x86_64" -OutFile $tmp
    $help = & $tmp HELP 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'the downloaded Pkg does not run on this computer' }
    $version = (($help | Select-Object -First 1).ToString() -split ' ')[1]

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Move-Item -Force -Path $tmp -Destination (Join-Path $dir 'pkg.exe')
}
finally {
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @($userPath -split ';' | Where-Object { $_ -ne '' })
if ($entries -notcontains $dir) {
    [Environment]::SetEnvironmentVariable('Path', (($entries + $dir) -join ';'), 'User')
    $env:Path = "$env:Path;$dir"
    Write-Output "Added $dir to your Path; open a new terminal to use it everywhere."
}
Write-Output "pkg $version installed in $dir; run: pkg HELP"
