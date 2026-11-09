# 由 logo.svg 生成 AIZip 图标（src/res/aizip.ico）。用法见 tools/README.md
#
# 用 Chrome/Edge 无头模式把 SVG 栅格化成 PNG，再打包成多尺寸 ICO。
# logo.svg 自身留白较大（viewBox 40x40，图形只占中间一条），
# 所以渲染前用 JS 按实际 bbox 收紧 viewBox，让图形占满画布。
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$svgPath = Join-Path $root "logo.svg"
$outIco = Join-Path $root "src\res\aizip.ico"

if (-not (Test-Path $svgPath)) { throw "找不到 $svgPath" }

function Find-Browser {
  $candidates = @(
    "C:\Program Files\Google\Chrome\Application\chrome.exe",
    "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
    "C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
  )
  foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
  throw "需要 Chrome 或 Edge 来栅格化 SVG"
}

$browser = Find-Browser
$svg = [System.IO.File]::ReadAllText($svgPath)
$work = Join-Path $env:TEMP ("aizip_icon_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

function Render-Svg([int]$px, [string]$outFile) {
  $page = Join-Path $work "page_$px.html"
  # 收紧 viewBox：取图形实际 bbox 的最长边，四周留 5% 余量
  $script = @"
<script>
const s = document.querySelector('svg');
const b = s.getBBox();
const side = Math.max(b.width, b.height) * 1.10;
s.setAttribute('viewBox', (b.x + b.width / 2 - side / 2) + ' ' +
                          (b.y + b.height / 2 - side / 2) + ' ' + side + ' ' + side);
</script>
"@
  $html = @"
<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
html,body{margin:0;padding:0;background:transparent;overflow:hidden}
svg{display:block;width:${px}px;height:${px}px}
</style></head><body>
$svg
$script
</body></html>
"@
  [System.IO.File]::WriteAllText($page, $html, (New-Object System.Text.UTF8Encoding($false)))

  $prof = Join-Path $work "prof_$px"
  $url = "file:///" + ($page -replace '\\', '/')
  $errFile = Join-Path $work "err_$px.txt"
  # Chrome 会把扩展加载告警写到 stderr，用 Start-Process 隔离，
  # 否则 $ErrorActionPreference=Stop 会把它当成致命错误。
  $args = @(
    "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
    "--default-background-color=00000000", "--virtual-time-budget=3000",
    "--user-data-dir=$prof", "--window-size=$px,$px", "--screenshot=$outFile", $url
  )
  $proc = Start-Process -FilePath $browser -ArgumentList $args -NoNewWindow -Wait -PassThru `
    -RedirectStandardError $errFile -RedirectStandardOutput "$errFile.out"
  Remove-Item $errFile, "$errFile.out" -Force -ErrorAction SilentlyContinue

  if (-not (Test-Path $outFile)) { throw "渲染 $px px 失败（退出码 $($proc.ExitCode)）" }
}

# 小尺寸用更高倍数超采样，边缘更干净
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$datas = @()

foreach ($sz in $sizes) {
  $big = if ($sz -lt 64) { $sz * 8 } else { $sz * 4 }
  $raw = Join-Path $work "raw_$sz.png"
  Render-Svg $big $raw

  $src = [System.Drawing.Image]::FromFile($raw)
  $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $sz, $sz)))
  $g.Dispose()
  $src.Dispose()

  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $datas += , $ms.ToArray()
  $ms.Dispose()
  $bmp.Dispose()
}

# 组装 ICO（每帧存 PNG 数据）
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([UInt16]0)                    # reserved
$bw.Write([UInt16]1)                    # type = icon
$bw.Write([UInt16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $sz = $sizes[$i]
  $n = if ($sz -ge 256) { 0 } else { $sz }   # 256 在目录项里记 0
  $bw.Write([Byte]$n)
  $bw.Write([Byte]$n)
  $bw.Write([Byte]0)      # 调色板颜色数
  $bw.Write([Byte]0)      # reserved
  $bw.Write([UInt16]1)    # planes
  $bw.Write([UInt16]32)   # bpp
  $bw.Write([UInt32]$datas[$i].Length)
  $bw.Write([UInt32]$offset)
  $offset += $datas[$i].Length
}
foreach ($d in $datas) { $bw.Write($d) }
$bw.Flush()

[System.IO.File]::WriteAllBytes($outIco, $out.ToArray())
$bw.Dispose(); $out.Dispose()
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ("  {0}  {1:N0} B  {2} frames" -f $outIco, (Get-Item $outIco).Length, $sizes.Count)
