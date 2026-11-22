// TextView：单行/多行文字，支持对齐、省略、颜色与内边距
//
// 与 AiUI 的差别只有一处：文字用 std::string（UTF-8），不用 std::wstring。
// 这决定了省略号必须按 UTF-8 字符边界二分，不能按字节切——否则中文和 emoji
// 会被切成半个码点，画出来是乱码方块。
#pragma once

#include "pyui/painter.h"
#include "pyui/text_renderer.h"
#include "pyui/theme.h"
#include "pyui/view.h"

#include <string>

namespace pyui {

enum class TextSize {
  kSmall,
  kNormal,
  kLarge,
  kHuge,
};

struct TextStyle {
  Color color = kNoColor;
  // 非空时文字色取自当前主题的相关字段，优先于 color
  Color Theme::*colorRole = nullptr;
  TextAlign align = TextAlign::kLeft;
  bool ellipsis = true;  // 超宽时显示省略号
  // 省略中段而不是尾部：文件名要保住扩展名，尾部省略会把 .png 裁掉
  bool middleEllipsis = false;
  bool bold = false;
  TextSize size = TextSize::kNormal;
  Insets padding{};

  // 按档位取字号（96 DPI 设计值）
  float pointSize() const {
    switch (size) {
      case TextSize::kSmall:
        return 8.0f;
      case TextSize::kLarge:
        return 11.0f;
      case TextSize::kHuge:
        return 14.0f;
      default:
        return 9.0f;
    }
  }
};

class TextView : public View {
 public:
  TextView() = default;
  explicit TextView(std::string text) { setText(std::move(text)); }

  void setText(std::string text);
  const std::string& text() const { return text_; }

  void setTextStyle(const TextStyle& style);
  const TextStyle& textStyle() const { return style_; }

  void setColor(Color color);
  void setAlign(TextAlign align);
  void setBold(bool bold);
  void setTextSize(TextSize size);
  void setEllipsis(bool ellipsis);
  bool hasEllipsis() const { return style_.ellipsis; }
  void setMiddleEllipsis(bool middleEllipsis);
  bool hasMiddleEllipsis() const { return style_.middleEllipsis; }

  const char* className() const override { return "TextView"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

  // 解析后的墨色；子类（如标题栏）可覆盖以跟随窗口激活状态
  virtual Color resolveInk() const;
  // 实际绘制用的字体，按 dpi 取缓存
  Font resolveFont() const;

 private:
  std::string text_;
  TextStyle style_{};
};

}  // namespace pyui
