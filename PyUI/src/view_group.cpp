#include "pyui/view_group.h"

#include <algorithm>

namespace pyui {

ViewGroup::~ViewGroup() { removeAllViews(); }

// ── 子树 ──

View* ViewGroup::addView(View* child, const LayoutParams& params) {
  if (child == nullptr || child == this) return nullptr;
  // 已经属于别的容器时先摘下来：一个视图只能有一个父，
  // 否则 absoluteBounds 会沿错的父链算，脏区就全乱了。
  if (child->parent_ != nullptr) child->parent_->removeView(child);

  child->parent_ = this;
  child->params_ = params;
  children_.push_back(child);

  if (isAttached()) child->attachTree(root_);

  onChildAdded(child);
  requestLayout();
  invalidate();
  return child;
}

void ViewGroup::removeView(View* child) {
  const auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return;

  children_.erase(it);
  onChildRemoved(child);

  if (child->isAttached()) child->detachTree();
  child->parent_ = nullptr;

  requestLayout();
  invalidate();
  // 容器拥有子视图：从树上摘掉即释放。调用方若要保留引用，
  // 必须在 removeView 之前自己拿走（这里不提供 detach 语义，
  // 因为「摘下来但还活着」的中间态几乎所有调用方都会忘记处理）。
  delete child;
}

void ViewGroup::removeAllViews() {
  const auto copy = children_;
  children_.clear();
  for (View* child : copy) {
    onChildRemoved(child);
    if (child->isAttached()) child->detachTree();
    child->parent_ = nullptr;
    delete child;
  }
  requestLayout();
  invalidate();
}

View* ViewGroup::childAt(int index) const {
  if (index < 0 || index >= static_cast<int>(children_.size())) return nullptr;
  return children_[static_cast<size_t>(index)];
}

int ViewGroup::indexOfChild(View* child) const {
  const auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return -1;
  return static_cast<int>(it - children_.begin());
}

// ── 内边距 ──

void ViewGroup::setPadding(int all) {
  padding_ = Insets::all(all);
  requestLayout();
  invalidate();
}

void ViewGroup::setPadding(int horizontal, int vertical) {
  padding_ = Insets::symmetric(horizontal, vertical);
  requestLayout();
  invalidate();
}

void ViewGroup::setPadding(int left, int top, int right, int bottom) {
  padding_ = Insets{left, top, right, bottom};
  requestLayout();
  invalidate();
}

Rect ViewGroup::contentRect() const {
  const Rect local = clientRect();
  return local.inset(px(padding_.left), px(padding_.top), px(padding_.right), px(padding_.bottom));
}

// ── 命中 ──

View* ViewGroup::hitTest(Point localPoint) const {
  if (!isVisible() || !clientRect().contains(localPoint)) return nullptr;

  // 后添加的盖在上面，所以从后往前找
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    View* child = *it;
    if (!child->isVisible()) continue;
    if (child->bounds_.isEmpty()) continue;
    // 装饰视图不参与命中：标题栏的文字标成透明后，按在标题上才会落到
    // TitleBar 自己，拖动与双击才生效。
    if (child->isMouseTransparent()) continue;
    const Point inChild = localPoint;
    const Rect& b = child->bounds_;
    if (b.contains(inChild)) {
      if (auto* group = dynamic_cast<ViewGroup*>(child)) {
        if (View* deep = group->hitTest(Point{inChild.x - b.left, inChild.y - b.top})) {
          return deep;
        }
        continue;
      }
      return child;
    }
  }
  return const_cast<ViewGroup*>(this);
}

View* ViewGroup::findViewById(int id) {
  if (View* hit = View::findViewById(id)) return hit;
  for (View* child : children_) {
    if (View* hit = child->findViewById(id)) return hit;
  }
  return nullptr;
}

// ── 布局 ──

void ViewGroup::onLayout(const Rect& frame) {
  (void)frame;
  arrangeChildren(contentRect());
}

void ViewGroup::arrangeChildren(const Rect& content) {
  // 基类行为即「叠放」：按布局参数定尺寸，再按对齐方式摆进内容区。
  // FrameLayout 直接沿用；LinearLayout 覆盖成顺序排列。
  const Size available{content.width(), content.height()};

  for (View* child : children_) {
    if (!child->isVisible()) continue;

    const LayoutParams& p = child->layoutParams();
    const int ml = px(p.marginLeft);
    const int mt = px(p.marginTop);
    const int mr = px(p.marginRight);
    const int mb = px(p.marginBottom);
    const int offsetX = px(p.x);
    const int offsetY = px(p.y);

    const Size box{std::max(0, available.width - ml - mr), std::max(0, available.height - mt - mb)};

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
    if (p.maxWidth > 0) width = std::min(width, px(p.maxWidth));
    if (p.maxHeight > 0) height = std::min(height, px(p.maxHeight));

    int x = content.left + ml + offsetX;
    int y = content.top + mt + offsetY;

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

// ── 绘制 ──

void ViewGroup::onDrawChildren(Painter& painter, const Rect& dirty) {
  for (View* child : children_) {
    if (!child->isVisible()) continue;
    const Rect& b = child->bounds();
    if (b.isEmpty()) continue;

    // 与自己的脏区求交：子视图完全在脏区外时不必进去
    const Rect overlap = b.intersect(dirty);
    if (overlap.isEmpty()) continue;

    painter.save();
    painter.translate(b.left, b.top);
    child->draw(painter, dirty.moved(-b.left, -b.top));
    painter.restore();
  }
}

}  // namespace pyui
