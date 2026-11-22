// Slider 实现
#include "AiUI/widgets/slider.h"

#include "AiUI/painter.h"

#include <algorithm>
#include <cmath>

namespace AiUI {
namespace {

// 键盘调节步长：1% 太细，按十次才挪一格；5% 在质量、透明度这些参数上
// 刚好是「按一下看得出变化」的粒度
constexpr float kKeyStep = 0.05f;

}  // namespace

void Slider::setSpec(const SliderSpec& spec) {
  spec_ = spec;
  requestLayout();
}

void Slider::setValue(float value, bool notify) {
  applyValue(value, notify);
}

void Slider::applyValue(float value, bool notify) {
  const float clamped = (std::max)(0.0f, (std::min)(1.0f, value));
  // 拖到同一位置时不回调：onChange 里常常要重算整张预览图，
  // 每个静止的 mouse move 都触发一次是白烧 CPU
  if (clamped == value_) return;
  value_ = clamped;
  invalidate();
  if (notify && onChange_) onChange_(value_);
}

Size Slider::onMeasure(const Size& available) {
  return Size{available.width, spec_.height * dpi() / 96};
}

// 圆心可移动的区间是 [半径, 宽-半径]：滑块本身有直径，端点要往里让
int Slider::thumbCenterX(const Rect& rect) const {
  const int r = spec_.thumbRadius * dpi() / 96;
  const int left = rect.left + r;
  const int right = rect.right - r;
  if (right <= left) return rect.left + rect.width() / 2;
  return left + static_cast<int>(std::lround(static_cast<float>(right - left) * value_));
}

void Slider::valueFromX(int x, const Rect& rect) {
  const int r = spec_.thumbRadius * dpi() / 96;
  const int left = rect.left + r;
  const int right = rect.right - r;
  if (right <= left) return;
  const float t = static_cast<float>(x - left) / static_cast<float>(right - left);
  applyValue(t, true);
}

void Slider::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const int d = dpi();
  const Theme& t = theme();
  const Color trackColor = spec_.track != kNoColor ? spec_.track : t.lineSoft;
  const Color fillColor = spec_.fill != kNoColor ? spec_.fill : t.link;
  const Color thumbColor = spec_.thumb != kNoColor ? spec_.thumb : t.link;

  const int trackH = spec_.trackHeight * d / 96;
  const int radius = spec_.trackRadius * d / 96;
  const int centerY = rect.top + rect.height() / 2;

  // 轨道只占控件中部一条：控件整体高是为了给滑块留出上下活动余地
  const Rect track{rect.left, centerY - trackH / 2, rect.right, centerY - trackH / 2 + trackH};
  painter.fillRoundRect(track, trackColor, radius);

  const int thumbX = thumbCenterX(rect);
  const int filledW = thumbX - rect.left;
  if (filledW > 0) {
    painter.fillRoundRect(Rect{rect.left, track.top, rect.left + filledW, track.bottom},
                          fillColor, radius);
  }

  // 滑块用圆角矩形画圆：painter 没有 fillCircle，半径取半边即可
  const int r = spec_.thumbRadius * d / 96;
  const Color ring = hovered_ || dragging_ ? t.accentHover : t.line;
  painter.fillRoundRect(Rect{thumbX - r, centerY - r, thumbX + r, centerY + r}, ring, r);
  const int inner = (std::max)(1, r - d / 96);
  painter.fillRoundRect(Rect{thumbX - inner, centerY - inner, thumbX + inner, centerY + inner},
                        thumbColor, inner);
}

bool Slider::onMouseDown(const MouseEvent& e) {
  if (!isEnabled() || e.button != MouseButton::kLeft) return false;
  dragging_ = true;
  captureMouse();
  // 不换光标：控件只有 28 高，指针在抓手、握拳、箭头之间来回跳反而干扰，
  // 拖出边界后残留的握拳形状还会让人以为卡住了
  // 按下即定位到点击处：用户点轨道中间某点，期待直接跳过去而不是从这里开始拖
  valueFromX(e.position.x, clientRect());
  invalidate();
  return true;
}

void Slider::onMouseMove(const MouseEvent& e) {
  if (dragging_) {
    valueFromX(e.position.x, clientRect());
    return;
  }
  if (!hovered_) {
    hovered_ = true;
    invalidate();
  }
}

void Slider::onMouseUp(const MouseEvent& e) {
  if (!dragging_) return;
  dragging_ = false;
  releaseMouse();
  // 松手时可能已经拖到控件外面了：悬停态要按落点重判，
  // 否则拇指会一直亮着悬停环，下次移进来也不会重新触发
  hovered_ = clientRect().contains(e.position);
  invalidate();
  if (onRelease_) onRelease_(value_);
}

void Slider::onMouseLeave() {
  if (dragging_ || !hovered_) return;
  hovered_ = false;
  invalidate();
}

bool Slider::onKeyDown(const Key& key, const Modifiers& modifiers) {
  if (!isEnabled()) return false;
  // 方向键微调，PageUp/Down 大步：键盘用户也需要能精确落在一个值上
  const float step = modifiers.shift ? kKeyStep * 0.2f : kKeyStep;
  if (key == Key::kLeft || key == Key::kDown) {
    applyValue(value_ - step, true);
    if (onRelease_) onRelease_(value_);
    return true;
  }
  if (key == Key::kRight || key == Key::kUp) {
    applyValue(value_ + step, true);
    if (onRelease_) onRelease_(value_);
    return true;
  }
  if (key == Key::kHome) {
    applyValue(0.0f, true);
    if (onRelease_) onRelease_(value_);
    return true;
  }
  if (key == Key::kEnd) {
    applyValue(1.0f, true);
    if (onRelease_) onRelease_(value_);
    return true;
  }
  return false;
}

}  // namespace AiUI
