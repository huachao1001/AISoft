#include "pyui/layouts/frame_layout.h"

#include <algorithm>

namespace pyui {

Size FrameLayout::onMeasure(const Size& available) {
  Size box = View::onMeasure(available);
  if (box.width <= 0) box.width = available.width;
  if (box.height <= 0) box.height = available.height;

  const int padH = px(padding_.horizontal());
  const int padV = px(padding_.vertical());
  const Size inner{std::max(0, box.width - padH), std::max(0, box.height - padV)};

  // 层叠布局的自然尺寸 = 所有子视图的并集（含 margin 与偏移）。
  // 不取最大值：一个偏右下的小控件也能把内容区撑开，否则它会被裁掉。
  int maxRight = 0;
  int maxBottom = 0;
  bool any = false;

  for (View* child : children_) {
    if (!child->isVisible()) continue;
    any = true;

    const LayoutParams& p = child->layoutParams();
    const Size avail{std::max(0, inner.width - px(p.marginLeft) - px(p.marginRight)),
                     std::max(0, inner.height - px(p.marginTop) - px(p.marginBottom))};
    Size desired{};
    if (p.width == LayoutParams::kWrapContent || p.height == LayoutParams::kWrapContent) {
      desired = child->measure(avail);
    }

    const int width = p.width == LayoutParams::kMatchParent
                          ? inner.width
                          : (p.width == LayoutParams::kWrapContent ? desired.width : px(p.width));
    const int height = p.height == LayoutParams::kMatchParent
                           ? inner.height
                           : (p.height == LayoutParams::kWrapContent ? desired.height : px(p.height));

    maxRight = std::max(maxRight, px(p.x) + px(p.marginLeft) + width + px(p.marginRight));
    maxBottom = std::max(maxBottom, px(p.y) + px(p.marginTop) + height + px(p.marginBottom));
  }

  if (!any) return Size{padH, padV};
  return Size{maxRight + padH, maxBottom + padV};
}

void FrameLayout::arrangeChildren(const Rect& content) {
  if (content.isEmpty()) return;

  for (View* child : children_) {
    if (!child->isVisible()) continue;

    const LayoutParams& p = child->layoutParams();
    const int ml = px(p.marginLeft);
    const int mt = px(p.marginTop);
    const int mr = px(p.marginRight);
    const int mb = px(p.marginBottom);

    const Size box{std::max(0, content.width() - ml - mr),
                   std::max(0, content.height() - mt - mb)};

    Size desired{};
    if (p.width == LayoutParams::kWrapContent || p.height == LayoutParams::kWrapContent) {
      desired = child->measure(box);
    }

    int width = p.width == LayoutParams::kMatchParent
                    ? box.width
                    : (p.width == LayoutParams::kWrapContent ? desired.width : px(p.width));
    int height = p.height == LayoutParams::kMatchParent
                     ? box.height
                     : (p.height == LayoutParams::kWrapContent ? desired.height : px(p.height));

    // 上限夹紧：标题这类「按内容自然排但不许挤走兄弟」的视图靠它
    if (p.maxWidth > 0) width = std::min(width, px(p.maxWidth));
    if (p.maxHeight > 0) height = std::min(height, px(p.maxHeight));
    width = std::min(width, box.width);
    height = std::min(height, box.height);

    int x = content.left + ml + px(p.x);
    int y = content.top + mt + px(p.y);

    const int freeX = box.width - width;
    if (p.horizontal == Gravity::kCenter) {
      x += freeX / 2;
    } else if (p.horizontal == Gravity::kEnd) {
      x += freeX;
    }

    const int freeY = box.height - height;
    if (p.vertical == Gravity::kCenter) {
      y += freeY / 2;
    } else if (p.vertical == Gravity::kEnd) {
      y += freeY;
    }

    child->layout(Rect::at(x, y, width, height));
  }
}

}  // namespace pyui
