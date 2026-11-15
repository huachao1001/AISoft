# 把构建产物同步到 dist\：复制有变化的 exe，并清掉已下线程序的旧副本。
#
# 用法：powershell -NoProfile -File scripts\sync_dist.ps1 -Config <build 目录>
# 只同步发布产物；test_arc 这类自检工具不发布。
#
# 每个工程各有一份产物清单。按 -Config 里编出来的主机名判断这次是谁在构建：
# 两个工程共用一个 dist\，清理时只能动自己那一族文件，否则编 AiPic 会把
# 已发布的 AiZip 删掉。
param(
  [string]$Config = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

# 必须指明是哪个构建目录：两个应用各有自己的 build\，猜错了会把别人的产物
# 当成「已下线」清掉。
if (-not $Config) {
  Write-Error "用法：sync_dist.ps1 -Config <build 目录>（例如 AiZip\build 或 AiPic\build）"
  exit 1
}

# -Config 既可是 "AiZip\build" 这类相对仓库根的路径，也可是绝对路径
$buildDir = if ([System.IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $root $Config }

# 发布清单：宿主 exe 名 → 该工程的产物表（源文件 → dist 里的名字）
#
# aizip_config.dll 必须随主程序一起发布：右键菜单、文件关联的实现都在
# 它里面，少了它主程序就修不了菜单，安装包也打不进这份能力。
$products = @(
  @{
    Host = "AiZip.exe"
    # 清理时认这些前缀，避免误删另一个工程的产物
    Prefixes = @("AiZip", "aizip")
    Artifacts = @(
      @{ Source = "AiZipSetup.exe"; Name = "AiZipSetup.exe" }
      @{ Source = "AiZip.exe"; Name = "AiZip.exe" }
      @{ Source = "aizip_config.dll"; Name = "aizip_config.dll" }
    )
  }
  @{
    Host = "AiPic.exe"
    Prefixes = @("AiPic", "aipic")
    Artifacts = @(
      @{ Source = "AiPic.exe"; Name = "AiPic.exe" }
    )
  }
)

# 选中这次构建的工程。两个宿主都不存在时（例如只编了库）干脆什么都不做，
# 免得在别人的构建里跑一遍清理。
$product = $products | Where-Object { Test-Path (Join-Path $buildDir $_.Host) } | Select-Object -First 1
if (-not $product) {
  Write-Output "未找到可发布的 exe，跳过同步"
  exit 0
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null

$updated = 0
$skipped = 0
foreach ($item in $product.Artifacts) {
  $source = Join-Path $buildDir $item.Source
  if (-not (Test-Path $source)) {
    Write-Warning "跳过（未构建）：$source"
    $skipped++
    continue
  }
  $target = Join-Path $dist $item.Name
  # 按内容判断是否需要复制：只比时间戳的话，一旦某次复制因文件被占用而跳过，
  # 源文件不再变化就永远不会补上，dist 会一直停在旧版本。
  if (Test-Path $target) {
    $sameBytes = $false
    try {
      $srcHash = (Get-FileHash $source -Algorithm SHA256).Hash
      $dstHash = (Get-FileHash $target -Algorithm SHA256).Hash
      $sameBytes = ($srcHash -eq $dstHash)
    } catch {
      # 哈希读不到（例如目标被占用）就当作需要复制，交给下面的 Copy-Item 处理
      $sameBytes = $false
    }
    if ($sameBytes) { continue }
  }
  try {
    Copy-Item -Force $source $target
  } catch {
    # 程序正在运行时会占住文件：不算构建失败，下次构建再补
    Write-Warning "跳过（目标被占用）：$($item.Name)，请关闭正在运行的副本后重新构建"
    $skipped++
    continue
  }
  $size = [math]::Round((Get-Item $target).Length / 1MB, 2)
  Write-Output "同步 $($item.Name)  ${size} MB"
  $updated++
}

# 只有真的没东西要同步时才算「最新」；被跳过说明 dist 可能落后，别说反了
if ($updated -eq 0 -and $skipped -eq 0) { Write-Output "dist 已是最新" }

# 本工程清单里没有的旧文件一律清掉，防止用户跑到已下线的程序。
# 只清自己那一族：dist 里同时放着别的工程的产物。
#
# 删不掉不算失败：带指纹的右键菜单 dll 会被 explorer 加载，改名换新之后
# 那个旧文件仍然被锁着，直到资源管理器重启。为它中断整次构建不划算——
# 它已经不在发布清单里，用户不会跑到它。
$keep = $product.Artifacts | ForEach-Object { $_.Name }
Get-ChildItem -File $dist | Where-Object {
  $name = $_.Name
  $mine = $false
  foreach ($prefix in $product.Prefixes) {
    if ($name.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { $mine = $true }
  }
  $mine -and ($keep -notcontains $name)
} | ForEach-Object {
  # 名字先存下来：进入 catch 后 $_ 变成错误记录，再取 $_ .Name 就是空的
  $gone = $_.Name
  try {
    Remove-Item -Force $_.FullName -ErrorAction Stop
    Write-Output "移除过期 $gone"
  } catch {
    Write-Warning "跳过（仍被占用）：$gone，可稍后手工删除"
  }
}
