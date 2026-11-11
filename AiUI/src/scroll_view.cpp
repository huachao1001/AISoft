// ScrollView 实现：滚动位置管理 + 自绘滚动条
#include "AiUI/scroll_view.h"

namespace AiUI {
namespace {

// 圆角半径取宽度的一半，两端才是标准的半圆端帽
int capRadius(int width) { return (std::max)(1, width / 2); }

}  // namespace

void ScrollView::setScrollBarStyle(const ScrollBarStyle& style) {
  style_ = style;
  invalidate();
}

int ScrollView::scrollBarReserve() const {
  // 只按"内容是否溢出"决定让位：布局期必须稳定，不能随鼠标悬停抖动
  if (!isScrollable()) return 0;
  return px(style_.margin + style_.hoverWidth);
}

bool ScrollView::scrollBarVisible() const {
  return isScrollable() && barShown_;
}

Rect ScrollView::viewportRect() const {
  const Rect rc = clientRect();
  const int top = viewportTop();
  return Rect{rc.left, rc.top + top, rc.right, rc.bottom};
}

// 轨道按当前展开状态取宽度：鼠标在滚动条上时更宽，便于抓取
Rect ScrollView::trackRect() const {
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int width = px(trackHot_ || thumbHot_ ? style_.hoverWidth : style_.width);
  return Rect{rc.right - margin - width, rc.top, rc.right - margin, rc.bottom};
}

Rect ScrollView::thumbRect() const {
  const Rect track = trackRect();
  if (!isScrollable() || track.isEmpty()) return Rect{};

  const int centerX = track.center().x;
  const int thumbWidth = px(trackHot_ || thumbHot_ ? style_.hoverThumbWidth : style_.thumbWidth);
  // 滑块高度按视口占比，太短时给个下限，保证可拖
  const int height = (std::max)(px(style_.minThumbHeight),
                                track.height() * viewportHeight() / contentHeight());
  const int travel = track.height() - height;
  const int maxPos = maxScroll();
  const int offset = maxPos > 0 ? travel * scroll_ / maxPos : 0;

  return Rect{centerX - thumbWidth / 2, track.top + offset, centerX + thumbWidth / 2,
              track.top + offset + height};
}

int ScrollView::scrollForThumbTop(int thumbTop) const {
  const Rect track = trackRect();
  const Rect thumb = thumbRect();
  const int travel = track.height() - thumb.height();
  if (travel <= 0) return 0;
  const int relative = thumbTop - track.top;
  const int maxPos = maxScroll();
  return (std::max)(0, (std::min)(maxPos, relative * maxPos / travel));
}

int ScrollView::clampScroll(int offset) const {
  return (std::max)(0, (std::min)(offset, maxScroll()));
}

void ScrollView::setScrollOffset(int offset) {
  const int next = clampScroll(offset);
  if (next == scroll_) return;
  scroll_ = next;
  // 滚动条位置变了，滑块也要重画
  onScrollChanged();
}

void ScrollView::scrollBy(int delta) { setScrollOffset(scroll_ + delta); }

void ScrollView::onLayout(const Rect& frame) {
  View::onLayout(frame);
  // 视口变小后原滚动位置可能越界
  const int maxPos = maxScroll();
  if (scroll_ > maxPos) {
    scroll_ = maxPos;
    onScrollChanged();
  }
  // 不再溢出了就不该继续显示滚动条
  if (!isScrollable() && barShown_) {
    barShown_ = false;
    postDelayed(0);
    invalidate();
  }
}

// ───────────────────────── 显示策略 ─────────────────────────

void ScrollView::setScrollBarShown(bool shown) {
  if (barShown_ == shown) return;
  barShown_ = shown;
  // 只是浮层显隐，内容占位不变：只刷滚动条那一条，避免整块重绘
  invalidateBarArea();
}

void ScrollView::invalidateBarArea() {
  // 用最宽的一档算区域，宽度切换时新旧痕迹都能覆盖到
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int widest = px(style_.hoverWidth);
  invalidateRect(Rect{rc.right - margin - widest, rc.top, rc.right, rc.bottom});
}

void ScrollView::revealScrollBar() {
  if (!isScrollable()) return;
  setScrollBarShown(true);
}

void ScrollView::onDelayed() {
  // 延时到期：鼠标已经回到视图内就继续显示
  if (mouseInside_ || draggingThumb_) return;
  setScrollBarShown(false);
}

bool ScrollView::hitScrollBar(Point local) const {
  if (!isScrollable()) return false;
  // 命中范围比轨道略宽，靠近边缘也能带出滚动条
  const Rect track = trackRect();
  return !track.isEmpty() && track.inflated(px(style_.margin), 0).contains(local);
}

// ───────────────────────── 绘制 ─────────────────────────

void ScrollView::onDrawForeground(Painter& painter, const Rect& rect) {
  if (!scrollBarVisible()) return;

  // 滚动条只画在视口内，表头之类固定区域不被覆盖
  const Rect viewport = viewportRect().intersect(rect);
  if (viewport.isEmpty()) return;

  painter.save();
  painter.clipRect(viewport);

  const int trackWidth = px(trackHot_ || thumbHot_ ? style_.hoverWidth : style_.width);
  const int radius = style_.rounded ? capRadius(trackWidth) : 0;

  const Rect track = trackRect();
  if (style_.track != kNoColor) {
    painter.fillRoundRect(track, style_.track, radius);
  }

  const Rect thumb = thumbRect();
  if (!thumb.isEmpty()) {
    const int thumbWidth =
        px(trackHot_ || thumbHot_ ? style_.hoverThumbWidth : style_.thumbWidth);
    const int thumbRadius = style_.rounded ? capRadius(thumbWidth) : 0;
    painter.fillRoundRect(thumb, (thumbHot_ || draggingThumb_) ? style_.thumbHover : style_.thumb,
                          thumbRadius);
  }

  painter.restore();
}

// ───────────────────────── 交互 ─────────────────────────

bool ScrollView::hitThumb(Point local) const {
  const Rect thumb = thumbRect();
  return !thumb.isEmpty() && thumb.inflated(px(2), 0).contains(local);
}

bool ScrollView::hitTrack(Point local) const {
  const Rect track = trackRect();
  return !track.isEmpty() && track.contains(local);
}

bool ScrollView::onMouseDown(const MouseEvent& e) {
  if (!isScrollable()) return false;

  if (hitThumb(e.position)) {
    draggingThumb_ = true;
    // 记住按下点相对滑块顶端的偏移，拖动时滑块不会跳到鼠标中心
    dragGrab_ = e.position.y - thumbRect().top;
    captureMouse();
    invalidate();
    return true;
  }
  // 点轨道空白：向点击方向跳一页
  if (hitTrack(e.position)) {
    const Rect thumb = thumbRect();
    const int page = (std::max)(1, viewportHeight());
    scrollBy(e.position.y < thumb.top ? -page : page);
    return true;
  }
  return false;
}

void ScrollView::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!draggingThumb_) return;
  draggingThumb_ = false;
  releaseMouse();
  // 松手后如果鼠标已经不在滚动条上，重新开始隐藏倒计时
  if (!thumbHot_ && !trackHot_) postDelayed(style_.hideDelayMs);
  invalidate();
}

void ScrollView::onMouseMove(const MouseEvent& e) {
  if (draggingThumb_) {
    setScrollOffset(scrollForThumbTop(e.position.y - dragGrab_));
    return;
  }

  // 鼠标在视图内就保持显示；倒计时只由"离开"启动，避免每次移动都重设系统定时器
  if (!mouseInside_) {
    mouseInside_ = true;
    postDelayed(0);
    revealScrollBar();
  }

  const bool hot = hitThumb(e.position);
  const bool onBar = hitScrollBar(e.position);
  if (hot != thumbHot_ || onBar != trackHot_) {
    thumbHot_ = hot;
    trackHot_ = onBar;
    // 宽度随悬停状态变化；新旧两档都要刷，否则留下的那一截画不掉
    invalidateBarArea();
  }
}

void ScrollView::onMouseLeave() {
  mouseInside_ = false;
  const bool changed = thumbHot_ || trackHot_;
  thumbHot_ = false;
  trackHot_ = false;
  if (changed) invalidateBarArea();
  // 离开视图后延时隐藏，而不是立刻消失
  if (style_.hideDelayMs > 0) {
    postDelayed(style_.hideDelayMs);
  } else {
    setScrollBarShown(false);
  }
}

bool ScrollView::onMouseWheel(const MouseEvent& e) {
  if (!isScrollable() || e.wheelDelta == 0) return false;
  // 滚动时也把滚动条带出来，方便看清位置
  revealScrollBar();
  postDelayed(style_.hideDelayMs);
  // 一格 120，向后滚为正（内容上移）
  scrollBy(-e.wheelDelta * px(wheelStep()) / 120);
  return true;
}

}  // namespace AiUI
