# 调试期快速部署：把构建产物直接替换到安装目录，跳过安装程序。
#
# 安装程序只在「安装范围 / 目标目录 / 清单与载荷」这些环节改动时才需要重跑。
# 日常改代码后用它部署：复制 AiZip.exe 与 aizip_config.dll，再让程序
# 自己校对一遍系统集成。
#
# 为什么不带 --register-shell-menu：那个通道已经删掉了。现在右键菜单与
# 文件关联的实现都在 aizip_config.dll 里，主程序每次启动会自己调用它，
# 发现不对就修好。部署完启动一次程序即可。
#
# 用法：powershell -NoProfile -File scripts\deploy_debug.ps1

param(
  [string]$BuildDir = "$PSScriptRoot\..\AiZip\build",
  [string]$TargetDir = "D:\AISoft\AIZip"
)

$ErrorActionPreference = "Stop"

$srcExe = Join-Path $BuildDir "AiZip.exe"
$srcConfig = Join-Path $BuildDir "aizip_config.dll"
if (-not (Test-Path $srcExe)) { throw "找不到构建产物：$srcExe" }
if (-not (Test-Path $srcConfig)) { throw "找不到配置模块：$srcConfig" }
if (-not (Test-Path $TargetDir)) { throw "安装目录不存在：$TargetDir" }

# 程序自己也在锁 exe 与配置 dll，先结束
Get-Process AiZip -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Copy-Item $srcExe (Join-Path $TargetDir "AiZip.exe") -Force
Write-Host "已替换 exe" -ForegroundColor Green

# 配置 dll 必须一起换：主程序要加载它来修菜单，缺了就只能静默跳过
Copy-Item $srcConfig (Join-Path $TargetDir "aizip_config.dll") -Force
Write-Host "已替换配置模块" -ForegroundColor Green

# 启动一次让程序自愈：它会发现组件指纹或注册表不对，重新释放并写回。
# 这里不等它退出——它是正常的主程序，留下来让开发者接着测。
$exe = Join-Path $TargetDir "AiZip.exe"
Start-Process $exe | Out-Null
Start-Sleep -Seconds 3
Write-Host "已启动程序完成自愈（右键菜单 + 文件关联）" -ForegroundColor Green

Get-ChildItem $TargetDir -Filter "*.dll" | ForEach-Object { Write-Host "  组件: $($_.Name)" }
