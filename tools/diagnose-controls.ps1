# ---------------------------------------------------------------------------
#  Bisect the control set to find which control crashes the window on render.
#
#  The app honours the environment variable SLU_UI_CONTROLS=n:
#     0 = only an empty StackPanel is set as window content
#     1 = + TextBlock title      2 = + TextBlock subtitle   3 = + Button
#     4 = + ProgressBar          5 = + TextBlock status     6 = + TextBox log
#
#  For each value the app is started, its window is forced on top so that it
#  actually renders, and the process is watched for a crash.
#
#  Usage: powershell -ExecutionPolicy Bypass -File tools\diagnose-controls.ps1
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\ui\Release\ShittimLogonUpdater.exe'
$log  = Join-Path $root 'build\ui\Release\ui-startup.log'

if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class DiagWin {
    public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    public const uint SWP_NOSIZE = 0x0001;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_SHOWWINDOW = 0x0040;
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@

Get-Process ShittimLogonUpdater -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "SLU_UI_CONTROLS  bisect"
Write-Host "----------------------"

foreach ($n in 0, 1, 2, 3, 4, 5, 6) {
    $env:SLU_UI_CONTROLS = "$n"
    Remove-Item $log -ErrorAction SilentlyContinue

    $proc = Start-Process -FilePath $exe -PassThru
    $handle = [IntPtr]::Zero

    # Wait for the main window (or for an early crash).
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        if ($proc.HasExited) { break }
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne 0) { $handle = $proc.MainWindowHandle; break }
    }

    $result = '?'

    if ($proc.HasExited) {
        $proc.WaitForExit(2000) | Out-Null
        $result = ('crash (startup)   0x{0:X8}' -f $proc.ExitCode)
    }
    elseif ($handle -eq [IntPtr]::Zero) {
        $result = 'no window handle'
        $proc.Kill()
    }
    else {
        # Force the window to the top so it really renders.
        [DiagWin]::ShowWindow($handle, 5) | Out-Null
        [DiagWin]::SetWindowPos($handle, [DiagWin]::HWND_TOPMOST, 0, 0, 0, 0,
            ([DiagWin]::SWP_NOMOVE -bor [DiagWin]::SWP_NOSIZE -bor [DiagWin]::SWP_SHOWWINDOW)) | Out-Null

        $crashed = $false
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep -Milliseconds 250
            if ($proc.HasExited) { $crashed = $true; break }
        }

        if ($crashed) {
            $proc.WaitForExit(2000) | Out-Null
            $result = ('crash (render)    0x{0:X8}' -f $proc.ExitCode)
        }
        else {
            $result = 'OK (window rendered)'
            $proc.Kill()
        }
    }

    $tail = ''
    if (Test-Path $log) {
        $lines = @(Get-Content $log -Encoding UTF8)
        if ($lines.Count -gt 0) { $tail = $lines[-1] }
    }

    Write-Host ("  n={0}  {1}" -f $n, $result)
    if ($tail) { Write-Host ("        last log: {0}" -f $tail.Trim()) }
}

Remove-Item Env:\SLU_UI_CONTROLS -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "done."
