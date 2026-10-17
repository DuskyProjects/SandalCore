# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  run_tests.ps1 - build all targets and run every dist\<Config>\test_*.exe
#
#  Usage (PowerShell):
#      .\scripts\run_tests.ps1                         # build + run
#      .\scripts\run_tests.ps1 -NoBuild                # run only
#      .\scripts\run_tests.ps1 -SaveBaseline FILE      # write summary to FILE
#      .\scripts\run_tests.ps1 -CompareTo FILE         # exit 1 if results differ
#      .\scripts\run_tests.ps1 -Heavy                  # also run the heavy tests (below)
#      .\scripts\run_tests.ps1 -Jobs 6                 # build at most 6 projects at a time (default: every core)
#
#  The heavy tests (test_worldd_region_load: 1 GB of ground, about a minute, wall-clock checks) are NOT in the default gate: on a PC that is also building or
#  playing a timing check can flip, and the PC hard-resets under load.  -Heavy runs them (G1 review B I1); the plan's last task and the lead do, alone, under the
#  heavy-job lock.  fixtures/test-baseline.txt does not list them.
#
#  A test that skipped part or all of its checks (no content.db, no ground file: it prints a skip/opt-in line and exits 0) is reported in a NOTE after
#  the summary; the compared lines stay "PASS exit=0" (review B I1).
#
#  Current test targets come from the CMake-generated manifest. Output is
#  retained in build\test-results\<Config>; failures print their diagnostics.
#  -TimeoutSeconds bounds each executable (default: 600 seconds).
# ============================================================================
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')] [string] $Config = 'Release',
    [switch] $NoBuild,
    [string] $SaveBaseline,
    [string] $CompareTo,
    [switch] $Heavy,
    [int]    $Jobs = 0,
    [ValidateRange(1,2147483)] [int] $TimeoutSeconds = 600,
    [string] $Filter = 'test_*',
    [string] $LogDirectory
)
$ErrorActionPreference = 'Stop'
if ($SaveBaseline -and $CompareTo) { throw "use -SaveBaseline or -CompareTo, not both" }
$root  = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$build = Join-Path $root 'build\x64'
$dist  = Join-Path $root "dist\$Config"
if (-not $LogDirectory) { $LogDirectory = Join-Path $root "build\test-results\$Config" }
$LogDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogDirectory)

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
# ~ CoA :: C Y B ~

if (-not $NoBuild) {
    # Drop stale test exes first: a test target dropped from CMake in a later
    # change would otherwise keep "passing" using its old binary. Test exes
    # link straight into dist\<Config> via CMAKE_RUNTIME_OUTPUT_DIRECTORY, so
    # MSBuild relinks the current ones below.
    Remove-Item (Join-Path $dist 'test_*.exe') -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Config $Config -Configure -Jobs $Jobs
    }
    $mArg = if ($Jobs -gt 0) { "/m:$Jobs" } else { '/m' }
    & $cmake --build $build --config $Config -- $mArg /nr:false /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

function Restore-ProcessEnvironment([string] $Name, $PreviousValue) {
    if ($null -eq $PreviousValue) {
        # PowerShell can bind a null string to an empty one; newer .NET keeps
        # that as an existing empty variable instead of removing it.
        Remove-Item -LiteralPath ("Env:" + $Name) -ErrorAction SilentlyContinue
    } else {
        [Environment]::SetEnvironmentVariable($Name, [string]$PreviousValue, 'Process')
    }
}

# Read stdout and stderr concurrently so a full pipe cannot deadlock a test.
function Invoke-TestExecutable([string] $Path) {
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $process.StartInfo.FileName = $Path
    $process.StartInfo.WorkingDirectory = $root
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    try {
        if (-not $process.Start()) { throw "could not start $Path" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) { $process.Kill() }
        $process.WaitForExit()
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($timedOut) { $output += "`nTIMEOUT after $TimeoutSeconds seconds`n" }
        [pscustomobject]@{ Code = $(if ($timedOut) { 'timeout' } else { $process.ExitCode }); Output = $output }
    } catch {
        [pscustomobject]@{ Code = 'start-error'; Output = $_.Exception.Message }
    } finally { $process.Dispose() }
}

$manifest = Join-Path $build "test-targets-$Config.txt"
if (Test-Path -LiteralPath $manifest) {
    $testPaths = @(Get-Content -LiteralPath $manifest | Where-Object { $_ })
} else {
    Write-Warning 'No CMake test manifest: executable discovery may include stale targets. Reconfigure CMake to generate it.'
    $testPaths = @(Get-ChildItem -LiteralPath $dist -Filter 'test_*.exe' | ForEach-Object { $_.FullName })
}
$fixtureTests = @('test_pc_spawn_diff','test_pawn_spawn_diff','test_pkt22_round_trip','test_pkt104_round_trip','test_name_update_bunch','test_replay_mutator')
$needsFixtures = @($testPaths | Where-Object {
    $name = [IO.Path]::GetFileNameWithoutExtension($_)
    $name -like $Filter -and $fixtureTests -contains $name
}).Count -gt 0
$fixtureDirectory = Join-Path $root 'src\protocol\tools'
$missingFixtures = @('captured_pc_spawn_reassembled.bin','captured_pkt_78.bin','captured_pkt_104.bin','captured_pkt_79.bin') |
    Where-Object { -not (Test-Path -LiteralPath (Join-Path $fixtureDirectory $_)) }
if ($needsFixtures -and $missingFixtures) {
    $python = Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if ($python) {
        & $python (Join-Path $PSScriptRoot 'restore_test_fixtures.py')
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Fixture restoration failed; affected tests will report their failures below.' }
    } else {
        Write-Warning 'Legacy capture fixtures are missing. Install Python 3 and run python scripts/restore_test_fixtures.py; affected tests will fail below.'
    }
}
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$previousContentDb = [Environment]::GetEnvironmentVariable('AOC_CONTENT_DB', 'Process')
$defaultContentDb = Join-Path $root 'dist\content\content.db'
if (-not $previousContentDb -and (Test-Path -LiteralPath $defaultContentDb)) {
    $env:AOC_CONTENT_DB = $defaultContentDb
}
Push-Location $root
try {
    $heavyTests = @('test_worldd_region_load')
    $skippers = New-Object System.Collections.Generic.List[string]
    $skipPattern = '(?i)\[skip\]|:\s*skipped\b|\b(?:is|are)\s+skipped\b|\bopt-in\s*\('
    $lines = @(foreach ($testPath in $testPaths | Sort-Object) {
        $name = [IO.Path]::GetFileNameWithoutExtension($testPath)
        if ($name -notlike $Filter -or (-not $Heavy -and $heavyTests -contains $name)) { continue }
        # test_worldd_node_pop has a half that is opt-in because it reads the baked ground (AOC_REAL_GROUND=1: the five city nodes' bowls on the real heightfield, 0.2 s): it runs whenever
        # dist\content\area_nav.bin is there (Nodes 1 review B, M7: the town floor's constants are tuned on exactly that data)
        $realGround = $name -eq 'test_worldd_node_pop' -and (Test-Path -LiteralPath (Join-Path $root 'dist\content\area_nav.bin'))
        $previousGround = [Environment]::GetEnvironmentVariable('AOC_REAL_GROUND', 'Process')
        try {
            if ($realGround) { $env:AOC_REAL_GROUND = '1' }
            $result = Invoke-TestExecutable $testPath
        } finally {
            Restore-ProcessEnvironment 'AOC_REAL_GROUND' $previousGround
        }
        $text = $result.Output
        $code = $result.Code
        $log = Join-Path $LogDirectory "$name.log"
        $text | Set-Content -LiteralPath $log -Encoding utf8
        $skipLines = @($text -split '\r?\n' | Where-Object {
            $_ -notmatch '(?i)^\s*(?:\[(?:ok\s*|pass)\]|PASS\b)' -and $_ -match $skipPattern
        })
        if ($code -eq 0 -and $skipLines.Count -gt 0) {
            $skippers.Add($name)
            Write-Host ("SKIP details for {0}:`n{1}" -f $name, ($skipLines -join "`n"))
        }
        if ($code -ne 0) {
            Write-Host ("FAIL diagnostics for {0} (log: {1}):`n{2}" -f $name, $log, $text)
        }
        $status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
        $line = '{0,-36} {1} exit={2}' -f $name, $status, $code
        Write-Host $line
        $line
    })
} finally {
    Pop-Location
    Restore-ProcessEnvironment 'AOC_CONTENT_DB' $previousContentDb
}

if ($lines.Count -eq 0) { throw "no test_*.exe in $dist" }

if (-not $Heavy) { Write-Host ("NOTE: not run (heavy, -Heavy): " + ($heavyTests -join ', ')) }
if ($skippers.Count -gt 0) { Write-Host ("NOTE: skipped part or all of their checks (a skip line, exit 0): " + ($skippers -join ', ')) }
$failed = @($lines | Where-Object { $_ -match ' FAIL ' }).Count
Write-Host ("{0} tests, {1} failing" -f $lines.Count, $failed)
Write-Host "Logs: $LogDirectory"

if ($SaveBaseline) { $lines | Set-Content -Encoding utf8 $SaveBaseline }
if ($CompareTo) {
    $diff = Compare-Object @(Get-Content -LiteralPath $CompareTo) $lines
    if ($diff) {
        Write-Host 'DIFFERS FROM BASELINE:'
        $diff | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
        exit 1
    }
    Write-Host 'Matches baseline.'
}
if ($failed -gt 0) { exit 1 }
exit 0
