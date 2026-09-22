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
# Before installing, it checks pkg.exe against Bootstrap/SHA256SUMS, whose
# signature ssh-keygen -Y verify checks with the key below (when the portal has
# one; Windows 10 and 11 carry ssh-keygen). $env:PKG_SKIP_VERIFY = 1 skips it.
# Running it again installs the newest Pkg over the old one.

$ErrorActionPreference = 'Stop'
$portal = '@@PORTAL@@'
$sshkey = '@@SSHKEY@@'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\pkg'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pkg-" + [guid]::NewGuid().ToString('N') + '.exe')

try {
    Invoke-WebRequest -UseBasicParsing -Uri "$portal/get/pkg/windows-x86_64" -OutFile $tmp
    if ($env:PKG_SKIP_VERIFY) {
        Write-Output 'Warning: PKG_SKIP_VERIFY is set, so this Pkg is not checked against its signed checksum.'
    }
    elseif (-not $sshkey) {
        Write-Output 'Note: this portal publishes no signed checksum list yet; the download is protected by https alone.'
    }
    else {
        if (-not (Get-Command ssh-keygen -ErrorAction SilentlyContinue)) {
            throw 'ssh-keygen is needed to check the download (the OpenSSH client of Windows); set PKG_SKIP_VERIFY=1 to install without the check'
        }
        $sums = "$tmp.sums"; $sig = "$tmp.sums.sig"; $signers = "$tmp.signers"
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$portal/pkg/Bootstrap/SHA256SUMS" -OutFile $sums
            Invoke-WebRequest -UseBasicParsing -Uri "$portal/pkg/Bootstrap/SHA256SUMS.sig" -OutFile $sig
            Set-Content -Path $signers -Value "pkg namespaces=`"aros-pkg-bootstrap`" $sshkey" -Encoding ascii
            Get-Content -Raw $sums | & ssh-keygen -Y verify -f $signers -I pkg -n aros-pkg-bootstrap -s $sig *> $null
            if ($LASTEXITCODE -ne 0) { throw 'the checksum list is not signed by the key this portal names; nothing was installed' }
            $want = (Get-Content $sums | Where-Object { ($_ -split '\s+')[1] -eq 'Bootstrap/windows-x86_64/pkg.exe' } | ForEach-Object { ($_ -split '\s+')[0] }) | Select-Object -First 1
            if (-not $want) { throw 'the signed checksum list names no Bootstrap/windows-x86_64/pkg.exe' }
            if ((Get-FileHash -Algorithm SHA256 $tmp).Hash.ToLowerInvariant() -ne $want) { throw 'the download does not match its signed checksum; nothing was installed' }
            Write-Output 'Checked: the download matches the checksum signed by the portal''s bootstrap key.'
        }
        finally { Remove-Item -Force -ErrorAction SilentlyContinue $sums, $sig, $signers }
    }
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

# Optional personal environment registration. No root is inferred from Windows.
$envName = $env:PKG_ENV_NAME
$envRoot = $env:PKG_ENV_ROOT
$envDefault = $env:PKG_ENV_DEFAULT -eq '1'
$envConfig = Join-Path $env:APPDATA 'aros-pkg\environments.conf'
if ($envName -or $envRoot) {
    if (-not $envName -or -not $envRoot) { throw 'set both PKG_ENV_NAME and PKG_ENV_ROOT to register an environment' }
}
elseif (-not $env:PKG_NO_ENV -and [Environment]::UserInteractive -and
        -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected -and
        -not ([Environment]::GetCommandLineArgs() -match '^-(NonInteractive|NonI)$')) {
    Write-Output 'Optional environments let pkg find a package root without ROOT on each command.'
    Write-Output "Personal configuration: $envConfig"
    if ((Read-Host 'Configure an environment? [y/N]') -match '^(y|yes)$') {
        $envName = Read-Host 'Environment name'
        $envRoot = Read-Host 'Absolute path of the package root'
        if (-not $envName -or -not $envRoot) { throw 'environment name and root are required' }
        $envDefault = (Read-Host 'Use this environment by default? [y/N]') -match '^(y|yes)$'
    }
}
if ($envName) {
    Write-Output "Registering $envName with root $envRoot in $envConfig"
    & (Join-Path $dir 'pkg.exe') ENV ADD $envName ROOT $envRoot
    if ($LASTEXITCODE -ne 0) { throw 'pkg was installed; environment registration failed' }
    if ($envDefault) {
        & (Join-Path $dir 'pkg.exe') ENV DEFAULT $envName
        if ($LASTEXITCODE -ne 0) { throw 'environment registered; setting its default failed' }
    }
}
