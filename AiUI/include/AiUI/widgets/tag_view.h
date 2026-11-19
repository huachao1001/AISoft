// TagView：圆角底色的短标签
//
// 状态栏里的格式标签（"PNG"）就是这个样子。标题栏这类地方也要摆一串
// 同款标签时，各控件各画一遍就会深浅不一，所以收成一个控件。
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <string>

namespace AiUI {

struct TagStyle {
  // 96 DPI 设计值
  int padX = 6;
  int height = 19;
  int radius = 4;
  // 颜色：kNoColor 表示跟随主题的 tagBg / tagInk
  Color background = kNoColor;
  Color ink = kNoColor;
  bool smallFont = true;
};

class TagView : public View {
 public:
  TagView() = default;
  explicit TagView(std::wstring text) { setText(std::move(text)); }

  void setText(const std::wstring& text);
  const std::wstring& text() const { return text_; }

  void setStyle(const TagStyle& style);
  const TagStyle& style() const { return style_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

 private:
  const Font& resolvedFont() const;

  std::wstring text_;
  TagStyle style_{};
};

}  // namespace AiUI
