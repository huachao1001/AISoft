// 自绘光标：系统没有「手掌抓取」这一对，只能自己画
//
// 为什么要两只：kGrab 是悬停时张开的手，kGrabbing 是按下去之后收拢的手。
// 二者必须一眼能分辨——否则用户按了半天看不出到底有没有抓住画布。
//
// 几何写在 32×32 的设计空间里，按 SM_CXCURSOR 等比放大；用 GDI+ 在 4 倍
// 超采样下画好再缩小，边缘才有抗锯齿。形状由「描边色铺满 + 实色盖回」两步
// 组成：白色盖住描边的内半边，只留外半边，指与掌重叠处不会残留内部线条，
// 比求并集简单且效果一致。
#include "cursor_windows.h"

#include "backend.h"

#include <cmath>
#include <cstring>

namespace AiUI {
namespace os {
namespace windows {

namespace {

using namespace Gdiplus;

// ── 设计参数：全部写在 32×32 的空间里 ──
// 手指：三个竖直胶囊 + 一个斜向拇指 + 一块圆角掌部。
// 三指而不是四指：32px 下四个指头会挤成一团，看不出是手。
constexpr float kDesign = 32.0f;
constexpr int kSuperSample = 4;

constexpr float kFingerR = 1.9f;
constexpr float kThumbR = 1.85f;
constexpr float kPalmR = 4.4f;
constexpr float kOutline = 1.45f;

constexpr float kFingerX[3] = {12.6f, 18.4f, 23.9f};
// 张开：指尖高低错落，像一只自然张开的手
constexpr float kTopOpen[3] = {4.4f, 1.6f, 3.8f};
// 抓取：三指一起收短，指节压平
constexpr float kTopGrip[3] = {9.6f, 7.2f, 9.2f};
constexpr float kFingerBottom = 17.4f;

constexpr float kThumbOpen[4] = {9.2f, 21.2f, 4.8f, 15.6f};   // x0,y0,x1,y1
// 抓取时拇指更往里收，指向掌心
constexpr float kThumbGrip[4] = {9.2f, 21.6f, 6.2f, 19.0f};

constexpr float kPalmOpen[4] = {9.2f, 13.0f, 27.4f, 28.8f};
constexpr float kPalmGrip[4] = {9.2f, 14.4f, 27.4f, 28.8f};

// 热区落在掌部左上角：和系统手形光标的点击位置一致，用户不会觉得偏
constexpr int kHotspotX = 6;
constexpr int kHotspotY = 4;

constexpr ARGB kInk = 0xFF1A1A1E;
constexpr ARGB kPaper = 0xFFFFFFFF;

// 圆角矩形路径（浮点）。painter_windows.cpp 里的同名函数在匿名空间里，
// 这里不能复用，而且这里要的是 float 版本，自己写一份更省事。
void roundRectPath(GraphicsPath& path, const RectF& rc, float radius) {
  const float w = rc.Width;
  const float h = rc.Height;
  if (w <= 0.0f || h <= 0.0f) return;
  const float r = (std::min)(radius, (std::min)(w, h) * 0.5f);
  if (r <= 0.0f) {
    path.AddRectangle(rc);
    return;
  }
  const float d = r * 2.0f;
  path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
  path.AddArc(rc.X + w - d, rc.Y, d, d, 270.0f, 90.0f);
  path.AddArc(rc.X + w - d, rc.Y + h - d, d, d, 0.0f, 90.0f);
  path.AddArc(rc.X, rc.Y + h - d, d, d, 90.0f, 90.0f);
  path.CloseFigure();
}

// 画一个圆头胶囊（填色）：先画一条粗线，再补两端圆点
void capsule(Graphics& g, ARGB color, float x0, float y0, float x1, float y1, float r) {
  SolidBrush brush(color);
  Pen pen(color, r * 2.0f);
  pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
  g.DrawLine(&pen, PointF(x0, y0), PointF(x1, y1));
  g.FillEllipse(&brush, x0 - r, y0 - r, r * 2.0f, r * 2.0f);
  g.FillEllipse(&brush, x1 - r, y1 - r, r * 2.0f, r * 2.0f);
}

struct Geometry {
  const float* top;
  const float* thumb;
  const float* palm;
};

Geometry geometryFor(bool grabbing) {
  if (!grabbing) return Geometry{kTopOpen, kThumbOpen, kPalmOpen};
  return Geometry{kTopGrip, kThumbGrip, kPalmGrip};
}

// 在同一张画布上先铺描边色、再盖实色，留下外半边描边
void paintHand(Graphics& g, bool grabbing, float grow, ARGB color) {
  const Geometry geo = geometryFor(grabbing);

  for (int i = 0; i < 3; ++i) {
    capsule(g, color, kFingerX[i], geo.top[i], kFingerX[i], kFingerBottom, kFingerR + grow);
  }
  capsule(g, color, geo.thumb[0], geo.thumb[1], geo.thumb[2], geo.thumb[3], kThumbR + grow);

  GraphicsPath path;
  const RectF palm(geo.palm[0] - grow, geo.palm[1] - grow,
                   geo.palm[2] - geo.palm[0] + grow * 2.0f,
                   geo.palm[3] - geo.palm[1] + grow * 2.0f);
  roundRectPath(path, palm, kPalmR + grow);
  SolidBrush brush(color);
  g.FillPath(&brush, &path);
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

  // 两步：描边色铺到外扩一圈，再盖实色。白色盖住描边的内半边，重叠处
  // 不会残留内部线条。
  paintHand(g, grabbing, kOutline, kInk);
  paintHand(g, grabbing, 0.0f, kPaper);
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

// 两只光标进程内各建一次：光标创建有系统调用代价，悬停切换很频繁。
// 句柄放在文件级静态量里，releaseCursorCache 才能真的删掉它们。
struct CursorCache {
  HICON open = nullptr;
  HICON closed = nullptr;
  bool built = false;
};

CursorCache& cursorCache() {
  static CursorCache cache;
  return cache;
}

}  // namespace

HICON handCursor(bool grabbing) {
  CursorCache& cache = cursorCache();
  if (!cache.built) {
    cache.built = true;
    cache.open = buildCursor(false);
    cache.closed = buildCursor(true);
  }
  return grabbing ? cache.closed : cache.open;
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
