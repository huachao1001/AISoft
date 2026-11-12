// LinearLayout 实现：按方向排列 + weight 分配剩余空间
#include "AiUI/layouts/linear_layout.h"

#include <algorithm>
#include <vector>

namespace AiUI {

LinearLayout::LinearLayout(Orientation orientation) : orientation_(orientation) {}

LinearLayout& LinearLayout::addChildView(View* child, const LayoutParams& params) {
  addView(child, params);
  return *this;
}

void LinearLayout::setOrientation(Orientation orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  invalidate();
  requestLayout();
}

void LinearLayout::setSpacing(int spacing) {
  if (spacing_ == spacing) return;
  spacing_ = spacing;
  invalidate();
  requestLayout();
}

Size LinearLayout::onMeasure(const Size& available) {
  const int d = dpi();
  const bool horizontal = (orientation_ == Orientation::kHorizontal);
  const Insets& padding = this->padding();
  const int padH = (padding.left + padding.right) * d / 96;
  const int padV = (padding.top + padding.bottom) * d / 96;
  const int gap = spacing_ * d / 96;

  int total = 0;
  int cross = 0;
  int visibleCount = 0;

  for (View* child : children()) {
    if (!child->isVisible()) continue;
    ++visibleCount;

    const Size inner{(std::max)(0, available.width - padH), (std::max)(0, available.height - padV)};
    const Size size = childMeasureSize(child, inner);
    const LayoutParams& lp = child->layoutParams();

    if (horizontal) {
      total += size.width + (lp.marginLeft + lp.marginRight) * d / 96;
      cross = (std::max)(cross, size.height + (lp.marginTop + lp.marginBottom) * d / 96);
    } else {
      total += size.height + (lp.marginTop + lp.marginBottom) * d / 96;
      cross = (std::max)(cross, size.width + (lp.marginLeft + lp.marginRight) * d / 96);
    }
  }
  if (visibleCount > 1) total += gap * (visibleCount - 1);

  return horizontal ? Size{total + padH, cross + padV} : Size{cross + padH, total + padV};
}

void LinearLayout::arrangeChildren(const Rect& content) {
  const int d = dpi();
  const bool horizontal = (orientation_ == Orientation::kHorizontal);
  const int gap = spacing_ * d / 96;

  // 第一遍：固定尺寸的子视图先占位，统计剩余空间与总权重
  struct Slot {
    View* child = nullptr;
    Size desired{};  // 物理像素
    int ml = 0;
    int mt = 0;
    int mr = 0;
    int mb = 0;
  };
  std::vector<Slot> slots;
  slots.reserve(children().size());

  int used = 0;
  float totalWeight = 0.0f;
  for (View* child : children()) {
    if (!child->isVisible()) continue;

    const LayoutParams& lp = child->layoutParams();
    Slot slot;
    slot.child = child;
    slot.ml = lp.marginLeft * d / 96;
    slot.mt = lp.marginTop * d / 96;
    slot.mr = lp.marginRight * d / 96;
    slot.mb = lp.marginBottom * d / 96;

    const Size available{(std::max)(0, content.width() - slot.ml - slot.mr),
                         (std::max)(0, content.height() - slot.mt - slot.mb)};
    slot.desired = child->measure(available);
    // 带权重的子视图不占固定空间，剩余空间全部分给它们
    if (lp.weight <= 0.0f) {
      used += horizontal ? (slot.desired.width + slot.ml + slot.mr)
                         : (slot.desired.height + slot.mt + slot.mb);
    }
    totalWeight += lp.weight;
    slots.push_back(slot);
  }
  if (slots.size() > 1) used += gap * (static_cast<int>(slots.size()) - 1);

  // 剩余空间按权重分配；权重为 0 的保持自身尺寸
  const int available = horizontal ? content.width() : content.height();
  const int remaining = (std::max)(0, available - used);

  int cursor = horizontal ? content.left : content.top;
  for (size_t i = 0; i < slots.size(); ++i) {
    Slot& slot = slots[i];
    const LayoutParams& lp = slot.child->layoutParams();

    int mainSize = horizontal ? slot.desired.width : slot.desired.height;
    if (lp.weight > 0.0f && totalWeight > 0.0f) {
      mainSize = (std::max)(0, static_cast<int>(remaining * (lp.weight / totalWeight)));
    }

    int crossSize = horizontal ? slot.desired.height : slot.desired.width;
    const int crossAvailable =
        horizontal ? (content.height() - slot.mt - slot.mb) : (content.width() - slot.ml - slot.mr);
    if ((horizontal ? lp.height : lp.width) == LayoutParams::kMatchParent) {
      crossSize = crossAvailable;
    }

    // 交叉轴对齐：子视图自己的 align / alignY 优先，没写才用容器 gravity
    const Gravity crossGravity = horizontal ? lp.vertical : lp.horizontal;
    const Gravity effective = crossGravity != Gravity::kStart ? crossGravity : gravity_;
    int crossPos = horizontal ? (content.top + slot.mt) : (content.left + slot.ml);
    if (effective == Gravity::kCenter) {
      crossPos = horizontal ? (content.top + (content.height() - crossSize) / 2)
                            : (content.left + (content.width() - crossSize) / 2);
    } else if (effective == Gravity::kEnd) {
      crossPos = horizontal ? (content.bottom - slot.mb - crossSize)
                            : (content.right - slot.mr - crossSize);
    }

    Rect frame;
    if (horizontal) {
      const int x = cursor + slot.ml;
      frame = Rect{x, crossPos, x + mainSize, crossPos + crossSize};
    } else {
      const int y = cursor + slot.mt;
      frame = Rect{crossPos, y, crossPos + crossSize, y + mainSize};
    }
    slot.child->layout(frame);

    cursor += mainSize + (horizontal ? (slot.ml + slot.mr) : (slot.mt + slot.mb));
    if (i + 1 < slots.size()) cursor += gap;
  }
}

}  // namespace AiUI
