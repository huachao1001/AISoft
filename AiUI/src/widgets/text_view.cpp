// TextView 实现
#include "AiUI/widgets/text_view.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"

namespace AiUI {

void TextView::setText(const std::wstring& text) {
  if (text_ == text) return;
  text_ = text;
  invalidate();
}

void TextView::setTextStyle(const TextStyle& style) {
  style_ = style;
  invalidate();
}

void TextView::setColor(Color color) {
  style_.color = color;
  invalidate();
}

void TextView::setAlign(TextAlign align) {
  style_.align = align;
  invalidate();
}

void TextView::onPaint(Painter& painter, const Rect& rect) {
  if (text_.empty()) return;

  const int d = dpi();
  const Rect inner =
      rect.inset(style_.paddingLeft * d / 96, style_.paddingTop * d / 96,
                 style_.paddingRight * d / 96, style_.paddingBottom * d / 96);
  const Font& font = style_.smallFont ? fonts.uiSmall : (style_.bold ? fonts.uiBold : fonts.ui);

  if (style_.ellipsis && style_.align == TextAlign::kLeft) {
    painter.drawTextEllipsis(inner, text_, style_.color, font);
    return;
  }
  painter.drawText(inner, text_, style_.color, font, style_.align);
}

Size TextView::onMeasure(const Size& available) {
  (void)available;
  const int d = dpi();
  const Font& font = style_.smallFont ? fonts.uiSmall : (style_.bold ? fonts.uiBold : fonts.ui);

  // 文字宽度由平台后端直接测量；控件还没建窗时也能算准
  int textWidth = platform()->measureText(text_, font);
  if (textWidth <= 0) textWidth = static_cast<int>(text_.size()) * font.pixelHeight();
  const int padH = (style_.paddingLeft + style_.paddingRight) * d / 96;
  const int padV = (style_.paddingTop + style_.paddingBottom) * d / 96;
  return Size{textWidth + padH, font.pixelHeight() + padV};
}

}  // namespace AiUI

