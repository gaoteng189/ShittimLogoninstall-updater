#Requires -Version 5.1
<#
.SYNOPSIS
    手动拉取 Windows App SDK 及其依赖树。

.DESCRIPTION
    本机没有 NuGet 还原能力（Visual Studio BuildTools 未包含 NuGet 组件，
    也没有 nuget.exe / .NET SDK），因此这里不依赖 PackageReference 还原，
    而是按 nuspec 声明的依赖关系递归地从 nuget.org 下载 .nupkg 并解压到本地，
    供 vcxproj 通过 $(NuGetPackageRoot) 直接引用。

    包是按 <id>.<version> 目录布局的，与 NuGet 全局包目录一致，
    因此 WindowsAppSDK 自带的 props/targets 里的相对引用可以直接工作。

.EXAMPLE
    .\fetch-windowsappsdk.ps1
    .\fetch-windowsappsdk.ps1 -Version 1.8.260921001 -Include AI,ML
#>
[CmdletBinding()]
param(
    # Windows App SDK 版本。默认取 1.8 LTS（兼容 Windows 10 1809+）。
    [string]$Version = '1.8.260921001',

    # 解压目标目录，留空则使用仓库根目录下的 packages/。
    # （$PSScriptRoot 在 param 默认值求值时还不可用，因此放到主体里解析）
    [string]$OutputDirectory = '',

    # 额外需要拉取的包 ID（默认跳过 AI / ML / Widgets 等 UI 用不到的大包）。
    [string[]]$Include = @(),

    # 强制重新下载，忽略已有目录。
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\packages'
}

$flatContainerUrl = 'https://api.nuget.org/v3-flatcontainer'
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$downloadCache = Join-Path $env:TEMP 'wasdk-nupkg-cache'

# 这些子包体积大且与桌面 UI 无关，默认不拉取。
$skipPatterns = @(
    'Microsoft.WindowsAppSDK.AI',
    'Microsoft.WindowsAppSDK.ML',
    'Microsoft.WindowsAppSDK.Widgets'
)

New-Item -ItemType Directory -Force -Path $OutputDirectory, $downloadCache | Out-Null

function Get-ExactVersion {
    param([string]$Raw)
    # nuspec 里可能是 [1.2.3] 或 [1.2.3,) 这类版本范围写法
    $clean = $Raw.Trim()
    $clean = $clean.Trim('[', ']', '(', ')')
    if ($clean.Contains(',')) {
        $clean = $clean.Split(',')[0].Trim()
    }
    return $clean
}

function Get-PackageDependencies {
    param([string]$NuspecPath)

    [xml]$nuspec = Get-Content -LiteralPath $NuspecPath
    $dependencies = @()

    # 部分包（例如 Microsoft.Windows.SDK.BuildTools）的 nuspec 根本没有
    # <dependencies> 节点；StrictMode 下直接取属性会抛错，必须先探测。
    $dependenciesProperty = $nuspec.package.metadata.PSObject.Properties['dependencies']
    if ($null -eq $dependenciesProperty) {
        return $dependencies
    }
    $node = $dependenciesProperty.Value

    # 依赖可能写在 <group> 里，也可能直接挂在 <dependencies> 下。
    # StrictMode 下访问不存在的属性会直接抛错，因此统一先用 PSObject.Properties 判断。
    $collected = @()

    $groupProperty = $node.PSObject.Properties['group']
    if ($null -ne $groupProperty) {
        foreach ($group in @($groupProperty.Value)) {
            if ($null -eq $group) { continue }
            $dependencyProperty = $group.PSObject.Properties['dependency']
            if ($null -eq $dependencyProperty) { continue }
            $collected += @($dependencyProperty.Value)
        }
    }

    $directProperty = $node.PSObject.Properties['dependency']
    if ($null -ne $directProperty) {
        $collected += @($directProperty.Value)
    }

    foreach ($dep in $collected) {
        if ($null -eq $dep) { continue }
        $dependencies += [pscustomobject]@{
            Id      = $dep.id
            Version = (Get-ExactVersion $dep.version)
        }
    }
    return $dependencies
}

function Get-NuGetPackage {
    param([string]$Id, [string]$PackageVersion)

    $lowerId = $Id.ToLowerInvariant()
    # 采用 NuGet 全局包目录的两层布局 <id>\<version>\：
    # Windows App SDK 的 props/targets 内部就是按这个约定去引用同级子包的，
    # 若拍平成 <id>.<version> 单层目录，子包会找不到。
    $target = Join-Path (Join-Path $OutputDirectory $lowerId) $PackageVersion
    $nuspecInTarget = Join-Path $target "$lowerId.nuspec"

    if ((Test-Path -LiteralPath $nuspecInTarget) -and (-not $Force)) {
        Write-Host "  [已存在] $Id $PackageVersion"
        # 包目录里必须保留一份 .nupkg：Windows App SDK 的 targets 会在构建期
        # 扫描 <包目录>\*.nupkg 做依赖完整性校验，缺失会导致 MSB4044。
        $nupkgInTarget = Join-Path $target $fileName
        if ((-not (Test-Path -LiteralPath $nupkgInTarget)) -and
            (Test-Path -LiteralPath $cached)) {
            Copy-Item -LiteralPath $cached -Destination $nupkgInTarget -Force
        }
        return $target
    }

    $fileName = "$lowerId.$PackageVersion.nupkg"
    $cached = Join-Path $downloadCache $fileName

    if ((-not (Test-Path -LiteralPath $cached)) -or $Force) {
        $url = "$flatContainerUrl/$lowerId/$PackageVersion/$fileName"
        Write-Host "  [下载中] $Id $PackageVersion"
        $attempt = 0
        while ($true) {
            try {
                Invoke-WebRequest -Uri $url -OutFile $cached -UseBasicParsing -TimeoutSec 180
                break
            } catch {
                $attempt++
                if ($attempt -ge 3) {
                    throw "下载 $url 失败：$($_.Exception.Message)"
                }
                Write-Host "     第 $attempt 次失败，稍后重试..."
                Start-Sleep -Seconds 3
            }
        }
    } else {
        Write-Host "  [用缓存] $Id $PackageVersion"
    }

    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $tempZip = Join-Path $downloadCache "$fileName.zip"
    Copy-Item -LiteralPath $cached -Destination $tempZip -Force
    Expand-Archive -LiteralPath $tempZip -DestinationPath $target -Force
    Remove-Item -LiteralPath $tempZip -Force

    # 与 NuGet 全局包目录一致，保留 nupkg 本身
    Copy-Item -LiteralPath $cached -Destination (Join-Path $target $fileName) -Force

    return $target
}

Write-Host "拉取 Windows App SDK $Version 及其依赖"
Write-Host "  目标目录：$OutputDirectory"
if ($Include.Count -gt 0) {
    Write-Host "  额外包含：$($Include -join ', ')"
}
Write-Host ''

$queue = New-Object System.Collections.Queue
$queue.Enqueue([pscustomobject]@{ Id = 'Microsoft.WindowsAppSDK'; Version = $Version })

$visited = @{}
$resolved = @()

while ($queue.Count -gt 0) {
    $item = $queue.Dequeue()
    $key = "$($item.Id) $($item.Version)"
    if ($visited.ContainsKey($key)) { continue }
    $visited[$key] = $true

    $isRequired = ($Include -contains $item.Id) -or ($item.Id -eq 'Microsoft.WindowsAppSDK')
    $shouldSkip = $false
    foreach ($pattern in $skipPatterns) {
        if (($item.Id -eq $pattern) -and (-not $isRequired)) {
            $shouldSkip = $true
            break
        }
    }
    if ($shouldSkip) {
        Write-Host "  [跳过] $($item.Id) $($item.Version)（与桌面 UI 无关，可用 -Include 强制拉取）"
        continue
    }

    $packageDirectory = Get-NuGetPackage -Id $item.Id -PackageVersion $item.Version
    $resolved += $key

    $nuspec = Get-ChildItem -LiteralPath $packageDirectory -Filter '*.nuspec' |
        Select-Object -First 1
    if ($nuspec) {
        foreach ($dependency in Get-PackageDependencies -NuspecPath $nuspec.FullName) {
            $queue.Enqueue($dependency)
        }
    }
}

$totalBytes = (Get-ChildItem -Recurse -File -LiteralPath $OutputDirectory |
    Measure-Object -Property Length -Sum).Sum

Write-Host ''
Write-Host "完成：$($resolved.Count) 个包，合计 $([math]::Round($totalBytes / 1MB, 1)) MB"
$resolved | Sort-Object | ForEach-Object { Write-Host "  $_" }
