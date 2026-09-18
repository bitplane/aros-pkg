# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Pkg's command-line contract, on Windows, macOS or Linux. Runs the sequence
# every host runs (steps.txt, the same file hosted AROS runs) against the
# channel in this kit, and compares each exit code and each MACHINE output
# byte for byte with what the reference build produced from the same channel
# (expected/). Then the checks that depend on the host: the signing key's
# permissions, a root named outside ASCII, and an image written here against
# the reference bytes. Writes report.txt next to this script.
#
#   Windows:        powershell -ExecutionPolicy Bypass -File run.ps1
#   macOS, Linux:   pwsh run.ps1        (PowerShell 7)

$ErrorActionPreference = 'Stop'
$kit  = $PSScriptRoot
$work = Join-Path $kit 'work'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory $work | Out-Null
# PowerShell 5.1, the one Windows ships, predates $IsWindows.
$onWindows = ($PSVersionTable.PSVersion.Major -lt 6) -or $IsWindows
$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLower()
if ($onWindows)  { $platform = 'windows-x86_64'; $exe = 'pkg.exe' }
elseif ($IsMacOS) { $platform = 'macos'; $exe = 'pkg' }
elseif ($arch -eq 'arm64') { $platform = 'linux-aarch64'; $exe = 'pkg' }
else { $platform = 'linux-x86_64'; $exe = 'pkg' }
$pkg = Join-Path (Join-Path (Join-Path $kit 'bin') $platform) $exe
$paths = [ordered]@{
    '{R3}' = (Join-Path $work 'root3'); '{R2}' = (Join-Path $work 'root2'); '{R}' = (Join-Path $work 'root')
    '{CH}' = (Join-Path $kit 'channel'); '{T}' = (Join-Path $kit 'tampered'); '{U}' = (Join-Path $kit 'unsigned')
}
$report = New-Object System.Collections.Generic.List[string]
$checks = 0; $fails = 0
function Check([bool]$good, [string]$what) {
    $script:checks++
    if ($good) { $script:report.Add("ok    $what") }
    else { $script:fails++; $script:report.Add("FAIL  $what"); Write-Host "  FAIL $what" }
}

function Invoke-Pkg([string[]]$argv, [hashtable]$env = @{}) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pkg
    $psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $psi.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    foreach ($k in $env.Keys) { $psi.EnvironmentVariables[$k] = $env[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return @{ Out = $outTask.Result; Err = $err; Code = $p.ExitCode }
}

function Normalise([string]$text) {
    foreach ($k in $paths.Keys) { $text = $text.Replace($paths[$k], $k) }
    return $text
}

$report.Add("pkg contract on Windows, $(Get-Date -Format s)")
$report.Add("os: $([System.Runtime.InteropServices.RuntimeInformation]::OSDescription), $arch, binary: bin/$platform/$exe")

# ---- the shared sequence ----------------------------------------------------
foreach ($line in Get-Content (Join-Path $kit 'steps.txt')) {
    if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
    $f = $line.Trim() -split '\s+'
    $name = $f[0]; $code = $f[1]
    if ($name -eq 'edit') {
        $target = Join-Path (Join-Path $paths['{R}'] 'C') 'Hello'
        [System.IO.File]::WriteAllBytes($target, [System.Text.Encoding]::ASCII.GetBytes("edited`n"))
        continue
    }
    $argv = @()
    foreach ($a in $f[2..($f.Length - 1)]) {
        foreach ($k in $paths.Keys) { $a = $a.Replace($k, $paths[$k]) }
        $argv += $a
    }
    $r = Invoke-Pkg $argv
    Check ($r.Code -eq [int]$code) "${name}: exit code $code (got $($r.Code))"
    if ($name.StartsWith('h_')) { continue }
    Check ($r.Err -eq '') "${name}: nothing on stderr"
    Check (-not $r.Out.Contains("`r")) "${name}: no carriage return in the output"
    $expected = [System.IO.File]::ReadAllText((Join-Path (Join-Path $kit 'expected') "$name.o"), (New-Object System.Text.UTF8Encoding($false)))
    $got = Normalise $r.Out
    Check ($got -ceq $expected) "${name}: output identical to the reference"
    if ($got -cne $expected) {
        $report.Add("      expected:"); $expected -split "`n" | ForEach-Object { $report.Add("        $_") }
        $report.Add("      got:");      $got -split "`n"      | ForEach-Object { $report.Add("        $_") }
    }
}

# ---- what depends on the host --------------------------------------------------
$key = Join-Path $work 'dev.key'
$r = Invoke-Pkg @('KEYGEN', 'FILE', $key)
Check ($r.Code -eq 0) "keygen writes a key"
if ($onWindows) {
    $acl = Get-Acl $key
    $who = @($acl.Access | ForEach-Object { $_.IdentityReference.Value })
    $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $report.Add("      key ACL: $($who -join ', '); inherited: $(-not $acl.AreAccessRulesProtected)")
    Check ($acl.AreAccessRulesProtected -and $who.Count -eq 1 -and ($who[0] -eq $me -or $who[0] -eq 'OWNER RIGHTS')) "the key is readable by its owner alone"
} else {
    $mode = [System.IO.File]::GetUnixFileMode($key)
    $report.Add("      key mode: $mode")
    Check ($mode -eq ([System.IO.UnixFileMode]::UserRead -bor [System.IO.UnixFileMode]::UserWrite)) "the key is readable by its owner alone, mode 0600"
}

$uni = Join-Path $work ([string]::Concat('racine-', [char]0x00E9, '-', [char]0x6F22))
$r = Invoke-Pkg @('INSTALL', 'hello', 'VERSION', '1.2', 'ROOT', $uni, 'CHANNEL', $paths['{CH}'])
Check ($r.Code -eq 0 -and (Test-Path -LiteralPath (Join-Path (Join-Path $uni 'C') 'Hello'))) "a root whose name is outside ASCII works"

$drawer = Join-Path $work 'drawer'
New-Item -ItemType Directory (Join-Path $drawer 'C') | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path (Join-Path $drawer 'C') 'Tool'), [System.Text.Encoding]::ASCII.GetBytes("tool`n"))
[System.IO.File]::WriteAllBytes((Join-Path $drawer 'Thumbs.db'), [byte[]](1, 2, 3))
$img = Join-Path $work 'tool.hdf'
$r = Invoke-Pkg @('IMAGE', $drawer, 'OUT', $img, 'NAME', 'Tool')
$sha = (Get-FileHash -Algorithm SHA256 $img).Hash.ToLower()
$want = (Get-Content (Join-Path (Join-Path $kit 'expected') 'image.sha256')).Trim()
Check ($r.Code -eq 0 -and $sha -eq $want) "IMAGE writes the same bytes as on macOS, Thumbs.db left out"

$report.Add("")
$report.Add("$checks checks, $fails failures")
[System.IO.File]::WriteAllLines((Join-Path $kit 'report.txt'), $report)
Write-Host "$checks checks, $fails failures. Report: $(Join-Path $kit 'report.txt')"
if ($fails -ne 0) { exit 1 } else { exit 0 }
