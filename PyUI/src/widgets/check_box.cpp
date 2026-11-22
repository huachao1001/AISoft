#include "pyui/widgets/check_box.h"

#include "pyui/text_renderer.h"

#include <algorithm>

namespace pyui {

void CheckBox::setLabel(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  requestLayout();
  invalidate();
}

void CheckBox::setStateInternal(CheckState state) {
  if (state_ == state) return;
  state_ = state;
  invalidate();
  if (onToggle_) onToggle_(state_);
}

void CheckBox::setState(CheckState state) {
  // 关掉三态时不允许停在「部分选」：那是一个没有对应点击路径的中间态，
  // 用户点一下会跳到已选，看起来像漏了一次点击。
  if (!tristate_ && state == CheckState::kIndeterminate) state = CheckState::kChecked;
  setStateInternal(state);
}

CheckState CheckBox::advanceState() const {
  if (!tristate_) return state_ == CheckState::kChecked ? CheckState::kUnchecked
                                                        : CheckState::kChecked;
  // 三态按「部分选 → 已选 → 未选」循环：树形勾选里部分选通常意味着
  // 用户想先看全选效果，所以下一步是全选而不是清空。
  switch (state_) {
    case CheckState::kIndeterminate:
      return CheckState::kChecked;
    case CheckState::kChecked:
      return CheckState::kUnchecked;
    default:
      return CheckState::kChecked;
  }
}

void CheckBox::dispatchClick() {
  if (!isEffectivelyEnabled()) return;
  setStateInternal(advanceState());
  View::dispatchClick();
}

namespace {

// 方框在整行里的位置：垂直居中、靠左，与文字之间留固定间距
Rect boxRect(const Rect& rect, int boxSize) {
  const int top = rect.top + (rect.height() - boxSize) / 2;
  return Rect{rect.left, top, rect.left + boxSize, top + boxSize};
}

}  // namespace

Size CheckBox::onMeasure(const Size& available) {
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

void CheckBox::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const bool enabled = isEffectivelyEnabled();

  const Color ink = enabled ? t.text : t.textDisabled;
  const Color boxBg = t.pane;
  const Color hoverBg = t.hover;
  const Color accent = t.accent;
  // 未选中的描边要比输入框的更重一档：复选框只有 18pt 见方，
  // 用输入框那支浅灰在浅底上会糊成一片虚影。
  const Color idleBorder = enabled ? blendColor(t.controlBorder, t.text, 0.38f) : edgeOn(boxBg, 8);
  const Color checkInk = t.accentInk;

  const int box = px(kSizeCheckBox);
  const Rect square = boxRect(rect, box);
  const int radius = px(kRadiusControl);

  const bool checked = state_ == CheckState::kChecked;
  const bool partial = state_ == CheckState::kIndeterminate;

  painter.fillRoundRect(square, checked || partial ? accent : (isHot() ? hoverBg : boxBg), radius);
  if (!checked && !partial) painter.strokeRoundRect(square, idleBorder, radius);

  if (checked) {
    const Rect mark = square.inset(square.width() / 5, square.height() / 5, square.width() / 5,
                                   square.height() / 5);
    painter.drawIcon(IconId::kCheck, mark, checkInk);
  } else if (partial) {
    // 部分选画一条横线而不是勾：勾代表「全部」，半勾会让人以为勾没画完
    const int insetX = square.width() * 3 / 10;
    const int midY = square.top + square.height() / 2;
    painter.fillRect(Rect{square.left + insetX, midY - px(1), square.right - insetX, midY + px(1)},
                     checkInk);
  }

  if (label_.empty()) return;

  const Font font = uiFont(9.0f, false, false, dpi());
  const Rect textRect{square.right + px(8), rect.top, rect.right, rect.bottom};
  if (textRect.width() <= 0) return;
  painter.drawText(textRect, label_, ink, font, TextAlign::kLeft);
}

bool CheckBox::onKeyDown(const Key& key, const Modifiers& mods) {
  (void)mods;
  // 与系统复选框一致：空格切换，回车不触发
  if (key != Key::kSpace) return false;
  if (!isEffectivelyEnabled()) return true;
  setStateInternal(advanceState());
  View::dispatchClick();
  return true;
}

}  // namespace pyui
