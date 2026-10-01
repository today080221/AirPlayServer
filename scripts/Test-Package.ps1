#requires -Version 7.0
param([Parameter(Mandatory)][string]$Archive)
$ErrorActionPreference = 'Stop'
$Archive = (Resolve-Path -LiteralPath $Archive).Path
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root ('build\package-verification-' + [guid]::NewGuid().ToString('N'))
$expectedArchive = (Get-Content -LiteralPath ($Archive + '.sha256') -Raw).Split(' ')[0]
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expectedArchive) { throw 'Archive checksum mismatch.' }
Expand-Archive -LiteralPath $Archive -DestinationPath $destination
$folders = @(Get-ChildItem -LiteralPath $destination -Directory)
if ($folders.Count -ne 1) { throw 'Expected a single package root.' }
$runtime = $folders[0].FullName
$count = 0
foreach ($line in Get-Content -LiteralPath (Join-Path $runtime 'SHA256SUMS.txt')) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Malformed checksum line: $line" }
    $expected = $Matches[1]
    $file = [IO.Path]::GetFullPath((Join-Path $runtime $Matches[2]))
    if (-not $file.StartsWith($runtime + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Checksum path escapes package.' }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) { throw "File checksum mismatch: $file" }
    $count++
}
Write-Host "PASS archive SHA256 and $count packaged file hashes"
& (Join-Path $PSScriptRoot 'Test-Desktop.ps1') -RuntimeDir $runtime -RequireLocalRuntime
Write-Host 'PASS extracted desktop start/restart/failure/exit; application and MSVC DLLs loaded from package; no MSYS2 runtime'
