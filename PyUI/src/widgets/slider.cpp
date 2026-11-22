#include "pyui/widgets/slider.h"

#include <algorithm>
#include <cmath>

namespace pyui {
namespace {

// 拖到两端时滑块中心不能越出轨道：轨道两端各让出滑块的半径，
// 否则滑块有一半压在视图外，看起来像被裁掉了。
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ── 范围与值 ──

void Slider::setRange(double minimum, double maximum) {
  // 调用方把 min/max 写反是常见笔误；反过来而不是报错，
  // 因为两个端点都是「合法取值」，没有哪种顺序更有意义。
  if (maximum < minimum) std::swap(minimum, maximum);
  minimum_ = minimum;
  maximum_ = maximum;

  // 范围变了旧值可能越界：先夹一次再量化，否则滑块会停在一个
  // 无法用键盘到达的位置。
  const double clamped = std::max(minimum_, std::min(maximum_, value_));
  quantize();
  if (clamped != value_) {
    value_ = clamped;
    quantize();
    invalidate();
    notifyChanged();
  } else {
    invalidate();
  }
}

void Slider::setValue(double value) {
  const double clamped = std::max(minimum_, std::min(maximum_, value));
  value_ = clamped;
  quantize();
  invalidate();
}

void Slider::quantize() {
  // step 为 0 表示连续：不要偷偷给一个默认步长，那会让「1..100 想要
  // 小数」的场景被强行取整。
  if (step_ <= 0.0) return;
  const double span = maximum_ - minimum_;
  if (span <= 0.0) {
    value_ = minimum_;
    return;
  }
  // 相对最小值的偏移量化，再夹回端点：直接 value/step 取整会在
  // minimum 不是 step 整数倍时把最小值本身挪走。
  const double offset = (value_ - minimum_) / step_;
  const double snapped = minimum_ + std::round(offset) * step_;
  value_ = std::max(minimum_, std::min(maximum_, snapped));
}

void Slider::setEnabled(bool enabled) {
  if (isEnabled() == enabled) return;
  View::setEnabled(enabled);
  if (!enabled) {
    dragging_ = false;
    clearVisualState();
  }
  invalidate();
}

// ── 几何 ──

int Slider::thumbRadius() const { return px(kSizeThumb) / 2; }

Rect Slider::trackRect(const Rect& rect) const {
  const int radius = thumbRadius();
  const int thickness = px(kSizeTrackH);
  const int top = rect.top + (rect.height() - thickness) / 2;
  // 左右各内缩一个滑块半径：滑块的行程区间而不是轨道的可见区间
  const int left = rect.left + radius;
  const int right = rect.right - radius;
  if (right < left) {
    // 视图比滑块还窄：退化成一条位于中心的零宽轨道，
    // 保证 valueToX 仍能返回一个有效坐标而不是负数区间。
    const int mid = rect.left + rect.width() / 2;
    return Rect{mid, top, mid, top + thickness};
  }
  return Rect{left, top, right, top + thickness};
}

int Slider::valueToX(double value, const Rect& rect) const {
  const Rect track = trackRect(rect);
  const double span = maximum_ - minimum_;
  if (span <= 0.0) return track.left;
  const double ratio = (value - minimum_) / span;
  return track.left + static_cast<int>(std::round(ratio * static_cast<double>(track.width())));
}

double Slider::xToValue(int x, const Rect& rect) const {
  const Rect track = trackRect(rect);
  if (track.width() <= 0) return minimum_;
  const int clampedX = clampInt(x, track.left, track.right);
  const double ratio = static_cast<double>(clampedX - track.left) / static_cast<double>(track.width());
  return minimum_ + ratio * (maximum_ - minimum_);
}

bool Slider::updateFromPointer(int x, const Rect& rect) {
  const double previous = value_;
  value_ = xToValue(x, rect);
  quantize();
  if (value_ == previous) return false;
  invalidate();
  notifyChanged();
  return true;
}

void Slider::notifyChanged() {
  if (onChange_) onChange_(value_);
}

void Slider::notifyCommitted() {
  if (onCommit_) onCommit_(value_);
}

// ── 测量与绘制 ──

Size Slider::onMeasure(const Size& available) {
  // 高度取滑块与轨道中较大者，再补几个像素给描边，避免描边贴边被裁
  const int height = std::max(px(kSizeThumb), px(kSizeTrackH)) + px(2);
  int width = available.width > 0 ? available.width : px(120);
  if (available.height > 0) return Size{width, std::min(height, available.height)};
  return Size{width, height};
}

void Slider::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const bool enabled = isEffectivelyEnabled();
  const bool focused = isFocused();

  const Color idleTrack = enabled ? t.trackBg : t.btnDisBg;
  // 禁用时填充也要褪掉：只褪轨道会让「已经走过的部分」看起来仍然可用
  const Color fillColor = enabled ? t.trackFill : blendColor(t.trackFill, t.pane, 0.7f);
  const Color thumbFill = enabled ? t.thumb : t.btnDisBg;
  const Color thumbBorder = enabled ? t.thumbBorder : edgeOn(t.btnDisBg, 12);

  const Rect track = trackRect(rect);
  const int trackHeight = px(kSizeTrackH);
  const int trackRadius = trackHeight / 2;

  painter.fillRoundRect(Rect{track.left, track.top, track.right, track.bottom}, idleTrack,
                        trackRadius);

  const int thumbX = valueToX(value_, rect);
  const int filledWidth = thumbX - track.left;
  if (filledWidth > 0) {
    // 已走过的部分画成同样圆角的条：左端与轨道左端重合，右端被滑块压住
    painter.fillRoundRect(Rect{track.left, track.top, thumbX, track.bottom}, fillColor,
                          trackRadius);
  }

  const int radius = thumbRadius();
  const Rect thumb{thumbX - radius, rect.top + (rect.height() - radius * 2) / 2,
                   thumbX + radius, rect.top + (rect.height() + radius * 2) / 2};

  painter.fillRoundRect(thumb, thumbFill, radius);
  // 聚焦时的描边换成强调色：滑块没有文字，不加这层就看不出键盘焦点在哪
  painter.strokeRoundRect(thumb, focused ? t.inputFocus : thumbBorder, radius, std::max(1, px(1)));

  // 拖动中给滑块加一圈淡光晕，让「正在拖」与「只是悬停」区分开
  if (dragging_ && enabled) {
    const Rect halo = thumb.inflated(px(3), px(3));
    painter.strokeRoundRect(halo, blendColor(t.accent, t.pane, 0.65f), halo.height() / 2,
                            std::max(1, px(1)));
  }
}

// ── 鼠标 ──

bool Slider::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  if (!isEffectivelyEnabled()) return false;

  setFocus();
  dragging_ = true;

  // 点到轨道任意位置都直接跳过去，而不是只允许抓滑块：
  // 这是滑块的通用习惯，也省掉「必须精确点中 18px 圆」的挫败感。
  updateFromPointer(e.position.x, clientRect());
  invalidate();
  return true;
}

void Slider::onMouseMove(const MouseEvent& e) {
  if (!dragging_) return;
  updateFromPointer(e.position.x, clientRect());
}

void Slider::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (!dragging_) return;
  dragging_ = false;
  invalidate();
  // 松手才算一次提交：拖动过程中已经发过很多次 change，重复发 commit
  // 会让「松手才保存」的调用方保存几十遍。
  notifyCommitted();
}

void Slider::onMouseLeave() {
  // 离开视图不等于松手：继续拖到外面还能拉回来，符合滑块习惯。
  // 只有真正松开时（onMouseUp，或外部 dispatchMouseLeave）才结束。
  if (!dragging_) clearVisualState();
}

// ── 键盘 ──

bool Slider::onKeyDown(const Key& key, const Modifiers& mods) {
  if (!isEffectivelyEnabled()) return false;

  // 键盘步长：没有显式 step 时按范围的 1/20 走，保证任何范围都
  // 能在 20 次按键内走完，而不是走不动（范围大）或一格跳过一半（范围小）。
  const double span = maximum_ - minimum_;
  double unit = step_ > 0.0 ? step_ : (span > 0.0 ? span / 20.0 : 1.0);
  if (mods.shift) unit *= 10.0;

  const double before = value_;
  switch (key) {
    case Key::kLeft:
    case Key::kDown:
      value_ = std::max(minimum_, value_ - unit);
      break;
    case Key::kRight:
    case Key::kUp:
      value_ = std::min(maximum_, value_ + unit);
      break;
    case Key::kHome:
      value_ = minimum_;
      break;
    case Key::kEnd:
      value_ = maximum_;
      break;
    case Key::kPageUp:
      value_ = std::min(maximum_, value_ + (span > 0.0 ? span / 10.0 : 1.0));
      break;
    case Key::kPageDown:
      value_ = std::max(minimum_, value_ - (span > 0.0 ? span / 10.0 : 1.0));
      break;
    default:
      return false;
  }

  quantize();
  if (value_ == before) return true;

  invalidate();
  notifyChanged();
  notifyCommitted();
  return true;
}

}  // namespace pyui
