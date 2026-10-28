# 生成 AIZip 图标（src/res/aizip.ico）。用法见 tools/README.md
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$src = @"
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

public class AIZipIcon {
  static GraphicsPath Rounded(RectangleF r, float rad) {
    var p = new GraphicsPath();
    float d = Math.Min(rad * 2, Math.Min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
    p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
    p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
    p.CloseFigure();
    return p;
  }

  // 青绿 -> 青 -> 蓝紫，对角三色渐变
  static LinearGradientBrush Grad(float S) {
    var b = new LinearGradientBrush(new PointF(0, 0), new PointF(S, S),
        Color.FromArgb(255, 0, 232, 176), Color.FromArgb(255, 118, 76, 255));
    var cb = new ColorBlend(3);
    cb.Colors = new Color[] {
      Color.FromArgb(255, 0, 232, 176),
      Color.FromArgb(255, 40, 200, 245),
      Color.FromArgb(255, 118, 76, 255) };
    cb.Positions = new float[] { 0f, 0.46f, 1f };
    b.InterpolationColors = cb;
    return b;
  }

  static void Tile(Graphics g, float S, float u) {
    var tile = Rounded(new RectangleF(20 * u, 20 * u, 984 * u, 984 * u), 226 * u);
    using (var br = Grad(S)) g.FillPath(br, tile);

    // 左上柔光，避免整块平渐变
    var sheen = new GraphicsPath();
    sheen.AddEllipse(-340 * u, -420 * u, 1180 * u, 980 * u);
    using (var pgb = new PathGradientBrush(sheen)) {
      pgb.CenterColor = Color.FromArgb(80, 255, 255, 255);
      pgb.SurroundColors = new Color[] { Color.FromArgb(0, 255, 255, 255) };
      g.FillPath(pgb, sheen);
    }
    sheen.Dispose();

    using (var pen = new Pen(Color.FromArgb(46, 255, 255, 255), 7 * u)) g.DrawPath(pen, tile);
    tile.Dispose();
  }

  // 白色 Z + 右侧竖拉链
  static void Glyph(Graphics g, float u) {
    using (var br = new SolidBrush(Color.White)) {
      var p = new GraphicsPath();
      float L = 196 * u, R = 596 * u, T = 286 * u, B = 738 * u, th = 140 * u;
      p.AddRectangle(new RectangleF(L, T, R - L, th));
      p.AddRectangle(new RectangleF(L, B - th, R - L, th));
      p.AddPolygon(new PointF[] {
        new PointF(R - th * 1.35f, T + th), new PointF(R, T + th),
        new PointF(L + th * 1.35f, B - th), new PointF(L, B - th) });
      g.FillPath(br, p);
      p.Dispose();

      // 拉链齿：只留 3 段粗齿，小尺寸下也不会糊成一条
      float zx = 664 * u, zw = 118 * u;
      for (int i = 0; i < 3; i++) {
        var t = Rounded(new RectangleF(zx, (376 + i * 116) * u, zw, 82 * u), 16 * u);
        g.FillPath(br, t); t.Dispose();
      }

      // 拉头：圆角方环，中间挖长孔
      var pull = Rounded(new RectangleF(640 * u, 214 * u, 166 * u, 108 * u), 50 * u);
      var hole = Rounded(new RectangleF(682 * u, 254 * u, 82 * u, 30 * u), 15 * u);
      pull.FillMode = FillMode.Alternate;
      pull.AddPath(hole, false);
      g.FillPath(br, pull);
      pull.Dispose(); hole.Dispose();
    }
  }

  public static Bitmap Render(int S) {
    // 小尺寸用更高倍数超采样，边缘更干净
    int big = S < 64 ? S * 8 : S * 4;
    var hi = new Bitmap(big, big, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(hi)) {
      g.SmoothingMode = SmoothingMode.AntiAlias;
      g.PixelOffsetMode = PixelOffsetMode.HighQuality;
      g.CompositingQuality = CompositingQuality.HighQuality;
      g.Clear(Color.Transparent);
      float u = big / 1024f;
      Tile(g, big, u);
      Glyph(g, u);
    }
    var outBmp = new Bitmap(S, S, PixelFormat.Format32bppArgb);
    using (var g2 = Graphics.FromImage(outBmp)) {
      g2.SmoothingMode = SmoothingMode.HighQuality;
      g2.InterpolationMode = InterpolationMode.HighQualityBicubic;
      g2.PixelOffsetMode = PixelOffsetMode.HighQuality;
      g2.DrawImage(hi, new Rectangle(0, 0, S, S));
    }
    hi.Dispose();
    return outBmp;
  }
}
"@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$outIco = Join-Path $root "src\res\aizip.ico"

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$datas = @()
foreach ($sz in $sizes) {
  $b = [AIZipIcon]::Render($sz)
  $ms = New-Object System.IO.MemoryStream
  $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $datas += , $ms.ToArray()
  $ms.Dispose()
  $b.Dispose()
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

Write-Host ("  {0}  {1:N0} B  {2} frames" -f $outIco, (Get-Item $outIco).Length, $sizes.Count)
