// 绘制抽象
//
// 分两层：
//   PaintBackend —— 平台后端只需实现这几个最基础的图元
//   Painter      —— 面向控件的画笔，圆角/渐变/省略号/图标等组合逻辑
//                   写在这里，换平台时不必重写
//
// 控件绘制代码只依赖 Painter，不接触后端。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/types.h"

#include <string>
#include <algorithm>

namespace AiUI {

// 字体句柄：由字体后端创建，绘制时按值传递
class Font {
 public:
  Font() = default;
  explicit Font(void* handle, int pixelHeight = 0)
      : handle_(handle), pixelHeight_(pixelHeight) {}
  void* handle() const { return handle_; }
  int pixelHeight() const { return pixelHeight_; }
  bool isValid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
  int pixelHeight_ = 0;
};

enum class TextAlign { kLeft, kCenter, kRight };

// 位图缩放时的采样方式。
// 放大到像素级时逐像素取整比插值更真实——用户要看清的是像素格，不是
// 一团被抹平的色块；其余场合用平滑采样，缩小时才不会出现锯齿摩尔纹。
enum class ImageSmoothing { kSmooth, kNearest };

// 平台后端实现的最小图元集合
class PaintBackend {
 public:
  virtual ~PaintBackend() = default;

  virtual void fillRect(const Rect& rc, Color color) = 0;
  virtual void fillRoundRect(const Rect& rc, Color color, int radius) = 0;
  // 棋盘格：透明区域的「没有像素」底纹，两色按 cell 大小交替铺满 rc。
  //
  // 图案相位锚在**坐标系原点**而不是 rc 的左上角：调用方就是要靠这个特性，
  // 让底纹在内容移动时保持不动（拖动图片时格子固定，才有放大镜的感觉）。
  // 若锚在 rc 上，每次 rc 一变图案就跟着平移，格子就「粘」在图片上了。
  // 默认实现逐行填，够用但慢；后端有贴图能力时应当覆盖。
  virtual void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) {
    if (rc.isEmpty() || cell <= 0) return;
    fillRect(rc, colorA);
    // 负坐标也要正确分块，所以用向下取整除法而不是整数截断
    auto floorDiv = [cell](int v) {
      return v >= 0 ? v / cell : -(((-v) + cell - 1) / cell);
    };
    // 分块边界必须落在 cell 的整数倍上，不能从 rc.top/left 起步：
    // 否则第一块只是被 rc 切掉一角的半格，图案和相邻块对不上。
    const int x0 = floorDiv(rc.left) * cell;
    const int y0 = floorDiv(rc.top) * cell;
    for (int y = y0; y < rc.bottom; y += cell) {
      const int top = (std::max)(y, rc.top);
      const int bottom = (std::min)(y + cell, rc.bottom);
      // y 已是 cell 的整数倍，直接取奇偶；负数在补码下 & 1 同样正确
      const bool oddRow = ((y / cell) & 1) != 0;
      for (int x = x0; x < rc.right; x += cell) {
        const int left = (std::max)(x, rc.left);
        const int right = (std::min)(x + cell, rc.right);
        if (right <= left || bottom <= top) continue;
        const bool oddCol = ((x / cell) & 1) != 0;
        if (oddRow == oddCol) continue;
        fillRect(Rect{left, top, right, bottom}, colorB);
      }
    }
  }
  virtual void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) = 0;
  virtual void drawGradientRect(const Rect& rc, Color top, Color bottom) = 0;
  virtual void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) = 0;

  // 文本绘制：rc 为行框，单行垂直居中
  virtual void drawText(const Rect& rc, const std::wstring& text, Color color, const Font& font,
                        TextAlign align) = 0;
  // 文本运行绘制：供需要逐字符定位的控件（输入框、编辑器）使用。
  // 必须与 Platform::glyphBounds / advanceText 用同一套排版，三者坐标才同源：
  // 排版原点就是 rc.left，字符单元边界就是插入点位置。
  // 后端未单独实现时退回普通绘制，普通绘制的留白会让插入点错位，
  // 所以有编辑功能的平台必须覆盖它。
  virtual void drawTextRun(const Rect& rc, const std::wstring& text, Color color,
                           const Font& font) {
    drawText(rc, text, color, font, TextAlign::kLeft);
  }
  virtual int measureText(const std::wstring& text, int from, int length, const Font& font) = 0;
  virtual int textHeight(const Font& font) = 0;

  // 图标字形：按墨迹框贴合 rc 居中
  virtual void drawIcon(IconId id, const Rect& rc, Color color) = 0;
  // 位图：等比缩放居中到 rc
  virtual void drawImage(const Image& image, const Rect& rc) = 0;
  // 位图：只取 source 那一块（原图像素坐标）铺到 rc。
  // 放大查看时视口只覆盖图像的一小部分，整张铺过去会让后端白算看不见的
  // 区域；给出源矩形后后端只需处理可见部分。
  // source 为空表示整张图。
  virtual void drawImage(const Image& image, const Rect& rc, const Rect& source) {
    (void)source;
    drawImage(image, rc);
  }
  // 同上的采样方式选择；后端不支持时按默认平滑处理。
  // alpha 把整张图按比例淡下去（0 全透明、1 不透明）：缩放跨过某个档位时
  // 新旧两种画法要交叉淡入淡出，就得能画半透明的图。
  virtual void drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                               ImageSmoothing smoothing, float alpha) {
    (void)smoothing;
    (void)alpha;
    drawImage(image, rc, source);
  }

  virtual void setClip(const Rect& rc) = 0;
  virtual void resetClip() = 0;
  // 与当前裁剪区求交；配合 save/restore 逐层收缩
  virtual void clipRect(const Rect& rc) = 0;
  // 圆角裁剪：与当前裁剪区求交，但边界带圆角。
  // 用于「圆角容器里铺满整行的底色」——矩形裁剪会盖住容器的圆角。
  virtual void clipRoundRect(const Rect& rc, int radius) {
    (void)radius;
    clipRect(rc);
  }

  // ── 坐标平移与绘制状态栈：子树绘制靠它把原点移到子视图左上角
  virtual void translate(int dx, int dy) = 0;
  // 绕当前原点旋转（角度，顺时针为正）。图片按 90° 倍数旋转靠它，
  // 避免每次转角都重采样一遍像素。
  virtual void rotate(float degrees) { (void)degrees; }
  virtual void save() = 0;
  virtual void restore() = 0;
};

// 面向控件的画笔：坐标是控件客户区绝对坐标
class Painter {
 public:
  explicit Painter(PaintBackend& backend) : backend_(backend) {}
  virtual ~Painter() = default;

  // ── 几何 ──
  void fillRect(const Rect& rc, Color color) { backend_.fillRect(rc, color); }
  void fillRoundRect(const Rect& rc, Color color, int radius) {
    backend_.fillRoundRect(rc, color, radius);
  }
  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth = 1) {
    backend_.strokeRoundRect(rc, color, radius, lineWidth);
  }
  void drawGradientRect(const Rect& rc, Color top, Color bottom) {
    backend_.drawGradientRect(rc, top, bottom);
  }
  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) {
    backend_.drawGradientRoundRect(rc, top, bottom, radius);
  }
  // 棋盘格底纹：格子相位锚在坐标系原点，内容移动时图案不动
  void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) {
    backend_.fillCheckerboard(rc, colorA, colorB, cell);
  }
  void drawHLine(int left, int right, int y, Color color) {
    backend_.fillRect(Rect{left, y, right, y + 1}, color);
  }
  void drawVLine(int top, int bottom, int x, Color color) {
    backend_.fillRect(Rect{x, top, x + 1, bottom}, color);
  }

  // ── 文本 ──
  void drawText(const Rect& rc, const std::wstring& text, Color color, const Font& font,
                TextAlign align = TextAlign::kLeft) {
    if (text.empty() || rc.isEmpty()) return;
    backend_.drawText(rc, text, color, font, align);
  }

  // 尾部省略号：二分找最长可容纳前缀
  void drawTextEllipsis(const Rect& rc, const std::wstring& text, Color color, const Font& font) {
    if (text.empty() || rc.isEmpty()) return;
    const int limit = rc.width();
    if (measureText(text, font) <= limit) {
      backend_.drawText(rc, text, color, font, TextAlign::kLeft);
      return;
    }
    static const std::wstring dots = L"…";
    const int dotsW = measureText(dots, font);
    int lo = 0;
    int hi = static_cast<int>(text.size());
    while (lo < hi) {
      const int mid = (lo + hi + 1) / 2;
      if (backend_.measureText(text, 0, mid, font) + dotsW <= limit) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    backend_.drawText(rc, text.substr(0, static_cast<size_t>(lo)) + dots, color, font,
                      TextAlign::kLeft);
  }

  // 中间省略号：保留首尾、省略中段。
  //
  // 文件名必须这样处理——尾部省略会把扩展名干掉，而「是不是 png」恰好是
  // 用户最先要认的信息；首部省略则认不出是哪份文件。两头各留一截才对。
  // 尾段优先保住扩展名与版本号，所以尾部保留的字数按可用宽度反推，
  // 但不能少于「扩展名 + 一点名字」，否则只剩个 ".png"。
  void drawTextMiddleEllipsis(const Rect& rc, const std::wstring& text, Color color,
                              const Font& font) {
    if (text.empty() || rc.isEmpty()) return;
    const int limit = rc.width();
    if (measureText(text, font) <= limit) {
      backend_.drawText(rc, text, color, font, TextAlign::kLeft);
      return;
    }
    static const std::wstring dots = L"…";
    const int dotsW = measureText(dots, font);
    if (dotsW >= limit) {
      // 连省略号都放不下，交给尾部省略去裁
      drawTextEllipsis(rc, text, color, font);
      return;
    }

    const int total = static_cast<int>(text.size());
    // 尾部先按一半宽度起步，再二分加长：把宽度预算均分给首尾，
    // 长文件名两侧都能留下可辨认的片段
    int tail = total / 2;
    int lo = 1;
    int hi = total;
    while (lo < hi) {
      const int mid = (lo + hi + 1) / 2;
      if (measureText(text.substr(static_cast<size_t>(total - mid)), font) + dotsW <=
          limit) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    tail = (std::min)(lo, total - 1);

    // 剩余宽度给首段
    const int tailW = measureText(text.substr(static_cast<size_t>(total - tail)), font);
    const int headBudget = limit - tailW - dotsW;
    int head = 0;
    if (headBudget > 0) {
      lo = 0;
      hi = total - tail;
      while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (backend_.measureText(text, 0, mid, font) <= headBudget) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      head = lo;
    }
    backend_.drawText(rc, text.substr(0, static_cast<size_t>(head)) + dots +
                              text.substr(static_cast<size_t>(total - tail)),
                      color, font, TextAlign::kLeft);
  }

  int measureText(const std::wstring& text, const Font& font) {
    return backend_.measureText(text, 0, static_cast<int>(text.size()), font);
  }
  int textHeight(const Font& font) { return backend_.textHeight(font); }

  // 按排版坐标绘制文本：原点即 rc.left，不加左右留白。
  // 光标/选区定位必须与它配合，否则会整体偏移半个留白。
  void drawTextRun(const Rect& rc, const std::wstring& text, Color color, const Font& font) {
    if (text.empty() || rc.isEmpty()) return;
    backend_.drawTextRun(rc, text, color, font);
  }

  // ── 图标与图片 ──
  void drawIcon(IconId id, const Rect& rc, Color color) { backend_.drawIcon(id, rc, color); }
  void drawImage(const Image& image, const Rect& rc) { backend_.drawImage(image, rc); }
  // 只画源图的某一块（原图像素坐标），放大查看时用
  void drawImage(const Image& image, const Rect& rc, const Rect& source) {
    backend_.drawImage(image, rc, source);
  }
  // 指定采样方式：放大到像素级时用 kNearest，其余用 kSmooth；
  // alpha < 1 时整张图半透明，用于跨档位的交叉淡化
  void drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                       ImageSmoothing smoothing, float alpha = 1.0f) {
    if (alpha <= 0.0f) return;
    backend_.drawImageScaled(image, rc, source, smoothing, alpha);
  }

  // ── 裁剪 ──
  void setClip(const Rect& rc) { backend_.setClip(rc); }
  void resetClip() { backend_.resetClip(); }
  // 与已有裁剪区求交，用于逐层收缩子树可见范围
  void clipRect(const Rect& rc) { backend_.clipRect(rc); }
  // 圆角裁剪：铺满整行的底色不会盖住容器圆角
  void clipRoundRect(const Rect& rc, int radius) { backend_.clipRoundRect(rc, radius); }

  // ── 坐标平移 ──
  // 进入子视图前平移原点，返回时用 save/restore 还原
  void translate(int dx, int dy) { backend_.translate(dx, dy); }
  // 绕当前原点旋转；配合 translate 把旋转中心挪到目标位置
  void rotate(float degrees) { backend_.rotate(degrees); }
  void save() { backend_.save(); }
  void restore() { backend_.restore(); }

 private:
  PaintBackend& backend_;
};

// 离屏画布：先画到位图再一次性贴回，避免闪烁与半帧
class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual Painter& painter() = 0;
  virtual void commit() = 0;
};

}  // namespace AiUI
