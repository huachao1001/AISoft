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
// 比张开态再缩到 0.85：握起来本来就更小，也和「抓住了」的手感一致。
// 指缝要浅——32px 下缝宽不足 1.5 像素时两侧描边会挤在一起，糊成黑楔子。
constexpr int kGripHandCount = 85;
constexpr float kGripHand[kGripHandCount][2] = {
    {9.00f, 22.17f}, {5.61f, 20.54f}, {5.30f, 20.21f}, {5.09f, 19.85f},
    {4.97f, 19.42f}, {4.96f, 19.02f}, {5.11f, 18.46f}, {5.46f, 17.99f},
    {5.97f, 17.64f}, {6.50f, 17.51f}, {7.38f, 17.72f}, {9.85f, 18.68f},
    {10.27f, 18.79f}, {9.25f, 13.95f}, {9.28f, 13.53f}, {9.39f, 13.19f},
    {9.60f, 12.86f}, {9.91f, 12.57f}, {10.30f, 12.39f}, {10.77f, 12.31f},
    {11.27f, 12.39f}, {11.63f, 12.60f}, {12.18f, 13.30f}, {13.98f, 16.07f},
    {14.38f, 16.57f}, {14.47f, 16.50f}, {14.49f, 16.35f}, {14.38f, 12.88f},
    {14.48f, 12.52f}, {14.67f, 12.21f}, {14.92f, 11.94f}, {15.25f, 11.73f},
    {15.80f, 11.60f}, {16.37f, 11.66f}, {16.90f, 11.94f}, {17.27f, 12.38f},
    {17.45f, 13.14f}, {17.61f, 15.53f}, {17.70f, 15.88f}, {19.39f, 16.07f},
    {20.27f, 13.78f}, {20.60f, 13.12f}, {20.96f, 12.84f}, {21.40f, 12.68f},
    {21.90f, 12.67f}, {22.34f, 12.79f}, {22.69f, 13.02f}, {22.97f, 13.34f},
    {23.16f, 13.72f}, {23.22f, 14.09f}, {23.16f, 14.61f}, {22.50f, 17.02f},
    {24.92f, 14.86f}, {25.28f, 14.70f}, {25.89f, 14.72f}, {26.44f, 15.02f},
    {26.79f, 15.51f}, {26.90f, 16.08f}, {26.80f, 16.53f}, {25.23f, 20.38f},
    {25.03f, 20.70f}, {24.62f, 21.06f}, {24.71f, 21.98f}, {24.68f, 22.79f},
    {24.52f, 23.64f}, {24.23f, 24.45f}, {23.84f, 25.20f}, {23.34f, 25.88f},
    {22.72f, 26.53f}, {22.04f, 27.08f}, {21.25f, 27.57f}, {20.40f, 27.95f},
    {19.50f, 28.23f}, {18.58f, 28.40f}, {17.64f, 28.46f}, {16.68f, 28.41f},
    {15.76f, 28.24f}, {14.85f, 27.97f}, {13.48f, 27.28f}, {12.31f, 26.35f},
    {11.66f, 25.60f}, {11.16f, 24.78f}, {10.80f, 23.93f}, {10.54f, 22.91f},
    {9.08f, 22.20f},
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
