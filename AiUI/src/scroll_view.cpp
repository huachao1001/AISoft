// ScrollView 实现：滚动位置管理 + 自绘滚动条
#include "AiUI/scroll_view.h"

namespace AiUI {

void ScrollView::setScrollBarStyle(const ScrollBarStyle& style) {
  style_ = style;
  invalidate();
}

int ScrollView::scrollBarReserve() const {
  if (!scrollBarVisible()) return 0;
  return px(style_.margin + style_.width);
}

bool ScrollView::scrollBarVisible() const { return maxScroll() > 0; }

Rect ScrollView::viewportRect() const {
  const Rect rc = clientRect();
  const int top = viewportTop();
  return Rect{rc.left, rc.top + top, rc.right, rc.bottom};
}

Rect ScrollView::trackRect() const {
  const Rect rc = viewportRect();
  const int margin = px(style_.margin);
  const int width = px(style_.width);
  return Rect{rc.right - margin - width, rc.top, rc.right - margin, rc.bottom};
}

Rect ScrollView::thumbRect() const {
  const Rect track = trackRect();
  if (!scrollBarVisible() || track.isEmpty()) return Rect{};

  const int centerX = track.center().x;
  const int thumbWidth = px(style_.thumbWidth);
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
}

// ───────────────────────── 绘制 ─────────────────────────

void ScrollView::onDrawForeground(Painter& painter, const Rect& rect) {
  if (!scrollBarVisible()) return;

  // 滚动条只画在视口内，表头之类固定区域不被覆盖
  const Rect viewport = viewportRect().intersect(rect);
  if (viewport.isEmpty()) return;

  painter.save();
  painter.clipRect(viewport);

  const Rect track = trackRect();
  if (style_.track != kNoColor) {
    painter.fillRoundRect(track, style_.track, px(style_.radius));
  }

  const Rect thumb = thumbRect();
  if (!thumb.isEmpty()) {
    painter.fillRoundRect(thumb, (thumbHot_ || draggingThumb_) ? style_.thumbHover : style_.thumb,
                          px(style_.radius));
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
  if (!scrollBarVisible()) return false;

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
  invalidate();
}

void ScrollView::onMouseMove(const MouseEvent& e) {
  if (draggingThumb_) {
    setScrollOffset(scrollForThumbTop(e.position.y - dragGrab_));
    return;
  }

  const bool hot = hitThumb(e.position);
  if (hot != thumbHot_) {
    thumbHot_ = hot;
    invalidate();
  }
}

void ScrollView::onMouseLeave() {
  if (!thumbHot_) return;
  thumbHot_ = false;
  invalidate();
}

bool ScrollView::onMouseWheel(const MouseEvent& e) {
  if (!scrollBarVisible() || e.wheelDelta == 0) return false;
  // 一格 120，向后滚为正（内容上移）
  scrollBy(-e.wheelDelta * px(wheelStep()) / 120);
  return true;
}

}  // namespace AiUI
