# 调试期快速部署：把构建产物直接替换到安装目录，跳过安装程序。
#
# 安装程序只在「安装范围 / 目标目录 / 清单与载荷」这些环节改动时才需要重跑。
# 日常改代码后用它部署：复制 exe 与两个 dll，再让程序自己校对一遍系统集成。
#
# 两个 dll 的换法不一样：
#   AiZipCore.dll 每次都换——它弹菜单时才加载、用完就卸，改了立刻生效。
#   AiZipExt.dll  内容没变就跳过——它被资源管理器长期持有，覆盖会失败；
#                 内容一样时根本没必要碰它。
#
# 用法：powershell -NoProfile -File scripts\deploy_debug.ps1

param(
  [string]$BuildDir = "$PSScriptRoot\..\AiZip\build",
  [string]$TargetDir = "D:\AISoft\AIZip"
)

$ErrorActionPreference = "Stop"

$srcExe = Join-Path $BuildDir "AiZip.exe"
$srcExt = Join-Path $BuildDir "AiZipExt.dll"
$srcCore = Join-Path $BuildDir "AiZipCore.dll"
foreach ($p in @($srcExe, $srcExt, $srcCore)) {
  if (-not (Test-Path $p)) { throw "找不到构建产物：$p" }
}
if (-not (Test-Path $TargetDir)) { throw "安装目录不存在：$TargetDir" }

# 程序自己也在锁 exe，先结束
Get-Process AiZip -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Copy-Item $srcExe (Join-Path $TargetDir "AiZip.exe") -Force
Write-Host "已替换 exe" -ForegroundColor Green

# 逻辑 dll：不被人长期持有，直接覆盖
Copy-Item $srcCore (Join-Path $TargetDir "AiZipCore.dll") -Force
Write-Host "已替换菜单逻辑" -ForegroundColor Green

# 外壳 dll：内容一致就跳过。资源管理器可能正持有它，覆盖会失败；
# 那次失败没有意义——文件本来就没有变化。
$dstExt = Join-Path $TargetDir "AiZipExt.dll"
$same = $false
if (Test-Path $dstExt) {
  try {
    $same = (Get-FileHash $srcExt -Algorithm SHA256).Hash -eq
            (Get-FileHash $dstExt -Algorithm SHA256).Hash
  } catch {
    $same = $false
  }
}
if ($same) {
  Write-Host "外壳未变化，跳过" -ForegroundColor DarkGray
} else {
  try {
    Copy-Item $srcExt $dstExt -Force -ErrorAction Stop
    Write-Host "已替换外壳（重启资源管理器后生效）" -ForegroundColor Yellow
  } catch {
    Write-Warning "外壳被占用，未能替换：重启资源管理器或重启系统后再试"
  }
}
# 启动一次让程序自愈：它会发现注册表不对就重新写入。
# 这里不等它退出——它是正常的主程序，留下来让开发者接着测。
$exe = Join-Path $TargetDir "AiZip.exe"
Start-Process $exe | Out-Null
Start-Sleep -Seconds 3
Write-Host "已启动程序完成自愈（右键菜单 + 文件关联）" -ForegroundColor Green

Get-ChildItem $TargetDir -Filter "*.dll" | ForEach-Object { Write-Host "  组件: $($_.Name)" }
