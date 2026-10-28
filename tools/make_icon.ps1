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

  // 白色 A：两条斜边 + 横梁，全部圆头圆角
  static void DrawA(Graphics g, float u) {
    var pen = new Pen(Color.White, 128 * u);
    pen.StartCap = LineCap.Round;
    pen.EndCap = LineCap.Round;
    pen.LineJoin = LineJoin.Round;

    // Λ 两笔：顶点在上，两脚落地
    g.DrawLines(pen, new PointF[] {
      new PointF(82 * u, 780 * u),
      new PointF(296 * u, 246 * u),
      new PointF(506 * u, 780 * u) });
    pen.Dispose();

    // 横梁：左右各内缩半个笔画，避免戳出斜边外沿
    var bar = new Pen(Color.White, 128 * u);
    bar.StartCap = LineCap.Round;
    bar.EndCap = LineCap.Round;
    g.DrawLine(bar, 160 * u, 566 * u, 428 * u, 566 * u);
    bar.Dispose();
  }

  // 白色 I：等宽竖条当大写 I，内部挖指孔 + 交错齿槽，读作拉链
  static void DrawI(Graphics g, float u, bool detail) {
    float cx = 736, top = 252, bot = 780, w = 140;

    var path = new GraphicsPath();
    path.AddPath(Rounded(new RectangleF((cx - w / 2) * u, top * u, w * u, (bot - top) * u), w * 0.30f * u), true);
    path.FillMode = FillMode.Alternate;

    // 拉头指孔
    float hd = w * 0.30f;
    var hole = new GraphicsPath();
    hole.AddEllipse((cx - hd / 2) * u, (top + w * 0.34f) * u, hd * u, hd * u);
    path.AddPath(hole, false);
    hole.Dispose();

    // 齿槽：左右交错，小尺寸下省略避免糊成一片
    if (detail) {
      int teeth = 5;
      float t0 = top + w * 1.05f;
      float span = bot - t0 - w * 0.42f;
      float step = span / teeth;
      float sh = step * 0.50f, sw = w * 0.30f;
      for (int i = 0; i < teeth; i++) {
        float sy = t0 + step * i + step * 0.5f - sh / 2;
        float sx = (i % 2 == 0) ? (cx - w / 2 - 2) : (cx + w / 2 - sw + 2);
        var s = Rounded(new RectangleF(sx * u, sy * u, sw * u, sh * u), sh * 0.34f * u);
        path.AddPath(s, false);
        s.Dispose();
      }
    }

    using (var br = new SolidBrush(Color.White)) g.FillPath(br, path);
    path.Dispose();
  }

  static void Glyph(Graphics g, float u, bool detail) {
    DrawA(g, u);
    DrawI(g, u, detail);
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
      Glyph(g, u, S >= 40);
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
