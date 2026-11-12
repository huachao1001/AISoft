// CheckBox / RadioButton 实现
#include "AiUI/widgets/option_view.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

namespace AiUI {

void OptionView::setSpec(const OptionSpec& spec) {
  spec_ = spec;
  invalidate();
}

void OptionView::setLabel(const std::wstring& label) {
  spec_.label = label;
  requestLayout();
}

void OptionView::setChecked(bool checked, bool notify) {
  if (spec_.checked == checked) return;
  spec_.checked = checked;
  invalidate();
  if (notify && onChange_) onChange_(checked);
}

void OptionView::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Color ink = spec_.ink != kNoColor ? spec_.ink
                                          : (isEnabled() ? kColorText : kColorTextFaint);
  const Color border = isEnabled() ? spec_.boxBorder : kColorBtnDisBorder;
  const Color accent = isEnabled() ? spec_.accent : kColorBtnDisBorder;

  const Rect box = boxRect(rect);
  const int radius = spec_.radius * d / 96;

  if (isRadio()) {
    // 圆形：外圈描边，选中时实心点
    painter.fillRoundRect(box, accent, box.height() / 2);
    const Rect inner = box.inset(1 * d / 96, 1 * d / 96, 1 * d / 96, 1 * d / 96);
    if (spec_.checked) {
      painter.fillRoundRect(inner, accent, inner.height() / 2);
    } else {
      // 未选中时挖白心，避免外圈看起来像实的
      painter.fillRoundRect(inner, hot_ ? kColorHover : spec_.boxBackground,
                            inner.height() / 2);
    }
  } else {
    painter.fillRoundRect(box, spec_.checked ? accent : (hot_ ? kColorHover : spec_.boxBackground),
                          radius);
    if (!spec_.checked) painter.strokeRoundRect(box, border, radius);
    if (spec_.checked) {
      const Rect mark = box.inset(box.width() / 5, box.height() / 5, box.width() / 5,
                                  box.height() / 5);
      painter.drawIcon(IconId::kCheck, mark, kColorPane);
    }
  }

  if (!spec_.label.empty()) {
    const int gap = spec_.gap * d / 96;
    const Rect textRect{box.right + gap, rect.top, rect.right, rect.bottom};
    painter.drawText(textRect, spec_.label, ink, fonts.ui, TextAlign::kLeft);
  }
}

Rect OptionView::boxRect(const Rect& rect) const {
  const int d = dpi();
  const int size = spec_.boxSize * d / 96;
  const int top = rect.top + (rect.height() - size) / 2;
  return Rect{rect.left, top, rect.left + size, top + size};
}

Size OptionView::onMeasure(const Size& available) {
  (void)available;
  const int d = dpi();
  const int box = spec_.boxSize * d / 96;
  int textWidth = 0;
  if (!spec_.label.empty()) textWidth = platform()->measureText(spec_.label, fonts.ui);
  const int gap = spec_.label.empty() ? 0 : spec_.gap * d / 96;
  return Size{box + gap + textWidth, box + 6 * d / 96};
}

bool OptionView::onMouseDown(const MouseEvent& e) {
  if (!isEnabled() || e.button != MouseButton::kLeft) return false;
  pressed_ = true;
  setFocus();
  captureMouse();
  invalidate();
  return true;
}

void OptionView::onMouseUp(const MouseEvent& e) {
  if (!pressed_) return;
  pressed_ = false;
  const bool inside = clientRect().contains(e.position);
  invalidate();
  if (!inside || !isEnabled()) return;

  // 复选框点已选中的会取消；单选框点已选中的保持选中
  if (!isRadio() || !spec_.checked) setChecked(!spec_.checked);
}

void OptionView::onMouseMove(const MouseEvent& e) {
  (void)e;
  if (!isEnabled()) return;
  if (!hot_) {
    hot_ = true;
    invalidate();
  }
  setCursor(CursorShape::kHand);
}

void OptionView::onMouseLeave() {
  if (hot_) {
    hot_ = false;
    invalidate();
  }
  setCursor(CursorShape::kArrow);
}

bool OptionView::onKeyDown(const Key& key, const Modifiers& mods) {
  if (!isEnabled()) return false;
  (void)mods;
  // 键盘与原生控件一致：空格切换
  if (key == Key::kSpace) {
    if (!isRadio() || !spec_.checked) setChecked(!spec_.checked);
    return true;
  }
  return false;
}

}  // namespace AiUI
