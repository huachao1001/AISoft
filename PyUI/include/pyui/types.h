// 平台无关的基础类型：几何、颜色、句柄
//
// 公开头文件只暴露这里的中性类型。任何 HWND / CGContextRef / cairo_t
// 都不允许出现在 include/pyui/ 下，换平台时只替换 src/os/<platform>/。
#pragma once

#include <cstdint>
#include <string>

namespace pyui {

// 平台原生句柄。仅用于把根视图挂到宿主窗口上，控件内部不解释它的含义。
// Win32 下是 HWND，macOS 下是 NSView*，Linux 下是 X11 Window 或 wl_surface*。
using NativeHandle = void*;

// 32 位 ARGB，与主流绘制后端的像素布局一致（A 在高位）
using Color = std::uint32_t;

constexpr Color makeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                          std::uint8_t a = 255) {
  return (static_cast<Color>(a) << 24) | (static_cast<Color>(r) << 16) |
         (static_cast<Color>(g) << 8) | static_cast<Color>(b);
}

constexpr std::uint8_t colorAlpha(Color c) { return static_cast<std::uint8_t>(c >> 24); }
constexpr std::uint8_t colorRed(Color c) { return static_cast<std::uint8_t>(c >> 16); }
constexpr std::uint8_t colorGreen(Color c) { return static_cast<std::uint8_t>(c >> 8); }
constexpr std::uint8_t colorBlue(Color c) { return static_cast<std::uint8_t>(c); }

// 完全透明。作为「交给主题决定」的哨兵值使用。
constexpr Color kNoColor = 0;

// 把 over 按 ratio（0..1）叠到 base 上，两端都当作不透明。
// 用于「在任意底色上淡出一层墨色」：底色换了结果自动跟着变。
constexpr Color blendColor(Color base, Color over, float ratio) {
  auto mix = [ratio](std::uint8_t b, std::uint8_t o) {
    const float v = static_cast<float>(b) + (static_cast<float>(o) - static_cast<float>(b)) * ratio;
    return static_cast<std::uint8_t>(v + 0.5f);
  };
  if (ratio <= 0.0f) return base | 0xFF000000u;
  if (ratio >= 1.0f) return over | 0xFF000000u;
  return makeColor(mix(colorRed(base), colorRed(over)), mix(colorGreen(base), colorGreen(over)),
                   mix(colorBlue(base), colorBlue(over)));
}

// 依底色明暗派生描边色：浅底叠淡墨、深底叠淡白。
// 控件不该写死描边色，否则换主题或换宿主底色时要么糊进背景、要么刺眼。
constexpr Color edgeOn(Color background, int percent = 18) {
  const int luma = (colorRed(background) * 299 + colorGreen(background) * 587 +
                    colorBlue(background) * 114) /
                   1000;
  const Color ink = luma < 128 ? makeColor(0xff, 0xff, 0xff) : makeColor(0x00, 0x00, 0x00);
  return blendColor(background, ink, static_cast<float>(percent) / 100.0f);
}

struct Point {
  int x = 0;
  int y = 0;
};

constexpr bool operator==(Point a, Point b) { return a.x == b.x && a.y == b.y; }
constexpr bool operator!=(Point a, Point b) { return !(a == b); }

struct Size {
  int width = 0;
  int height = 0;

  bool isEmpty() const { return width <= 0 || height <= 0; }
};

constexpr bool operator==(Size a, Size b) {
  return a.width == b.width && a.height == b.height;
}
constexpr bool operator!=(Size a, Size b) { return !(a == b); }

// 四边间距
struct Insets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  static constexpr Insets all(int v) { return Insets{v, v, v, v}; }
  static constexpr Insets symmetric(int horizontal, int vertical) {
    return Insets{horizontal, vertical, horizontal, vertical};
  }
  int horizontal() const { return left + right; }
  int vertical() const { return top + bottom; }
};

constexpr bool operator==(Insets a, Insets b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
constexpr bool operator!=(Insets a, Insets b) { return !(a == b); }

// 子控件在容器内的对齐方式
enum class Gravity {
  kStart,
  kCenter,
  kEnd,
};

// 矩形：右下边界开区间，与绘制后端的惯例一致（{0,0,10,10} 是 100 像素）
struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  static constexpr Rect fromLtrb(int l, int t, int r, int b) { return Rect{l, t, r, b}; }
  static constexpr Rect fromSize(int w, int h) { return Rect{0, 0, w, h}; }
  static constexpr Rect at(int x, int y, int w, int h) { return Rect{x, y, x + w, y + h}; }

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  bool isEmpty() const { return right <= left || bottom <= top; }

  bool contains(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
  bool contains(int x, int y) const { return contains(Point{x, y}); }

  Point center() const { return Point{(left + right) / 2, (top + bottom) / 2}; }

  Rect inflated(int dx, int dy) const { return Rect{left - dx, top - dy, right + dx, bottom + dy}; }
  Rect deflated(int dx, int dy) const { return Rect{left + dx, top + dy, right - dx, bottom - dy}; }
  Rect inset(int l, int t, int r, int b) const {
    return Rect{left + l, top + t, right - r, bottom - b};
  }
  Rect inset(const Insets& i) const { return inset(i.left, i.top, i.right, i.bottom); }
  Rect moved(int dx, int dy) const { return Rect{left + dx, top + dy, right + dx, bottom + dy}; }

  Rect intersect(const Rect& o) const {
    Rect r{left > o.left ? left : o.left, top > o.top ? top : o.top,
           right < o.right ? right : o.right, bottom < o.bottom ? bottom : o.bottom};
    if (r.isEmpty()) return Rect{};
    return r;
  }

  bool operator==(const Rect& o) const {
    return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
  }
  bool operator!=(const Rect& o) const { return !(*this == o); }
};

// 设计尺寸到物理像素的换算基准。96 表示 1:1。
// 控件里所有 int 尺寸参数都是 96 DPI 设计值，内部用 View::px() 换算。
int designDpi();
void setDesignDpi(int dpi);
int scaleDesign(int value);  // 按当前 designDpi 换算并四舍五入

}  // namespace pyui
