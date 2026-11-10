// ButtonBar 实现
#include "AiUI/widgets/button_bar.h"

#include <algorithm>

namespace AiUI {

Button* ButtonBar::addButton(const ButtonSpec& spec, int group) {
  auto* button = new Button();
  button->setSpec(spec);
  button->setFocusable(false);

  items_.push_back(ButtonBarItem{button, group});
  // 宽度由内容自适应，高度按设计值固定
  addView(button, LayoutParams{}.withSize(LayoutParams::kWrapContent, spec.height));
  relayout();
  return button;
}

Button* ButtonBar::buttonAt(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return nullptr;
  return items_[static_cast<size_t>(index)].button;
}

void ButtonBar::clear() {
  // removeView 会连带 delete，不要重复销毁
  for (auto& item : items_) removeView(item.button);
  items_.clear();
  groupLines_.clear();
  invalidate();
}

void ButtonBar::setEdgePad(int pad) {
  edgePad_ = pad;
  relayout();
}

void ButtonBar::setGap(int gap) {
  gap_ = gap;
  relayout();
}

Size ButtonBar::onMeasure(const Size& available) {
  (void)available;
  return Size{0, kSizeToolH};
}

void ButtonBar::relayout() {
  arrangeChildren(contentRect());
  invalidate();
}

void ButtonBar::arrangeChildren(const Rect& content) {
  const int d = dpi();
  groupLines_.clear();

  const int barHeight = height_ * d / 96;
  const int top = content.top + (content.height() - barHeight) / 2;
  const int gap = gap_ * d / 96;
  const int groupGap = groupGap_ * d / 96;

  int x = content.left + edgePad_ * d / 96;
  int previousGroup = 0;

  for (size_t i = 0; i < items_.size(); ++i) {
    ButtonBarItem& item = items_[i];
    if (i > 0) {
      if (item.group != previousGroup) {
        // 组间留出更宽间距，分隔线插在正中
        x += groupGap;
        const int lineX = x - groupGap / 2;
        groupLines_.push_back(Rect{lineX, top, lineX + 1, top + barHeight});
      } else {
        x += gap;
      }
    }
    previousGroup = item.group;

    // 每个按钮按内容自适应宽度，不占满整段
    const int width = item.button->preferredWidth();
    item.button->layout(Rect{x, top, x + width, top + barHeight});
    x += width;
  }
}

void ButtonBar::onPaint(Painter& painter, const Rect& rect) {
  for (const Rect& line : groupLines_) {
    painter.drawVLine(line.top, line.bottom, line.left, kColorLine);
  }
  painter.drawHLine(rect.left, rect.right, rect.bottom - 1, kColorLine);
}

}  // namespace AiUI
