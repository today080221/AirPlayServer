#requires -Version 7.0
param([Parameter(Mandatory)][string]$RuntimeDir, [switch]$RequireLocalRuntime)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DesktopTestWindow {
    private delegate bool EnumProc(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder title, int count);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    public static bool Close(uint pid) {
        bool sent = false;
        EnumWindows((window, context) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner != pid) return true;
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            if (title.ToString() != "AirPlay Receiver (Personal)") return true;
            sent = PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); // WM_CLOSE
            return false;
        }, IntPtr.Zero);
        return sent;
    }
}
'@
$root = Split-Path $PSScriptRoot -Parent
$RuntimeDir = [IO.Path]::GetFullPath($RuntimeDir)
$desktopDir = Join-Path $root ('build\desktop-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $desktopDir | Out-Null
Get-ChildItem -LiteralPath $RuntimeDir -File | Where-Object { $_.Extension -eq '.dll' -or $_.Name -eq 'AirPlayServer.exe' } |
    Copy-Item -Destination $desktopDir
[IO.File]::WriteAllText((Join-Path $desktopDir 'airplay_settings.ini'), "device_name=Personal regression receiver`r`n", [Text.UTF8Encoding]::new($false))

function Get-Listeners([int]$OwnerId) {
    @(& netstat -ano -p tcp | ForEach-Object {
        $columns = $_.Trim() -split '\s+'
        if ($columns.Count -eq 5 -and $columns[3] -eq 'LISTENING' -and $columns[4] -eq "$OwnerId") {
            [int]($columns[1] -replace '^.*:', '')
        }
    } | Select-Object -Unique)
}

foreach ($case in @('start', 'restart', 'startup-failure')) {
    $blocker = $null
    $process = $null
    try {
        if ($case -eq 'startup-failure') {
            $blocker = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork, [Net.Sockets.SocketType]::Stream, [Net.Sockets.ProtocolType]::Tcp)
            $blocker.ExclusiveAddressUse = $true
            $blocker.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Any, 7100))
        }
        $local = Join-Path $desktopDir $case
        New-Item -ItemType Directory -Path $local | Out-Null
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = Join-Path $desktopDir 'AirPlayServer.exe'
        $info.WorkingDirectory = $desktopDir
        $info.ArgumentList.Add('--debug-quiet')
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $info.Environment['LOCALAPPDATA'] = $local
        $info.Environment['APPDATA'] = $local
        $info.Environment.Remove('MIRRORSIM_EXTERNAL_DNSSD') | Out-Null
        $process = [Diagnostics.Process]::Start($info)
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $logText = ''
        do {
            if ($process.HasExited) { throw "Desktop exited during $case ($($process.ExitCode))" }
            $logs = @(Get-ChildItem -LiteralPath "$local\AirPlayServer\logs" -Filter '*.log' -ErrorAction SilentlyContinue)
            if ($logs.Count) { $logText = Get-Content -LiteralPath $logs[0].FullName -Raw }
            $ready = if ($blocker) { $logText.Contains('Receiver startup failed') } else { $logText.Contains('Startup complete') }
            if (-not $ready) { Start-Sleep -Milliseconds 100 }
        } while (-not $ready -and $timer.ElapsedMilliseconds -lt 15000)
        if (-not $ready) { throw "No startup result for $case" }
        if ($blocker) { Start-Sleep -Milliseconds 1500 }
        if ($RequireLocalRuntime -and -not $blocker) {
            $process.Refresh()
            $modules = @($process.Modules)
            foreach ($name in @('airplay2dll.dll','dnssd.dll','SDL2.dll','avcodec-62.dll','avutil-60.dll','swscale-9.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
                $module = @($modules | Where-Object ModuleName -eq $name)
                if ($module.Count -ne 1 -or [IO.Path]::GetDirectoryName($module[0].FileName) -ne $desktopDir) {
                    throw "Runtime DLL was not loaded from the isolated package: $name"
                }
            }
            if ($modules.ModuleName -contains 'msys-2.0.dll') { throw 'Unexpected MSYS2 runtime dependency.' }
        }
        $listeners = @(Get-Listeners $process.Id)
        if ($blocker) {
            if ($listeners.Count) { throw "Failure left listeners: $listeners" }
        } else {
            $reported = @([regex]::Matches($logText, 'TCP listening: requested=\d+ actual=(\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
            if ($reported.Count -ne 3 -or (Compare-Object ($reported | Sort-Object) ($listeners | Sort-Object))) {
                throw "Desktop listeners differ from bound ports: reported=$reported observed=$listeners"
            }
        }
        # Process.MainWindowHandle can select an untitled SDL helper window.
        # Address only the main receiver HWND owned by this test's child process.
        if (-not [DesktopTestWindow]::Close($process.Id)) { throw 'Could not request graceful desktop exit.' }
        if (-not $process.WaitForExit(12000)) { throw 'Desktop exit did not finish within 12 seconds.' }
        if ($process.ExitCode -ne 0) { throw "Desktop exited with $($process.ExitCode)" }
        if (@(Get-Listeners $process.Id).Count) { throw 'Exited desktop still has a listener.' }
        $logText = Get-Content -LiteralPath $logs[0].FullName -Raw
        if ($logText.Contains('[crash]')) { throw 'Desktop logged a native crash.' }
        Write-Host "PASS desktop $case; observed ports=$($listeners -join ','); graceful exit=0"
    } finally {
        if ($process) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
        if ($blocker) { $blocker.Dispose() }
    }
}
