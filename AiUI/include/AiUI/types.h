// 平台无关的基础类型：几何与颜色
//
// 公开头文件只暴露这些中性类型，不出现 HWND / HDC / COLORREF 等
// 平台名词。换平台时只需替换 src/os/<platform>/ 下的后端实现。
#pragma once

#include <cstdint>
#include <string>

namespace AiUI {

// 平台原生句柄（Win32 下是 HWND）。仅用于把控件挂到宿主窗口上，
// 控件内部不解释它的含义，换平台时由后端负责转换。
using NativeHandle = void*;

// 32 位 ARGB 颜色，与主流绘制后端的像素布局一致
using Color = std::uint32_t;

// 由分量拼出颜色，a 缺省为不透明
constexpr Color makeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                          std::uint8_t a = 255) {
  return (static_cast<Color>(a) << 24) | (static_cast<Color>(r) << 16) |
         (static_cast<Color>(g) << 8) | static_cast<Color>(b);
}

// 逐分量取色
constexpr std::uint8_t colorAlpha(Color c) { return static_cast<std::uint8_t>(c >> 24); }
constexpr std::uint8_t colorRed(Color c) { return static_cast<std::uint8_t>(c >> 16); }
constexpr std::uint8_t colorGreen(Color c) { return static_cast<std::uint8_t>(c >> 8); }
constexpr std::uint8_t colorBlue(Color c) { return static_cast<std::uint8_t>(c); }

// 完全透明的哨兵值，表示“交给主题决定”
constexpr Color kNoColor = 0;

// 把 over 按 ratio（0..1）叠到 base 上，两端颜色都当作不透明。
// 用于「在任意底色上淡出一层墨色」这类派生色：底色换了，结果自动跟着变。
constexpr Color blendColor(Color base, Color over, float ratio) {
  if (ratio <= 0.0f) return base;
  if (ratio >= 1.0f) return over;
  auto mix = [ratio](std::uint8_t b, std::uint8_t o) {
    return static_cast<std::uint8_t>(static_cast<float>(b) +
                                     (static_cast<float>(o) - static_cast<float>(b)) * ratio +
                                     0.5f);
  };
  return makeColor(mix(colorRed(base), colorRed(over)), mix(colorGreen(base), colorGreen(over)),
                   mix(colorBlue(base), colorBlue(over)));
}

struct Point {
  int x = 0;
  int y = 0;
};

struct Size {
  int width = 0;
  int height = 0;
};

// 四边间距
struct Insets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  static Insets all(int v) { return Insets{v, v, v, v}; }
  static Insets symmetric(int horizontal, int vertical) {
    return Insets{horizontal, vertical, horizontal, vertical};
  }
  int horizontal() const { return left + right; }
  int vertical() const { return top + bottom; }
};

// 子控件在容器内的对齐方式
enum class Gravity {
  kStart,   // 靠左 / 靠上
  kCenter,  // 居中
  kEnd,     // 靠右 / 靠下
};

// 矩形：右下边界开区间，与绘制后端的惯例一致
struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  static Rect fromLtrb(int l, int t, int r, int b) { return Rect{l, t, r, b}; }
  static Rect fromSize(int w, int h) { return Rect{0, 0, w, h}; }

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  bool isEmpty() const { return right <= left || bottom <= top; }

  bool contains(Point p) const {
    return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
  }
  bool contains(int x, int y) const { return contains(Point{x, y}); }

  Point center() const { return Point{(left + right) / 2, (top + bottom) / 2}; }

  // 四边同时放大/缩小
  Rect inflated(int dx, int dy) const { return Rect{left - dx, top - dy, right + dx, bottom + dy}; }
  Rect deflated(int dx, int dy) const { return Rect{left + dx, top + dy, right - dx, bottom - dy}; }
  Rect inset(int l, int t, int r, int b) const {
    return Rect{left + l, top + t, right - r, bottom - b};
  }
  Rect moved(int dx, int dy) const { return Rect{left + dx, top + dy, right + dx, bottom + dy}; }

  // 取交集；无交集时返回空矩形
  Rect intersect(const Rect& o) const {
    Rect r{(left > o.left ? left : o.left), (top > o.top ? top : o.top),
           (right < o.right ? right : o.right), (bottom < o.bottom ? bottom : o.bottom)};
    if (r.isEmpty()) return Rect{};
    return r;
  }

  bool operator==(const Rect& o) const {
    return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
  }
  bool operator!=(const Rect& o) const { return !(*this == o); }
};

}  // namespace AiUI
