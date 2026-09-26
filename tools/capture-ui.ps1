# ---------------------------------------------------------------------------
#  启动 GUI 客户端并把主窗口截图保存为 PNG，用于人工确认渲染效果。
#
#  用法：powershell -ExecutionPolicy Bypass -File tools\capture-ui.ps1
# ---------------------------------------------------------------------------
param(
    [string]$Exe,
    [string]$Out,
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\ui\Release\ShittimLogonUpdater.exe' }
if (-not $Out) { $Out = Join-Path $root 'build\ui-window.png' }

if (-not (Test-Path $Exe)) { throw "找不到可执行文件: $Exe" }

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct WinRect { public int Left; public int Top; public int Right; public int Bottom; }
public class WinCap {
    public static readonly IntPtr HWND_TOPMOST   = new IntPtr(-1);
    public static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
    public const uint SWP_NOSIZE     = 0x0001;
    public const uint SWP_NOMOVE     = 0x0002;
    public const uint SWP_SHOWWINDOW = 0x0040;

    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out WinRect r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@

Get-Process ShittimLogonUpdater -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

# 先把其它窗口最小化，这样随后启动的应用天然处于最前，截屏即可拍到它。
# 不要改用 SetWindowPos(HWND_TOPMOST) 强制置顶：实测强制激活窗口会让 WinUI
# 在此环境下抛 XAML 异常并崩溃。
try { (New-Object -ComObject Shell.Application).MinimizeAll() } catch { }
Start-Sleep -Milliseconds 1200

$proc = Start-Process -FilePath $Exe -PassThru
$handle = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) { break }
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne 0) { $handle = $proc.MainWindowHandle; break }
}

if ($proc.HasExited) {
    Write-Host "进程已退出，退出码 $($proc.ExitCode) (0x$('{0:X8}' -f $proc.ExitCode))"
    exit 1
}

if ($handle -eq [IntPtr]::Zero) {
    Write-Host "在 $TimeoutSeconds 秒内未取得主窗口句柄"
    $proc.Kill()
    exit 1
}

Write-Host "窗口句柄: $handle  标题: '$($proc.MainWindowTitle)'"

if (-not [WinCap]::IsWindow($handle)) {
    Write-Host "句柄不是有效窗口"
    $proc.Kill()
    exit 1
}

# 不再使用 SetWindowPos/SetForegroundWindow 强行置顶（会触发崩溃）。
# 窗口已经是可见状态，直接等待它完成首帧渲染后截屏。
[WinCap]::BringWindowToTop($handle) | Out-Null
Start-Sleep -Milliseconds 1800

if ($proc.HasExited) {
    Write-Host "进程在截图前退出，退出码 $($proc.ExitCode) (0x$('{0:X8}' -f $proc.ExitCode))"
    exit 1
}

$rect = New-Object WinRect
[WinCap]::GetWindowRect($handle, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host "窗口区域: $($rect.Left),$($rect.Top)  ${width}x${height}  可见=$([WinCap]::IsWindowVisible($handle))"

if ($width -le 0 -or $height -le 0) {
    Write-Host "窗口矩形无效，放弃截图"
    $proc.Kill()
    exit 1
}

$bitmap = New-Object Drawing.Bitmap $width, $height
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object Drawing.Size $width, $height))
$bitmap.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()

Write-Host "截图已保存: $Out"

[WinCap]::SetWindowPos($handle, [WinCap]::HWND_NOTOPMOST, 0, 0, 0, 0,
    ([WinCap]::SWP_NOMOVE -bor [WinCap]::SWP_NOSIZE)) | Out-Null

Start-Sleep -Milliseconds 300
$proc.Kill()
Write-Host "已结束测试实例"
