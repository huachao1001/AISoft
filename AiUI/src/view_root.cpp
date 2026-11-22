// ViewRoot 实现：唯一窗口、按更新区重绘、事件坐标转换、焦点与捕获
#include "AiUI/view_root.h"

#include "AiUI/events/event_dispatcher.h"

#include "AiUI/theme.h"
#include "AiUI/view_group.h"

namespace AiUI {
namespace {

// 鼠标停住多久才浮出悬停提示。太短会跟着鼠标一路弹，太长则像没反应。
constexpr int kTooltipDelayMs = 600;

}  // namespace

ViewRoot::ViewRoot() = default;

ViewRoot::~ViewRoot() {
  // content_ 由调用方持有，这里只断开引用
  content_ = nullptr;
  focused_ = nullptr;
  hovered_ = nullptr;
  capture_ = nullptr;
  pressed_ = nullptr;
  tooltipOwner_ = nullptr;
  destroyWindow();
}

void ViewRoot::destroyWindow() {
  if (!window_) return;
  window_->setBlinkTimer(false);
  platform()->destroyWindow(window_);
  window_ = nullptr;
}

// ───────────────────────── 挂载 ─────────────────────────

bool ViewRoot::attach(NativeHandle parent, const Rect& bounds) {
  WindowParams params;
  params.className = L"AiUIViewRoot";
  params.visible = true;
  params.tabStop = true;
  return attach(parent, params, bounds);
}

bool ViewRoot::attach(NativeHandle parent, const WindowParams& params, const Rect& bounds) {
  if (window_) return true;

  window_ = platform()->createWindow(parent, params, *this);
  if (!window_) return false;

  // 顶层窗口的 bounds 指客户区；子窗口同理按客户区铺满
  if (!bounds.isEmpty()) window_->setBounds(bounds);
  return true;
}

NativeHandle ViewRoot::handle() const { return window_ ? window_->handle() : nullptr; }

void ViewRoot::setContentView(View* view) {
  content_ = view;
  requestLayout();
}

int ViewRoot::dpi() const {
  if (window_) return window_->dpi();
  return platform()->screenDpi();
}

Rect ViewRoot::clientRect() const {
  if (window_) return window_->clientRect();
  if (content_) return content_->clientRect();
  return Rect{};
}

void ViewRoot::layout(const Rect& bounds) {
  if (!content_) return;
  // 根视图的矩形就是窗口区域。尺寸真的变了才移动窗口，
  // 否则 setBounds 触发的 WM_SIZE 会再排一次版。
  if (window_) {
    const Rect current = window_->clientRect();
    if (!bounds.isEmpty() &&
        (current.width() != bounds.width() || current.height() != bounds.height())) {
      window_->setBounds(bounds);
      return;  // WM_SIZE 里会调用 onResize 完成重排
    }
  }
  performLayout(window_ ? window_->clientRect() : bounds);
}

void ViewRoot::performLayout(const Rect& area) {
  if (!content_) return;

  // 根视图铺满客户区，尺寸由窗口决定，不看布局参数
  content_->bounds_ = Rect{0, 0, area.width(), area.height()};
  content_->measure(Size{area.width(), area.height()});
  content_->onLayout(content_->bounds_);
  layoutPending_ = false;
}

void ViewRoot::requestLayout() {
  if (layoutPending_) return;
  layoutPending_ = true;
  // 只标脏，真正的测量摆放放到下一次绘制前
  invalidate();
}

// ───────────────────────── 刷新 ─────────────────────────

void ViewRoot::invalidate() {
  if (window_) window_->invalidate();
}

void ViewRoot::invalidateRect(const Rect& windowRect) {
  if (!window_ || windowRect.isEmpty()) return;
  window_->invalidateRect(windowRect);
}

void ViewRoot::onExpose(const Rect& update) {
  if (!window_ || !content_) return;

  const Rect area = window_->clientRect();
  // 有待处理的布局先做，避免用旧尺寸绘制
  if (layoutPending_ || content_->bounds().width() != area.width() ||
      content_->bounds().height() != area.height()) {
    performLayout(area);
  }

  // 系统给的更新区为空表示整窗失效
  const Rect target = update.isEmpty() ? area : update.intersect(area);
  if (target.isEmpty()) return;

  auto canvas = window_->createCanvas(target);
  if (!canvas) return;

  // 画布内部已经把原点移到 target 左上角，这里开始一律用窗口坐标
  paintTree(canvas->painter(), area, target);
  canvas->commit();
}

// 整棵树的绘制：窗口重绘与离屏渲染共用，两边才不会画出两种样子
void ViewRoot::paintTree(Painter& painter, const Rect& area, const Rect& target) {
  painter.save();
  painter.clipRect(target);

  // 兜底铺底：内容留白或根视图未覆盖到的部分
  const Color fallback = content_ ? content_->theme().body : defaultTheme().body;
  painter.fillRect(target, background_ == kNoColor ? fallback : background_);

  // draw 的 dirty 用窗口坐标，各层自己换算到局部
  content_->draw(painter, target);

  // 浮层画在整棵树之上，不受内容区裁剪
  if (overlay_) {
    const Rect overlayArea = overlay_->overlayBounds().intersect(target);
    if (!overlayArea.isEmpty()) {
      painter.save();
      painter.clipRect(overlayArea);
      overlay_->paintOverlay(painter);
      painter.restore();
    }
  }

  // 提示条压在最上方：它是对刚才那次操作的即时回执，不该被任何内容挡住。
  // 裁剪用 dirtyBounds：上浮中的块可能比标脏时的位置更高，用 bounds 会切掉顶部。
  if (toast_.isVisible()) {
    const Rect toastArea = toast_.dirtyBounds(area, dpi()).intersect(target);
    if (!toastArea.isEmpty()) {
      painter.save();
      painter.clipRect(toastArea);
      toast_.paint(painter, area, dpi());
      painter.restore();
    }
  }

  // 悬停提示：压在内容之上、提示条之下——它随时会被鼠标带走，不该盖住回执
  if (tooltip_.isVisible() && tooltipOwner_ && tooltipOwner_->root() == this) {
    const Rect anchor = tooltipAnchor(tooltipOwner_);
    const Rect tipArea = tooltip_.bounds(overlayArea(), anchor, dpi()).intersect(target);
    if (!tipArea.isEmpty()) {
      painter.save();
      painter.clipRect(tipArea);
      tooltip_.paint(painter, overlayArea(), anchor, dpi());
      painter.restore();
    }
  }

  painter.restore();
}

bool ViewRoot::renderToImage(RgbaImage& out, const Size& size) {
  if (!content_) return false;
  // 没有窗口时以给定尺寸排版；有窗口也允许显式尺寸，快照才可复现
  Rect area = size.width > 0 && size.height > 0 ? Rect{0, 0, size.width, size.height}
                                                : (window_ ? window_->clientRect() : content_->clientRect());
  if (area.isEmpty()) return false;
  performLayout(area);
  return platform()->renderOffscreen(
      area, [this, area](Painter& painter) { paintTree(painter, area, area); }, out);
}

void ViewRoot::sendMouseMove(const Point& windowPoint) {
  MouseEvent e;
  e.position = windowPoint;
  onMouseMove(e);
}

void ViewRoot::sendMouseDown(const Point& windowPoint, MouseButton button) {
  MouseEvent e;
  e.position = windowPoint;
  e.button = button;
  onMouseDown(e);
}

void ViewRoot::sendMouseUp(const Point& windowPoint, MouseButton button) {
  MouseEvent e;
  e.position = windowPoint;
  e.button = button;
  onMouseUp(e);
}

// ───────────────────────── 浮层 ─────────────────────────

void ViewRoot::setOverlay(Overlay* overlay) {
  if (overlay_ == overlay) return;
  // 先刷掉旧浮层区域，否则撤下后那块还留着旧像素
  if (overlay_) {
    const Rect area = overlay_->overlayBounds();
    overlay_ = overlay;
    if (!area.isEmpty()) invalidateRect(area);
  }
  overlay_ = overlay;
  if (overlay_) {
    const Rect area = overlay_->overlayBounds();
    if (!area.isEmpty()) invalidateRect(area);
  }
}

// ───────────────────────── 尺寸 ─────────────────────────

void ViewRoot::onResize(int width, int height) {
  (void)width;
  (void)height;
  if (!content_) return;
  // 以真实客户区为准，避免边框与缩放误差
  performLayout(window_ ? window_->clientRect() : Rect{0, 0, width, height});

  // 尺寸变了就整窗重画：新露出来的那圈没有任何人画过，
  // 等下一次 WM_PAINT 才补会露出黑底（最大化动画期间最明显）
  invalidate();

  // 最大化状态可能在缩放后才稳定，这里统一通知一次
  if (window_ && onMaximizeState_) {
    const bool maximized = window_->isMaximized();
    if (maximized != lastMaximized_) {
      lastMaximized_ = maximized;
      onMaximizeState_(maximized);
    }
  }
}
// ───────────────────────── 焦点 ─────────────────────────

View* ViewRoot::findFocusable(View* view) const {
  if (!view || !view->isVisible() || !view->isEnabled()) return nullptr;
  if (view->isFocusable()) return view;
  if (auto* group = dynamic_cast<ViewGroup*>(view)) {
    for (View* child : group->children()) {
      if (View* hit = findFocusable(child)) return hit;
    }
  }
  return nullptr;
}

void ViewRoot::requestFocus(View* view) {
  const View* target = findFocusable(view);
  if (target == focused_) {
    if (target && window_) window_->setFocus();
    return;
  }

  if (focused_) {
    View* previous = focused_;
    focused_ = nullptr;
    previous->stopBlinkTimer();
    previous->dispatchFocusChanged(false);
    previous->invalidate();
  }
  focused_ = const_cast<View*>(target);
  if (focused_) {
    if (window_) window_->setFocus();
    focused_->dispatchFocusChanged(true);
    focused_->invalidate();
  }
}

void ViewRoot::clearFocus() {
  if (!focused_) return;
  View* previous = focused_;
  focused_ = nullptr;
  previous->stopBlinkTimer();
  previous->dispatchFocusChanged(false);
  previous->invalidate();
}

void ViewRoot::onFocusChanged(bool focused) {
  // 窗口失焦只收起光标，逻辑焦点保留，切回来还能继续输入
  if (!focused) {
    if (focused_) {
      focused_->stopBlinkTimer();
      // 不通知 onFocusChanged：那会触发失焦回调，面包屑会直接退出编辑态。
      // 这里只是窗口不在前台，插入点不该继续显示。
      focused_->dispatchWindowActivated(false);
      focused_->invalidate();
    }
    if (hovered_) {
      hovered_->dispatchMouseLeave();
      hovered_ = nullptr;
    }
    return;
  }
  if (focused_) {
    focused_->dispatchWindowActivated(true);
    focused_->dispatchFocusChanged(true);
    focused_->invalidate();
  }
}

void ViewRoot::onTimer() {
  // 只发当前焦点视图，避免所有输入框一起闪
  if (focused_) focused_->dispatchTimer();
}

void ViewRoot::onDelayTimer() {
  const int64_t now = platform()->monotonicMillis();

  // 计时器只有一个：到期时把已到点的任务一次性发完，再按最早的那个重装
  std::vector<View*> due;
  std::vector<DelayedTask> rest;
  due.reserve(delayed_.size());
  for (const DelayedTask& task : delayed_) {
    if (task.deadline <= now) {
      due.push_back(task.view);
    } else {
      rest.push_back(task);
    }
  }
  delayed_.swap(rest);

  // 提示条到点就推进；推进结果决定下一次醒来的时刻。
  // 区域必须在 tick 之前取：淡出收尾那一拍 tick 会立刻 hide()，
  // 之后 bounds() 已是空矩形，那一帧的旧像素就再也刷不掉了。
  // 用 dirtyBounds：它覆盖整段上浮行程，不会因为标脏与落笔之间的时间差漏边。
  if (toast_.isVisible() && (toastWakeMs_ < 0 || toastWakeMs_ <= now)) {
    const Rect before = toast_.dirtyBounds(clientRect(), dpi());
    toastWakeMs_ = toast_.tick(now);
    if (!before.isEmpty()) invalidateRect(before);
    const Rect after = toast_.dirtyBounds(clientRect(), dpi());
    if (!after.isEmpty()) invalidateRect(after);
  }

  // 悬停拖到这个时刻还没动，才把提示浮出来
  if (tooltipOwner_ && tooltipWakeMs_ >= 0 && tooltipWakeMs_ <= now) {
    View* owner = tooltipOwner_;
    tooltipWakeMs_ = -1;
    if (owner->root() == this && !owner->tooltip().empty()) {
      tooltip_.show(owner->tooltip());
      const Rect area = overlayArea();
      invalidateRect(tooltip_.bounds(area, tooltipAnchor(owner), dpi()).inflated(1, 1));
    } else {
      tooltipOwner_ = nullptr;
    }
  }

  // 先重装定时器再回调：回调里可能又 postDelayed，那时会再次改写
  scheduleNextDelayed();

  for (View* view : due) {
    // 目标可能在等待期间被移除，先确认还挂在树上再回调
    if (view && view->root_ == this) view->dispatchDelayed();
  }
}

// ───────────────────────── 定时器与输入法 ─────────────────────────

void ViewRoot::setBlinkTimer(bool on) {
  if (window_) window_->setBlinkTimer(on);
}

void ViewRoot::postDelayed(View* view, int ms) {
  if (!view) return;
  // 同一个视图只保留一个等待，重复调用视为重新计时
  for (size_t i = 0; i < delayed_.size(); ++i) {
    if (delayed_[i].view == view) {
      if (ms > 0) {
        delayed_[i].deadline = platform()->monotonicMillis() + ms;
      } else {
        delayed_.erase(delayed_.begin() + static_cast<std::ptrdiff_t>(i));
      }
      scheduleNextDelayed();
      return;
    }
  }
  if (ms <= 0) return;
  delayed_.push_back(DelayedTask{view, platform()->monotonicMillis() + ms});
  scheduleNextDelayed();
}

void ViewRoot::showToast(const std::wstring& text) {
  toast_.show(text);
  toastWakeMs_ = platform()->monotonicMillis();
  // 新提示可能比旧的长，先把旧提示整段行程刷掉，再刷新提示的区域
  invalidateRect(toast_.dirtyBounds(clientRect(), dpi()));
  // 立刻排第一帧，之后由 onDelayTimer 自己按需续期
  scheduleNextDelayed();
}

void ViewRoot::hideToast() {
  if (!toast_.isVisible()) return;
  const Rect dirty = toast_.dirtyBounds(clientRect(), dpi());
  toast_.hide();
  toastWakeMs_ = -1;
  if (!dirty.isEmpty()) invalidateRect(dirty);
  scheduleNextDelayed();
}

void ViewRoot::scheduleNextDelayed() {
  // 系统只有一只单次定时器：视图等待与提示条取最早的那个
  int64_t earliest = -1;
  for (const DelayedTask& task : delayed_) {
    if (earliest < 0 || task.deadline < earliest) earliest = task.deadline;
  }
  if (toastWakeMs_ >= 0 && (earliest < 0 || toastWakeMs_ < earliest)) {
    earliest = toastWakeMs_;
  }
  if (tooltipWakeMs_ >= 0 && (earliest < 0 || tooltipWakeMs_ < earliest)) {
    earliest = tooltipWakeMs_;
  }

  if (!window_) return;
  if (earliest < 0) {
    window_->setDelayTimer(0);
    return;
  }
  const int64_t wait = earliest - platform()->monotonicMillis();
  window_->setDelayTimer(static_cast<int>(wait > 0 ? wait : 1));
}

void ViewRoot::setImeCaret(int x, int y) {
  if (window_) window_->setImeCaret(x, y);
}

// ───────────────────────── 鼠标捕获与光标 ─────────────────────────

void ViewRoot::capture(View* view) {
  capture_ = view;
  if (window_) window_->captureMouse(true);
}

void ViewRoot::releaseCapture(View* view) {
  // 只有当前持有者能主动释放
  if (capture_ && capture_ != view) return;
  capture_ = nullptr;
  if (window_) window_->captureMouse(false);
}

void ViewRoot::applyCursor(View* view) {
  if (!window_ || !view) return;
  // 捕获期间以捕获视图为准
  if (capture_) {
    if (capture_ != view) return;
  } else if (hovered_ && hovered_ != view) {
    return;
  }
  window_->setCursorShape(view->cursorShape());
}

// ───────────────────────── 命中与坐标 ─────────────────────────

View* ViewRoot::hitTest(Point windowPoint) const {
  if (!content_ || !content_->isVisible()) return nullptr;
  if (!content_->absoluteBounds().contains(windowPoint)) return nullptr;

  // 根视图没有父容器，自己换算到局部坐标
  const Point local = content_->windowToLocal(windowPoint);
  if (auto* group = dynamic_cast<ViewGroup*>(content_)) return group->hitTest(local);
  return content_;
}

Rect ViewRoot::tooltipAnchor(View* owner) const {
  const Rect box = owner->absoluteBounds();
  // 只取顶部一条：顶栏这类高瘦视图占满整列，用整块当锚会把提示甩到远处
  const int height = (std::min)(box.height(), dpi() * 36 / 96);
  return Rect{box.left, box.top, box.right, box.top + height};
}

MouseEvent ViewRoot::localEvent(View* target, const MouseEvent& e) const {
  MouseEvent local = e;
  if (target) local.position = target->windowToLocal(e.position);
  return local;
}

void ViewRoot::setHovered(View* target, const MouseEvent* e) {
  if (target == hovered_) return;

  if (hovered_) hovered_->dispatchMouseLeave();
  hovered_ = target;
  if (hovered_ && e) hovered_->dispatchMouseMove(localEvent(hovered_, *e));
}

// ───────────────────────── 事件 ─────────────────────────

void ViewRoot::onMouseDown(const MouseEvent& e) {
  // 浮层在最上层，先给它机会：点浮层外通常意味着收起
  if (overlay_ && overlay_->onOverlayMouseDown(e.position)) return;

  View* target = hitTest(e.position);
  // 点空白处收起键盘焦点
  if (!target) {
    clearFocus();
    return;
  }
  // 焦点给可聚焦的祖先：点输入框里的文字区也落在输入框本身
  View* focusTarget = nullptr;
  for (View* v = target; v; v = v->parent()) {
    if (v->isFocusable() && v->isEnabled()) {
      focusTarget = v;
      break;
    }
  }
  if (focusTarget) {
    requestFocus(focusTarget);
  } else if (focused_ && target != focused_ && !isDescendantOf(target, focused_)) {
    // 单窗口后没有系统焦点切换：点到按钮这类不可聚焦控件也要让输入框失焦
    clearFocus();
  }

  pressed_ = target;
  // 捕获 → 目标 → 冒泡：容器能在捕获阶段拦下，控件消费后不再外传
  EventDispatcher dispatcher(pathFromRoot(target), e);
  dispatcher.dispatchMouseDown();
  // 按下被容器拦下时，后续 up/move 要回到容器而不是原来的叶子
  if (View* handler = dispatcher.handler()) pressed_ = handler;
}

bool ViewRoot::isDescendantOf(const View* view, const View* ancestor) const {
  for (const View* v = view; v; v = v->parent()) {
    if (v == ancestor) return true;
  }
  return false;
}

void ViewRoot::onMouseUp(const MouseEvent& e) {
  // 抬起必须回到按下的那个视图：即使没开捕获，拖出控件外松手也能正确结束
  View* target = capture_ ? capture_ : (pressed_ ? pressed_ : hitTest(e.position));
  if (target) {
    // 抬起也用同一套冒泡：容器要能收到 up 才会复位拖动状态
    EventDispatcher dispatcher(pathFromRoot(target), e);
    dispatcher.dispatchMouseUp();
  }

  pressed_ = nullptr;
  if (capture_) {
    capture_ = nullptr;
    if (window_) window_->captureMouse(false);
  }

  // 拖动期间事件全给捕获者，悬停与光标一直没更新。松手时鼠标可能已经
  // 拖到别处（甚至拖出控件外），这里按落点重算：否则图案会停在上一个控件的
  // 拖动态，看着像卡住了。
  if (overlay_ && overlay_->onOverlayMouseMove(e.position)) return;
  View* under = hitTest(e.position);
  setHovered(under, &e);
  if (window_) window_->setCursorShape(under ? under->cursorShape() : CursorShape::kArrow);
}

void ViewRoot::onMouseMove(const MouseEvent& e) {
  if (capture_) {
    // 拖动期间不再切换悬停，事件全部给捕获者
    capture_->dispatchMouseMove(localEvent(capture_, e));
    return;
  }

  // 浮层可见时由它接管悬停，底下的内容不该再响应
  if (overlay_ && overlay_->onOverlayMouseMove(e.position)) {
    if (window_) window_->setCursorShape(CursorShape::kHand);
    return;
  }

  View* target = hitTest(e.position);
  // 鼠标一动就把提示收掉：提示只在「停住」时出现，跟着鼠标跑会挡住内容
  hideTooltip();
  setHovered(target, &e);
  if (target) {
    EventDispatcher dispatcher(pathFromRoot(target), e);
    dispatcher.dispatchMouseMove();
    if (window_) window_->setCursorShape(target->cursorShape());
    // 停住才浮出：立刻弹会让人误以为是随鼠标飘的浮标
    if (!target->tooltip().empty()) {
      tooltipOwner_ = target;
      tooltipWakeMs_ = platform()->monotonicMillis() + kTooltipDelayMs;
      scheduleNextDelayed();
    }
  } else if (window_) {
    window_->setCursorShape(CursorShape::kArrow);
  }
}

void ViewRoot::onMouseLeave() {
  hideTooltip();
  if (!hovered_) return;
  hovered_->dispatchMouseLeave();
  hovered_ = nullptr;
  if (window_) window_->setCursorShape(CursorShape::kArrow);
}

void ViewRoot::showTooltipFor(View* owner) {
  if (!owner || owner->root() != this || owner->tooltip().empty()) return;
  tooltipWakeMs_ = -1;
  tooltipOwner_ = owner;
  tooltip_.show(owner->tooltip());
}

void ViewRoot::hideTooltip() {
  tooltipWakeMs_ = -1;
  if (!tooltip_.isVisible()) {
    tooltipOwner_ = nullptr;
    return;
  }
  // 位置要按 owner 当前的样子算：它可能刚被移走，那时用旧框刷不干净
  if (tooltipOwner_ && tooltipOwner_->root() == this) {
    const Rect area = overlayArea();
    invalidateRect(
        tooltip_.bounds(area, tooltipAnchor(tooltipOwner_), dpi()).inflated(1, 1));
  } else {
    invalidate();
  }
  tooltip_.hide();
  tooltipOwner_ = nullptr;
  scheduleNextDelayed();
}

void ViewRoot::onMouseWheel(const MouseEvent& e) {
  View* target = hitTest(e.position);
  if (!target) return;
  // 滚轮同样走冒泡：命中视图不消费就交给外层滚动容器
  EventDispatcher dispatcher(pathFromRoot(target), e);
  dispatcher.dispatchMouseWheel();
}

void ViewRoot::onDoubleClick(const MouseEvent& e) {
  if (View* target = hitTest(e.position)) {
    EventDispatcher dispatcher(pathFromRoot(target), e);
    dispatcher.dispatchDoubleClick();
  }
}

void ViewRoot::onContextMenu(const MouseEvent& e) {
  // 空白处命中的是容器自身，所以空列表区同样能弹菜单
  if (View* target = hitTest(e.position)) {
    EventDispatcher dispatcher(pathFromRoot(target), e);
    dispatcher.dispatchContextMenu();
  }
}

bool ViewRoot::onKeyDown(const Key& key, const Modifiers& mods) {
  // 内容区优先；Esc 在对话框里用于请求关闭
  if (focused_ && focused_->dispatchKeyDown(key, mods)) return true;
  // 内容区没接住的交给宿主，供窗口级快捷键使用
  if (onKey_ && onKey_(key, mods)) return true;
  if (key == Key::kEscape && escapeCloses_) {
    requestClose();
    return true;
  }
  return false;
}

bool ViewRoot::onChar(wchar_t ch) {
  if (!focused_) return false;
  return focused_->dispatchChar(ch);
}

void ViewRoot::setBackgroundColor(Color color) {
  background_ = color;
  // 同步给平台窗口：窗口变大时系统先铺它，避免新增区域露出黑底
  if (window_) {
    window_->setBackgroundColor(color == kNoColor ? defaultTheme().body : color);
  }
  invalidate();
}

// ───────────────────────── 非客户区 ─────────────────────────

HitTarget ViewRoot::onHitTest(const Point& position) {
  if (!content_) return HitTarget::kClient;

  // 大小与边框由平台窗口负责；这里只问视图树。
  // 标题栏这类需要系统拖动的区域会返回 kCaption，其余返回 kClient。
  return content_->onHitTest(position);
}

void ViewRoot::requestClose() {
  if (onClose_ && !onClose_()) return;
  if (window_) window_->close();
}

bool ViewRoot::onCloseRequest() {
  // 有回调时由它决定是否放行；返回 false 让平台继续走默认销毁
  if (onClose_ && !onClose_()) return true;
  return false;
}

}  // namespace AiUI
