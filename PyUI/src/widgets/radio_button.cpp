#include "pyui/widgets/radio_button.h"

#include "pyui/text_renderer.h"

#include <algorithm>

namespace pyui {

// ── RadioGroup ──

void RadioGroup::join(RadioButton* button) {
  if (button == nullptr) return;
  if (button->group_ == this) return;
  if (button->group_ != nullptr) button->group_->leave(button);

  button->group_ = this;
  buttons_.push_back(button);

  // 组内还没有选中项时，第一个声明 checked 的按钮成为选中项；
  // 没有 checked 的就不用管——允许「初始全不选」，由宿主决定何时选。
  if (selected_ == nullptr && button->isChecked()) selected_ = button;
}

void RadioGroup::leave(RadioButton* button) {
  const auto it = std::find(buttons_.begin(), buttons_.end(), button);
  if (it == buttons_.end()) return;
  buttons_.erase(it);
  if (selected_ == button) selected_ = nullptr;
  button->group_ = nullptr;
}

void RadioGroup::releaseAll() {
  // 组先于按钮销毁的情况：把按钮上的组指针清掉，否则按钮析构时会
  // 反向访问已经释放的组。
  for (RadioButton* button : buttons_) button->group_ = nullptr;
  buttons_.clear();
  selected_ = nullptr;
}

void RadioGroup::select(RadioButton* button) {
  if (button != nullptr && button->group_ != this) return;
  if (selected_ == button) return;
  selected_ = button;
  notifyChanged(button);
}

void RadioGroup::notifyChanged(RadioButton* button) {
  // 先把全组外观同步到「只有 button 选中」，再发通知。
  // 顺序反了的话，回调里读 isChecked 会拿到旧状态。
  const auto buttons = buttons_;
  for (RadioButton* item : buttons) {
    item->setCheckedInternal(item == button);
  }
  if (onChange_) onChange_(button);
}

// ── RadioButton ──

RadioButton::~RadioButton() {
  if (group_ != nullptr) group_->leave(this);
}

void RadioButton::setLabel(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  requestLayout();
  invalidate();
}

void RadioButton::setGroup(RadioGroup* group) {
  if (group_ == group) return;
  if (group_ != nullptr) group_->leave(this);
  if (group != nullptr) {
    group->join(this);
  } else {
    group_ = nullptr;
  }
}

void RadioButton::setCheckedInternal(bool checked) {
  if (checked_ == checked) return;
  checked_ = checked;
  invalidate();
}

void RadioButton::setChecked(bool checked) {
  if (checked_ == checked) return;

  if (group_ != nullptr) {
    // 组内选中走 RadioGroup::select：它会同步取消其它项并发一次通知。
    // 直接改 checked_ 会让同组出现两个选中项。
    group_->select(checked ? this : nullptr);
    return;
  }

  setCheckedInternal(checked);
}

void RadioButton::dispatchClick() {
  if (!isEffectivelyEnabled()) return;
  // 单选按钮不接受「点自己取消」：那会让用户处于一个系统里不存在的状态
  if (!checked_) setChecked(true);
  View::dispatchClick();
}

namespace {

Rect circleRect(const Rect& rect, int size) {
  const int top = rect.top + (rect.height() - size) / 2;
  return Rect{rect.left, top, rect.left + size, top + size};
}

}  // namespace

Size RadioButton::onMeasure(const Size& available) {
  const int box = px(kSizeCheckBox);
  const int gap = label_.empty() ? 0 : px(8);
  const Font font = uiFont(9.0f, false, false, dpi());
  const int textWidth = label_.empty() ? 0 : measureTextWidth(label_, font);
  const int lineHeight = textLineHeight(font);

  int width = box + gap + textWidth;
  int height = std::max(box, lineHeight);
  if (available.width > 0) width = std::min(width, available.width);
  if (available.height > 0) height = std::min(height, available.height);
  return Size{width, height};
}

void RadioButton::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const bool enabled = isEffectivelyEnabled();

  const Color ink = enabled ? t.text : t.textDisabled;
  const Color boxBg = t.pane;
  const Color hoverBg = t.hover;
  const Color accent = t.accent;
  const Color idleBorder = enabled ? blendColor(t.controlBorder, t.text, 0.38f) : edgeOn(boxBg, 8);

  const int box = px(kSizeCheckBox);
  const Rect circle = circleRect(rect, box);
  const int outerRadius = box / 2;

  // 小圆圈的 1px 描边在高 DPI 下会摊成半透明，跟着放大一档才看得清
  const int ring = std::max(1, px(1));

  if (checked_) {
    // 先铺底色再描边：半透明描边叠在别的颜色上会发灰
    painter.fillRoundRect(circle, boxBg, outerRadius);
  } else if (isHot()) {
    painter.fillRoundRect(circle, hoverBg, outerRadius);
  }
  painter.strokeRoundRect(circle, checked_ ? accent : idleBorder, outerRadius, ring);

  if (checked_) {
    // 内缩到约半径的一半再填实心，与外圈之间留一圈空隙，
    // 否则实心点会贴到环上，看起来就是一坨
    const int dotInset = std::max(ring + 1, box * 29 / 100);
    const Rect dot = circle.inset(dotInset, dotInset, dotInset, dotInset);
    if (!dot.isEmpty()) painter.fillRoundRect(dot, accent, dot.height() / 2);
  }

  if (label_.empty()) return;

  const Font font = uiFont(9.0f, false, false, dpi());
  const Rect textRect{circle.right + px(8), rect.top, rect.right, rect.bottom};
  if (textRect.width() <= 0) return;
  painter.drawText(textRect, label_, ink, font, TextAlign::kLeft);
}

bool RadioButton::onKeyDown(const Key& key, const Modifiers& mods) {
  (void)mods;
  if (key != Key::kSpace) return false;
  if (!isEffectivelyEnabled()) return true;
  if (!checked_) setChecked(true);
  View::dispatchClick();
  return true;
}

}  // namespace pyui
