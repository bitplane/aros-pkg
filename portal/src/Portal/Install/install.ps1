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

# Optional personal environment registration. The suggested directory is an
# AROS package root; registration preserves files already in that directory.
function Register-PkgEnvironment {
    $envName = $env:PKG_ENV_NAME
    $envRoot = $env:PKG_ENV_ROOT
    $envDefault = $env:PKG_ENV_DEFAULT -eq '1'
    $envConfig = Join-Path $env:APPDATA 'aros-pkg\environments.conf'
    $suggestedRoot = Join-Path $HOME 'AROS\System'
    $interactive = -not $env:PKG_NO_ENV -and [Environment]::UserInteractive -and
        -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected -and
        -not ([Environment]::GetCommandLineArgs() -match '^-(NonInteractive|NonI)$')
    $explicit = [bool]($envName -or $envRoot)
    if (-not $explicit -and -not $interactive) { return }

    Write-Host 'Optional environments let pkg find a package root without ROOT on each command.'
    Write-Host "Personal configuration: $envConfig"
    if (-not $explicit -and (Read-Host 'Configure an environment? [y/N]') -notmatch '^(y|yes)$') { return }
    $prompt = -not $explicit
    while ($true) {
        if ($prompt) {
            Write-Host 'Press Enter to accept a suggestion. Type :cancel to keep pkg installed and skip setup.'
            $nameSuggestion = if ($envName -match '^[A-Za-z0-9_.-]{1,63}$') { $envName } else { 'aros' }
            $nameAnswer = Read-Host "Environment name [$nameSuggestion]"
            if ($nameAnswer -eq ':cancel') { return }
            $envName = if ($nameAnswer -eq '') { $nameSuggestion } else { $nameAnswer }
            $rootSuggestion = if ($envRoot) { $envRoot } else { $suggestedRoot }
            $rootAnswer = Read-Host "Absolute path of the package root [$rootSuggestion]"
            if ($rootAnswer -eq ':cancel') { return }
            $envRoot = if ($rootAnswer -eq '') { $rootSuggestion } else { $rootAnswer }
        }
        try {
            if ($envName -notmatch '^[A-Za-z0-9_.-]{1,63}$') {
                throw 'Use an environment name of 1 to 63 letters, digits, dots, underscores or hyphens.'
            }
            if (-not $envRoot -or $envRoot -ne $envRoot.Trim() -or
                $envRoot -match '[\x00-\x1f\x7f]' -or
                $envRoot -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+)') {
                throw 'Use an absolute Windows directory path, such as C:\AROS\System.'
            }
            if (Test-Path -LiteralPath $envRoot) {
                if (-not (Test-Path -LiteralPath $envRoot -PathType Container)) { throw 'The chosen root is a file. Choose a directory.' }
                $nonempty = $null -ne (Get-ChildItem -LiteralPath $envRoot -Force | Select-Object -First 1)
                if ($nonempty) {
                    Write-Host "This directory already contains files: $envRoot"
                    Write-Host 'Registration only records its location. Existing files are preserved.'
                    if ($interactive) {
                        if ((Read-Host 'Use this directory? [y/N]') -notmatch '^(y|yes)$') {
                            if ((Read-Host 'Choose another directory? [Y/n]') -match '^(n|no)$') { return }
                            $envRoot = $null
                            $prompt = $true
                            continue
                        }
                    }
                    elseif ($env:PKG_ENV_REUSE -ne '1') {
                        throw 'Set PKG_ENV_REUSE=1 to explicitly register this nonempty directory, or choose an empty directory.'
                    }
                }
            }
            else {
                Write-Host "Creating package root: $envRoot"
                [IO.Directory]::CreateDirectory($envRoot) | Out-Null
            }
            Write-Host "Registering $envName with root $envRoot in $envConfig"
            & (Join-Path $dir 'pkg.exe') ENV ADD $envName ROOT $envRoot
            if ($LASTEXITCODE -ne 0) { throw 'Environment registration failed; pkg itself is installed.' }
        }
        catch {
            if (-not $interactive) { throw }
            Write-Host $_.Exception.Message
            if ((Read-Host 'Try another name or root? [Y/n]') -match '^(n|no)$') { return }
            $prompt = $true
            continue
        }
        if ($interactive -and -not $explicit) {
            $envDefault = (Read-Host 'Use this environment by default? [Y/n]') -notmatch '^(n|no)$'
        }
        if ($envDefault) {
            & (Join-Path $dir 'pkg.exe') ENV DEFAULT $envName
            if ($LASTEXITCODE -ne 0) {
                if (-not $interactive) { throw 'Environment registered; setting its default failed.' }
                Write-Host "Environment registered. To retry selecting its default: pkg ENV DEFAULT $envName"
            }
        }
        return
    }
}
Register-PkgEnvironment
