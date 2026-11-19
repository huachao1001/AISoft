// TagView 实现
#include "AiUI/widgets/tag_view.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

namespace AiUI {

void TagView::setText(const std::wstring& text) {
  if (text_ == text) return;
  text_ = text;
  // wrap 布局的宽度跟着文字走，改了文字必须重排
  requestLayout();
  invalidate();
}

void TagView::setStyle(const TagStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

const Font& TagView::resolvedFont() const {
  return style_.smallFont ? fonts.uiSmall : fonts.ui;
}

void TagView::onPaint(Painter& painter, const Rect& rect) {
  if (text_.empty() || rect.isEmpty()) return;

  // 颜色按主题角色取：换主题后标签自动跟着变，宿主不必重下样式
  const Theme& t = theme();
  const Color bg = style_.background != kNoColor ? style_.background : t.tagBg;
  const Color ink = style_.ink != kNoColor ? style_.ink : t.tagInk;
  const int d = dpi();

  painter.fillRoundRect(rect, bg, style_.radius * d / 96);
  painter.drawText(rect, text_, ink, resolvedFont(), TextAlign::kCenter);
}

Size TagView::onMeasure(const Size& available) {
  (void)available;
  const int d = dpi();
  const Font& font = resolvedFont();
  int textWidth = platform()->measureText(text_, font);
  if (textWidth <= 0) textWidth = static_cast<int>(text_.size()) * font.pixelHeight();
  return Size{textWidth + style_.padX * 2 * d / 96, style_.height * d / 96};
}

}  // namespace AiUI
