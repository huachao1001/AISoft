// 控件基类实现：树结构、测量、布局、事件转发
#include "AiUI/widget.h"

#include "AiUI/theme.h"

#include <algorithm>

namespace AiUI {
namespace {

// 所有控件共用一个宿主适配器：把平台窗口回调转给具体的 Widget
class WidgetHost : public WindowHost {
 public:
  explicit WidgetHost(Widget* owner) : owner_(owner) {}

  void onPaint(Canvas& canvas) override { owner_->dispatchPaint(canvas); }
  void onResize(int width, int height) override { owner_->dispatchResize(width, height); }
  void onMouseDown(const MouseEvent& e) override { owner_->dispatchMouseDown(e); }
  void onMouseUp(const MouseEvent& e) override { owner_->dispatchMouseUp(e); }
  void onMouseMove(const MouseEvent& e) override { owner_->dispatchMouseMove(e); }
  void onMouseLeave() override { owner_->dispatchMouseLeave(); }
  void onMouseWheel(const MouseEvent& e) override { owner_->dispatchMouseWheel(e); }
  void onDoubleClick(const MouseEvent& e) override { owner_->dispatchDoubleClick(e); }
  void onContextMenu(const MouseEvent& e) override { owner_->dispatchContextMenu(e); }
  bool onKeyDown(const Key& key, const Modifiers& mods) override {
    return owner_->dispatchKeyDown(key, mods);
  }
  bool onChar(wchar_t ch) override { return owner_->dispatchChar(ch); }
  void onFocusChanged(bool focused) override { owner_->dispatchFocusChanged(focused); }
  void onTimer() override { owner_->dispatchTimer(); }
  void onScroll(const ScrollEvent& e) override { owner_->dispatchScroll(e); }

 private:
  Widget* owner_;
};

}  // namespace

Widget::~Widget() {
  destroy();
  // 父控件拥有子控件的生命周期：子控件都是 new 出来的
  for (Widget* child : children_) {
    child->parent_ = nullptr;
    delete child;
  }
  children_.clear();
}

// ───────────────────────── 树结构 ─────────────────────────

Widget* Widget::addChild(Widget* child, const LayoutParams& params) {
  if (!child || child == this) return nullptr;
  if (child->parent_) child->parent_->removeChild(child);

  child->parent_ = this;
  child->params_ = params;
  children_.push_back(child);

  // 容器已经有窗口时，立刻给新子控件建窗口
  if (window_ && !child->window_) {
    child->create(window_->handle(), child->id_, child->bounds_, child->className_);
  }
  invalidate();
  return child;
}

void Widget::attach() {
  if (!parent_ || !parent_->window_ || window_) return;
  create(parent_->window_->handle(), id_, bounds_, className_);
}

void Widget::removeChild(Widget* child) {
  auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return;
  (*it)->parent_ = nullptr;
  children_.erase(it);
  delete child;
  invalidate();
}

void Widget::removeAllChildren() {
  for (Widget* child : children_) {
    child->parent_ = nullptr;
    delete child;
  }
  children_.clear();
  invalidate();
}

void Widget::setLayoutParams(const LayoutParams& params) {
  params_ = params;
  invalidateParent();
}

// ───────────────────────── 生命周期 ─────────────────────────

NativeHandle Widget::create(NativeHandle parentHandle, int id, const Rect& bounds,
                            const std::wstring& className) {
  if (window_) return window_->handle();

  parentHandle_ = parentHandle;
  id_ = id;
  bounds_ = bounds;
  if (!className.empty()) className_ = className;

  WindowParams params;
  params.id = id;
  params.className = className_;
  params.tabStop = true;
  params.scrollable = scrollable_;
  host_ = std::make_unique<WidgetHost>(this);

  window_ = platform()->createWindow(parentHandle, params, *host_);
  if (!window_) {
    host_.reset();
    return nullptr;
  }

  window_->setBounds(bounds);
  onCreated();

  // 树可能在建窗之前就搭好了，这里补建子控件窗口
  for (Widget* child : children_) {
    if (!child->window_) {
      child->create(window_->handle(), child->id_, child->bounds_, child->className_);
    }
  }

  arrangeChildren(Rect{0, 0, bounds.width(), bounds.height()});
  return window_->handle();
}

void Widget::destroy() {
  if (window_) {
    platform()->destroyWindow(window_);
    window_ = nullptr;
  }
  host_.reset();
}

NativeHandle Widget::handle() const { return window_ ? window_->handle() : nullptr; }

// ───────────────────────── 几何 ─────────────────────────

Rect Widget::clientRect() const {
  if (!window_) return Rect{0, 0, bounds_.width(), bounds_.height()};
  return window_->clientRect();
}

int Widget::dpi() const {
  if (window_) return window_->dpi();
  if (parent_) return parent_->dpi();
  return platform()->screenDpi();
}

int Widget::px(int value) const { return value * dpi() / 96; }

Point Widget::absolutePosition() const {
  Point p{bounds_.left, bounds_.top};
  for (const Widget* w = parent_; w; w = w->parent_) {
    p.x += w->bounds_.left;
    p.y += w->bounds_.top;
  }
  return p;
}

Size Widget::measure(const Size& available) {
  // 先让子类给内容尺寸，再用布局参数覆盖
  measured_ = onMeasure(available);
  const int d = dpi();

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
  return measured_;
}

void Widget::layout(const Rect& frame) {
  bounds_ = frame;
  if (window_) window_->setBounds(frame);
  onLayout(frame);
  arrangeChildren(Rect{0, 0, frame.width(), frame.height()});
}

void Widget::setBounds(const Rect& frame) { layout(frame); }

void Widget::invalidate() {
  if (window_) window_->invalidate();
}

void Widget::invalidateRect(const Rect& rc) {
  if (window_) window_->invalidateRect(rc);
}

void Widget::show(bool visible) {
  visible_ = visible;
  if (window_) window_->show(visible);
}

void Widget::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  invalidate();
}

void Widget::setFocus() {
  if (window_) window_->setFocus();
}

void Widget::invalidateParent() {
  if (parent_) parent_->invalidate();
}

// ───────────────────────── 默认排列 ─────────────────────────

void Widget::arrangeChildren(const Rect& content) {
  // 默认行为接近 FrameLayout：绝对坐标 + 对齐
  for (Widget* child : children_) {
    if (!child->visible_) continue;

    const int d = dpi();
    const int ml = child->params_.marginLeft * d / 96;
    const int mt = child->params_.marginTop * d / 96;
    const int mr = child->params_.marginRight * d / 96;
    const int mb = child->params_.marginBottom * d / 96;

    const Size available{content.width() - ml - mr, content.height() - mt - mb};
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

// ───────────────────────── 事件转发 ─────────────────────────

void Widget::dispatchPaint(Canvas& canvas) {
  const Rect rc = clientRect();
  if (rc.isEmpty()) return;

  Painter& painter = canvas.painter();
  // 圆角控件的四角会露出底色，所以必须铺满：取自身或最近的上级底色
  painter.fillRect(rc, resolvedBackground());

  onPaint(painter, rc);
  // 子控件各自有窗口，由系统按 z 序绘制
}

Color Widget::resolvedBackground() const {
  for (const Widget* w = this; w; w = w->parent_) {
    const Color color = w->backgroundColor();
    if (color != kNoColor) return color;
  }
  return kColorBody;
}

void Widget::dispatchResize(int width, int height) {
  bounds_ = Rect{bounds_.left, bounds_.top, bounds_.left + width, bounds_.top + height};
  onLayout(bounds_);
  arrangeChildren(Rect{0, 0, width, height});
}

void Widget::dispatchMouseDown(const MouseEvent& e) {
  if (!enabled_) return;
  onMouseDown(e);
  if (onMouseDownFn_) onMouseDownFn_(e);
}

void Widget::dispatchMouseUp(const MouseEvent& e) {
  releaseMouse();
  if (!enabled_) return;
  onMouseUp(e);
  if (onMouseUpFn_) onMouseUpFn_(e);
}

void Widget::dispatchMouseMove(const MouseEvent& e) {
  onMouseMove(e);
  if (onMouseMoveFn_) onMouseMoveFn_(e);
}

void Widget::dispatchMouseLeave() {
  onMouseLeave();
  if (onMouseLeaveFn_) onMouseLeaveFn_();
}

void Widget::dispatchMouseWheel(const MouseEvent& e) { onMouseWheel(e); }

void Widget::dispatchContextMenu(const MouseEvent& e) { onContextMenu(e); }

void Widget::dispatchDoubleClick(const MouseEvent& e) { onDoubleClick(e); }

bool Widget::dispatchKeyDown(const Key& key, const Modifiers& mods) {
  if (!enabled_) return false;
  if (onKeyDown(key, mods)) return true;
  if (onKeyDownFn_) return onKeyDownFn_(key, mods);
  return false;
}

bool Widget::dispatchChar(wchar_t ch) {
  if (!enabled_) return false;
  return onChar(ch);
}

void Widget::dispatchFocusChanged(bool focused) { onFocusChanged(focused); }

void Widget::dispatchScroll(const ScrollEvent& e) { onScroll(e); }

void Widget::dispatchTimer() { onTimer(); }

void Widget::dispatchCompositionStart() { onCompositionStart(); }

void Widget::dispatchCompositionUpdate(const std::wstring& text) {
  onCompositionUpdate(text);
}

void Widget::dispatchCompositionEnd() { onCompositionEnd(); }

void Widget::dispatchClick() {
  if (onClick_) onClick_();
}

// ───────────────────────── 辅助 ─────────────────────────

Size Widget::onMeasure(const Size& available) {
  // 默认：横向撑满、纵向无内容
  return Size{available.width, 0};
}

Color Widget::backgroundColor() const { return kNoColor; }

void Widget::captureMouse() {
  if (window_) window_->captureMouse(true);
}

void Widget::releaseMouse() {
  if (window_) window_->captureMouse(false);
}

void Widget::setCursor(CursorShape shape) {
  if (window_) window_->setCursorShape(shape);
}

Size Widget::childMeasureSize(Widget* child, const Size& available) const {
  const int d = dpi();
  const Size size{available.width - (child->params_.marginLeft + child->params_.marginRight) * d / 96,
                  available.height -
                      (child->params_.marginTop + child->params_.marginBottom) * d / 96};
  return child->measure(size);
}

}  // namespace AiUI
