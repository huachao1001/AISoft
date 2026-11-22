#include "pyui/widgets/text_view.h"

#include <algorithm>

namespace pyui {

void TextView::setText(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  // 文字变了期望宽度就变：撑满的父容器要重新分配，所以不能只 invalidate
  requestLayout();
  invalidate();
}

void TextView::setTextStyle(const TextStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

void TextView::setColor(Color color) {
  if (style_.color == color) return;
  style_.color = color;
  // 显式色优先：清掉角色绑定，否则换主题时又会被角色覆盖回去
  style_.colorRole = nullptr;
  invalidate();
}

void TextView::setAlign(TextAlign align) {
  if (style_.align == align) return;
  style_.align = align;
  invalidate();
}

void TextView::setBold(bool bold) {
  if (style_.bold == bold) return;
  style_.bold = bold;
  requestLayout();
  invalidate();
}

void TextView::setEllipsis(bool ellipsis) {
  if (style_.ellipsis == ellipsis) return;
  style_.ellipsis = ellipsis;
  invalidate();
}

void TextView::setMiddleEllipsis(bool middleEllipsis) {
  if (style_.middleEllipsis == middleEllipsis) return;
  style_.middleEllipsis = middleEllipsis;
  invalidate();
}

void TextView::setTextSize(TextSize size) {
  if (style_.size == size) return;
  style_.size = size;
  requestLayout();
  invalidate();
}

Color TextView::resolveInk() const {
  if (style_.color != kNoColor) return style_.color;
  if (style_.colorRole != nullptr) return theme().*style_.colorRole;
  if (!isEffectivelyEnabled()) return theme().textDisabled;
  return theme().text;
}

Font TextView::resolveFont() const {
  return uiFont(style_.pointSize(), style_.bold, false, dpi());
}

Size TextView::onMeasure(const Size& available) {
  const Font font = resolveFont();
  const Insets& pad = style_.padding;

  // 行高由文本后端给出，不用 pointSize 自己估：估的值与真实字形差一两个像素，
  // 一列文字排下来就会参差不齐。
  const int lineHeight = textLineHeight(font);
  const int padH = px(pad.horizontal());
  const int padV = px(pad.vertical());

  // 空文本仍占一行高：否则空白标签会把整行高度塌掉，布局跟着跳
  int width = text_.empty() ? 0 : measureTextWidth(text_, font);
  width += padH;
  int height = lineHeight + padV;

  // wrapContent 时不要超过父给的可用宽度，否则会把兄弟视图挤出去
  if (available.width > 0) width = std::min(width, available.width);
  if (available.height > 0) height = std::min(height, available.height);

  return Size{width, height};
}

void TextView::onPaint(Painter& painter, const Rect& rect) {
  if (text_.empty()) return;

  const Color ink = resolveInk();
  const Font font = resolveFont();
  const Insets& pad = style_.padding;
  const Rect area = rect.inset(px(pad.left), px(pad.top), px(pad.right), px(pad.bottom));
  if (area.isEmpty()) return;

  if (style_.ellipsis) {
    if (style_.middleEllipsis) {
      painter.drawTextMiddleEllipsis(area, text_, ink, font, style_.align);
    } else {
      painter.drawTextEllipsis(area, text_, ink, font, style_.align);
    }
    return;
  }

  // 关闭省略时必须自己裁剪：否则长文本会溢出到兄弟视图上
  painter.save();
  painter.clipRect(area);
  painter.drawText(area, text_, ink, font, style_.align);
  painter.restore();
}

}  // namespace pyui
