# ---------------------------------------------------------------------------
#  End-to-end check of the .NET client.
#
#  Uses plain Win32 messages rather than UI Automation: WinForms controls are
#  real HWNDs, so BM_CLICK / WM_GETTEXT are simpler and more reliable than
#  UIA patterns.
#
#  Usage: powershell -ExecutionPolicy Bypass -File tools\test-client.ps1 [-WaitSeconds 90]
# ---------------------------------------------------------------------------
param(
    [int]$WaitSeconds = 90,
    [string]$Shot
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\client\ShittimLogonUpdater.exe'
if (-not (Test-Path $exe)) { throw "not found: $exe" }

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Ui32 {
    public const uint BM_CLICK = 0x00F5;
    public const uint WM_GETTEXT = 0x000D;
    public const uint WM_GETTEXTLENGTH = 0x000E;

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, StringBuilder lParam);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, StringBuilder name, int max);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    public static string TextOf(IntPtr hWnd) {
        int length = (int)SendMessageW(hWnd, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero);
        if (length <= 0) return string.Empty;
        var buffer = new StringBuilder(length + 2);
        SendMessageW(hWnd, WM_GETTEXT, new IntPtr(buffer.Capacity), buffer);
        return buffer.ToString();
    }

    public static string ClassOf(IntPtr hWnd) {
        var buffer = new StringBuilder(256);
        GetClassNameW(hWnd, buffer, buffer.Capacity);
        return buffer.ToString();
    }
}
'@

Get-Process ShittimLogonUpdater -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $exe -PassThru

# --- wait for the main window -------------------------------------------------
$main = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and $main -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 400
    $proc.Refresh()
    if ($proc.HasExited) { Write-Host 'process exited early'; exit 1 }
    $main = $proc.MainWindowHandle
}
if ($main -eq [IntPtr]::Zero) { Write-Host 'main window not found'; $proc.Kill(); exit 1 }
Write-Host ("main window: " + $proc.MainWindowTitle)

# --- enumerate child controls ------------------------------------------------
$controls = New-Object System.Collections.ArrayList
$collector = [Ui32+EnumProc] {
    param($hWnd, $lParam)
    [void]$controls.Add([pscustomobject]@{
        Handle = $hWnd
        Class  = [Ui32]::ClassOf($hWnd)
        Text   = [Ui32]::TextOf($hWnd)
    })
    return $true
}
[void][Ui32]::EnumChildWindows($main, $collector, [IntPtr]::Zero)

$startButton = $controls | Where-Object { $_.Class -like '*BUTTON*' -and $_.Text -eq '开始' } | Select-Object -First 1
if (-not $startButton) {
    Write-Host 'start button not found. children:'
    $controls | ForEach-Object { "  $($_.Class)  '$($_.Text)'" }
    $proc.Kill()
    exit 1
}
Write-Host ("start button found")

# --- click it -----------------------------------------------------------------
[void][Ui32]::SendMessageW($startButton.Handle, [Ui32]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host 'clicked start'

# --- wait until the button is enabled again (flow finished) -------------------
$finished = $false
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($proc.HasExited) {
        Write-Host ("process exited: 0x{0:X8}" -f $proc.ExitCode)
        break
    }
    if ([Ui32]::IsWindowEnabled($startButton.Handle)) { $finished = $true; break }
}
Write-Host ("finished=$finished")

# --- dump status labels and the log box --------------------------------------
$controls.Clear()
[void][Ui32]::EnumChildWindows($main, $collector, [IntPtr]::Zero)

Write-Host '--- status texts ---'
$controls | Where-Object { $_.Class -like '*STATIC*' -and $_.Text.Trim().Length -gt 0 } |
    ForEach-Object { "  $($_.Text)" }

Write-Host '--- log box (last 30 lines) ---'
$log = $controls | Where-Object { $_.Class -like '*EDIT*' } |
    Where-Object { @($_.Text -split "`r?`n").Count -gt 1 } | Select-Object -First 1
if ($log) {
    @($log.Text -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }) |
        Select-Object -Last 30 | ForEach-Object { "  $_" }
} else {
    Write-Host '  (no multiline edit control found)'
}

if ($Shot) {
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct ShotRect { public int Left; public int Top; public int Right; public int Bottom; }
public class ShotWin {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out ShotRect r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
    [ShotWin]::SetForegroundWindow($main) | Out-Null
    Start-Sleep -Milliseconds 1200
    $r = New-Object ShotRect
    [ShotWin]::GetWindowRect($main, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    if ($w -gt 0 -and $h -gt 0) {
        $bmp = New-Object Drawing.Bitmap $w, $h
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object Drawing.Size $w, $h))
        $bmp.Save($Shot, [Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Host "screenshot saved: $Shot"
    }
}

Start-Sleep -Seconds 1
if (-not $proc.HasExited) { $proc.Kill() }
Write-Host 'done'
