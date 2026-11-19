// StatusBar 实现
#include "AiUI/widgets/status_bar.h"

#include "AiUI/font.h"

#include <algorithm>
#include <vector>

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
  const Theme& t = theme();
  const int padX = style_.padX * d / 96;
  const int centerY = rect.center().y;
  int x = rect.left + padX;

  const Color textColor = style_.text != kNoColor ? style_.text : t.textMuted;
  const Color divider = style_.divider != kNoColor ? style_.divider : t.line;
  const Color tagBg = style_.tagBg != kNoColor ? style_.tagBg : t.tagBg;
  const Color tagInk = style_.tagInk != kNoColor ? style_.tagInk : t.tagInk;

  // 右侧统计先量宽，左侧被挤时用它做边界
  const int rightWidth = painter.measureText(fields_.right, fonts.uiSmall);
  const int rightLeft = rect.right - padX - rightWidth;
  const int limit = rightLeft - 16 * d / 96;

  if (fields_.showHome) {
    const int size = style_.homeIconSize * d / 96;
    const Rect iconRect{x, centerY - size / 2, x + size, centerY + size / 2};
    painter.drawIcon(IconId::kHome, iconRect, tagInk);
    x = iconRect.right + 8 * d / 96;
  }

  const int tagPadX = style_.tagPadX * d / 96;
  const int tagGap = 8 * d / 96;

  // 规格胶囊（格式、分辨率、大小、序号）是这张图的硬信息，比路径更该保住：
  // 先量出它们一共要多宽，路径只拿剩下的部分。路径本身可从左侧裁掉，
  // 胶囊不行——裁一半的「1600×…」毫无用处。
  std::vector<std::wstring> pills;
  if (!fields_.tag.empty()) pills.push_back(fields_.tag);
  for (const std::wstring& badge : fields_.badges) {
    if (!badge.empty()) pills.push_back(badge);
  }
  int pillsWidth = 0;
  for (const std::wstring& text : pills) {
    pillsWidth += tagGap + painter.measureText(text, fonts.uiSmall) + tagPadX * 2;
  }

  // 路径的右边界：给胶囊留够位置，实在放不下就一条路径都不画
  const int pathLimit = (std::max)(x, limit - pillsWidth);

  // 路径：按可用宽度从左侧裁掉，保证后面的标签不错位
  if (pathLimit > x) {
    std::wstring shown = fields_.left;
    while (!shown.empty() && painter.measureText(shown, fonts.uiSmall) > (pathLimit - x)) {
      shown.erase(shown.begin());
    }
    const Rect textRect{x, rect.top, pathLimit, rect.bottom};
    painter.drawTextEllipsis(textRect, shown, textColor, fonts.uiSmall);
    x += painter.measureText(shown, fonts.uiSmall);
  }

  // 一个胶囊：放不下就停在这里，后面的也不再画——只画一半的胶囊比不画更糟
  auto drawPill = [&](const std::wstring& text) {
    if (text.empty()) return;
    const int width = painter.measureText(text, fonts.uiSmall) + tagPadX * 2;
    const int height = style_.tagHeight * d / 96;
    const Rect pill{x + tagGap, centerY - height / 2, x + tagGap + width, centerY + height / 2};
    if (pill.right > limit) return;
    painter.fillRoundRect(pill, tagBg, style_.tagRadius * d / 96);
    painter.drawText(pill, text, tagInk, fonts.uiSmall, TextAlign::kCenter);
    x = pill.right;
  };

  for (const std::wstring& text : pills) drawPill(text);

  if (!fields_.size.empty() && x < limit) {
    const Rect sizeRect{x + tagGap, rect.top, limit, rect.bottom};
    painter.drawText(sizeRect, fields_.size, textColor, fonts.uiSmall, TextAlign::kLeft);
  }
  if (rightLeft > 0) {
    const Rect rightRect{rightLeft, rect.top, rect.right - padX, rect.bottom};
    painter.drawTextEllipsis(rightRect, fields_.right, textColor, fonts.uiSmall);
  }

  painter.drawHLine(rect.left, rect.right, rect.top, divider);
}

Color StatusBar::backgroundColor() const {
  return style_.background != kNoColor ? style_.background : theme().tool;
}

}  // namespace AiUI
