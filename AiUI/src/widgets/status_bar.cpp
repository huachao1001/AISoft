// StatusBar 实现
#include "AiUI/widgets/status_bar.h"

#include "AiUI/font.h"

#include <algorithm>

namespace AiUI {

void StatusBar::setFields(const StatusFields& fields) {
  fields_ = fields;
  invalidate();
}

void StatusBar::setStyle(const StatusStyle& style) {
  style_ = style;
  invalidate();
}

Size StatusBar::onMeasure(const Size& available) {
  (void)available;
  return Size{0, kSizeStatusH};
}

void StatusBar::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int padX = style_.padX * d / 96;
  const int centerY = rect.center().y;
  int x = padX;

  // 右侧统计先量宽，左侧被挤时用它做边界
  const int rightWidth = painter.measureText(fields_.right, fonts.uiSmall);
  const int rightLeft = rect.right - padX - rightWidth;
  const int limit = rightLeft - 16 * d / 96;

  if (fields_.showHome) {
    const int size = style_.homeIconSize * d / 96;
    const Rect iconRect{x, centerY - size / 2, x + size, centerY + size / 2};
    painter.drawIcon(IconId::kHome, iconRect, kColorTagInk);
    x = iconRect.right + 8 * d / 96;
  }

  // 路径：按可用宽度从左侧裁掉，保证后面的标签不错位
  std::wstring shown = fields_.left;
  while (!shown.empty() && painter.measureText(shown, fonts.uiSmall) > (limit - x)) {
    shown.erase(shown.begin());
  }
  const Rect textRect{x, rect.top, limit, rect.bottom};
  painter.drawTextEllipsis(textRect, shown, style_.text, fonts.uiSmall);
  x += painter.measureText(shown, fonts.uiSmall);
  x = (std::min)(x, limit);

  if (!fields_.tag.empty()) {
    const int tagPadX = style_.tagPadX * d / 96;
    const int tagWidth = painter.measureText(fields_.tag, fonts.uiSmall) + tagPadX * 2;
    const int tagHeight = style_.tagHeight * d / 96;
    const Rect tagRect{x + 8 * d / 96, centerY - tagHeight / 2,
                       x + 8 * d / 96 + tagWidth, centerY + tagHeight / 2};
    if (tagRect.right < limit) {
      painter.fillRoundRect(tagRect, kColorTagBg, style_.tagRadius * d / 96);
      painter.drawText(tagRect, fields_.tag, kColorTagInk, fonts.uiSmall, TextAlign::kCenter);
      x = tagRect.right + 8 * d / 96;
    }
  }

  if (!fields_.size.empty() && x < limit) {
    const Rect sizeRect{x, rect.top, limit, rect.bottom};
    painter.drawText(sizeRect, fields_.size, style_.text, fonts.uiSmall, TextAlign::kLeft);
  }
  if (rightLeft > 0) {
    const Rect rightRect{rightLeft, rect.top, rect.right - padX, rect.bottom};
    painter.drawTextEllipsis(rightRect, fields_.right, style_.text, fonts.uiSmall);
  }

  painter.drawHLine(rect.left, rect.right, rect.top, style_.divider);
}

}  // namespace AiUI
