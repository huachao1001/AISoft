# 把构建产物同步到 dist\：复制有变化的 exe，并清掉已删掉程序的旧副本。
#
# 用法：powershell -NoProfile -File scripts\sync_dist.ps1
# 只同步发布产物（AiZip.exe）；test_arc 这类自检工具不发布。
param(
  [string]$Config = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

# -Config 既可是 "build" 这类子目录名，也可是 CMake 传进来的绝对路径
$buildDir = if ([System.IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $root "AiZip\$Config" }

# 发布清单：源文件 → dist 里的名字
$artifacts = @(
  @{ Source = Join-Path $buildDir "AiZipSetup.exe"; Name = "AiZipSetup.exe" }
  @{ Source = Join-Path $buildDir "AiZip.exe"; Name = "AiZip.exe" }
)

New-Item -ItemType Directory -Force -Path $dist | Out-Null

$updated = 0
$skipped = 0
foreach ($item in $artifacts) {
  if (-not (Test-Path $item.Source)) {
    Write-Warning "跳过（未构建）：$($item.Source)"
    $skipped++
    continue
  }
  $target = Join-Path $dist $item.Name
  # 按内容判断是否需要复制：只比时间戳的话，一旦某次复制因文件被占用而跳过，
  # 源文件不再变化就永远不会补上，dist 会一直停在旧版本。
  if (Test-Path $target) {
    $sameBytes = $false
    try {
      $srcHash = (Get-FileHash $item.Source -Algorithm SHA256).Hash
      $dstHash = (Get-FileHash $target -Algorithm SHA256).Hash
      $sameBytes = ($srcHash -eq $dstHash)
    } catch {
      # 哈希读不到（例如目标被占用）就当作需要复制，交给下面的 Copy-Item 处理
      $sameBytes = $false
    }
    if ($sameBytes) { continue }
  }
  try {
    Copy-Item -Force $item.Source $target
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

# 清单里没有的旧文件一律清掉，防止用户跑到已下线的程序
$keep = $artifacts | ForEach-Object { $_.Name }
Get-ChildItem -File $dist | Where-Object { $keep -notcontains $_.Name } | ForEach-Object {
  Remove-Item -Force $_.FullName
  Write-Output "移除过期 $($_.Name)"
}
