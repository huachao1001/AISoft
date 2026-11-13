# 调试期快速部署：把构建产物直接替换到安装目录并重新注册，跳过安装程序。
#
# 安装程序只在「安装范围 / 目标目录 / 内嵌资源」这些环节改动时才需要重跑。
# 日常改代码后用它部署：文件复制 + 让程序自己重新释放组件、写注册表。
#
# 用法：powershell -NoProfile -File scripts\deploy_debug.ps1

param(
  [string]$BuildDir = "$PSScriptRoot\..\AiZip\build",
  [string]$TargetDir = "D:\AISoft\AIZip"
)

$ErrorActionPreference = "Stop"

$srcExe = Join-Path $BuildDir "AiZip.exe"
if (-not (Test-Path $srcExe)) { throw "找不到构建产物：$srcExe" }
if (-not (Test-Path $TargetDir)) { throw "安装目录不存在：$TargetDir" }

# 程序自己也在锁 exe，先结束
Get-Process AiZip -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Copy-Item $srcExe (Join-Path $TargetDir "AiZip.exe") -Force
Write-Host "已替换 exe" -ForegroundColor Green

# 以「所有用户」范围重新注册：会释放新指纹的组件、清理旧组件、刷新文件关联。
# 需要管理员权限，已在提权终端里运行时不会弹 UAC。
$exe = Join-Path $TargetDir "AiZip.exe"
$proc = Start-Process $exe -ArgumentList "--register-shell-menu", "--scope=all-users" -PassThru -Wait
if ($proc.ExitCode -ne 0) { throw "注册失败，退出码 $($proc.ExitCode)" }
Write-Host "已重新注册（右键菜单 + 文件关联）" -ForegroundColor Green

Get-ChildItem $TargetDir -Filter "*.dll" | ForEach-Object { Write-Host "  组件: $($_.Name)" }
