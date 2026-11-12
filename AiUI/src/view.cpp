// View 实现：树结构、几何、绘制、事件分发
#include "AiUI/view.h"

#include "AiUI/theme.h"
#include "AiUI/view_group.h"
#include "AiUI/view_root.h"

#include <algorithm>

namespace AiUI {

View::View() = default;

View::~View() {
  // 子视图由容器拥有；析构时统一断开，避免悬空 root
  detachTree();
}

void View::setLayoutParams(const LayoutParams& params) {
  params_ = params;
  requestLayout();
}

// ───────────────────────── 生命周期 ─────────────────────────

bool View::create(NativeHandle parent, const Rect& bounds) {
  if (ownedRoot_) return true;

  auto root = std::make_unique<ViewRoot>();
  if (!root->attach(parent, bounds)) return false;
  ownedRoot_ = std::move(root);
  root_ = ownedRoot_.get();
  root_->setContentView(this);

  attachTree();
  root_->layout(bounds);
  return true;
}

bool View::create(NativeHandle parent, const WindowParams& params, const Rect& bounds) {
  if (ownedRoot_) return true;

  auto root = std::make_unique<ViewRoot>();
  if (!root->attach(parent, params, bounds)) return false;
  ownedRoot_ = std::move(root);
  root_ = ownedRoot_.get();
  root_->setContentView(this);

  attachTree();
  root_->layout(bounds);
  return true;
}

void View::attachTree() {
  // 容器可能在 onCreated 里追加子视图，那条路径会立即挂载，这里防重入
  if (created_) return;
  created_ = true;
  onAttach();
  // 先递归挂载已有子树，再回调 onCreated，让它在已挂载的树上补建子视图
  if (auto* group = dynamic_cast<ViewGroup*>(this)) group->attachChildren();
  onCreated();
}

void View::detachTree() {
  // 脱树前撤掉自己的延时任务，否则到期会回调已销毁的视图
  if (root_) root_->postDelayed(this, 0);
  if (auto* group = dynamic_cast<ViewGroup*>(this)) group->detachChildren();
  if (created_) onDetach();
  created_ = false;
  root_ = nullptr;
}

NativeHandle View::handle() const { return root_ ? root_->handle() : nullptr; }

void View::layout(const Rect& frame) {
  // 根视图的尺寸就是窗口尺寸，交给 ViewRoot 一并移动窗口
  if (ownedRoot_) {
    ownedRoot_->layout(frame);
    return;
  }
  bounds_ = frame;
  onLayout(frame);
}

// ───────────────────────── 几何 ─────────────────────────

Rect View::clientRect() const { return Rect{0, 0, bounds_.width(), bounds_.height()}; }

Rect View::absoluteBounds() const {
  int x = bounds_.left;
  int y = bounds_.top;
  for (const View* p = parent_; p; p = p->parent_) {
    x += p->bounds_.left;
    y += p->bounds_.top;
  }
  return Rect{x, y, x + bounds_.width(), y + bounds_.height()};
}

Point View::absolutePosition() const {
  const Rect rc = absoluteBounds();
  return Point{rc.left, rc.top};
}

Rect View::localToWindow(const Rect& localRect) const {
  const Point origin = absolutePosition();
  return localRect.moved(origin.x, origin.y);
}

Point View::localToWindow(Point localPoint) const {
  const Point origin = absolutePosition();
  return Point{localPoint.x + origin.x, localPoint.y + origin.y};
}

Point View::windowToLocal(Point windowPoint) const {
  const Point origin = absolutePosition();
  return Point{windowPoint.x - origin.x, windowPoint.y - origin.y};
}

int View::dpi() const {
  if (root_) return root_->dpi();
  if (parent_) return parent_->dpi();
  return platform()->screenDpi();
}

int View::px(int value) const { return value * dpi() / 96; }

Size View::measure(const Size& available) {
  measured_ = onMeasure(available);
  const int d = dpi();

  // 布局参数优先于内容尺寸
  if (params_.width == LayoutParams::kMatchParent) {
    measured_.width = available.width;
  } else if (params_.width != LayoutParams::kWrapContent) {
    measured_.width = params_.width * d / 96;
  }
  if (params_.height == LayoutParams::kMatchParent) {
    measured_.height = available.height;
  } else if (params_.height != LayoutParams::kWrapContent) {
    measured_.height = params_.height * d / 96;
  }
  measured_.width = (std::max)(0, measured_.width);
  measured_.height = (std::max)(0, measured_.height);
  return measured_;
}

Size View::childMeasureSize(View* child, const Size& available) const {
  const int d = dpi();
  const int w = available.width - (child->params_.marginLeft + child->params_.marginRight) * d / 96;
  const int h = available.height - (child->params_.marginTop + child->params_.marginBottom) * d / 96;
  return child->measure(Size{(std::max)(0, w), (std::max)(0, h)});
}

// 叶子视图只可能命中自己
View* View::findViewById(int id) {
  return (id != 0 && id_ == id) ? this : nullptr;
}

// ───────────────────────── 刷新 ─────────────────────────

void View::invalidate() {
  if (!visible_ || !root_) return;
  root_->invalidateRect(absoluteBounds());
}

void View::invalidateRect(const Rect& localRect) {
  if (!visible_ || !root_ || localRect.isEmpty()) return;
  // 局部矩形先夹到自身范围，越界部分不必麻烦上层
  const Rect clipped = localRect.intersect(clientRect());
  if (clipped.isEmpty()) return;
  root_->invalidateRect(localToWindow(clipped));
}

void View::requestLayout() {
  if (root_) root_->requestLayout();
}

// ───────────────────────── 外观与状态 ─────────────────────────

void View::show(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  if (root_) root_->requestLayout();
  // 隐藏后原位置要由父容器补画
  if (parent_) parent_->invalidate();
  else invalidate();
}

void View::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  invalidate();
}

void View::setBackgroundColor(Color color) {
  if (explicitBackground_ == color && !backgroundRole_) return;
  explicitBackground_ = color;
  backgroundRole_ = nullptr;
  invalidate();
}

void View::setBackgroundRole(Color Theme::*role) {
  if (backgroundRole_ == role) return;
  backgroundRole_ = role;
  invalidate();
}

void View::setFocus() {
  // 由 ViewRoot 在本视图内部找到第一个可聚焦节点，容器也能这样拿到焦点
  if (root_) root_->requestFocus(this);
}

bool View::isFocused() const { return root_ && root_->focused() == this; }

// ───────────────────────── 绘制 ─────────────────────────

void View::draw(Painter& painter, const Rect& dirty) {
  if (!visible_) return;

  const Rect self = clientRect();
  const Rect clipped = dirty.intersect(self);
  if (clipped.isEmpty()) return;

  painter.save();
  // 只画脏区：子树再与自身范围求交
  painter.clipRect(clipped);

  // 只有显式背景才铺底。没有背景的视图让父容器的底色透上来，
  // 这样容器上的渐变、圆角不会被整块矩形盖掉。
  const Color background = backgroundColor();
  if (background != kNoColor) painter.fillRect(self, background);

  onPaint(painter, self);
  onDrawChildren(painter, clipped);
  onDrawForeground(painter, self);

  painter.restore();
}

// ───────────────────────── 主题 ─────────────────────────

void View::invalidateSubtree(View* view) {
  if (!view) return;
  view->invalidate();
  for (View* child : view->children_) {
    invalidateSubtree(child);
  }
}

void View::setTheme(const Theme* theme) {
  if (theme_ == theme) return;
  theme_ = theme;
  // 换配色只影响绘制，不涉及尺寸
  invalidateSubtree(this);
}

const Theme& View::theme() const {
  for (const View* v = this; v; v = v->parent_) {
    if (v->theme_) return *v->theme_;
  }
  return defaultTheme();
}

Color View::resolvedBackground() const {
  for (const View* v = this; v; v = v->parent_) {
    const Color color = v->backgroundColor();
    if (color != kNoColor) return color;
  }
  return theme().body;
}

Color View::backgroundColor() const {
  // 主题角色优先：换主题后不必让宿主重新下发颜色
  if (backgroundRole_) return theme().*backgroundRole_;
  return explicitBackground_;
}

// ───────────────────────── 事件 ─────────────────────────

bool View::dispatchMouseDown(const MouseEvent& e) {
  if (!enabled_) return false;
  const bool consumed = onMouseDown(e);
  if (onMouseDownFn_) onMouseDownFn_(e);
  return consumed;
}

void View::dispatchMouseUp(const MouseEvent& e) {
  if (!enabled_) return;
  onMouseUp(e);
  if (onMouseUpFn_) onMouseUpFn_(e);
}

// ── 捕获阶段：容器在子视图之前拿到事件，返回 true 表示拦下 ──

bool View::dispatchInterceptMouseDown(const MouseEvent& e) {
  if (!enabled_) return false;
  return onInterceptMouseDown(e);
}

bool View::dispatchInterceptMouseMove(const MouseEvent& e) {
  if (!enabled_) return false;
  return onInterceptMouseMove(e);
}

bool View::dispatchInterceptMouseUp(const MouseEvent& e) {
  if (!enabled_) return false;
  return onInterceptMouseUp(e);
}

void View::dispatchMouseMove(const MouseEvent& e) {
  onMouseMove(e);
  if (onMouseMoveFn_) onMouseMoveFn_(e);
}

void View::dispatchMouseLeave() {
  onMouseLeave();
  if (onMouseLeaveFn_) onMouseLeaveFn_();
}

bool View::dispatchMouseWheel(const MouseEvent& e) {
  if (!enabled_) return false;
  return onMouseWheel(e);
}

void View::dispatchContextMenu(const MouseEvent& e) {
  if (!enabled_) return;
  onContextMenu(e);
}

void View::dispatchDoubleClick(const MouseEvent& e) {
  if (!enabled_) return;
  onDoubleClick(e);
}

bool View::dispatchKeyDown(const Key& key, const Modifiers& mods) {
  if (!enabled_) return false;
  if (onKeyDown(key, mods)) return true;
  if (onKeyDownFn_) return onKeyDownFn_(key, mods);
  return false;
}

bool View::dispatchChar(wchar_t ch) {
  if (!enabled_) return false;
  return onChar(ch);
}

void View::dispatchFocusChanged(bool focused) { onFocusChanged(focused); }

void View::dispatchWindowActivated(bool active) { onWindowActivated(active); }

void View::dispatchTimer() { onTimer(); }

void View::dispatchDelayed() { onDelayed(); }

void View::dispatchClick() {
  if (onClick_) onClick_();
}

// ───────────────────────── 辅助 ─────────────────────────

Size View::onMeasure(const Size& available) {
  // 默认：横向撑满、纵向无内容
  return Size{available.width, 0};
}

void View::captureMouse() {
  if (root_) root_->capture(this);
}

void View::releaseMouse() {
  if (root_) root_->releaseCapture(this);
}

void View::setCursor(CursorShape shape) {
  cursorShape_ = shape;
  if (root_) root_->applyCursor(this);
}

void View::startBlinkTimer() {
  if (root_) root_->setBlinkTimer(true);
}

void View::stopBlinkTimer() {
  if (root_) root_->setBlinkTimer(false);
}

void View::postDelayed(int ms) {
  if (root_) root_->postDelayed(this, ms);
}

void View::updateImeCaret(int localX, int localY) {
  if (!root_) return;
  const Point p = localToWindow(Point{localX, localY});
  root_->setImeCaret(p.x, p.y);
}

}  // namespace AiUI
