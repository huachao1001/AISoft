// TextView：单行/多行文字，支持对齐、省略、颜色与内边距
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <string>

namespace AiUI {

struct TextStyle {
  Color color = kColorText;
  TextAlign align = TextAlign::kLeft;
  bool ellipsis = true;       // 超宽时显示省略号
  bool bold = false;
  bool small = false;         // 用小字号
  int paddingLeft = 0;
  int paddingTop = 0;
  int paddingRight = 0;
  int paddingBottom = 0;
};

class TextView : public Widget {
 public:
  TextView() { className_ = L"AiUITextView"; }
  explicit TextView(std::wstring text) : TextView() { setText(std::move(text)); }

  void setText(const std::wstring& text);
  const std::wstring& text() const { return text_; }

  void setTextStyle(const TextStyle& style);
  const TextStyle& textStyle() const { return style_; }

  void setColor(Color color);
  void setAlign(TextAlign align);

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

 private:
  std::wstring text_;
  TextStyle style_{};
};

}  // namespace AiUI
