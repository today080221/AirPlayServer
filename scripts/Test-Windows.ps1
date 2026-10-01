#requires -Version 7.0
param([string]$RuntimeDir = '', [switch]$SkipDesktop,
    [int]$TestRaopPort = 5001, [int]$TestAirplayPort = 7001,
    [switch]$ExpectAdapterPortConflict)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $RuntimeDir) { $RuntimeDir = Join-Path $root 'x64\Release' }
$RuntimeDir = [IO.Path]::GetFullPath($RuntimeDir)
$evidence = Join-Path $root 'build\test-results'
$profile = Join-Path $root 'build\test-profile'
New-Item -ItemType Directory -Force -Path $evidence, "$profile\roaming", "$profile\local" | Out-Null
# An interrupted or failed rerun must not leave a previous successful attestation.
[ordered]@{ result = 'running'; started_utc = [DateTime]::UtcNow.ToString('o') } |
    ConvertTo-Json | Set-Content -LiteralPath "$evidence\results.json" -Encoding utf8NoBOM

function Invoke-Check([string]$Executable, [string[]]$Arguments, [string]$Name, [int]$Timeout = 300000) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $RuntimeDir
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.Environment['APPDATA'] = "$profile\roaming"
    $info.Environment['LOCALAPPDATA'] = "$profile\local"
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($Timeout)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "$Name timed out"
        }
        [IO.File]::WriteAllText("$evidence\$Name.log", $stdout.Result + $stderr.Result, [Text.UTF8Encoding]::new($false))
        if ($process.ExitCode -ne 0) { throw "$Name failed ($($process.ExitCode)); see $evidence\$Name.log" }
        Write-Host "PASS $Name"
    } finally { $process.Dispose() }
}

Invoke-Check (Join-Path $RuntimeDir 'AirPlayLibSmokeTests.exe') @() 'upstream-smoke'
$adapterArguments = @('-NoProfile', '-File', "$root\.github\scripts\Test-MirrorSimAdapter.ps1", '-RuntimeDir', $RuntimeDir)
if ($ExpectAdapterPortConflict) { $adapterArguments += '-ExpectPortConflict' }
Invoke-Check (Get-Command pwsh).Source $adapterArguments 'upstream-adapter'
Invoke-Check (Join-Path $RuntimeDir 'PortRegressionTests.exe') @("$TestRaopPort", "$TestAirplayPort") 'port-regression'
if (-not $SkipDesktop) {
    Invoke-Check (Get-Command pwsh).Source @('-NoProfile', '-File', "$root\scripts\Test-Desktop.ps1", '-RuntimeDir', $RuntimeDir) 'desktop'
}
$hashes = @{}
foreach ($name in @('AirPlayServer.exe','airplay2dll.dll','dnssd.dll','SDL2.dll','avcodec-62.dll','avutil-60.dll','swscale-9.dll','AirPlayLibSmokeTests.exe','PortRegressionTests.exe','MirrorSimAdapter.exe')) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $RuntimeDir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{
    result = 'passed'
    completed_utc = [DateTime]::UtcNow.ToString('o')
    desktop_tested = -not $SkipDesktop
    preferred_test_ports = @($TestRaopPort, $TestAirplayPort)
    external_adapter_mode = $(if ($ExpectAdapterPortConflict) { 'expected-conflict' } else { 'successful-start' })
    binary_sha256 = $hashes
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$evidence\results.json" -Encoding utf8NoBOM
Write-Host "All checks passed. Evidence: $evidence"
