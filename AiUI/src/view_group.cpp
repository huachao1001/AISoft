// ViewGroup 实现：子树管理、逐层绘制、命中测试
#include "AiUI/view_group.h"

#include "AiUI/view_root.h"

#include <algorithm>

namespace AiUI {

ViewGroup::~ViewGroup() {
  // 父容器拥有子视图
  for (View* child : children_) {
    child->parent_ = nullptr;
    child->detachTree();
    delete child;
  }
  children_.clear();
}

// ───────────────────────── 子树 ─────────────────────────

View* ViewGroup::addView(View* child, const LayoutParams& params) {
  if (!child || child == this) return nullptr;
  // 视图只属于一个父容器。换父容器需要先显式 removeView（它会销毁对象），
  // 所以这里只拒绝，不偷偷转移所有权。
  if (child->parent_) return nullptr;

  child->parent_ = this;
  child->params_ = params;
  children_.push_back(child);

  // 容器已挂到窗口时，新子视图立刻递归挂载
  if (root_) {
    child->root_ = root_;
    child->attachTree();
    root_->requestLayout();
  }
  invalidate();
  onChildAdded(child);
  return child;
}

void ViewGroup::removeView(View* child) {
  const auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return;

  (*it)->parent_ = nullptr;
  (*it)->detachTree();
  children_.erase(it);
  // 先让父容器补画被占的区域，再销毁
  invalidate();
  delete child;
  if (root_) root_->requestLayout();
}

void ViewGroup::removeAllViews() {
  for (View* child : children_) {
    child->parent_ = nullptr;
    child->detachTree();
    delete child;
  }
  children_.clear();
  invalidate();
  if (root_) root_->requestLayout();
}

View* ViewGroup::childAt(int index) const {
  if (index < 0 || index >= static_cast<int>(children_.size())) return nullptr;
  return children_[static_cast<size_t>(index)];
}

int ViewGroup::indexOfChild(View* child) const {
  for (size_t i = 0; i < children_.size(); ++i) {
    if (children_[i] == child) return static_cast<int>(i);
  }
  return -1;
}

void ViewGroup::attachChildren() {
  for (View* child : children_) {
    child->root_ = root_;
    child->attachTree();
  }
}

void ViewGroup::detachChildren() {
  for (View* child : children_) child->detachTree();
}

// ───────────────────────── 内边距 ─────────────────────────

void ViewGroup::setPadding(int all) { setPadding(all, all, all, all); }

void ViewGroup::setPadding(int horizontal, int vertical) {
  setPadding(horizontal, vertical, horizontal, vertical);
}

void ViewGroup::setPadding(int left, int top, int right, int bottom) {
  if (padding_.left == left && padding_.top == top && padding_.right == right &&
      padding_.bottom == bottom) {
    return;
  }
  padding_ = Insets{left, top, right, bottom};
  invalidate();
  if (root_) root_->requestLayout();
}

Rect ViewGroup::contentRect() const {
  const int d = dpi();
  return clientRect().inset(padding_.left * d / 96, padding_.top * d / 96,
                            padding_.right * d / 96, padding_.bottom * d / 96);
}

// ───────────────────────── 布局 ─────────────────────────

void ViewGroup::onLayout(const Rect& frame) {
  (void)frame;
  arrangeChildren(contentRect());
}

void ViewGroup::arrangeChildren(const Rect& content) {
  // 默认行为接近 FrameLayout：绝对坐标 + 对齐
  for (View* child : children_) {
    if (!child->isVisible()) continue;

    const int d = dpi();
    const int ml = child->params_.marginLeft * d / 96;
    const int mt = child->params_.marginTop * d / 96;
    const int mr = child->params_.marginRight * d / 96;
    const int mb = child->params_.marginBottom * d / 96;

    const Size available{(std::max)(0, content.width() - ml - mr),
                         (std::max)(0, content.height() - mt - mb)};
    const Size desired = child->measure(available);

    const int w = desired.width;
    const int h = desired.height;
    int x = content.left + ml + child->params_.x * d / 96;
    int y = content.top + mt + child->params_.y * d / 96;

    // x/y 为 0 时才按对齐摆放，避免与绝对坐标冲突
    if (child->params_.x == 0) {
      if (child->params_.width == LayoutParams::kMatchParent) {
        x = content.left + ml;
      } else if (child->params_.horizontal == Gravity::kCenter) {
        x = content.left + (content.width() - w) / 2;
      } else if (child->params_.horizontal == Gravity::kEnd) {
        x = content.right - mr - w;
      }
    }
    if (child->params_.y == 0) {
      if (child->params_.height == LayoutParams::kMatchParent) {
        y = content.top + mt;
      } else if (child->params_.vertical == Gravity::kCenter) {
        y = content.top + (content.height() - h) / 2;
      } else if (child->params_.vertical == Gravity::kEnd) {
        y = content.bottom - mb - h;
      }
    }
    child->layout(Rect{x, y, x + w, y + h});
  }
}

// ───────────────────────── 查找 ─────────────────────────

View* ViewGroup::findViewById(int id) {
  if (id == 0) return nullptr;
  if (id_ == id) return this;
  for (View* child : children_) {
    if (View* hit = child->findViewById(id)) return hit;
  }
  return nullptr;
}

// ───────────────────────── 绘制 ─────────────────────────

void ViewGroup::onDrawChildren(Painter& painter, const Rect& dirty) {
  for (View* child : children_) {

    if (!child->isVisible()) continue;

    const Rect childRect = child->bounds();
    // 完全在脏区之外就跳过
    if (childRect.intersect(dirty).isEmpty()) continue;

    painter.save();
    // 平移到子视图局部坐标；draw 内部按局部坐标裁剪
    painter.translate(childRect.left, childRect.top);
    child->draw(painter, dirty.moved(-childRect.left, -childRect.top));
    painter.restore();
  }
}

// ───────────────────────── 命中 ─────────────────────────

HitTarget ViewGroup::onHitTest(const Point& windowPoint) {
  // 先看有没有真正需要鼠标的子视图（含子孙）：拖窗容器里得把它们
  // 留在客户区，否则落在按钮上的点击会被当成拖窗口。
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    View* child = *it;
    if (child->claimsPointerAt(windowPoint)) return HitTarget::kClient;
  }
  // 再看子视图自己声明的非客户区（如标题栏的最大化按钮）
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    View* child = *it;
    if (!child->isVisible()) continue;
    if (!child->absoluteBounds().contains(windowPoint)) continue;
    const HitTarget target = child->onHitTest(windowPoint);
    if (target != HitTarget::kClient) return target;
  }
  // 没人认领才回落到容器自身：开了 dragWindow 就是可拖动区域
  return View::onHitTest(windowPoint);
}

bool ViewGroup::claimsPointerAt(const Point& windowPoint) {
  if (!isVisible() || !absoluteBounds().contains(windowPoint)) return false;
  for (View* child : children_) {
    if (child->claimsPointerAt(windowPoint)) return true;
  }
  return View::claimsPointerAt(windowPoint);
}

View* ViewGroup::hitTest(Point localPoint) const {
  // 从上层往下找，后加的画在上面
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    View* child = *it;
    if (!child->isVisible()) continue;
    const Rect rc = child->bounds();
    if (!rc.contains(localPoint)) continue;

    const Point inner{localPoint.x - rc.left, localPoint.y - rc.top};
    if (auto* group = dynamic_cast<ViewGroup*>(child)) {
      // 容器自身也可能响应（如滚动条的轨道空白）
      if (View* deeper = group->hitTest(inner)) return deeper;
    }
    return child;
  }
  // 落在容器空白处：返回容器自己
  return const_cast<ViewGroup*>(this);
}

}  // namespace AiUI
