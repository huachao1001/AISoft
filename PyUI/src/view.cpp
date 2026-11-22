#include "pyui/view.h"

#include "pyui/view_group.h"
#include "pyui/view_root.h"

#include <algorithm>

namespace pyui {

View::View() = default;
View::~View() = default;

// ── 窗口角色 ──
//
// 一张表同时供两处使用：生成期校验 XML 的取值域、运行期把字符串翻成枚举。
// 分两处写就会出现「生成器认、运行期不认」这类只在运行期才暴露的裂缝。
const char* windowRoleName(WindowRole role) {
  switch (role) {
    case WindowRole::kCaption: return "caption";
    case WindowRole::kMaximize: return "maximize";
    case WindowRole::kMinimize: return "minimize";
    case WindowRole::kClose: return "close";
    case WindowRole::kTitle: return "title";
    case WindowRole::kIcon: return "icon";
    case WindowRole::kNone: break;
  }
  return "none";
}

bool parseWindowRole(const std::string& text, WindowRole* out) {
  if (out == nullptr) return false;
  if (text == "caption") { *out = WindowRole::kCaption; return true; }
  if (text == "maximize") { *out = WindowRole::kMaximize; return true; }
  if (text == "minimize") { *out = WindowRole::kMinimize; return true; }
  if (text == "close") { *out = WindowRole::kClose; return true; }
  if (text == "title") { *out = WindowRole::kTitle; return true; }
  if (text == "icon") { *out = WindowRole::kIcon; return true; }
  if (text == "none") { *out = WindowRole::kNone; return true; }
  return false;
}

// ── 布局参数 ──

void View::setLayoutParams(const LayoutParams& params) {
  params_ = params;
  requestLayout();
}

// ── 标识 ──

View* View::findViewById(int id) {
  // id 为 0 表示「没设 id」：不参与任何匹配。
  // 否则所有没设 id 的视图都会被 findView("") 之类的一次性全命中。
  if (id == 0) return nullptr;
  return id_ == id ? this : nullptr;
}

// ── 几何 ──

void View::layout(const Rect& frame) {
  const Size previous{bounds_.width(), bounds_.height()};
  bounds_ = frame;
  const Size current{frame.width(), frame.height()};

  // 尺寸变了要让子视图重新摆放：否则容器变大时子视图还停在旧位置。
  // 这里不重新 measure —— 父容器在 performLayout 里已经按新尺寸量过；
  // 直接走进 onLayout，让子类按新的客户区摆放子视图。
  (void)previous;
  onLayout(Rect{0, 0, current.width, current.height});
}

Rect View::absoluteBounds() const {
  Rect rc = bounds_;
  const View* node = parent_;
  while (node != nullptr) {
    rc = rc.moved(node->bounds_.left, node->bounds_.top);
    node = node->parent_;
  }
  return rc;
}

Point View::absolutePosition() const {
  const Rect rc = absoluteBounds();
  return Point{rc.left, rc.top};
}

Point View::windowToLocal(Point windowPoint) const {
  const Point origin = absolutePosition();
  return Point{windowPoint.x - origin.x, windowPoint.y - origin.y};
}

int View::dpi() const {
  if (root_ != nullptr) return root_->dpi();
  return designDpi();
}

int View::px(int value) const {
  // 96 DPI 设计值 → 物理像素。零值原样返回，避免 0 * dpi 出现意外符号。
  if (value == 0) return 0;
  const long long scaled = static_cast<long long>(value) * dpi();
  if (scaled >= 0) return static_cast<int>((scaled + 48) / 96);
  return -static_cast<int>((-scaled + 48) / 96);
}

Size View::measure(const Size& available) {
  measured_ = onMeasure(available);
  if (measured_.width < 0) measured_.width = 0;
  if (measured_.height < 0) measured_.height = 0;
  return measured_;
}

Size View::onMeasure(const Size& available) {
  // 基类按布局参数给出尺寸：wrap 时退化为「可用空间」，
  // 否则子类不覆盖 onMeasure 也能被容器正常摆放。
  Size out = available;
  if (params_.width != LayoutParams::kMatchParent && params_.width != LayoutParams::kWrapContent) {
    out.width = px(params_.width);
  }
  if (params_.height != LayoutParams::kMatchParent && params_.height != LayoutParams::kWrapContent) {
    out.height = px(params_.height);
  }
  if (out.width < 0) out.width = 0;
  if (out.height < 0) out.height = 0;
  return out;
}

Size View::childMeasureSize(View* child, const Size& available) const {
  const LayoutParams& p = child->layoutParams();
  Size out{};
  out.width = p.width == LayoutParams::kMatchParent
                  ? available.width
                  : (p.width == LayoutParams::kWrapContent ? 0 : px(p.width));
  out.height = p.height == LayoutParams::kMatchParent
                   ? available.height
                   : (p.height == LayoutParams::kWrapContent ? 0 : px(p.height));
  return out;
}

// ── 刷新 ──

void View::invalidate() {
  if (root_ != nullptr) {
    root_->invalidateRect(absoluteBounds());
  }
}

void View::invalidateRect(const Rect& localRect) {
  if (root_ == nullptr) return;
  const Point origin = absolutePosition();
  root_->invalidateRect(localRect.moved(origin.x, origin.y));
}

void View::requestLayout() {
  if (root_ != nullptr) root_->requestLayout();
}

// ── 主题 ──

void View::setTheme(const Theme* theme) {
  theme_ = theme;
  invalidateSubtree(this);
}

const Theme& View::theme() const {
  if (theme_ != nullptr) return *theme_;
  if (parent_ != nullptr) return parent_->theme();
  return defaultTheme();
}

void View::setBackgroundColor(Color color) {
  explicitBackground_ = color;
  backgroundRole_ = nullptr;
  invalidate();
}

void View::setBackgroundRole(Color Theme::*role) {
  backgroundRole_ = role;
  invalidate();
}

Color View::resolveBackground() const {
  // 显式底色优先于主题角色：业务代码显式设过就不再被换肤改掉。
  if (explicitBackground_ != kNoColor) return explicitBackground_;
  if (backgroundRole_ != nullptr) return theme().*backgroundRole_;
  return kNoColor;
}

// ── 外观与状态 ──

void View::show(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  // 隐藏后不再接收命中，所以即使指针停在上面也要重新算悬停
  if (!visible_ && root_ != nullptr) {
    root_->dispatchMouseLeave();
  }
  requestLayout();
  invalidate();
}

void View::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  invalidate();
}

bool View::isEffectivelyEnabled() const {
  // 祖先被禁用时整棵子树都不可用：逐个检查而不是维护计数，
  // 因为启用/禁用没有严格的栈式配对关系。
  if (!enabled_) return false;
  for (const View* node = parent_; node != nullptr; node = node->parent_) {
    if (!node->enabled_) return false;
  }
  return true;
}

void View::clearVisualState() {
  hot_ = false;
  pressed_ = false;
}

void View::setFocus() {
  if (root_ != nullptr) root_->setFocusedView(this);
}

bool View::isFocused() const {
  return root_ != nullptr && root_->focusedView() == this;
}

// ── 绘制 ──

void View::draw(Painter& painter, const Rect& dirty) {
  if (!visible_ || bounds_.isEmpty()) return;

  const Rect local = clientRect();
  const Rect clipped = local.intersect(dirty);
  if (clipped.isEmpty()) return;

  painter.save();
  // 裁剪到自身矩形：否则控件画超出边界会覆盖兄弟视图
  painter.clipRect(local);

  const Color background = resolveBackground();
  if (background != kNoColor) painter.fillRect(local, background);

  onPaint(painter, local);
  onDrawChildren(painter, clipped);
  onDrawForeground(painter, local);

  painter.restore();
}

// ── 事件默认实现 ──

bool View::dispatchMouseDown(const MouseEvent& e) {
  if (!visible_ || !isEffectivelyEnabled()) return false;
  pressed_ = true;
  const bool handled = onMouseDown(e);
  if (onMouseDownFn_) onMouseDownFn_(e);
  if (handled) e.markHandled();
  invalidate();
  return true;
}

void View::dispatchMouseUp(const MouseEvent& e) {
  const bool wasPressed = pressed_;
  pressed_ = false;
  onMouseUp(e);
  if (onMouseUpFn_) onMouseUpFn_(e);
  (void)wasPressed;
  invalidate();
}

void View::dispatchMouseMove(const MouseEvent& e) {
  onMouseMove(e);
  if (onMouseMoveFn_) onMouseMoveFn_(e);
}

void View::dispatchMouseLeave() {
  hot_ = false;
  pressed_ = false;
  onMouseLeave();
  if (onMouseLeaveFn_) onMouseLeaveFn_();
  invalidate();
}

bool View::dispatchMouseWheel(const MouseEvent& e) {
  if (!visible_) return false;
  return onMouseWheel(e);
}

void View::dispatchContextMenu(const MouseEvent& e) {
  // 回调优先，再走虚函数：两者共存不冲突，控件子类覆盖 onContextMenu 时
  // 挂上来的回调也仍然会被调用。
  if (onContextMenuFn_) onContextMenuFn_(e);
  onContextMenu(e);
}
void View::dispatchDoubleClick(const MouseEvent& e) {
  if (onDoubleClickFn_) onDoubleClickFn_(e);
  onDoubleClick(e);
}

bool View::dispatchKeyDown(const Key& key, const Modifiers& mods) {
  if (!visible_ || !isEffectivelyEnabled()) return false;
  if (onKeyDownFn_ && onKeyDownFn_(key, mods)) return true;
  return onKeyDown(key, mods);
}

bool View::dispatchChar(std::uint32_t codepoint) {
  if (!visible_ || !isEffectivelyEnabled()) return false;
  return onChar(codepoint);
}

void View::dispatchFocusChanged(bool focused) {
  onFocusChanged(focused);
  invalidate();
}

void View::dispatchTimer() { onTimer(); }

void View::dispatchClick() {
  if (onClick_) onClick_();
}

// ── 挂载 ──

void View::attachTree(ViewRoot* root) {
  root_ = root;
  attached_ = true;
  onAttached();
  for (View* child : children_) child->attachTree(root);
}

void View::detachTree() {
  // 先让子视图解绑再解自己：子类的 onDetached 可能还要访问父链，
  // 顺序反了会读到半拆状态。
  for (View* child : children_) child->detachTree();
  onDetached();
  attached_ = false;
  created_ = false;
  root_ = nullptr;
}

void View::invalidateSubtree(View* view) {
  view->invalidate();
  for (View* child : view->children_) invalidateSubtree(child);
}

}  // namespace pyui
