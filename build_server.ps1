# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Configure
)

$root = $PSScriptRoot
$buildScript = Join-Path $root 'scripts\build.ps1'

if (-not (Test-Path $buildScript)) {
    throw "Build script not found: $buildScript"
}

$buildArgs = @{
    Target = 'aoc_server'
    Config = $Config
}
if ($Clean) { $buildArgs['Clean'] = $true }
if ($Configure) { $buildArgs['Configure'] = $true }

# Build the main server target via the portable build driver
& $buildScript @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Also compile the EAC popup mover (small C# helper).
# Kill any running instance first so the output file is not locked.
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$cs  = Join-Path $root 'src\eac_popup_mover.cs'
$out = Join-Path $root "dist\$Config\eac_popup_mover.exe"
if ((Test-Path $csc) -and (Test-Path $cs)) {
    Get-Process 'eac_popup_mover' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "Building eac_popup_mover.exe ..."
    $outDir = Split-Path $out
    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    Push-Location $outDir
    & $csc /target:winexe /nologo /out:$out $cs
    Pop-Location
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "eac_popup_mover.exe failed to compile (non-fatal)"
    }
}
exit 0
