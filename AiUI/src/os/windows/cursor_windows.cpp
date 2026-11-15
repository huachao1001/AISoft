// 自绘光标：系统没有「手掌抓取」这一对，只能自己画
//
// 两只手对应两个形状：kGrab 是张开的手（提示「可以抓住拖」），kGrabbing 是
// 按下去之后收拢的手。看图这类控件只在按住拖拽时用后者，松手就回箭头——但
// 形状本身仍要成对提供，否则按下与不按下无从区分。
//
// 造型取自 AiPic/contact-prevention.svg：四指短粗、指缝只切进指长的三分之一、
// 掌部大而圆，整只手是一条剪影，没有内部线条。
//   · 张开态直接沿参考图的内轮廓追踪再简化（84 点），所以比例和参考图一致。
//   · 抓取态用「星形径向并集」生成：以掌心为极点，对掌心椭圆、四根短指胶囊、
//     拇指胶囊逐角度取径向最大值。数学上不可能自交，指缝深度也天然受限于
//     胶囊半径——手绘或按 y 硬压都会在指根留下直墙或钩状的缺口。
//
// 画法：一条闭合多边形 → 填白 → 把画布裁到区域**外**再描粗笔。粗笔只有外
// 半边可见，等于只给剪影描边，内部不会出现任何接缝。
#include "cursor_windows.h"

#include "backend.h"

#include <cstdint>
#include <cstring>

namespace AiUI {
namespace os {
namespace windows {

namespace {

using namespace Gdiplus;

constexpr float kDesign = 32.0f;
constexpr int kSuperSample = 4;

// 张开的手：参考 SVG 内轮廓追踪 + Douglas-Peucker 简化，32×32 设计空间，顺时针。
constexpr int kOpenHandCount = 84;
constexpr float kOpenHand[kOpenHandCount][2] = {
    {18.04f, 11.56f}, {18.26f, 12.18f}, {18.58f, 12.47f}, {18.89f, 12.60f},
    {19.54f, 12.56f}, {20.06f, 12.15f}, {20.23f, 11.75f}, {21.47f, 5.76f},
    {21.67f, 5.42f}, {21.97f, 5.15f}, {22.34f, 5.00f}, {22.73f, 4.96f},
    {23.24f, 5.07f}, {23.65f, 5.34f}, {23.93f, 5.75f}, {24.05f, 6.24f},
    {22.99f, 13.99f}, {23.02f, 14.43f}, {23.29f, 14.91f}, {23.65f, 15.16f},
    {24.08f, 15.25f}, {24.51f, 15.17f}, {24.95f, 14.84f}, {26.49f, 11.06f},
    {26.72f, 10.74f}, {27.03f, 10.51f}, {27.67f, 10.36f}, {28.45f, 10.62f},
    {28.90f, 11.13f}, {29.04f, 11.81f}, {25.00f, 24.92f}, {24.47f, 26.22f},
    {24.09f, 26.91f}, {23.59f, 27.54f}, {22.90f, 28.17f}, {22.10f, 28.75f},
    {21.33f, 29.13f}, {20.41f, 29.41f}, {19.37f, 29.59f}, {18.24f, 29.68f},
    {17.07f, 29.67f}, {15.30f, 29.46f}, {13.63f, 29.02f}, {12.33f, 28.43f},
    {11.25f, 27.63f}, {9.85f, 26.12f}, {8.44f, 24.35f}, {3.23f, 17.14f},
    {3.02f, 16.72f}, {2.95f, 16.25f}, {3.08f, 15.63f}, {3.57f, 15.03f},
    {4.08f, 14.82f}, {4.77f, 14.86f}, {5.34f, 15.25f}, {8.72f, 19.00f},
    {9.13f, 19.19f}, {9.47f, 19.21f}, {9.80f, 19.14f}, {10.17f, 18.89f},
    {10.41f, 18.51f}, {10.48f, 17.96f}, {8.87f, 6.51f}, {8.93f, 5.86f},
    {9.31f, 5.30f}, {9.92f, 4.99f}, {10.32f, 4.96f}, {10.71f, 5.04f},
    {11.24f, 5.42f}, {11.52f, 6.01f}, {12.75f, 11.96f}, {13.00f, 12.32f},
    {13.37f, 12.55f}, {13.80f, 12.62f}, {14.23f, 12.52f}, {14.65f, 12.18f},
    {14.86f, 11.67f}, {15.13f, 3.41f}, {15.44f, 2.76f}, {16.02f, 2.37f},
    {16.62f, 2.31f}, {17.12f, 2.47f}, {17.53f, 2.83f}, {17.77f, 3.35f},
};

// 抓取的手：四指蜷成短粗的指节，指尖只比掌心高一点点，拇指伸在左侧。
// 指缝要浅：32px 下缝宽不足 1.5 像素时两侧描边会挤在一起，糊成黑楔子。
constexpr int kGripHandCount = 85;
constexpr float kGripHand[kGripHandCount][2] = {
    {7.77f, 22.20f}, {3.78f, 20.28f}, {3.41f, 19.90f}, {3.16f, 19.47f},
    {3.02f, 18.97f}, {3.01f, 18.49f}, {3.19f, 17.84f}, {3.60f, 17.28f},
    {4.20f, 16.87f}, {4.82f, 16.72f}, {5.86f, 16.97f}, {8.76f, 18.09f},
    {9.26f, 18.22f}, {8.06f, 12.53f}, {8.09f, 12.04f}, {8.22f, 11.64f},
    {8.47f, 11.25f}, {8.84f, 10.91f}, {9.30f, 10.69f}, {9.85f, 10.60f},
    {10.43f, 10.70f}, {10.86f, 10.94f}, {11.50f, 11.77f}, {13.62f, 15.02f},
    {14.10f, 15.61f}, {14.20f, 15.53f}, {14.22f, 15.35f}, {14.09f, 11.27f},
    {14.21f, 10.85f}, {14.43f, 10.48f}, {14.73f, 10.17f}, {15.12f, 9.92f},
    {15.76f, 9.76f}, {16.44f, 9.83f}, {17.06f, 10.16f}, {17.50f, 10.68f},
    {17.71f, 11.58f}, {17.90f, 14.39f}, {18.00f, 14.80f}, {19.99f, 15.02f},
    {21.02f, 12.33f}, {21.41f, 11.55f}, {21.84f, 11.22f}, {22.35f, 11.04f},
    {22.94f, 11.02f}, {23.46f, 11.17f}, {23.87f, 11.43f}, {24.20f, 11.81f},
    {24.42f, 12.26f}, {24.49f, 12.70f}, {24.42f, 13.30f}, {23.65f, 16.14f},
    {26.50f, 13.60f}, {26.92f, 13.41f}, {27.63f, 13.44f}, {28.28f, 13.79f},
    {28.69f, 14.36f}, {28.82f, 15.04f}, {28.71f, 15.57f}, {26.86f, 20.09f},
    {26.62f, 20.47f}, {26.14f, 20.90f}, {26.25f, 21.98f}, {26.21f, 22.93f},
    {26.02f, 23.93f}, {25.68f, 24.88f}, {25.22f, 25.77f}, {24.64f, 26.57f},
    {23.91f, 27.33f}, {23.11f, 27.98f}, {22.18f, 28.55f}, {21.18f, 29.00f},
    {20.12f, 29.33f}, {19.03f, 29.53f}, {17.93f, 29.60f}, {16.80f, 29.54f},
    {15.72f, 29.34f}, {14.65f, 29.02f}, {13.03f, 28.21f}, {11.66f, 27.12f},
    {10.90f, 26.24f}, {10.30f, 25.27f}, {9.88f, 24.27f}, {9.58f, 23.07f},
    {7.86f, 22.24f},
};

// 描边 0.85：32px 下再粗就会糊成一团黑，再细抗锯齿会把它冲淡成灰色。
constexpr float kOutline = 0.85f;

// 热区落在掌心：拖动画布时手在指针位置居中，视觉上才像「抓着」。
// 系统 hand 光标把热点放在指尖是因为它表示「指向」，这里语义不同。
constexpr int kHotspotX = 16;
constexpr int kHotspotY = 22;

constexpr ARGB kInk = 0xFF14141A;
constexpr ARGB kPaper = 0xFFFFFFFF;

// 点表 → 一条闭合多边形路径。首点与末点之间的收口单独补一段，
// 否则 AddLine 只连相邻两点，轮廓会缺一条边。
void pathFromTable(GraphicsPath& path, const float (*points)[2], int count) {
  path.SetFillMode(FillModeWinding);
  path.StartFigure();
  path.AddLine(points[0][0], points[0][1], points[1][0], points[1][1]);
  for (int i = 2; i < count; ++i) {
    path.AddLine(points[i - 1][0], points[i - 1][1], points[i][0], points[i][1]);
  }
  path.AddLine(points[count - 1][0], points[count - 1][1], points[0][0], points[0][1]);
  path.CloseFigure();
}

// 填白之后把画布裁到区域外再描边：粗笔只留外半边，内部接缝就看不见。
void paintHand(Graphics& g, bool grabbing) {
  GraphicsPath hand;
  if (grabbing) {
    pathFromTable(hand, kGripHand, kGripHandCount);
  } else {
    pathFromTable(hand, kOpenHand, kOpenHandCount);
  }

  SolidBrush paper(kPaper);
  g.FillPath(&paper, &hand);

  Region inside(&hand);
  g.SetClip(&inside, CombineModeExclude);
  Pen ink(kInk, kOutline * 2.0f);
  ink.SetLineJoin(LineJoinRound);
  g.DrawPath(&ink, &hand);
  g.ResetClip();
}

// 把一张 ARGB 位图转成 HICON（32bpp DIB + 1bpp mask）
HICON iconFromBitmap(Bitmap& bitmap, int size) {
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;  // 负高度 = 自上而下
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HBITMAP color = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color || !bits) {
    if (color) ::DeleteObject(color);
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }

  // 拷像素：GDI+ 的 stride 可能大于 width*4，必须按行取
  Gdiplus::Rect lockRect(0, 0, size, size);
  Gdiplus::BitmapData data{};
  if (bitmap.LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
    for (int y = 0; y < size; ++y) {
      const auto* src =
          static_cast<const uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
      std::memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * size * 4, src,
                  static_cast<size_t>(size) * 4);
    }
    bitmap.UnlockBits(&data);
  }
  ::ReleaseDC(nullptr, screen);

  // mask 全 0：透视靠 alpha 通道，mask 只是 ICONINFO 的必备字段
  HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);

  ICONINFO info{};
  info.fIcon = FALSE;  // 光标而不是图标
  info.xHotspot = kHotspotX * size / static_cast<int>(kDesign);
  info.yHotspot = kHotspotY * size / static_cast<int>(kDesign);
  info.hbmColor = color;
  info.hbmMask = mask;
  HICON icon = ::CreateIconIndirect(&info);

  ::DeleteObject(color);
  if (mask) ::DeleteObject(mask);
  return icon;
}

HICON buildCursor(bool grabbing) {
  const int size = ::GetSystemMetrics(SM_CXCURSOR);
  if (size <= 0) return nullptr;

  const int hi = size * kSuperSample;
  Bitmap canvas(hi, hi, PixelFormat32bppARGB);
  if (canvas.GetLastStatus() != Ok) return nullptr;

  Graphics g(&canvas);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
  // 超采样：先放大 k 倍画，缩回去时边缘才有抗锯齿
  g.ScaleTransform(static_cast<REAL>(hi) / kDesign, static_cast<REAL>(hi) / kDesign);

  paintHand(g, grabbing);
  g.ResetTransform();

  // 缩到目标边长
  Bitmap scaled(size, size, PixelFormat32bppARGB);
  if (scaled.GetLastStatus() != Ok) return nullptr;
  {
    Graphics sg(&scaled);
    sg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    sg.SetPixelOffsetMode(PixelOffsetModeHalf);
    sg.DrawImage(&canvas, Gdiplus::Rect(0, 0, size, size));
  }
  return iconFromBitmap(scaled, size);
}

// 两只光标进程内各建一次：光标创建有系统调用代价，拖拽时切换很频繁。
// 句柄放在文件级静态量里，releaseCursorCache 才能真的删掉它们。
// 分别记「是否已尝试」而不是只记一个总开关：只用握紧那只时不必顺带画张开那只。
struct CursorCache {
  HICON open = nullptr;
  HICON closed = nullptr;
  bool triedOpen = false;
  bool triedClosed = false;
};

CursorCache& cursorCache() {
  static CursorCache cache;
  return cache;
}

}  // namespace

HICON handCursor(bool grabbing) {
  CursorCache& cache = cursorCache();
  if (grabbing) {
    if (!cache.triedClosed) {
      cache.triedClosed = true;
      cache.closed = buildCursor(true);
    }
    return cache.closed;
  }
  if (!cache.triedOpen) {
    cache.triedOpen = true;
    cache.open = buildCursor(false);
  }
  return cache.open;
}

void releaseCursorCache() {
  CursorCache& cache = cursorCache();
  if (cache.open) ::DestroyIcon(cache.open);
  if (cache.closed) ::DestroyIcon(cache.closed);
  cache = CursorCache{};
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
