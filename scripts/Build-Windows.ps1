#requires -Version 7.0
param(
    [string]$VisualStudio = 'N:\Microsoft Visual Studio\18\Enterprise',
    [string]$Toolset = 'v145'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$msbuild = Join-Path $VisualStudio 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild not found: $msbuild" }
New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null

function Invoke-Build([string]$Project, [string]$LogName) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $msbuild
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    # Some desktop hosts inherit both PATH and Path. Normalize only the child
    # environment; MSBuild's .NET Framework process rejects duplicate names.
    $info.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $info.Environment[$entry.Key] = $entry.Value
    }
    foreach ($argument in @($Project, '/p:Configuration=Release', '/p:Platform=x64',
        "/p:PlatformToolset=$Toolset", '/m:1', '/nr:false', '/v:quiet', '/fl',
        "/flp:logfile=build/$LogName;verbosity=normal")) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Build failed ($($process.ExitCode)); see build/$LogName" }
    } finally { $process.Dispose() }
}
Invoke-Build 'AirPlay.sln' 'release.log'
Invoke-Build 'tests/PortRegressionTests/PortRegressionTests.vcxproj' 'port-tests-build.log'
Write-Host 'Release x64 application and regression tests built successfully.'
