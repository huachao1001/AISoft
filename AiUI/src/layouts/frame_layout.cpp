// FrameLayout 实现：子控件按绝对坐标叠放
#include "AiUI/layouts/frame_layout.h"

#include <algorithm>

namespace AiUI {

FrameLayout& FrameLayout::add(Widget* child, const LayoutParams& params) {
  addChild(child, params);
  return *this;
}

void FrameLayout::setPadding(int all) {
  padding_ = Insets::all(all);
  invalidate();
}

void FrameLayout::setPadding(int horizontal, int vertical) {
  padding_ = Insets::symmetric(horizontal, vertical);
  invalidate();
}

void FrameLayout::setPadding(int left, int top, int right, int bottom) {
  padding_ = Insets{left, top, right, bottom};
  invalidate();
}

void FrameLayout::onPaint(Painter& painter, const Rect& rect) {
  (void)painter;
  (void)rect;
  // 容器本身只铺底色，内容由子控件自己画
}

Size FrameLayout::onMeasure(const Size& available) {
  const int d = dpi();
  const int padH = (padding_.left + padding_.right) * d / 96;
  const int padV = (padding_.top + padding_.bottom) * d / 96;

  // 取所有子控件里最大的期望尺寸
  int maxW = 0;
  int maxH = 0;
  for (Widget* child : children()) {
    if (!child->isVisible()) continue;
    const Size inner{available.width - padH, available.height - padV};
    const Size size = childMeasureSize(child, inner);
    const int offsetX = (child->layoutParams().x + child->layoutParams().marginLeft +
                         child->layoutParams().marginRight) * d / 96;
    const int offsetY = (child->layoutParams().y + child->layoutParams().marginTop +
                         child->layoutParams().marginBottom) * d / 96;
    maxW = (std::max)(maxW, size.width + offsetX);
    maxH = (std::max)(maxH, size.height + offsetY);
  }
  return Size{maxW + padH, maxH + padV};
}

void FrameLayout::arrangeChildren(const Rect& content) {
  const int d = dpi();
  const Rect inner = content.deflated(padding_.left * d / 96, padding_.top * d / 96)
                         .deflated(padding_.right * d / 96, padding_.bottom * d / 96);

  for (Widget* child : children()) {
    if (!child->isVisible()) continue;

    const LayoutParams& lp = child->layoutParams();
    const int ml = lp.marginLeft * d / 96;
    const int mt = lp.marginTop * d / 96;
    const int mr = lp.marginRight * d / 96;
    const int mb = lp.marginBottom * d / 96;

    // 关键修正：撑满时交给子控件整块可用区域，先算尺寸再定位
    const Size available{inner.width() - ml - mr, inner.height() - mt - mb};
    const Size desired = child->measure(available);

    int w = desired.width;
    int h = desired.height;

    int x = inner.left + ml;
    int y = inner.top + mt;

    // 显式给了坐标就用坐标，否则按对齐方式摆放
    if (lp.x != 0) x += lp.x * d / 96;
    if (lp.y != 0) y += lp.y * d / 96;

    if (lp.x == 0) {
      switch (lp.horizontal) {
        case Gravity::kCenter: x = inner.left + (inner.width() - w) / 2; break;
        case Gravity::kEnd: x = inner.right - mr - w; break;
        default: break;
      }
    }
    if (lp.y == 0) {
      switch (lp.vertical) {
        case Gravity::kCenter: y = inner.top + (inner.height() - h) / 2; break;
        case Gravity::kEnd: y = inner.bottom - mb - h; break;
        default: break;
      }
    }

    if (w < 0) w = inner.width();
    if (h < 0) h = inner.height();

    child->layout(Rect{x, y, x + w, y + h});
  }
}

}  // namespace AiUI
