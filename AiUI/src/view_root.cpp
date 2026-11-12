// ViewRoot 实现：唯一窗口、按更新区重绘、事件坐标转换、焦点与捕获
#include "AiUI/view_root.h"

#include "AiUI/events/event_dispatcher.h"

#include "AiUI/theme.h"
#include "AiUI/view_group.h"

namespace AiUI {

ViewRoot::ViewRoot() = default;

ViewRoot::~ViewRoot() {
  // content_ 由调用方持有，这里只断开引用
  content_ = nullptr;
  focused_ = nullptr;
  hovered_ = nullptr;
  capture_ = nullptr;
  pressed_ = nullptr;
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
  Painter& painter = canvas->painter();
  painter.save();
  painter.clipRect(target);

  // 兜底铺底：内容留白或根视图未覆盖到的部分
  const Color fallback =
      content_ ? content_->theme().body : defaultTheme().body;
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

  painter.restore();
  canvas->commit();
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
  if (delayed_.empty()) return;

  // 计时器只有一个：到期时把已到点的任务一次性发完，再按最早的那个重装
  const int64_t now = platform()->monotonicMillis();
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

void ViewRoot::scheduleNextDelayed() {
  if (delayed_.empty()) {
    if (window_) window_->setDelayTimer(0);
    return;
  }
  // 系统只有一只单次定时器，按最近到期的时间重装；到点后再装下一个
  int64_t earliest = delayed_.front().deadline;
  for (const DelayedTask& task : delayed_) {
    if (task.deadline < earliest) earliest = task.deadline;
  }
  const int64_t wait = earliest - platform()->monotonicMillis();
  if (window_) window_->setDelayTimer(static_cast<int>(wait > 0 ? wait : 1));
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
  setHovered(target, &e);
  if (target) {
    EventDispatcher dispatcher(pathFromRoot(target), e);
    dispatcher.dispatchMouseMove();
    window_->setCursorShape(target->cursorShape());
  } else if (window_) {
    window_->setCursorShape(CursorShape::kArrow);
  }
}

void ViewRoot::onMouseLeave() {
  if (!hovered_) return;
  hovered_->dispatchMouseLeave();
  hovered_ = nullptr;
  if (window_) window_->setCursorShape(CursorShape::kArrow);
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
