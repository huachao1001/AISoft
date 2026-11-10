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

// 平台后端实现的最小图元集合
class PaintBackend {
 public:
  virtual ~PaintBackend() = default;

  virtual void fillRect(const Rect& rc, Color color) = 0;
  virtual void fillRoundRect(const Rect& rc, Color color, int radius) = 0;
  virtual void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) = 0;
  virtual void drawGradientRect(const Rect& rc, Color top, Color bottom) = 0;
  virtual void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) = 0;

  // 文本绘制：rc 为行框，单行垂直居中
  virtual void drawText(const Rect& rc, const std::wstring& text, Color color, const Font& font,
                        TextAlign align) = 0;
  virtual int measureText(const std::wstring& text, int from, int length, const Font& font) = 0;
  virtual int textHeight(const Font& font) = 0;

  // 图标字形：按墨迹框贴合 rc 居中
  virtual void drawIcon(IconId id, const Rect& rc, Color color) = 0;
  // 位图：等比缩放居中到 rc
  virtual void drawImage(const Image& image, const Rect& rc) = 0;

  virtual void setClip(const Rect& rc) = 0;
  virtual void resetClip() = 0;
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

  int measureText(const std::wstring& text, const Font& font) {
    return backend_.measureText(text, 0, static_cast<int>(text.size()), font);
  }
  int textHeight(const Font& font) { return backend_.textHeight(font); }

  // ── 图标与图片 ──
  void drawIcon(IconId id, const Rect& rc, Color color) { backend_.drawIcon(id, rc, color); }
  void drawImage(const Image& image, const Rect& rc) { backend_.drawImage(image, rc); }

  // ── 裁剪 ──
  void setClip(const Rect& rc) { backend_.setClip(rc); }
  void resetClip() { backend_.resetClip(); }

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
