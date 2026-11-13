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
  const Theme& t = theme();

  const Color ink =
      spec_.ink != kNoColor ? spec_.ink : (isEnabled() ? t.text : t.textFaint);
  const Color boxBg = spec_.boxBackground != kNoColor ? spec_.boxBackground : t.pane;
  const Color hoverBg = spec_.hoverBackground != kNoColor ? spec_.hoverBackground : t.hover;
  // 强调色只用于选中态；未选中的圈/框用主题的统一描边，
  // 这样它和输入框、下拉框是同一支灰，不会一个蓝一个灰
  const Color accent = spec_.accent != kNoColor ? spec_.accent : t.inputFocus;
  // 未选中的描边要比输入框的更重一档：输入框是整条长矩形，浅浅一条也看得出来，
  // 而单选框只有 16pt 的小圆圈，用同一支灰在浅底上会糊成一团虚影。
  const Color idleBorder = blendColor(t.controlBorder, t.text, 0.38);
  const Color border = spec_.boxBorder != kNoColor  ? spec_.boxBorder
                       : !isEnabled()               ? edgeOn(boxBg, 8)
                                                    : idleBorder;
  const Color checkInk = spec_.checkInk != kNoColor ? spec_.checkInk : t.pane;

  const Rect box = boxRect(rect);
  const int radius = spec_.radius * d / 96;

  if (isRadio()) {
    // 外圈只描边，不铺满：选中态是"实心点 + 空心环"，
    // 填满整个圆会糊成一坨，分不清选中边界
    // 小圆圈的 1px 描边在 150% 屏上要跟着放大一档，否则曲线段的抗锯齿会把它
    // 摊成半透明，看着像蒙了一层雾
    const int ring = (std::max)(1, (1 * d + 48) / 96);
    const int outerRadius = box.height() / 2;
    if (spec_.checked) {
      // 先铺底色再描边，避免半透明描边叠在别的颜色上
      painter.fillRoundRect(box, boxBg, outerRadius);
    } else if (hot_) {
      painter.fillRoundRect(box, hoverBg, outerRadius);
    }
    // 未选中的圈用统一描边灰，选中的圈用强调色
    painter.strokeRoundRect(box, spec_.checked ? accent : border, outerRadius, ring);

    if (spec_.checked) {
      // 内缩到约半径的一半再填实心，与外圈之间留出一圈空隙
      const int dotInset = (std::max)(ring + 1, box.height() * 29 / 100);
      const Rect dot = box.inset(dotInset, dotInset, dotInset, dotInset);
      if (dot.width() > 0 && dot.height() > 0) {
        painter.fillRoundRect(dot, accent, dot.height() / 2);
      }
    }
  } else {
    painter.fillRoundRect(box, spec_.checked ? accent : (hot_ ? hoverBg : boxBg), radius);
    if (!spec_.checked) painter.strokeRoundRect(box, border, radius);
    if (spec_.checked) {
      const Rect mark = box.inset(box.width() / 5, box.height() / 5, box.width() / 5,
                                  box.height() / 5);
      painter.drawIcon(IconId::kCheck, mark, checkInk);
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
