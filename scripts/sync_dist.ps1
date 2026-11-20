# 把构建产物同步到本工程的 dist\：复制有变化的 exe，并清掉已下线程序的旧副本。
#
# 用法：powershell -NoProfile -File scripts\sync_dist.ps1 -Config AiZip\build
#
# 每个工程各有自己的 dist\（与 build\ 并列，例如 AiZip\dist\），互不干扰：
# 共用一个仓库根的 dist\ 时，清理过期产物只能靠文件名前缀猜哪些是自己那一族，
# 加一个工程就要多一条前缀规则，删错一次就是删掉别人的发布产物。
param(
  [string]$Config = ""
)

$ErrorActionPreference = "Stop"

# 必须指明是哪个构建目录：dist\ 的位置由它推出，猜错了会同步到别人的工程下
if (-not $Config) {
  Write-Error "用法：sync_dist.ps1 -Config <build 目录>（例如 AiZip\build 或 AiPic\build）"
  exit 1
}

# -Config 既可是 "AiZip\build" 这类相对仓库根的路径，也可是绝对路径
$root = Split-Path -Parent $PSScriptRoot
$buildDir = if ([System.IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $root $Config }

# dist\ 放在工程目录下（build\ 的父目录），不放在仓库根：
# 一个工程一个发布目录，清理时只动自己那份，不需要区分「谁的文件」。
$projectDir = Split-Path -Parent ([System.IO.Path]::GetFullPath($buildDir))
$dist = Join-Path $projectDir "dist"

# 发布清单：宿主 exe 名 → 该工程的产物表（源文件 → dist 里的名字）
#
# 两个 dll 必须随主程序一起发布，职责不同：
#   AiZipExt.dll  配置 ABI + COM 外壳，安装器与资源管理器都加载它
#   AiZipCore.dll 菜单文案与规则，每次弹菜单才加载，改它即刻生效
$products = @(
  @{
    Host = "AiZip.exe"
    Artifacts = @(
      @{ Source = "AiZipSetup.exe"; Name = "AiZipSetup.exe" }
      @{ Source = "AiZip.exe"; Name = "AiZip.exe" }
      @{ Source = "AiZipExt.dll"; Name = "AiZipExt.dll" }
      @{ Source = "AiZipCore.dll"; Name = "AiZipCore.dll" }
    )
  }
  @{
    Host = "AiPic.exe"
    Artifacts = @(
      @{ Source = "AiPic.exe"; Name = "AiPic.exe" }
    )
  }
  @{
    # 编辑器与看图分开两个 exe，但同属一个工程的 dist\
    Host = "AiPicEditor.exe"
    Artifacts = @(
      @{ Source = "AiPicEditor.exe"; Name = "AiPicEditor.exe" }
    )
  }
)

# 选中这次构建的工程。两个宿主都不存在时（例如只编了库）干脆什么都不做。
#
# 同一个工程可以有多个 exe（AiPic 看图 + AiPicEditor 编辑），所以这里取
# 「所有宿主已构建的项」，不是只取第一个——否则同一构建目录里只会同步一个。
$selected = @($products | Where-Object { Test-Path (Join-Path $buildDir $_.Host) })
if ($selected.Count -eq 0) {
  Write-Output "未找到可发布的 exe，跳过同步"
  exit 0
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null

$updated = 0
$skipped = 0
foreach ($product in $selected) {
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
    Write-Output "同步 dist\$($item.Name)  ${size} MB"
    $updated++
  }
}

# 只有真的没东西要同步时才算「最新」；被跳过说明 dist 可能落后，别说反了
if ($updated -eq 0 -and $skipped -eq 0) { Write-Output "dist 已是最新" }

# 本工程清单里没有的旧文件一律清掉，防止用户跑到已下线的程序。
# dist\ 已经按工程分开，这里可以放心清：目录里的东西全是本工程的。
#
# 删不掉不算失败：带指纹的右键菜单 dll 会被 explorer 加载，改名换新之后
# 那个旧文件仍然被锁着，直到资源管理器重启。为它中断整次构建不划算——
# 它已经不在发布清单里，用户不会跑到它。
$keep = @($selected | ForEach-Object { $_.Artifacts } | ForEach-Object { $_.Name })
Get-ChildItem -File $dist | Where-Object { $keep -notcontains $_.Name } | ForEach-Object {
  # 名字先存下来：进入 catch 后 $_ 变成错误记录，再取 $_ .Name 就是空的
  $gone = $_.Name
  try {
    Remove-Item -Force $_.FullName -ErrorAction Stop
    Write-Output "移除过期 $gone"
  } catch {
    Write-Warning "跳过（仍被占用）：$gone，可稍后手工删除"
  }
}
