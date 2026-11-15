// ChoiceCard / ChoiceCardGroup 实现
#include "AiUI/widgets/choice_card.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"

#include <algorithm>

namespace AiUI {

// ───────────────────────── ChoiceCard ─────────────────────────

void ChoiceCard::setSpec(const ChoiceCardSpec& spec) {
  spec_ = spec;
  requestLayout();
  invalidate();
}

void ChoiceCard::setTitle(const std::wstring& title) {
  spec_.title = title;
  requestLayout();
  invalidate();
}

void ChoiceCard::setDetail(const std::wstring& detail) {
  spec_.detail = detail;
  requestLayout();
  invalidate();
}

void ChoiceCard::setChecked(bool checked, bool notify) {
  if (spec_.checked == checked) return;
  spec_.checked = checked;
  invalidate();
  if (notify && onChange_) onChange_(checked);
}

void ChoiceCard::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Theme& t = theme();
  const int radius = spec_.radius * d / 96;

  const bool active = spec_.checked;
  const bool enabled = isEnabled();

  // 底色分三档：选中、悬停、常态。选中优先，悬停只是「鼠标在上面」的提示。
  const Color baseBg =
      spec_.background != kNoColor ? spec_.background : t.pane;
  const Color checkedBg =
      spec_.checkedBackground != kNoColor ? spec_.checkedBackground : t.currentBg;
  const Color hoverBg = spec_.hoverBackground != kNoColor ? spec_.hoverBackground : t.rowHover;

  Color bg = active ? checkedBg : (hot_ && enabled ? hoverBg : baseBg);
  if (!enabled) bg = blendColor(baseBg, t.pane, 0.35f);

  const Color idleBorder =
      spec_.border != kNoColor ? spec_.border : static_cast<Color>(edgeOn(bg, 22));
  const Color activeBorder =
      spec_.checkedBorder != kNoColor ? spec_.checkedBorder : t.accent;
  const Color border = active ? activeBorder : idleBorder;

  // 选中时整块铺底 + 强调色描边，圆角与常态一致，避免选中后形状跳变
  painter.fillRoundRect(rect, bg, radius);
  // 描边要跟着 DPI 放大一档：150% 屏上 1px 圆弧会被抗锯齿摊淡
  const int stroke = active ? (std::max)(1, (2 * d + 48) / 96)
                            : (std::max)(1, (1 * d + 48) / 96);
  painter.strokeRoundRect(rect, border, radius, stroke);

  const int padX = spec_.padX * d / 96;
  const int padY = spec_.padY * d / 96;
  const int iconSize = spec_.iconSize * d / 96;
  const int iconGap = spec_.iconGap * d / 96;

  // 图标在左、文字在右：纵向居中对齐，两行文字之间留 titleGap
  const int iconTop = rect.top + (rect.height() - iconSize) / 2;
  const Color iconInk = active ? (spec_.checkedIconInk != kNoColor ? spec_.checkedIconInk : t.accent)
                              : (spec_.iconInk != kNoColor ? spec_.iconInk : t.iconMuted);
  if (spec_.icon != IconId::kNone) {
    painter.drawIcon(spec_.icon, Rect{rect.left + padX, iconTop, rect.left + padX + iconSize,
                                      iconTop + iconSize},
                     enabled ? iconInk : t.textFaint);
  }

  const int textLeft = rect.left + padX + (spec_.icon == IconId::kNone ? 0 : iconSize + iconGap);
  // 给右侧的选中指示点留出位置，否则长标题会压到它上面
  const int indicatorSpace = spec_.showIndicator ? 20 * d / 96 : 0;
  const int textRight = rect.right - padX - indicatorSpace;

  const Color titleInk = spec_.ink != kNoColor ? spec_.ink : (enabled ? t.text : t.textFaint);
  const Color detailInk =
      spec_.detailInk != kNoColor ? spec_.detailInk : (enabled ? t.textMuted : t.textFaint);

  const int titleH = fonts.uiBold.pixelHeight();
  const int detailH = fonts.uiSmall.pixelHeight();
  const int gap = spec_.titleGap * d / 96;

  const bool hasDetail = !spec_.detail.empty();
  const int blockH = hasDetail ? titleH + gap + detailH : titleH;
  // 文字块在卡片里垂直居中，和左侧图标同一基准
  int textTop = rect.top + (rect.height() - blockH) / 2;
  if (textTop < rect.top + padY) textTop = rect.top + padY;

  const Rect titleRect{textLeft, textTop, textRight, textTop + titleH};
  if (!spec_.title.empty()) {
    painter.drawTextEllipsis(titleRect, spec_.title, titleInk, fonts.uiBold);
  }
  if (hasDetail) {
    const Rect detailRect{textLeft, textTop + titleH + gap, textRight, textTop + titleH + gap + detailH};
    painter.drawTextEllipsis(detailRect, spec_.detail, detailInk, fonts.uiSmall);
  }

  // 右侧选中指示：单选画圆点，复选画方框勾
  if (spec_.showIndicator) {
    const int box = 18 * d / 96;
    const Rect boxRect{rect.right - padX - box, rect.top + (rect.height() - box) / 2,
                       rect.right - padX, rect.top + (rect.height() - box) / 2 + box};
    const int ring = (std::max)(1, (1 * d + 48) / 96);
    if (spec_.radio) {
      const int outer = boxRect.height() / 2;
      if (active) painter.fillRoundRect(boxRect, checkedBg, outer);
      painter.strokeRoundRect(boxRect, active ? t.accent : idleBorder, outer, ring);
      if (active) {
        const int inset = (std::max)(ring + 1, boxRect.height() * 29 / 100);
        const Rect dot = boxRect.inset(inset, inset, inset, inset);
        if (dot.width() > 0 && dot.height() > 0) {
          painter.fillRoundRect(dot, t.accent, dot.height() / 2);
        }
      }
    } else {
      painter.fillRoundRect(boxRect, active ? t.accent : (hot_ ? hoverBg : baseBg),
                            spec_.radius > 0 ? 4 * d / 96 : 0);
      if (!active) painter.strokeRoundRect(boxRect, idleBorder, 4 * d / 96, ring);
      if (active) {
        const Rect mark = boxRect.inset(boxRect.width() / 5, boxRect.height() / 5,
                                        boxRect.width() / 5, boxRect.height() / 5);
        painter.drawIcon(IconId::kCheck, mark, t.accentInk);
      }
    }
  }
}

Size ChoiceCard::onMeasure(const Size& available) {
  (void)available;
  const int d = dpi();

  const int padX = spec_.padX * d / 96;
  const int padY = spec_.padY * d / 96;
  const int iconSize = spec_.icon == IconId::kNone ? 0 : spec_.iconSize * d / 96;
  const int iconGap = spec_.icon == IconId::kNone ? 0 : spec_.iconGap * d / 96;
  const int indicatorSpace = spec_.showIndicator ? 20 * d / 96 : 0;

  int textWidth = 0;
  if (!spec_.title.empty()) textWidth = platform()->measureText(spec_.title, fonts.uiBold);
  if (!spec_.detail.empty()) {
    textWidth = (std::max)(textWidth, platform()->measureText(spec_.detail, fonts.uiSmall));
  }

  const int titleH = fonts.uiBold.pixelHeight();
  const int detailH = spec_.detail.empty() ? 0 : fonts.uiSmall.pixelHeight();
  const int gap = spec_.detail.empty() ? 0 : spec_.titleGap * d / 96;
  const int textH = titleH + gap + detailH;

  const int contentH = (std::max)(iconSize, textH);
  const int width = padX * 2 + iconSize + iconGap + textWidth + indicatorSpace;
  const int height = contentH + padY * 2;
  return Size{width, height};
}

bool ChoiceCard::onMouseDown(const MouseEvent& e) {
  if (!isEnabled() || e.button != MouseButton::kLeft) return false;
  pressed_ = true;
  setFocus();
  captureMouse();
  invalidate();
  return true;
}

void ChoiceCard::onMouseUp(const MouseEvent& e) {
  if (!pressed_) return;
  pressed_ = false;
  const bool inside = clientRect().contains(e.position);
  invalidate();
  if (!inside || !isEnabled()) return;

  // 单选卡片点自己不该取消自己：取消是组的事，与 RadioButton 同一条规则
  if (!spec_.radio || !spec_.checked) setChecked(!spec_.checked);
}

void ChoiceCard::onMouseMove(const MouseEvent& e) {
  (void)e;
  if (!isEnabled()) return;
  if (!hot_) {
    hot_ = true;
    invalidate();
  }
  setCursor(CursorShape::kHand);
}

void ChoiceCard::onMouseLeave() {
  if (hot_) {
    hot_ = false;
    invalidate();
  }
  setCursor(CursorShape::kArrow);
}

bool ChoiceCard::onKeyDown(const Key& key, const Modifiers& mods) {
  if (!isEnabled()) return false;
  (void)mods;
  if (key == Key::kSpace) {
    if (!spec_.radio || !spec_.checked) setChecked(!spec_.checked);
    return true;
  }
  return false;
}

// ─────────────────────── ChoiceCardGroup ───────────────────────

ChoiceCard* ChoiceCardGroup::cardAt(int index) const {
  if (index < 0 || index >= static_cast<int>(cards_.size())) return nullptr;
  return cards_[index];
}

int ChoiceCardGroup::checkedIndex() const {
  for (size_t i = 0; i < cards_.size(); ++i) {
    if (cards_[i]->isChecked()) return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> ChoiceCardGroup::checkedIndices() const {
  std::vector<int> out;
  for (size_t i = 0; i < cards_.size(); ++i) {
    if (cards_[i]->isChecked()) out.push_back(static_cast<int>(i));
  }
  return out;
}

void ChoiceCardGroup::setCheckedIndex(int index) {
  if (index < 0 || index >= static_cast<int>(cards_.size())) return;
  if (checkedIndex() == index) return;
  syncFrom(index);
}

void ChoiceCardGroup::onChildAdded(View* child) {
  auto* card = dynamic_cast<ChoiceCard*>(child);
  if (!card) return;

  // 卡片默认铺满整组宽度：顶部对齐的两个选项要等宽才像一组。
  // 高度必须收回 wrap：卡片默认的 match_parent 会把它撑成整块可用高度，
  // 两张卡就各占半屏，中间留出的空白比内容还多。
  {
    LayoutParams params = card->layoutParams();
    bool changed = false;
    if (params.height == LayoutParams::kMatchParent) {
      params.height = LayoutParams::kWrapContent;
      changed = true;
    }
    if (changed) card->setLayoutParams(params);
  }

  const int index = static_cast<int>(cards_.size());
  cards_.push_back(card);
  card->setOnChange([this, index](bool checked) {
    if (!radio_) {
      if (onChange_) onChange_(index);
      return;
    }
    if (checked) syncFrom(index);
  });

  // 单选组首次有子项时选第一个，与 RadioGroup 一致：XML 里不必写 checked
  if (radio_ && checkedIndex() < 0 && cards_.size() == 1) {
    card->setChecked(true, false);
  }
}

void ChoiceCardGroup::syncFrom(int index) {
  // 回调里可能再次调用 setCheckedIndex：嵌套时只同步外观
  const bool nested = syncing_;
  syncing_ = true;
  for (size_t i = 0; i < cards_.size(); ++i) {
    cards_[i]->setChecked(static_cast<int>(i) == index, false);
  }
  syncing_ = false;
  if (!nested && onChange_) onChange_(index);
}

Size ChoiceCardGroup::onMeasure(const Size& available) {
  const int d = dpi();
  const int gap = gap_ * d / 96;

  // 纵向排列：宽度取最宽的一张（通常全部铺满），高度是累加
  int width = 0;
  int height = 0;
  for (size_t i = 0; i < cards_.size(); ++i) {
    const Size size = cards_[i]->measure(available);
    width = (std::max)(width, size.width);
    height += size.height;
    if (i + 1 < cards_.size()) height += gap;
  }
  return Size{width, height};
}

void ChoiceCardGroup::onLayout(const Rect& frame) {
  (void)frame;
  // onLayout 收到的是父坐标，排布用局部坐标
  const Rect area = clientRect();
  const int d = dpi();
  const int gap = gap_ * d / 96;

  int y = area.top;
  for (auto* card : cards_) {
    const int height = card->measure(Size{area.width(), area.height()}).height;
    card->layout(Rect{area.left, y, area.right, y + height});
    y += height + gap;
  }
}

}  // namespace AiUI
