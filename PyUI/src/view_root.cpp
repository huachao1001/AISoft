#include "pyui/view_root.h"

#include <algorithm>

namespace pyui {

ViewRoot::~ViewRoot() {
  detachContent();
  content_.reset();
}

int ViewRoot::px(int designValue) const {
  if (designValue == 0) return 0;
  const long long scaled = static_cast<long long>(designValue) * dpi_;
  if (scaled >= 0) return static_cast<int>((scaled + 48) / 96);
  return -static_cast<int>((-scaled + 48) / 96);
}

// ── 内容根 ──

void ViewRoot::setContentView(std::unique_ptr<ViewGroup> content) {
  detachContent();
  content_ = std::move(content);
  attachContent();
  requestLayout();
  invalidate();
}

void ViewRoot::attachContent() {
  if (content_ == nullptr) return;
  if (theme_ != nullptr) content_->setTheme(theme_);
  content_->attachTree(this);
}

void ViewRoot::detachContent() {
  if (content_ == nullptr) return;
  // 焦点/悬停/按下都指向树内视图，树一拆就全部悬空
  focused_ = nullptr;
  hot_ = nullptr;
  pressed_ = nullptr;
  clickCandidate_ = nullptr;
  pressInside_ = false;
  content_->detachTree();
}

void ViewRoot::setTheme(const Theme* theme) {
  theme_ = theme;
  if (content_ != nullptr) content_->setTheme(theme);
  invalidate();
}

// ── 尺寸 ──

void ViewRoot::setSize(Size size) {
  if (size.width < 0) size.width = 0;
  if (size.height < 0) size.height = 0;
  if (size_ == size) return;
  size_ = size;
  requestLayout();
  invalidate();
}

// ── 布局 ──

void ViewRoot::requestLayout() {
  // 只置位：调用方常常一次改十几个属性，每个都立刻整树重排会白做很多遍。
  // 真正重排发生在 draw 之前或显式 performLayout。
  layoutRequested_ = true;
}

void ViewRoot::performLayout() {
  layoutRequested_ = false;
  if (content_ == nullptr) return;
  content_->measure(size_);
  const Rect frame = Rect::fromSize(size_.width, size_.height);
  content_->layout(frame);

  // 布局变化后，之前记录的悬停/按下视图可能已经移走或不可见
  if (hot_ != nullptr && (!hot_->isVisible() || !hot_->isAttached())) hot_ = nullptr;
  if (pressed_ != nullptr && (!pressed_->isVisible() || !pressed_->isAttached())) {
    pressed_ = nullptr;
    clickCandidate_ = nullptr;
  }
  if (focused_ != nullptr && (!focused_->isVisible() || !focused_->isAttached())) {
    focused_ = nullptr;
  }
}

// ── 脏区 ──

void ViewRoot::invalidate() {
  hasDirty_ = true;
  dirty_ = Rect::fromSize(size_.width, size_.height);
  if (onInvalidate_) onInvalidate_(dirty_);
}

void ViewRoot::invalidateRect(const Rect& windowRect) {
  // 先夹到客户区：控件可能算出负坐标或超出边界（正在移入/移出的动画），
  // 把这类矩形直接交给平台会得到一块无意义的区域。
  const Rect clamped = windowRect.intersect(Rect::fromSize(size_.width, size_.height));
  if (clamped.isEmpty()) return;

  if (!hasDirty_) {
    dirty_ = clamped;
    hasDirty_ = true;
  } else {
    dirty_ = Rect{std::min(dirty_.left, clamped.left), std::min(dirty_.top, clamped.top),
                  std::max(dirty_.right, clamped.right), std::max(dirty_.bottom, clamped.bottom)};
  }
  if (onInvalidate_) onInvalidate_(clamped);
}

bool ViewRoot::consumeDirtyRect(Rect& out) {
  if (!hasDirty_) return false;
  out = dirty_;
  hasDirty_ = false;
  dirty_ = Rect{};
  return true;
}

void ViewRoot::clearDirty() {
  hasDirty_ = false;
  dirty_ = Rect{};
}

// ── 绘制 ──

void ViewRoot::draw(Painter& painter, const Rect& dirty) {
  // 有挂起的布局请求时先补齐：宿主在属性变更后直接重绘也能拿到正确的几何。
  if (layoutRequested_) performLayout();

  if (content_ == nullptr) return;
  const Rect area = dirty.intersect(Rect::fromSize(size_.width, size_.height));
  if (area.isEmpty()) return;

  painter.save();
  painter.clipRect(area);
  content_->draw(painter, area);
  painter.restore();
}

void ViewRoot::drawAll(Painter& painter) {
  draw(painter, Rect::fromSize(size_.width, size_.height));
}

// ── 焦点 ──

void ViewRoot::setFocusedView(View* view) {
  if (focused_ == view) return;
  if (view != nullptr && (!view->isFocusable() || !view->isEffectivelyEnabled())) return;

  View* previous = focused_;
  focused_ = view;
  if (previous != nullptr) previous->dispatchFocusChanged(false);
  if (focused_ != nullptr) focused_->dispatchFocusChanged(true);
  invalidate();
}

namespace {

// 深度优先收集可聚焦视图，顺序即 Tab 顺序（先序 = 视觉顺序）
void collectFocusable(View* view, std::vector<View*>& out) {
  if (view == nullptr || !view->isVisible() || !view->isEffectivelyEnabled()) return;
  if (view->isFocusable()) out.push_back(view);
  for (View* child : view->children_) collectFocusable(child, out);
}

}  // namespace

View* ViewRoot::findNextFocusable(View* from, bool forward) {
  if (content_ == nullptr) return nullptr;
  std::vector<View*> order;
  collectFocusable(content_.get(), order);
  if (order.empty()) return nullptr;

  if (from == nullptr) return forward ? order.front() : order.back();

  const auto it = std::find(order.begin(), order.end(), from);
  if (it == order.end()) return forward ? order.front() : order.back();

  const auto index = static_cast<long>(it - order.begin());
  const long count = static_cast<long>(order.size());
  const long next = forward ? (index + 1) % count : (index - 1 + count) % count;
  return order[static_cast<size_t>(next)];
}

// ── 命中 ──

View* ViewRoot::hitTest(Point windowPoint) const {
  if (content_ == nullptr) return nullptr;
  if (!Rect::fromSize(size_.width, size_.height).contains(windowPoint)) return nullptr;

  View* hit = content_->hitTest(windowPoint);
  if (hit == nullptr) return nullptr;

  // 容器自身被命中时，命中结果就是容器；交互语义上这算「落在空白处」，
  // 但仍要返回它，否则点击空白不会清除焦点/悬停。
  return hit;
}

Rect ViewRoot::contentBounds() const {
  if (content_ == nullptr) return Rect{};
  return content_->absoluteBounds();
}

// ── 鼠标分发 ──

namespace {

// 把窗口坐标换成目标视图的局部坐标
MouseEvent toLocal(const MouseEvent& e, const View* target) {
  MouseEvent local = e;
  const Point origin = target->absolutePosition();
  local.position = Point{e.position.x - origin.x, e.position.y - origin.y};
  return local;
}

}  // namespace

// 悬停目标切换：旧目标收到 leave（它自己会 invalidate），新目标必须显式
// 请求重绘。
//
// 这一步漏掉过一次，症状是「鼠标移到按钮上底色不变」。hot_ 只是 view 上的
// 一个 bool，没有任何东西会因为它的变化自动重画——dispatchMouseLeave 里
// 有 invalidate，进入时却没有，于是只有离开方向有反馈，进入方向没有。
void ViewRoot::setHotView(View* target) {
  if (hot_ == target) return;
  if (hot_ != nullptr) hot_->dispatchMouseLeave();
  hot_ = target;
  if (hot_ != nullptr) {
    hot_->hot_ = true;
    hot_->invalidate();
  }
}

bool ViewRoot::dispatchMouseDown(const MouseEvent& e) {
  View* target = hitTest(e.position);

  // 悬停在按下瞬间同步：否则「按下时指针已在视图上但还没触发 move」的
  // 第一次点击会缺悬停态。
  setHotView(target);

  if (target == nullptr) {
    pressed_ = nullptr;
    clickCandidate_ = nullptr;
    pressInside_ = false;
    return false;
  }

  // 点击可聚焦控件时顺便移焦点（与系统习惯一致）
  if (target->isFocusable()) setFocusedView(target);

  pressed_ = target;
  clickCandidate_ = target;
  pressInside_ = true;

  EventState state;
  MouseEvent local = toLocal(e, target);
  local.propagation = &state;
  target->dispatchMouseDown(local);
  return state.handled || true;
}

void ViewRoot::dispatchMouseUp(const MouseEvent& e) {
  View* target = hitTest(e.position);
  View* pressed = pressed_;
  const bool same = pressed != nullptr && target == pressed;

  if (pressed != nullptr) {
    EventState state;
    MouseEvent local = toLocal(e, pressed);
    local.propagation = &state;
    pressed->dispatchMouseUp(local);
  }

  pressed_ = nullptr;
  pressInside_ = false;

  // 「在同一视图上按下又松开」才算点击
  if (same && clickCandidate_ == target && target != nullptr) {
    target->dispatchClick();
  }
  clickCandidate_ = nullptr;

  // 松开后指针可能已经移出，重新算一次悬停（同样要重绘新目标）
  setHotView(target);
}

bool ViewRoot::dispatchWindowRoleClick(Point windowPoint) {
  View* target = hitTest(windowPoint);
  if (target == nullptr) return false;

  // 角色声明在控件本身上。装饰性子视图（图标、标题）标了透明，命中不会落到
  // 它们；万一内部还有嵌套，往上找第一个带角色的祖先即可。
  View* owner = target;
  while (owner != nullptr && owner->windowRole() == WindowRole::kNone) owner = owner->parent();
  if (owner == nullptr) return false;

  // 只触发点击，不构造鼠标事件：控件自己的 onClick 不接受事件参数，
  // 而上层如果需要点击位置，会在 MouseDown 那条正常路径里已经拿到。
  owner->dispatchClick();
  invalidate();
  return true;
}

void ViewRoot::dispatchMouseMove(const MouseEvent& e) {
  // 按住鼠标期间指针被「捕获」：移动事件继续投给按下时命中的视图，
  // 而不是重新做命中测试。滑块拖出控件外再拉回来、在输入框外拖选文字、
  // 拖到窗口边缘这些操作全靠它；按命中结果分发会导致一移出就断线。
  if (pressed_ != nullptr) {
    EventState state;
    MouseEvent local = toLocal(e, pressed_);
    local.propagation = &state;
    pressed_->dispatchMouseMove(local);
    return;
  }

  View* target = hitTest(e.position);

  setHotView(target);

  if (target != nullptr) {
    EventState state;
    MouseEvent local = toLocal(e, target);
    local.propagation = &state;
    target->dispatchMouseMove(local);
  }
}

void ViewRoot::dispatchMouseLeave() {
  if (hot_ != nullptr) {
    hot_->dispatchMouseLeave();
    hot_ = nullptr;
  }
  if (pressed_ != nullptr) {
    pressed_->pressed_ = false;
    pressed_->invalidate();
    pressed_ = nullptr;
  }
  clickCandidate_ = nullptr;
  pressInside_ = false;
}

bool ViewRoot::dispatchMouseWheel(const MouseEvent& e) {
  View* target = hitTest(e.position);
  if (target == nullptr) return false;

  EventState state;
  MouseEvent local = toLocal(e, target);
  local.propagation = &state;
  const bool handled = target->dispatchMouseWheel(local);
  return handled || state.handled;
}

void ViewRoot::dispatchContextMenu(const MouseEvent& e) {
  View* target = hitTest(e.position);
  if (target == nullptr) return;
  EventState state;
  MouseEvent local = toLocal(e, target);
  local.propagation = &state;
  target->dispatchContextMenu(local);
}

void ViewRoot::dispatchDoubleClick(const MouseEvent& e) {
  View* target = hitTest(e.position);
  if (target == nullptr) return;
  EventState state;
  MouseEvent local = toLocal(e, target);
  local.propagation = &state;
  target->dispatchDoubleClick(local);
}

// ── 键盘 ──

bool ViewRoot::dispatchKeyDown(const Key& key, const Modifiers& mods) {
  // Tab 是全局的：没有控件该独占它，否则焦点就永远走不出那个控件。
  if (key == Key::kTab) {
    View* next = findNextFocusable(focused_, !mods.shift);
    if (next != nullptr) setFocusedView(next);
    return true;
  }

  if (focused_ == nullptr) return false;
  return focused_->dispatchKeyDown(key, mods);
}

bool ViewRoot::dispatchChar(std::uint32_t codepoint) {
  if (focused_ == nullptr) return false;
  return focused_->dispatchChar(codepoint);
}

// ── 计时 ──

void ViewRoot::dispatchTimer() {
  if (content_ == nullptr) return;

  // 只驱动可见子树的计时器：隐藏的控件不该继续推进动画。
  // 这里刻意从根开始递归而不是收集列表——onTimer 里可能改动树。
  struct Walker {
    static void run(View* view) {
      if (view == nullptr || !view->isVisible()) return;
      view->dispatchTimer();
      const auto children = view->children_;
      for (View* child : children) run(child);
    }
  };
  Walker::run(content_.get());
}

}  // namespace pyui
