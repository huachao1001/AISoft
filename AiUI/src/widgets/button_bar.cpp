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

void ButtonBar::setCentered(bool centered) {
  centered_ = centered;
  relayout();
}

void ButtonBar::setGroupDividers(bool show) {
  showGroupDividers_ = show;
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
  const int edgePad = edgePad_ * d / 96;

  // 总宽按「每个按钮的宽度 + 各处间距」算。紧凑与否会改变按钮宽度，
  // 所以 compact 作为参数传进来试算。
  auto totalWidth = [&](bool compact, int edgePadNow, int gapNow, int groupGapNow) {
    int total = edgePadNow;
    int previousGroup = items_.empty() ? 0 : items_.front().group;
    for (size_t i = 0; i < items_.size(); ++i) {
      const ButtonBarItem& item = items_[i];
      if (i > 0) {
        total += (item.group != previousGroup) ? groupGapNow : gapNow;
        previousGroup = item.group;
      }
      total += compact ? item.button->compactWidth() : item.button->preferredWidth();
    }
    return total;
  };

  // 放不下就转紧凑：用最低限度的判断而不是按比例压缩文字，是因为被截断的
  // 「适应窗口」和「适应…」一样看不懂，图标反而能认出来。
  const bool compact = totalWidth(false, edgePad, gap, groupGap) > content.width();
  for (ButtonBarItem& item : items_) item.button->setCompact(compact);

  // 紧凑也放不下时收紧间距，而不是让按钮溢出到窗口外——溢出的按钮点不到。
  // 按实际的 gap 组合逐档试，而不是估算扣多少：组间与组内的间距数目不同，
  // 估算很容易少扣一处，结果仍然差几个像素。
  int edgePadNow = edgePad;
  int gapNow = gap;
  int groupGapNow = groupGap;
  if (totalWidth(compact, edgePadNow, gapNow, groupGapNow) > content.width()) {
    // 先关掉内边距，再把两档间距一起往回收；收到 0 为止
    edgePadNow = 0;
    while (totalWidth(compact, edgePadNow, gapNow, groupGapNow) > content.width() &&
           (gapNow > 0 || groupGapNow > 0)) {
      gapNow = (std::max)(0, gapNow - d / 96);
      groupGapNow = (std::max)(0, groupGapNow - d / 96);
    }
  }

  int x = content.left + edgePadNow;
  // 居中时按「按钮组净宽」对半分：totalWidth 含左侧留白，直接用它算会整体右偏
  if (centered_) {
    const int groupWidth = totalWidth(compact, 0, gapNow, groupGapNow);
    const int slack = content.width() - groupWidth;
    if (slack > 0) x = content.left + slack / 2;
  }
  int previousGroup = items_.empty() ? 0 : items_.front().group;

  for (size_t i = 0; i < items_.size(); ++i) {
    ButtonBarItem& item = items_[i];
    if (i > 0) {
      if (item.group != previousGroup) {
        // 组间留出更宽间距，分隔线插在正中
        x += groupGapNow;
        if (showGroupDividers_) {
          const int lineX = x - groupGapNow / 2;
          groupLines_.push_back(Rect{lineX, top, lineX + 1, top + barHeight});
        }
      } else {
        x += gapNow;
      }
    }
    previousGroup = item.group;

    // 每个按钮按内容自适应宽度，不占满整段
    const int width = compact ? item.button->compactWidth() : item.button->preferredWidth();
    item.button->layout(Rect{x, top, x + width, top + barHeight});
    x += width;
  }
}

void ButtonBar::onPaint(Painter& painter, const Rect& rect) {
  const Color lineColor = theme().line;
  for (const Rect& line : groupLines_) {
    painter.drawVLine(line.top, line.bottom, line.left, lineColor);
  }
  painter.drawHLine(rect.left, rect.right, rect.bottom - 1, lineColor);
}

}  // namespace AiUI
