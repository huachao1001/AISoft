// LinearLayout 实现：按方向排列 + weight 分配剩余空间
#include "AiUI/layouts/linear_layout.h"

#include <algorithm>

namespace AiUI {

LinearLayout::LinearLayout(Orientation orientation) : orientation_(orientation) {
  className_ = L"AiUILinearLayout";
}

LinearLayout& LinearLayout::add(Widget* child, const LayoutParams& params) {
  addChild(child, params);
  return *this;
}

void LinearLayout::setOrientation(Orientation orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  invalidate();
}

void LinearLayout::setSpacing(int spacing) {
  spacing_ = spacing;
  invalidate();
}

void LinearLayout::setPadding(int all) {
  padding_ = Insets::all(all);
  invalidate();
}

void LinearLayout::setPadding(int horizontal, int vertical) {
  padding_ = Insets::symmetric(horizontal, vertical);
  invalidate();
}

void LinearLayout::setPadding(int left, int top, int right, int bottom) {
  padding_ = Insets{left, top, right, bottom};
  invalidate();
}

void LinearLayout::onPaint(Painter& painter, const Rect& rect) {
  (void)painter;
  (void)rect;
}

Size LinearLayout::onMeasure(const Size& available) {
  const int d = dpi();
  const bool horizontal = (orientation_ == Orientation::kHorizontal);
  const int padH = (padding_.left + padding_.right) * d / 96;
  const int padV = (padding_.top + padding_.bottom) * d / 96;
  const int gap = spacing_ * d / 96;

  int total = 0;
  int cross = 0;
  int visibleCount = 0;

  for (Widget* child : children()) {
    if (!child->isVisible()) continue;
    ++visibleCount;

    const Size inner{available.width - padH, available.height - padV};
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

  const Rect inner = content.deflated(padding_.left * d / 96, padding_.top * d / 96)
                         .deflated(padding_.right * d / 96, padding_.bottom * d / 96);

  // 第一遍：固定尺寸的子控件先占位，统计剩余空间与总权重
  struct Slot {
    Widget* child = nullptr;
    Size desired{};   // 物理像素
    int ml = 0, mt = 0, mr = 0, mb = 0;
  };
  std::vector<Slot> slots;
  slots.reserve(children().size());

  int used = 0;
  float totalWeight = 0.0f;
  for (Widget* child : children()) {
    if (!child->isVisible()) continue;

    const LayoutParams& lp = child->layoutParams();
    Slot slot;
    slot.child = child;
    slot.ml = lp.marginLeft * d / 96;
    slot.mt = lp.marginTop * d / 96;
    slot.mr = lp.marginRight * d / 96;
    slot.mb = lp.marginBottom * d / 96;

    const Size available{inner.width() - slot.ml - slot.mr, inner.height() - slot.mt - slot.mb};
    slot.desired = child->measure(available);
    // 带权重的子控件不占固定空间，剩余空间全部分给它们
    if (lp.weight <= 0.0f) {
      used += horizontal ? (slot.desired.width + slot.ml + slot.mr)
                         : (slot.desired.height + slot.mt + slot.mb);
    }
    totalWeight += lp.weight;
    slots.push_back(slot);
  }
  if (slots.size() > 1) used += gap * (static_cast<int>(slots.size()) - 1);

  // 剩余空间按权重分配；权重为 0 的保持自身尺寸
  const int available = horizontal ? inner.width() : inner.height();
  const int remaining = available - used;

  int cursor = horizontal ? inner.left : inner.top;
  for (size_t i = 0; i < slots.size(); ++i) {
    Slot& slot = slots[i];
    const LayoutParams& lp = slot.child->layoutParams();

    int mainSize = horizontal ? slot.desired.width : slot.desired.height;
    if (lp.weight > 0.0f && totalWeight > 0.0f) {
      mainSize = (std::max)(0, static_cast<int>(remaining * (lp.weight / totalWeight)));
    }

    int crossSize = horizontal ? slot.desired.height : slot.desired.width;
    const int crossAvailable =
        horizontal ? (inner.height() - slot.mt - slot.mb) : (inner.width() - slot.ml - slot.mr);
    if ((horizontal ? lp.height : lp.width) == LayoutParams::kMatchParent) {
      crossSize = crossAvailable;
    }

    // 交叉轴对齐
    int crossPos = horizontal ? (inner.top + slot.mt) : (inner.left + slot.ml);
    if (gravity_ == Gravity::kCenter) {
      crossPos = horizontal ? (inner.top + (inner.height() - crossSize) / 2)
                            : (inner.left + (inner.width() - crossSize) / 2);
    } else if (gravity_ == Gravity::kEnd) {
      crossPos = horizontal ? (inner.bottom - slot.mb - crossSize)
                            : (inner.right - slot.mr - crossSize);
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
