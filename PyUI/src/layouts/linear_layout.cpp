#include "pyui/layouts/linear_layout.h"

#include <algorithm>
#include <vector>

namespace pyui {

void LinearLayout::setOrientation(Orientation orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  requestLayout();
  invalidate();
}

namespace {

// 主轴方向上一个子视图"能长到多大"的可用尺寸。
// 顺序布局里主轴可用空间给的是整个容器，让控件能按内容撑开；
// 交叉轴给的是去掉 margin 之后的净空间，这样 match 才能真正撑满。
Size childAvailable(bool horizontal, const Rect& content, const LayoutParams& p, int dpi) {
  auto px = [dpi](int v) {
    if (v == 0) return 0;
    const long long scaled = static_cast<long long>(v) * dpi;
    return scaled >= 0 ? static_cast<int>((scaled + 48) / 96) : -static_cast<int>((-scaled + 48) / 96);
  };

  const int ml = px(p.marginLeft);
  const int mt = px(p.marginTop);
  const int mr = px(p.marginRight);
  const int mb = px(p.marginBottom);

  if (horizontal) {
    return Size{std::max(0, content.width()), std::max(0, content.height() - mt - mb)};
  }
  return Size{std::max(0, content.width() - ml - mr), std::max(0, content.height())};
}

}  // namespace

Size LinearLayout::onMeasure(const Size& available) {
  // 先把「自己作为 View 的布局参数」落实（宽高可能是固定值）
  Size box = View::onMeasure(available);
  if (box.width <= 0) box.width = available.width;
  if (box.height <= 0) box.height = available.height;

  const bool horizontal = orientation_ == Orientation::kHorizontal;
  const int spacing = px(spacing_);
  const int dpiValue = dpi();
  (void)dpiValue;

  const int padH = px(padding_.horizontal());
  const int padV = px(padding_.vertical());
  const Rect inner = Rect::fromSize(std::max(0, box.width - padH), std::max(0, box.height - padV));

  int mainExtent = 0;
  int crossExtent = 0;
  int visible = 0;

  for (View* child : children_) {
    if (!child->isVisible()) continue;
    ++visible;

    const LayoutParams& p = child->layoutParams();
    const Size avail = childAvailable(horizontal, inner, p, dpi());
    const Size desired = child->measure(avail);

    int childMain = horizontal ? desired.width : desired.height;
    int childCross = horizontal ? desired.height : desired.width;

    if (horizontal) {
      if (p.width > 0) childMain = px(p.width);
      if (p.maxWidth > 0) childMain = std::min(childMain, px(p.maxWidth));
      if (p.height > 0) childCross = px(p.height);
      if (p.maxHeight > 0) childCross = std::min(childCross, px(p.maxHeight));
    } else {
      if (p.height > 0) childMain = px(p.height);
      if (p.maxHeight > 0) childMain = std::min(childMain, px(p.maxHeight));
      if (p.width > 0) childCross = px(p.width);
      if (p.maxWidth > 0) childCross = std::min(childCross, px(p.maxWidth));
    }

    const int marginMain = horizontal ? px(p.marginLeft) + px(p.marginRight)
                                      : px(p.marginTop) + px(p.marginBottom);
    const int marginCross = horizontal ? px(p.marginTop) + px(p.marginBottom)
                                       : px(p.marginLeft) + px(p.marginRight);

    mainExtent += childMain + marginMain;
    crossExtent = std::max(crossExtent, childCross + marginCross);
  }

  if (visible > 1) mainExtent += spacing * (visible - 1);

  if (horizontal) return Size{mainExtent + padH, crossExtent + padV};
  return Size{crossExtent + padH, mainExtent + padV};
}

void LinearLayout::arrangeChildren(const Rect& content) {
  if (content.isEmpty()) return;

  const bool horizontal = orientation_ == Orientation::kHorizontal;
  const int spacing = px(spacing_);
  const int mainExtent = horizontal ? content.width() : content.height();
  const int crossExtent = horizontal ? content.height() : content.width();

  struct Slot {
    View* view = nullptr;
    LayoutParams params{};
    int mainSize = 0;
    int crossSize = 0;
    int marginMainStart = 0;
    int marginMainEnd = 0;
    int marginCrossStart = 0;
    int marginCrossEnd = 0;
  };

  std::vector<Slot> slots;
  slots.reserve(children_.size());

  int totalSpacing = 0;
  int fixedMain = 0;
  float totalWeight = 0.0f;

  // ── 第一遍：定尺寸 ──
  for (View* child : children_) {
    if (!child->isVisible()) continue;

    const LayoutParams& p = child->layoutParams();
    Slot slot;
    slot.view = child;
    slot.params = p;
    slot.marginMainStart = horizontal ? px(p.marginLeft) : px(p.marginTop);
    slot.marginMainEnd = horizontal ? px(p.marginRight) : px(p.marginBottom);
    slot.marginCrossStart = horizontal ? px(p.marginTop) : px(p.marginLeft);
    slot.marginCrossEnd = horizontal ? px(p.marginBottom) : px(p.marginRight);

    const int crossBox = std::max(0, crossExtent - slot.marginCrossStart - slot.marginCrossEnd);

    // 交叉轴：match 撑满，wrap 按内容，固定值原样
    const int crossParam = horizontal ? p.height : p.width;
    if (crossParam == LayoutParams::kMatchParent) {
      slot.crossSize = crossBox;
    } else if (crossParam == LayoutParams::kWrapContent) {
      const Size avail = childAvailable(horizontal, content, p, dpi());
      slot.crossSize = horizontal ? child->measure(avail).height : child->measure(avail).width;
      if (slot.crossSize > crossBox) slot.crossSize = crossBox;
    } else {
      slot.crossSize = px(crossParam);
      if (slot.crossSize > crossBox) slot.crossSize = crossBox;
    }
    const int maxCross = horizontal ? p.maxHeight : p.maxWidth;
    if (maxCross > 0) slot.crossSize = std::min(slot.crossSize, px(maxCross));

    // 主轴
    const int mainParam = horizontal ? p.width : p.height;
    if (p.weight > 0.0f) {
      totalWeight += p.weight;
      slot.mainSize = 0;  // 第二遍按权重分配
    } else if (mainParam == LayoutParams::kMatchParent) {
      // 顺序布局里主轴撑满会挤掉兄弟，按内容处理
      const Size avail = childAvailable(horizontal, content, p, dpi());
      const Size desired = child->measure(avail);
      slot.mainSize = horizontal ? desired.width : desired.height;
    } else if (mainParam == LayoutParams::kWrapContent) {
      const Size avail = childAvailable(horizontal, content, p, dpi());
      const Size desired = child->measure(avail);
      slot.mainSize = horizontal ? desired.width : desired.height;
    } else {
      slot.mainSize = px(mainParam);
    }
    const int maxMain = horizontal ? p.maxWidth : p.maxHeight;
    if (maxMain > 0) slot.mainSize = std::min(slot.mainSize, px(maxMain));
    if (slot.mainSize < 0) slot.mainSize = 0;

    fixedMain += slot.mainSize + slot.marginMainStart + slot.marginMainEnd;
    slots.push_back(slot);
  }

  if (slots.size() > 1) totalSpacing = spacing * (static_cast<int>(slots.size()) - 1);

  // ── 剩余空间与主轴起点 ──
  const int remaining = std::max(0, mainExtent - fixedMain - totalSpacing);

  if (totalWeight > 0.0f) {
    // 有 weight 时剩余空间全给它们，gravity 不再生效
    for (Slot& slot : slots) {
      if (slot.params.weight <= 0.0f) continue;
      const float share = slot.params.weight / totalWeight;
      slot.mainSize = std::max(0, static_cast<int>(static_cast<float>(remaining) * share + 0.5f));
    }
  }

  int cursor = horizontal ? content.left : content.top;
  if (totalWeight <= 0.0f) {
    if (gravity_ == Gravity::kCenter) {
      cursor += remaining / 2;
    } else if (gravity_ == Gravity::kEnd) {
      cursor += remaining;
    }
  }

  // ── 第二遍：摆放 ──
  bool first = true;
  for (const Slot& slot : slots) {
    if (!first) cursor += spacing;
    first = false;

    const int freeCross =
        std::max(0, crossExtent - slot.marginCrossStart - slot.marginCrossEnd - slot.crossSize);
    const Gravity crossGravity = horizontal ? slot.params.vertical : slot.params.horizontal;
    int crossOffset = 0;
    if (crossGravity == Gravity::kCenter) {
      crossOffset = freeCross / 2;
    } else if (crossGravity == Gravity::kEnd) {
      crossOffset = freeCross;
    }

    const int mainStart = cursor + slot.marginMainStart;
    const int crossStart = (horizontal ? content.top : content.left) + slot.marginCrossStart +
                           crossOffset;

    Rect frame{};
    if (horizontal) {
      frame = Rect::at(mainStart, crossStart, slot.mainSize, slot.crossSize);
    } else {
      frame = Rect::at(crossStart, mainStart, slot.crossSize, slot.mainSize);
    }

    // 绝对偏移（x/y）叠加在顺序位置之上，供「微调几个像素」的场景用
    if (slot.params.x != 0 || slot.params.y != 0) {
      frame = frame.moved(px(slot.params.x), px(slot.params.y));
    }

    slot.view->layout(frame);
    cursor += slot.mainSize + slot.marginMainStart + slot.marginMainEnd;
  }
}

}  // namespace pyui
