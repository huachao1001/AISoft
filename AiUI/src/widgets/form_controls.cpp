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
  const Theme& t = theme();
  const Color track = spec_.track != kNoColor ? spec_.track : t.lineSoft;
  const Color fill = spec_.fill != kNoColor ? spec_.fill : t.link;

  painter.fillRoundRect(rect, track, radius);

  const int fillWidth = static_cast<int>(rect.width() * progress_);
  if (fillWidth <= 0) return;
  const Rect region{rect.left, rect.top, rect.left + fillWidth, rect.bottom};
  painter.fillRoundRect(region, fill, radius);
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
  painter.drawText(textRect, title_, theme().textHead, fonts.ui, TextAlign::kLeft);
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

ComboBox::ComboBox() = default;

ComboBox::~ComboBox() {
  // 浮层在 ViewRoot 上只是裸指针：控件先销毁时必须摘掉，否则根上留悬空引用
  if (popupOpen_ && root()) root()->setOverlay(nullptr);
}

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
  const Theme& t = theme();

  const Color background = spec_.background != kNoColor ? spec_.background : t.pane;
  const Color border = spec_.border != kNoColor ? spec_.border : edgeOn(background, 17);
  const Color focusBorder = spec_.focusBorder != kNoColor ? spec_.focusBorder : t.inputFocus;
  const Color ink = spec_.ink != kNoColor ? spec_.ink : t.text;
  const Color arrowColor = spec_.arrow != kNoColor ? spec_.arrow : t.textMuted;
  const Color disabledInk = spec_.disabledInk != kNoColor ? spec_.disabledInk : t.textFaint;

  painter.fillRoundRect(rect, background, radius);
  // 展开时高亮边框，给用户"正开着"的反馈
  painter.strokeRoundRect(rect, focused_ ? focusBorder : border, radius);

  const int padX = spec_.padX * d / 96;
  const int arrowSize = 12 * d / 96;
  const Rect arrowRect{rect.right - padX - arrowSize, rect.top + (rect.height() - arrowSize) / 2,
                       rect.right - padX, rect.top + (rect.height() + arrowSize) / 2};
  painter.drawIcon(IconId::kChevronDown, arrowRect, arrowColor);

  const Rect textRect{rect.left + padX, rect.top, arrowRect.left - padX / 2, rect.bottom};
  painter.drawTextEllipsis(textRect, selectedText(), isEnabled() ? ink : disabledInk, fonts.ui);
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
  // 展开时高亮跟着键盘走，否则列表里看不出当前会选中哪一项
  if (popupOpen_ && next != hotRow_) {
    hotRow_ = next;
    if (root()) root()->invalidateRect(overlayBounds());
  }
}

void ComboBox::openPopup() {
  if (items_.empty()) return;
  if (!root() || !root()->platformWindow()) return;
  if (popupOpen_) return;
  popupOpen_ = true;
  hotRow_ = selected_;
  // 交给 ViewRoot 置顶绘制：画在整棵树之上，且优先收鼠标事件
  root()->setOverlay(this);
}

void ComboBox::closePopup() {
  if (!popupOpen_) return;
  popupOpen_ = false;
  hotRow_ = -1;
  if (ViewRoot* r = root()) r->setOverlay(nullptr);
  invalidate();
}

int ComboBox::popupRowHeight() const {
  const int d = dpi();
  // 行高与显示框一致，字号也一致，观感才是同一套控件
  return spec_.height * d / 96;
}

Rect ComboBox::popupWindowRect() const {
  const int d = dpi();
  const Rect self = absoluteBounds();
  const Rect client = root() ? root()->platformWindow()->clientRect() : Rect{};
  const int rows = static_cast<int>(items_.size());
  const int height = popupRowHeight() * rows;
  const int gap = 2 * d / 96;

  int top = self.bottom + gap;
  // 下面放不下就翻到控件上方，避免列表被窗口底边截断
  if (client.bottom > 0 && top + height > client.bottom - gap) {
    const int above = self.top - gap - height;
    if (above >= client.top) top = above;
  }
  // 与显示框严格等宽：弹出层看起来就是控件本身展开
  return Rect{self.left, top, self.right, top + height};
}

int ComboBox::popupRowAt(const Point& windowPoint) const {
  const Rect popup = popupWindowRect();
  if (!popup.contains(windowPoint)) return -1;
  const int rowHeight = popupRowHeight();
  if (rowHeight <= 0) return -1;
  const int row = (windowPoint.y - popup.top) / rowHeight;
  if (row < 0 || row >= static_cast<int>(items_.size())) return -1;
  return row;
}

bool ComboBox::onOverlayMouseDown(const Point& windowPoint) {
  const int row = popupRowAt(windowPoint);
  // 收起放在选中之前：setSelectedIndex 会重绘，此时浮层必须已经撤下
  if (row >= 0) {
    closePopup();
    setSelectedIndex(row);
    return true;
  }
  // 点在列表外就算放弃选择：点控件自身也走这里（收起，不重新展开）
  closePopup();
  return true;
}

bool ComboBox::onOverlayMouseMove(const Point& windowPoint) {
  const int row = popupRowAt(windowPoint);
  if (row != hotRow_) {
    hotRow_ = row;
    if (root()) root()->invalidateRect(overlayBounds());
  }
  return true;
}

Rect ComboBox::overlayBounds() const { return popupWindowRect(); }

void ComboBox::paintOverlay(Painter& painter) {
  const Theme& t = theme();
  const Rect popup = popupWindowRect();
  if (popup.isEmpty()) return;

  const int d = dpi();
  const int radius = (std::max)(4, spec_.radius * d / 96);
  const int rowHeight = popupRowHeight();

  // 阴影留白由外框自己撑：先铺一层描边色的圆角当边界
  painter.fillRoundRect(popup, t.pane, radius);
  painter.strokeRoundRect(popup, edgeOn(t.pane, 17), radius);
  painter.clipRect(popup.inset(1, 1, 1, 1));

  for (size_t i = 0; i < items_.size(); ++i) {
    const int top = popup.top + static_cast<int>(i) * rowHeight;
    const Rect row{popup.left, top, popup.right, top + rowHeight};
    const bool isSelected = static_cast<int>(i) == selected_;
    const bool isHot = static_cast<int>(i) == hotRow_;

    if (isSelected) {
      painter.fillRect(row, t.selBg);
    } else if (isHot) {
      painter.fillRect(row, t.rowHover);
    }

    // 文字用与显示框相同的字体与内边距，字号不会跳变
    const int padX = spec_.padX * d / 96;
    const Rect textRect{row.left + padX, row.top, row.right - padX, row.bottom};
    painter.drawTextEllipsis(textRect, items_[i], t.text, fonts.ui);
  }
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
  // 列表展开时 Esc 只负责收起，不冒泡给窗口（否则会顺带关掉对话框）
  if (key == Key::kEscape) {
    if (!popupOpen_) return false;
    closePopup();
    return true;
  }
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

void ComboBox::onDetach() {
  // 浮层挂在 ViewRoot 上只是裸指针；脱树时先撤下，避免留着已销毁的控件
  if (popupOpen_) closePopup();
}

}  // namespace AiUI
