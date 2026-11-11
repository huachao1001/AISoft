// ScrollView 实现：横竖两个方向的滚动位置管理 + 自绘滚动条
#include "AiUI/scroll_view.h"

namespace AiUI {
namespace {

// 圆角半径取厚度的一半，两端才是标准的半圆端帽
int capRadius(int thickness) { return (std::max)(1, thickness / 2); }

}  // namespace

void ScrollView::setScrollBarStyle(const ScrollBarStyle& style) {
  style_ = style;
  invalidate();
}

void ScrollView::setVerticalScrollEnabled(bool enabled) {
  if (verticalEnabled_ == enabled) return;
  verticalEnabled_ = enabled;
  if (!enabled) scroll_ = 0;
  invalidate();
}

void ScrollView::setHorizontalScrollEnabled(bool enabled) {
  if (horizontalEnabled_ == enabled) return;
  horizontalEnabled_ = enabled;
  if (!enabled) scrollX_ = 0;
  invalidate();
}

int ScrollView::scrollBarReserve() const {
  // 只按"内容是否溢出"决定让位：布局期必须稳定，不能随鼠标悬停抖动
  if (!isScrollable()) return 0;
  return px(style_.margin + style_.width);
}

bool ScrollView::scrollBarVisible() const { return isScrollable() && barShown_; }
bool ScrollView::hScrollBarVisible() const { return isScrollableX() && barShown_; }

Rect ScrollView::viewportRect() const {
  const Rect rc = clientRect();
  const int top = viewportTop();
  const int left = viewportLeft();
  return Rect{rc.left + left, rc.top + top, rc.right, rc.bottom};
}

// 垂直条贴右边，水平条贴底边；两条相交的角落各自让出对方的厚度
Rect ScrollView::vTrackRect() const {
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int width = px(style_.width);
  // 有水平条时上收一格，避免两条在角上重叠
  const int bottom = rc.bottom - (isScrollableX() ? px(style_.width + margin) : 0);
  return Rect{rc.right - margin - width, rc.top, rc.right - margin, bottom};
}

Rect ScrollView::hTrackRect() const {
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int width = px(style_.width);
  const int right = rc.right - (isScrollable() ? px(style_.width + margin) : 0);
  return Rect{rc.left, rc.bottom - margin - width, right, rc.bottom - margin};
}

Rect ScrollView::vThumbRect() const {
  const Rect track = vTrackRect();
  if (!isScrollable() || track.isEmpty()) return Rect{};

  // 只按是否悬停在滑块上决定粗细：停在轨道上不加粗，避免整条乱跳
  const int thickness = px(vThumbHot_ ? style_.hotThumbWidth : style_.thumbWidth);
  const int centerX = track.center().x;
  const int left = centerX - thickness / 2;
  const int length = (std::max)(px(style_.minThumbLength),
                                track.height() * viewportHeight() / contentHeight());
  const int travel = track.height() - length;
  const int maxPos = maxScroll();
  const int offset = maxPos > 0 ? travel * scroll_ / maxPos : 0;

  return Rect{left, track.top + offset, left + thickness, track.top + offset + length};
}

Rect ScrollView::hThumbRect() const {
  const Rect track = hTrackRect();
  if (!isScrollableX() || track.isEmpty()) return Rect{};

  const int thickness = px(hThumbHot_ ? style_.hotThumbWidth : style_.thumbWidth);
  const int centerY = track.center().y;
  const int top = centerY - thickness / 2;
  const int length = (std::max)(px(style_.minThumbLength),
                                track.width() * viewportWidth() / contentWidth());
  const int travel = track.width() - length;
  const int maxPos = maxScrollX();
  const int offset = maxPos > 0 ? travel * scrollX_ / maxPos : 0;

  return Rect{track.left + offset, top, track.left + offset + length, top + thickness};
}

int ScrollView::scrollForVThumb(int thumbTop) const {
  const Rect track = vTrackRect();
  const Rect thumb = vThumbRect();
  const int travel = track.height() - thumb.height();
  if (travel <= 0) return 0;
  const int relative = thumbTop - track.top;
  const int maxPos = maxScroll();
  return (std::max)(0, (std::min)(maxPos, relative * maxPos / travel));
}

int ScrollView::scrollForHThumb(int thumbLeft) const {
  const Rect track = hTrackRect();
  const Rect thumb = hThumbRect();
  const int travel = track.width() - thumb.width();
  if (travel <= 0) return 0;
  const int relative = thumbLeft - track.left;
  const int maxPos = maxScrollX();
  return (std::max)(0, (std::min)(maxPos, relative * maxPos / travel));
}

int ScrollView::clampScroll(int offset) const {
  return (std::max)(0, (std::min)(offset, maxScroll()));
}

int ScrollView::clampScrollX(int offset) const {
  return (std::max)(0, (std::min)(offset, maxScrollX()));
}

void ScrollView::setScrollOffset(int offset) {
  const int next = clampScroll(offset);
  if (next == scroll_) return;
  scroll_ = next;
  // 滑块位置变了，滚动条也要重画
  onScrollChanged();
}

void ScrollView::scrollBy(int delta) { setScrollOffset(scroll_ + delta); }

void ScrollView::setScrollOffsetX(int offset) {
  const int next = clampScrollX(offset);
  if (next == scrollX_) return;
  scrollX_ = next;
  onScrollChanged();
}

void ScrollView::scrollByX(int delta) { setScrollOffsetX(scrollX_ + delta); }

void ScrollView::onLayout(const Rect& frame) {
  View::onLayout(frame);
  // 视口变小后原滚动位置可能越界
  bool changed = false;
  if (scroll_ > maxScroll()) {
    scroll_ = maxScroll();
    changed = true;
  }
  if (scrollX_ > maxScrollX()) {
    scrollX_ = maxScrollX();
    changed = true;
  }
  if (changed) onScrollChanged();

  // 不再溢出了就不该继续显示滚动条
  const bool anyScrollable = isScrollable() || isScrollableX();
  if (!anyScrollable && barShown_) {
    barShown_ = false;
    postDelayed(0);
    invalidate();
  }
}

// ───────────────────────── 显示策略 ─────────────────────────

void ScrollView::setScrollBarShown(bool shown) {
  if (barShown_ == shown) return;
  barShown_ = shown;
  // 只是浮层显隐，内容占位不变：只刷滚动条占据的边带
  invalidateBarArea();
}

void ScrollView::invalidateBarArea() {
  // 两条边带都刷：两个方向可能同时存在
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int thickness = px(style_.width);
  const int band = margin + thickness;
  // 用最粗的一档（含悬停加粗）保证切换时旧痕迹被覆盖
  const int pad = (std::max)(band, px(style_.hotThumbWidth) - px(style_.thumbWidth) / 2 + 2);
  invalidateRect(Rect{rc.right - pad, rc.top, rc.right, rc.bottom});
  invalidateRect(Rect{rc.left, rc.bottom - pad, rc.right, rc.bottom});
}

void ScrollView::revealScrollBar() {
  if (!isScrollable() && !isScrollableX()) return;
  setScrollBarShown(true);
}

void ScrollView::onDelayed() {
  // 延时到期：鼠标已经回到视图内就继续显示
  if (mouseInside_ || dragging_ != Bar::kNone) return;
  setScrollBarShown(false);
}

bool ScrollView::hitScrollBar(Point local) const {
  return hitVTrack(local) || hitHTrack(local);
}

// ───────────────────────── 绘制 ─────────────────────────

void ScrollView::onDrawForeground(Painter& painter, const Rect& rect) {
  if (!scrollBarVisible() && !hScrollBarVisible()) return;

  // 滚动条只画在视口内，表头之类固定区域不被覆盖
  const Rect viewport = viewportRect().intersect(rect);
  if (viewport.isEmpty()) return;

  painter.save();
  painter.clipRect(viewport);

  if (scrollBarVisible()) {
    const Rect track = vTrackRect();
    const int radius = style_.rounded ? capRadius(px(style_.width)) : 0;
    if (style_.track != kNoColor) painter.fillRoundRect(track, style_.track, radius);

    const Rect thumb = vThumbRect();
    if (!thumb.isEmpty()) {
      const int thickness = px(vThumbHot_ ? style_.hotThumbWidth : style_.thumbWidth);
      painter.fillRoundRect(thumb, (vThumbHot_ || dragging_ == Bar::kVertical) ? style_.thumbHover
                                                                             : style_.thumb,
                            style_.rounded ? capRadius(thickness) : 0);
    }
  }

  if (hScrollBarVisible()) {
    const Rect track = hTrackRect();
    const int radius = style_.rounded ? capRadius(px(style_.width)) : 0;
    if (style_.track != kNoColor) painter.fillRoundRect(track, style_.track, radius);

    const Rect thumb = hThumbRect();
    if (!thumb.isEmpty()) {
      const int thickness = px(hThumbHot_ ? style_.hotThumbWidth : style_.thumbWidth);
      painter.fillRoundRect(thumb,
                            (hThumbHot_ || dragging_ == Bar::kHorizontal) ? style_.thumbHover
                                                                         : style_.thumb,
                            style_.rounded ? capRadius(thickness) : 0);
    }
  }

  painter.restore();
}

// ───────────────────────── 交互 ─────────────────────────

bool ScrollView::hitVThumb(Point local) const {
  const Rect thumb = vThumbRect();
  return !thumb.isEmpty() && thumb.inflated(px(2), 0).contains(local);
}

bool ScrollView::hitHThumb(Point local) const {
  const Rect thumb = hThumbRect();
  return !thumb.isEmpty() && thumb.inflated(0, px(2)).contains(local);
}

bool ScrollView::hitVTrack(Point local) const {
  const Rect track = vTrackRect();
  return !track.isEmpty() && track.contains(local);
}

bool ScrollView::hitHTrack(Point local) const {
  const Rect track = hTrackRect();
  return !track.isEmpty() && track.contains(local);
}

bool ScrollView::onMouseDown(const MouseEvent& e) {
  if (hitVThumb(e.position)) {
    dragging_ = Bar::kVertical;
    dragGrab_ = e.position.y - vThumbRect().top;
    captureMouse();
    invalidateBarArea();
    return true;
  }
  if (hitHThumb(e.position)) {
    dragging_ = Bar::kHorizontal;
    dragGrab_ = e.position.x - hThumbRect().left;
    captureMouse();
    invalidateBarArea();
    return true;
  }
  // 点轨道空白：向点击方向跳一页
  if (isScrollable() && hitVTrack(e.position)) {
    const int page = (std::max)(1, viewportHeight());
    scrollBy(e.position.y < vThumbRect().top ? -page : page);
    return true;
  }
  if (isScrollableX() && hitHTrack(e.position)) {
    const int page = (std::max)(1, viewportWidth());
    scrollByX(e.position.x < hThumbRect().left ? -page : page);
    return true;
  }
  return false;
}

void ScrollView::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (dragging_ == Bar::kNone) return;
  dragging_ = Bar::kNone;
  releaseMouse();
  // 松手后如果鼠标已经不在滚动条上，重新开始隐藏倒计时
  if (!vThumbHot_ && !hThumbHot_) postDelayed(style_.hideDelayMs);
  invalidateBarArea();
}

void ScrollView::onMouseMove(const MouseEvent& e) {
  if (dragging_ == Bar::kVertical) {
    setScrollOffset(scrollForVThumb(e.position.y - dragGrab_));
    return;
  }
  if (dragging_ == Bar::kHorizontal) {
    setScrollOffsetX(scrollForHThumb(e.position.x - dragGrab_));
    return;
  }

  // 鼠标在视图内就保持显示；倒计时只由"离开"启动，避免每次移动都重设系统定时器
  if (!mouseInside_) {
    mouseInside_ = true;
    postDelayed(0);
    revealScrollBar();
  }

  const bool vHot = hitVThumb(e.position);
  const bool hHot = hitHThumb(e.position);
  if (vHot != vThumbHot_ || hHot != hThumbHot_) {
    vThumbHot_ = vHot;
    hThumbHot_ = hHot;
    // 粗细变化只发生在滑块周围，刷两条边带足够
    invalidateBarArea();
  }
}

void ScrollView::onMouseLeave() {
  mouseInside_ = false;
  const bool changed = vThumbHot_ || hThumbHot_;
  vThumbHot_ = false;
  hThumbHot_ = false;
  if (changed) invalidateBarArea();
  // 离开视图后延时隐藏，而不是立刻消失
  if (style_.hideDelayMs > 0) {
    postDelayed(style_.hideDelayMs);
  } else {
    setScrollBarShown(false);
  }
}

bool ScrollView::onMouseWheel(const MouseEvent& e) {
  if (e.wheelDelta == 0) return false;

  // 有横向溢出且按住 Shift 时滚横向，这是桌面软件的通行习惯
  if (e.modifiers.shift && isScrollableX()) {
    revealScrollBar();
    postDelayed(style_.hideDelayMs);
    scrollByX(-e.wheelDelta * px(wheelStep()) / 120);
    return true;
  }
  if (!isScrollable()) return false;
  // 滚动时也把滚动条带出来，方便看清位置
  revealScrollBar();
  postDelayed(style_.hideDelayMs);
  // 一格 120，向后滚为正（内容上移）
  scrollBy(-e.wheelDelta * px(wheelStep()) / 120);
  return true;
}

}  // namespace AiUI
