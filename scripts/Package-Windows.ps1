#requires -Version 7.0
param([string]$VisualStudio = 'N:\Microsoft Visual Studio\18\Enterprise')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $revision = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source revision.' }
    if (& git status --porcelain) { throw 'Commit source changes before packaging.' }
    $runtime = Join-Path $root 'x64\Release'
    $testDir = Join-Path $root 'build\test-results'
    $results = Get-Content -LiteralPath "$testDir\results.json" -Raw | ConvertFrom-Json
    if ($results.result -ne 'passed' -or -not $results.desktop_tested) { throw 'Complete all Windows tests before packaging.' }
    $files = @('AirPlayServer.exe','airplay2dll.dll','dnssd.dll','SDL2.dll','avcodec-62.dll','avutil-60.dll','swscale-9.dll')
    foreach ($name in $files) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $runtime $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $results.binary_sha256.$name) { throw "Binary changed after tests: $name" }
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $name = "AirPlayServer-Personal-v1.2.5-win-x64-$($revision.Substring(0,8))-$stamp"
    $dist = Join-Path $root 'dist'
    $stage = Join-Path $dist $name
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $runtime $file) -Destination $stage }

    $redistRoot = Join-Path $VisualStudio 'VC\Redist\MSVC'
    $redistVersion = Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $crtDir = Join-Path $redistVersion.FullName 'x64\Microsoft.VC145.CRT'
    if (-not (Test-Path -LiteralPath $crtDir)) { throw "Release x64 CRT not found: $crtDir" }
    # Application-local redistribution only; never run vc_redist or install tools.
    Get-ChildItem -LiteralPath $crtDir -Filter '*.dll' -File | Copy-Item -Destination $stage
    foreach ($dir in @('licenses','licenses\ffmpeg','licenses\libplist','licenses\bonjour','test-results')) {
        New-Item -ItemType Directory -Path (Join-Path $stage $dir) -Force | Out-Null
    }
    Copy-Item -LiteralPath LICENSE,THIRD_PARTY_NOTICES.md -Destination $stage
    Copy-Item -LiteralPath 'docs\PERSONAL-BUILD.zh-CN.md' -Destination (Join-Path $stage '使用说明.md')
    Copy-Item -LiteralPath 'docs\SOURCE-REVIEW.zh-CN.md' -Destination $stage
    Copy-Item -LiteralPath 'docs\licenses\ImGui-MIT.txt' -Destination (Join-Path $stage 'licenses')
    Copy-Item -LiteralPath 'docs\licenses\Apache-2.0.txt' -Destination (Join-Path $stage 'licenses\bonjour')
    Copy-Item -LiteralPath 'external\SDL2\LICENSE.txt' -Destination (Join-Path $stage 'licenses\SDL2.txt')
    Copy-Item -LiteralPath 'AirPlayServerLib\lib\playfair\LICENSE.md' -Destination (Join-Path $stage 'licenses\PlayFair-GPLv3.md')
    Copy-Item -LiteralPath 'external\ffmpeg\licenses\COPYING.LGPLv2.1','external\ffmpeg\licenses\LICENSE.md','external\ffmpeg\licenses\FFMPEG_BUILD_INFO-x64.txt' -Destination (Join-Path $stage 'licenses\ffmpeg')
    Copy-Item -LiteralPath 'external\ffmpeg\licenses\COPYING.LGPLv2.1','external\plist\include\plist.h' -Destination (Join-Path $stage 'licenses\libplist')
    Copy-Item -LiteralPath (Join-Path $VisualStudio 'Licenses\1033\Redist.txt') -Destination (Join-Path $stage 'licenses\MSVC-Redist.txt')
    # Preserve all notices of the exact Bonjour sources compiled by this project.
    [xml]$project = Get-Content -LiteralPath 'dnssd\dnssd.vcxproj' -Raw
    $nodes = $project.SelectNodes('//*[local-name()="ClCompile"][@Include]')
    foreach ($node in $nodes) {
        $sourcePath = Join-Path "$root\dnssd" $node.Include
        if (Test-Path -LiteralPath $sourcePath) {
            $text = [IO.File]::ReadAllText($sourcePath)
            $block = [regex]::Match($text, '(?s)^\s*/\*.*?\*/').Value
            if ($block) { [IO.File]::WriteAllText((Join-Path $stage ('licenses\bonjour\' + [IO.Path]::GetFileName($sourcePath) + '.notice.txt')), $block, [Text.UTF8Encoding]::new($false)) }
        }
    }
    foreach ($file in @('results.json','upstream-smoke.log','upstream-adapter.log','port-regression.log','desktop.log')) {
        Copy-Item -LiteralPath (Join-Path $testDir $file) -Destination (Join-Path $stage 'test-results')
    }
    if (Test-Path -LiteralPath "$testDir\pe-dependencies.txt") {
        Copy-Item -LiteralPath "$testDir\pe-dependencies.txt" -Destination (Join-Path $stage 'test-results')
    }
    [ordered]@{
        edition = 'Personal modification by today080221; not an upstream release'
        upstream = 'https://github.com/xenos1337/AirPlayServer'
        upstream_version = 'v1.2.5'
        upstream_commit = '85bb8dcbe82f75f40f30bb603255668f9162a8f9'
        source_commit = $revision
        source_url = "https://github.com/today080221/AirPlayServer/tree/$revision"
        configuration = 'Release x64 v145'
        msvc_redist_version = $redistVersion.Name
        packaged_utc = [DateTime]::UtcNow.ToString('o')
        iphone_validation = 'Not performed'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'BUILD-INFO.json') -Encoding utf8NoBOM
    [IO.File]::WriteAllText((Join-Path $stage '启动调试.cmd'), "@echo off`r`ncd /d `"%~dp0`"`r`nAirPlayServer.exe --debug`r`n", [Text.UTF8Encoding]::new($false))
    $lines = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\','/')
        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative
    }
    $lines | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding utf8NoBOM
    $archive = Join-Path $dist ($name + '.zip')
    Compress-Archive -LiteralPath $stage -DestinationPath $archive
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$archiveHash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding utf8NoBOM
    $sourceArchive = Join-Path $dist ($name + '-source.zip')
    & git archive --format=zip "--output=$sourceArchive" HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source archive failed.' }
    "$((Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($sourceArchive))" |
        Set-Content -LiteralPath ($sourceArchive + '.sha256') -Encoding utf8NoBOM
    Write-Host "Package: $archive"
    Write-Host "SHA256: $archiveHash"
    Write-Host "Source: $sourceArchive"
} finally { Pop-Location }
