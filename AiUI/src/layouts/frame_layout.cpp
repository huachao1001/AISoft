// FrameLayout 实现：子视图按绝对坐标叠放
#include "AiUI/layouts/frame_layout.h"

#include <algorithm>

namespace AiUI {

FrameLayout& FrameLayout::addChildView(View* child, const LayoutParams& params) {
  addView(child, params);
  return *this;
}

Size FrameLayout::onMeasure(const Size& available) {
  const int d = dpi();
  const Insets& padding = this->padding();
  const int padH = (padding.left + padding.right) * d / 96;
  const int padV = (padding.top + padding.bottom) * d / 96;

  // 取所有子视图里最大的期望尺寸
  int maxW = 0;
  int maxH = 0;
  for (View* child : children()) {
    if (!child->isVisible()) continue;
    const Size inner{(std::max)(0, available.width - padH), (std::max)(0, available.height - padV)};
    const Size size = childMeasureSize(child, inner);
    const LayoutParams& lp = child->layoutParams();
    const int offsetX = (lp.x + lp.marginLeft + lp.marginRight) * d / 96;
    const int offsetY = (lp.y + lp.marginTop + lp.marginBottom) * d / 96;
    maxW = (std::max)(maxW, size.width + offsetX);
    maxH = (std::max)(maxH, size.height + offsetY);
  }
  return Size{maxW + padH, maxH + padV};
}

void FrameLayout::arrangeChildren(const Rect& content) {
  const int d = dpi();

  for (View* child : children()) {
    if (!child->isVisible()) continue;

    const LayoutParams& lp = child->layoutParams();
    const int ml = lp.marginLeft * d / 96;
    const int mt = lp.marginTop * d / 96;
    const int mr = lp.marginRight * d / 96;
    const int mb = lp.marginBottom * d / 96;

    // 撑满时先把整块可用区域交给子视图，再定位置
    const Size available{(std::max)(0, content.width() - ml - mr),
                         (std::max)(0, content.height() - mt - mb)};
    const Size desired = child->measure(available);

    int x = content.left + ml;
    int y = content.top + mt;

    // 显式给了坐标就用坐标，否则按对齐方式摆放
    if (lp.x != 0) x += lp.x * d / 96;
    if (lp.y != 0) y += lp.y * d / 96;

    if (lp.x == 0) {
      switch (lp.horizontal) {
        case Gravity::kCenter: x = content.left + (content.width() - desired.width) / 2; break;
        case Gravity::kEnd: x = content.right - mr - desired.width; break;
        default: break;
      }
    }
    if (lp.y == 0) {
      switch (lp.vertical) {
        case Gravity::kCenter: y = content.top + (content.height() - desired.height) / 2; break;
        case Gravity::kEnd: y = content.bottom - mb - desired.height; break;
        default: break;
      }
    }

    child->layout(Rect{x, y, x + desired.width, y + desired.height});
  }
}

}  // namespace AiUI
