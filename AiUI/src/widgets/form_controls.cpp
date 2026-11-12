// RadioGroup / ProgressBar / GroupBox / ComboBox 实现
#include "AiUI/widgets/form_controls.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/view_root.h"
#include "AiUI/widgets/option_view.h"

#include <algorithm>

namespace AiUI {

// ───────────────────────── RadioGroup ─────────────────────────

// 代码里建项：包一层 addView，剩下的交给 onChildAdded
RadioButton* RadioGroup::addItem(const std::wstring& label) {
  auto* item = new RadioButton();
  OptionSpec spec;
  spec.label = label;
  item->setSpec(spec);
  addView(item);
  return item;
}

void RadioGroup::onChildAdded(View* child) {
  auto* radio = dynamic_cast<RadioButton*>(child);
  if (!radio) return;

  // 单选项按自身内容排布：撑满会让每项都占满整组，只看得见第一项
  LayoutParams params = radio->layoutParams();
  bool changed = false;
  if (params.width == LayoutParams::kMatchParent) {
    params.width = LayoutParams::kWrapContent;
    changed = true;
  }
  if (params.height == LayoutParams::kMatchParent) {
    params.height = LayoutParams::kWrapContent;
    changed = true;
  }
  if (changed) radio->setLayoutParams(params);

  const int index = static_cast<int>(items_.size());
  items_.push_back(radio);
  radio->setOnChange([this, index](bool checked) {
    if (checked) syncFrom(index);
  });
  ensureInitialChoice();
}

// 全组都没有选中时选第一个：XML 里不必写 checked，也能得到确定状态
void RadioGroup::ensureInitialChoice() {
  if (initialChoiceDone_ || items_.empty()) return;
  for (auto* item : items_) {
    if (item->isChecked()) {
      initialChoiceDone_ = true;
      return;
    }
  }
  initialChoiceDone_ = true;
  items_.front()->setChecked(true, false);
}

RadioButton* RadioGroup::itemAt(int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) return nullptr;
  return items_[index];
}

int RadioGroup::checkedIndex() const {
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i]->isChecked()) return static_cast<int>(i);
  }
  return -1;
}

void RadioGroup::setCheckedIndex(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return;
  // 已经是这一项就什么都不做：否则 onChange 里再调本函数会无限递归
  if (checkedIndex() == index) return;
  syncFrom(index);
}

void RadioGroup::syncFrom(int index) {
  // 回调里可能再次调用 setCheckedIndex：嵌套时只同步外观，通知留给最外层，
  // 否则回调 → 同步 → 回调 会一直递归下去
  const bool nested = syncing_;
  syncing_ = true;
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i]->setChecked(static_cast<int>(i) == index, false);
  }
  syncing_ = false;
  if (!nested && onChange_) onChange_(index);
}

Size RadioGroup::onMeasure(const Size& available) {
  const int d = dpi();
  const int gap = rowGap_ * d / 96;
  if (orientation_ == Orientation::kHorizontal) {
    int width = 0;
    int height = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
      const Size size = items_[i]->measure(available);
      width += size.width + (i == 0 ? 0 : gap);
      height = std::max(height, size.height);
    }
    return Size{width, height};
  }

  int width = 0;
  int height = 0;
  for (auto* item : items_) {
    const Size size = item->measure(available);
    width = std::max(width, size.width);
    height += size.height;
  }
  if (items_.size() > 1) height += gap * (static_cast<int>(items_.size()) - 1);
  return Size{width, height};
}

void RadioGroup::onLayout(const Rect& frame) {
  (void)frame;
  // 用局部坐标：onLayout 收到的是父坐标
  const Rect area = clientRect();
  const int d = dpi();
  const int gap = rowGap_ * d / 96;

  if (orientation_ == Orientation::kHorizontal) {
    int x = area.left;
    for (auto* item : items_) {
      const Size size = item->measure(Size{area.width(), area.height()});
      item->layout(Rect{x, area.top, x + size.width, area.top + size.height});
      x += size.width + gap;
    }
    return;
  }

  int y = area.top;
  for (auto* item : items_) {
    const int height = item->measure(Size{area.width(), area.height()}).height;
    item->layout(Rect{area.left, y, area.right, y + height});
    y += height + gap;
  }
}

// ───────────────────────── ProgressBar ─────────────────────────

void ProgressBar::setSpec(const ProgressSpec& spec) {
  spec_ = spec;
  requestLayout();
}

void ProgressBar::setProgress(float value) {
  const float clamped = std::max(0.0f, std::min(1.0f, value));
  if (clamped == progress_) return;
  progress_ = clamped;
  invalidate();
}

Size ProgressBar::onMeasure(const Size& available) {
  const int d = dpi();
  return Size{available.width, spec_.height * d / 96};
}

void ProgressBar::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int radius = spec_.radius * d / 96;
  painter.fillRoundRect(rect, spec_.track, radius);

  const int fillWidth = static_cast<int>(rect.width() * progress_);
  if (fillWidth <= 0) return;
  const Rect fill{rect.left, rect.top, rect.left + fillWidth, rect.bottom};
  painter.fillRoundRect(fill, spec_.fill, radius);
}

// ───────────────────────── GroupBox ─────────────────────────

void GroupBox::setTitle(const std::wstring& title) {
  title_ = title;
  requestLayout();
}

// View 的默认测量会返回高度 0，GroupBox 必须自己按内容算高
Size GroupBox::onMeasure(const Size& available) {
  const int d = dpi();
  const int pad = 12 * d / 96;
  const int titleHeight = titleHeight_ * d / 96;

  const Size inner{(std::max)(0, available.width - pad * 2), (std::max)(0, available.height - pad)};
  int contentHeight = 0;
  int contentWidth = 0;
  for (View* child : children_) {
    if (!child->isVisible()) continue;
    const Size size = child->measure(inner);
    contentHeight = (std::max)(contentHeight, size.height);
    contentWidth = (std::max)(contentWidth, size.width);
  }
  return Size{contentWidth + pad * 2, contentHeight + titleHeight + pad};
}

void GroupBox::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int titleHeight = titleHeight_ * d / 96;

  // 边框从标题中线开始，标题压在缺口上；颜色跟着所在容器的底色走
  const Rect frame{rect.left, rect.top + titleHeight / 2, rect.right, rect.bottom};
  painter.strokeRoundRect(frame, edgeOn(resolvedBackground(), 15), 6 * d / 96);

  if (title_.empty()) return;

  const int padX = 6 * d / 96;
  const int textWidth = platform()->measureText(title_, fonts.ui);
  const int textHeight = fonts.ui.pixelHeight();
  const Rect textRect{rect.left + 10 * d / 96, rect.top, rect.left + 10 * d / 96 + textWidth,
                      rect.top + textHeight};
  // 先铺底色再写字，盖掉边框那一段
  painter.fillRect(textRect.inset(-padX, 0, -padX, 0), resolvedBackground());
  painter.drawText(textRect, title_, kColorTextHead, fonts.ui, TextAlign::kLeft);
}

void GroupBox::onLayout(const Rect& frame) {
  (void)frame;
  // 用局部坐标：onLayout 收到的是父坐标，混用会把内容整体推偏
  const int d = dpi();
  const int pad = 12 * d / 96;
  const int titleHeight = titleHeight_ * d / 96;
  const Rect inner = clientRect().inset(pad, titleHeight, pad, pad);
  arrangeChildren(inner);
}

// ───────────────────────── ComboBox ─────────────────────────

void ComboBox::setSpec(const ComboSpec& spec) {
  spec_ = spec;
  requestLayout();
}

void ComboBox::setItems(const std::vector<std::wstring>& items) {
  items_ = items;
  if (selected_ >= static_cast<int>(items_.size())) selected_ = items_.empty() ? -1 : 0;
  invalidate();
}

void ComboBox::addItem(const std::wstring& item) {
  items_.push_back(item);
  if (selected_ < 0) selected_ = 0;
  invalidate();
}

const std::wstring& ComboBox::itemAt(int index) const {
  static const std::wstring kEmpty;
  if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
  return items_[index];
}

const std::wstring& ComboBox::selectedText() const { return itemAt(selected_); }

void ComboBox::setSelectedIndex(int index, bool notify) {
  if (index < -1 || index >= static_cast<int>(items_.size())) return;
  if (index == selected_) return;
  selected_ = index;
  invalidate();
  if (notify && onChange_) onChange_(selected_);
}

Size ComboBox::onMeasure(const Size& available) {
  const int d = dpi();
  int width = 120 * d / 96;
  for (const auto& item : items_) {
    width = std::max(width, platform()->measureText(item, fonts.ui) + spec_.padX * 2 * d / 96 +
                               20 * d / 96);
  }
  if (available.width > 0) width = available.width;
  return Size{width, spec_.height * d / 96};
}

void ComboBox::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int radius = spec_.radius * d / 96;

  painter.fillRoundRect(rect, spec_.background, radius);
  // 展开时高亮边框，给用户"正开着"的反馈；未指定边框色时按背景自动派生
  const Color border =
      spec_.border != kNoColor ? spec_.border : edgeOn(spec_.background, 17);
  painter.strokeRoundRect(rect, focused_ ? kColorInputFocus : border, radius);

  const int padX = spec_.padX * d / 96;
  const int arrowSize = 12 * d / 96;
  const Rect arrowRect{rect.right - padX - arrowSize, rect.top + (rect.height() - arrowSize) / 2,
                       rect.right - padX, rect.top + (rect.height() + arrowSize) / 2};
  painter.drawIcon(IconId::kChevronDown, arrowRect, spec_.arrow);

  const Rect textRect{rect.left + padX, rect.top, arrowRect.left - padX / 2, rect.bottom};
  painter.drawTextEllipsis(textRect, selectedText(), isEnabled() ? spec_.ink : kColorTextFaint,
                           fonts.ui);
}

bool ComboBox::onMouseDown(const MouseEvent& e) {
  if (!isEnabled() || e.button != MouseButton::kLeft) return false;
  setFocus();
  // 在自绘控件上弹原生菜单：弹出位置以控件左下角为准
  openPopup();
  return true;
}

void ComboBox::moveSelection(int delta) {
  if (items_.empty()) return;
  int next = selected_ + delta;
  next = std::max(0, std::min(static_cast<int>(items_.size()) - 1, next));
  setSelectedIndex(next);
}

void ComboBox::openPopup() {
  if (items_.empty() || !root() || !root()->platformWindow()) return;

  std::vector<MenuItem> menu;
  menu.reserve(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    MenuItem entry;
    entry.label = items_[i];
    // 当前项打勾，与系统下拉观感一致
    entry.checked = static_cast<int>(i) == selected_;
    menu.push_back(entry);
  }

  PlatformWindow* window = root()->platformWindow();
  const Point origin = window->clientOrigin();
  const Point topLeft = localToWindow(Point{0, 0});
  // 菜单从控件下沿展开；空间不够时平台层自己会往上翻
  const Point position{origin.x + topLeft.x,
                       origin.y + topLeft.y + clientRect().height()};

  auto* platform = AiUI::platform();
  platform->bringToFront(window->handle());
  const int chosen = platform->showPopupMenu(window->handle(), position, menu);
  if (chosen >= 0) setSelectedIndex(chosen);
}

void ComboBox::onMouseMove(const MouseEvent& e) {
  (void)e;
  if (!isEnabled()) return;
  if (!hot_) {
    hot_ = true;
    invalidate();
  }
  setCursor(CursorShape::kHand);
}

void ComboBox::onMouseLeave() {
  if (hot_) {
    hot_ = false;
    invalidate();
  }
  setCursor(CursorShape::kArrow);
}

bool ComboBox::onKeyDown(const Key& key, const Modifiers& mods) {
  if (!isEnabled()) return false;
  (void)mods;
  switch (key) {
    case Key::kUp:
      moveSelection(-1);
      return true;
    case Key::kDown:
      moveSelection(1);
      return true;
    case Key::kEnter:
    case Key::kSpace:
      openPopup();
      return true;
    case Key::kHome:
      setSelectedIndex(0);
      return true;
    case Key::kEnd:
      setSelectedIndex(static_cast<int>(items_.size()) - 1);
      return true;
    default:
      return false;
  }
}

void ComboBox::onFocusChanged(bool focused) {
  focused_ = focused;
  invalidate();
}

}  // namespace AiUI
