// ListView 实现：表头、行绘制、选择、滚动、列宽拖动、排序指示
#include "AiUI/widgets/list_view.h"

#include "AiUI/font.h"

#include <algorithm>

namespace AiUI {
namespace {

// 表头右侧留给排序箭头的宽度
constexpr int kSortArrowArea = 18;
// 命中列边界的容差，用于拖列宽
constexpr int kResizeEdge = 4;
// 最小列宽
constexpr int kMinColumnWidth = 48;

}  // namespace

void ListView::setStyle(const ListStyle& style) {
  style_ = style;
  layoutColumns();
  updateScrollBar();
  invalidate();
}

void ListView::setColumns(std::vector<ListColumn> columns) {
  columns_ = std::move(columns);
  colWidth_.assign(columns_.size(), 0);
  userWidth_.assign(columns_.size(), 0);
  if (sortColumn_ >= static_cast<int>(columns_.size())) sortColumn_ = 0;
  layoutColumns();
  updateScrollBar();
  invalidate();
}

void ListView::setCallbacks(ListCallbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void ListView::setRows(std::vector<ListRow> rows) {
  rows_ = std::move(rows);
  // 行内容整体变了，按行号保留选中没有意义，直接清空
  selected_.assign(rows_.size(), false);
  anchor_ = -1;
  scroll_ = (std::max)(0, scroll_);
  scroll_ = (std::min)(scroll_, maxScroll());
  updateScrollBar();
  invalidate();
}

int ListView::rowTag(int row) const {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
  return rows_[row].tag;
}

int ListView::headHeight() const { return style_.headHeight * dpi() / 96; }
int ListView::rowHeight() const { return style_.rowHeight * dpi() / 96; }
int ListView::contentTop() const { return headHeight(); }
int ListView::pageHeight() const { return clientRect().height() - contentTop(); }

Size ListView::onMeasure(const Size& available) { return available; }

int ListView::selectedCount() const {
  int count = 0;
  for (bool selected : selected_) {
    if (selected) ++count;
  }
  return count;
}

std::vector<int> ListView::selectedRows() const {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(selected_.size()); ++i) {
    if (selected_[i]) out.push_back(i);
  }
  return out;
}

int ListView::columnWidth(int column) const {
  if (column < 0 || column >= static_cast<int>(userWidth_.size())) return 0;
  return userWidth_[column];
}

int ListView::cellLeft(int column) const {
  int x = 0;
  for (int i = 0; i < column && i < static_cast<int>(colWidth_.size()); ++i) x += colWidth_[i];
  return x;
}

Rect ListView::rowRect(int row) const {
  const int top = contentTop() + row * rowHeight() - scroll_;
  return Rect{0, top, clientRect().right, top + rowHeight()};
}

int ListView::columnAtX(int x) const {
  int accumulated = 0;
  for (int i = 0; i < static_cast<int>(colWidth_.size()); ++i) {
    accumulated += colWidth_[i];
    if (x < accumulated) return i;
  }
  return -1;
}

int ListView::hitRow(Point pt) const {
  if (pt.y < contentTop()) return -1;
  const int index = (pt.y - contentTop() + scroll_) / rowHeight();
  if (index < 0 || index >= rowCount()) return -1;
  return index;
}

void ListView::layoutColumns() {
  if (columns_.empty()) return;
  const int available = clientRect().right;
  if (available <= 0) return;

  const int d = dpi();
  int fixed = 0;
  int stretchIndex = -1;
  for (int i = 0; i < static_cast<int>(columns_.size()); ++i) {
    if (columns_[i].stretch) {
      stretchIndex = i;
      continue;
    }
    // 用户拖过的宽度优先于设计值
    colWidth_[i] = userWidth_[i] > 0 ? userWidth_[i] : columns_[i].width * d / 96;
    fixed += colWidth_[i];
  }
  if (stretchIndex >= 0) {
    // 拉伸列被用户拖过就尊重用户宽度，不再自动铺满
    colWidth_[stretchIndex] =
        userWidth_[stretchIndex] > 0 ? userWidth_[stretchIndex]
                                     : (std::max)(180 * d / 96, available - fixed);
  }
}

void ListView::clearSelection() {
  std::fill(selected_.begin(), selected_.end(), false);
  anchor_ = -1;
  if (callbacks_.onSelectionChanged) callbacks_.onSelectionChanged();
  invalidateContent();
}

void ListView::selectAll() {
  std::fill(selected_.begin(), selected_.end(), true);
  if (!selected_.empty() && anchor_ < 0) anchor_ = 0;
  if (callbacks_.onSelectionChanged) callbacks_.onSelectionChanged();
  invalidateContent();
}

void ListView::selectRow(int row, bool scrollIntoView) {
  const int count = rowCount();
  if (row < 0 || row >= count) return;
  selected_.assign(count, false);
  selected_[row] = true;
  anchor_ = row;
  if (scrollIntoView) ensureRowVisible(row);
  if (callbacks_.onSelectionChanged) callbacks_.onSelectionChanged();
  invalidateContent();
}

void ListView::syncSelection(int row, bool ctrl, bool shift) {
  const int count = rowCount();
  if (row < 0 || row >= count) return;

  if (shift && anchor_ >= 0) {
    const int low = (std::min)(anchor_, row);
    const int high = (std::max)(anchor_, row);
    if (!ctrl) std::fill(selected_.begin(), selected_.end(), false);
    for (int i = low; i <= high && i < count; ++i) selected_[i] = true;
  } else if (ctrl) {
    selected_[row] = !selected_[row];
    anchor_ = row;
  } else {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[row] = true;
    anchor_ = row;
  }
  if (callbacks_.onSelectionChanged) callbacks_.onSelectionChanged();
  invalidateContent();
}

void ListView::invalidateContent() {
  // 选中变化只影响行区，表头不必重画
  const Rect rc = clientRect();
  const int top = contentTop();
  if (rc.bottom <= top) return;
  invalidateRect(Rect{rc.left, top, rc.right, rc.bottom});
}

void ListView::invalidateRow(int row) {
  if (row < 0 || row >= rowCount()) return;
  Rect rc = rowRect(row);
  // 滚出可视区的行不必重画
  const int top = contentTop();
  if (rc.bottom <= top || rc.top >= clientRect().bottom) return;
  rc.top = (std::max)(rc.top, top);
  rc.bottom = (std::min)(rc.bottom, clientRect().bottom);
  invalidateRect(rc);
}

void ListView::setSort(int column, bool ascending) {
  sortColumn_ = column;
  sortAscending_ = ascending;
  invalidate();
}

void ListView::setUserWidth(int column, int physicalWidth) {
  if (column < 0 || column >= static_cast<int>(userWidth_.size())) return;
  userWidth_[column] = physicalWidth > 0 ? physicalWidth : 0;
  layoutColumns();
  invalidate();
}

void ListView::onLayout(const Rect& frame) {
  (void)frame;
  // 宽度变化后重新分配列宽：构造期控件还没建窗，拿不到客户区宽度
  layoutColumns();
  updateScrollBar();
}

int ListView::maxScroll() const {
  const int total = rowCount() * rowHeight();
  return (std::max)(0, total - pageHeight());
}

void ListView::updateScrollBar() {
  if (!window()) return;
  window()->setScrollInfo(rowCount() * rowHeight(), pageHeight(), scroll_);
}

void ListView::setScroll(int position) {
  const int next = std::clamp(position, 0, maxScroll());
  if (next == scroll_) return;
  scroll_ = next;
  updateScrollBar();
  // 整行平移，只能重画行区
  invalidateContent();
}

void ListView::scrollBy(int delta) { setScroll(scroll_ + delta); }

void ListView::ensureRowVisible(int row) {
  const int page = pageHeight();
  if (page <= 0) return;
  const int top = row * rowHeight();
  if (top < scroll_) {
    scroll_ = top;
  } else if (top + rowHeight() > scroll_ + page) {
    scroll_ = top + rowHeight() - page;
  }
  scroll_ = std::clamp(scroll_, 0, maxScroll());
}

void ListView::paintHeader(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Rect headRect{0, 0, rect.right, headHeight()};
  painter.fillRect(headRect, style_.headBg);

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = cellLeft(col);
    if (left >= rect.right) break;

    const Rect cell{left, 0, left + colWidth_[col], headHeight()};
    Rect textRect = cell;
    textRect.left += style_.padX * d / 96;
    textRect.right -= style_.padX * d / 96;
    if (col == sortColumn_) textRect.right -= kSortArrowArea * d / 96;

    const TextAlign align =
        columns_[col].align == ListAlign::kRight ? TextAlign::kRight : TextAlign::kLeft;
    painter.drawText(textRect, columns_[col].title, style_.headText, fonts.ui, align);

    // 排序箭头只在当前排序列显示
    if (col == sortColumn_) {
      const int size = 11 * d / 96;
      const int centerX = cell.right - kSortArrowArea * d / 96;
      const int centerY = headHeight() / 2;
      const Rect arrowRect{centerX - size / 2, centerY - size / 2, centerX + size / 2,
                           centerY + size / 2};
      painter.drawIcon(sortAscending_ ? IconId::kChevronUp : IconId::kChevronDown, arrowRect,
                       kColorTextMuted);
    }
  }

  painter.drawHLine(0, rect.right, headHeight() - 1, style_.headLine);
}

void ListView::paintRow(Painter& painter, int row, const Rect& rowRect) {
  const int d = dpi();
  const bool selected = (row < static_cast<int>(selected_.size())) && selected_[row];
  const bool hot = (row == hot_) && !selected;
  painter.fillRect(rowRect, selected ? style_.selBg : (hot ? style_.hoverBg : style_.background));

  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  const ListRow& data = rows_[row];

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = cellLeft(col);
    if (left >= rowRect.right) break;

    Rect textRect{left + style_.padX * d / 96, rowRect.top,
                  left + colWidth_[col] - style_.padX * d / 96, rowRect.bottom};

    // 图标只跟着第一列画
    if (col == 0) {
      const int iconSize = style_.iconSize * d / 96;
      const int iconTop = (rowRect.top + rowRect.bottom - iconSize) / 2;
      const Rect iconRect{left + style_.padX * d / 96, iconTop,
                          left + style_.padX * d / 96 + iconSize, iconTop + iconSize};
      if (data.icon.image.isValid()) {
        painter.drawImage(data.icon.image, iconRect);
      } else {
        const IconId glyph = data.icon.glyph == IconId::kNone ? IconId::kFile : data.icon.glyph;
        const Color color = data.icon.color != kNoColor ? data.icon.color : style_.text;
        painter.drawIcon(glyph, iconRect, color);
      }
      textRect.left = iconRect.right + style_.iconGap * d / 96;
    }

    if (col >= static_cast<int>(data.cells.size())) continue;
    const ListCell& cell = data.cells[col];
    if (cell.text.empty()) continue;

    const Color color =
        cell.textColor != kNoColor ? cell.textColor : (data.dimmed ? style_.headText : style_.text);
    if (columns_[col].align == ListAlign::kRight) {
      painter.drawText(textRect, cell.text, color, fonts.ui, TextAlign::kRight);
    } else {
      painter.drawTextEllipsis(textRect, cell.text, color, fonts.ui);
    }
  }

  painter.drawHLine(rowRect.left, rowRect.right, rowRect.bottom - 1, style_.rowLine);
}

void ListView::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  // 行区裁剪在表头之下
  const int top = contentTop();
  painter.setClip(Rect{0, top, rect.right, rect.bottom});

  const int first = scroll_ / rowHeight();
  const int last = (std::min)(rowCount() - 1, first + pageHeight() / rowHeight() + 1);
  for (int row = first; row <= last; ++row) {
    const Rect rr = rowRect(row);
    if (rr.bottom <= top) continue;
    if (rr.top >= rect.bottom) break;
    paintRow(painter, row, rr);
  }

  painter.resetClip();
  paintHeader(painter, rect);
}

void ListView::onMouseDown(const MouseEvent& e) {
  const int d = dpi();

  if (e.position.y < headHeight()) {
    const int col = columnAtX(e.position.x);
    if (col < 0) return;
    const int right = cellLeft(col) + colWidth_[col];
    if (e.position.x > right - kResizeEdge * d / 96) {
      // 开始拖列宽
      dragColumn_ = col;
      dragStartX_ = e.position.x;
      dragStartWidth_ = colWidth_[col];
      captureMouse();
    } else {
      // 点表头排序：同列再次点击切换方向
      const bool ascending = (col == sortColumn_) ? !sortAscending_ : true;
      sortColumn_ = col;
      sortAscending_ = ascending;
      if (callbacks_.onSort) callbacks_.onSort(col, ascending);
      invalidate();
    }
    return;
  }

  setFocus();
  const int row = hitRow(e.position);
  if (row < 0) {
    clearSelection();
    return;
  }
  syncSelection(row, e.modifiers.ctrl, e.modifiers.shift);
}

void ListView::onMouseUp(const MouseEvent& e) {
  (void)e;
  if (dragColumn_ < 0) return;
  userWidth_[dragColumn_] = colWidth_[dragColumn_];
  dragColumn_ = -1;
  releaseMouse();
  layoutColumns();
  invalidate();
  if (callbacks_.onColumnResized) callbacks_.onColumnResized();
}

void ListView::onMouseMove(const MouseEvent& e) {
  const int d = dpi();

  if (dragColumn_ >= 0) {
    const int next = dragStartWidth_ + (e.position.x - dragStartX_);
    colWidth_[dragColumn_] = (std::max)(kMinColumnWidth * d / 96, next);
    invalidate();
    return;
  }

  if (e.position.y < headHeight()) {
    // 表头：靠近列右边界显示左右调整光标
    const int col = columnAtX(e.position.x);
    bool onEdge = false;
    if (col >= 0) {
      const int right = cellLeft(col) + colWidth_[col];
      onEdge = e.position.x > right - kResizeEdge * d / 96;
    }
    setCursor(onEdge ? CursorShape::kSizeHorizontal : CursorShape::kArrow);
    if (hot_ != -1) {
      const int previous = hot_;
      hot_ = -1;
      invalidateRow(previous);
    }
    return;
  }

  const int row = hitRow(e.position);
  if (row != hot_) {
    const int previous = hot_;
    hot_ = row;
    // 只重画进出悬停的两行
    invalidateRow(previous);
    invalidateRow(hot_);
  }
  setCursor(CursorShape::kArrow);
}

void ListView::onMouseLeave() {
  if (hot_ == -1) return;
  const int previous = hot_;
  hot_ = -1;
  invalidateRow(previous);
}

void ListView::onMouseWheel(const MouseEvent& e) { scrollBy(-e.wheelDelta * rowHeight() * 3 / 120); }

void ListView::onDoubleClick(const MouseEvent& e) {
  if (e.position.y < headHeight()) return;
  const int row = hitRow(e.position);
  if (row >= 0 && callbacks_.onActivate) callbacks_.onActivate(row);
}

void ListView::onContextMenu(const MouseEvent& e) {
  (void)e;
  if (callbacks_.onContextMenu) callbacks_.onContextMenu();
}

void ListView::onScroll(const ScrollEvent& e) {
  switch (e.code) {
    case ScrollCode::kLineUp: scrollBy(-rowHeight()); break;
    case ScrollCode::kLineDown: scrollBy(rowHeight()); break;
    case ScrollCode::kPageUp: scrollBy(-pageHeight()); break;
    case ScrollCode::kPageDown: scrollBy(pageHeight()); break;
    case ScrollCode::kThumb: setScroll(e.position); break;
    case ScrollCode::kTop: setScroll(0); break;
    case ScrollCode::kBottom: setScroll(maxScroll()); break;
  }
}

bool ListView::onKeyDown(const Key& key, const Modifiers& mods) {
  const int count = rowCount();

  switch (key) {
    case Key::kUp:
    case Key::kDown:
    case Key::kHome:
    case Key::kEnd:
    case Key::kPageUp:
    case Key::kPageDown: {
      if (count <= 0) return true;
      const int page = (std::max)(1, pageHeight() / rowHeight());
      int target = anchor_ < 0 ? 0 : anchor_;
      if (key == Key::kUp) target = (std::max)(0, target - 1);
      if (key == Key::kDown) target = (std::min)(count - 1, target + 1);
      if (key == Key::kHome) target = 0;
      if (key == Key::kEnd) target = count - 1;
      if (key == Key::kPageUp) target = (std::max)(0, target - page);
      if (key == Key::kPageDown) target = (std::min)(count - 1, target + page);
      syncSelection(target, false, mods.shift);
      ensureRowVisible(target);
      updateScrollBar();
      invalidate();
      return true;
    }
    case Key::kEnter:
      if (anchor_ >= 0 && callbacks_.onActivate) callbacks_.onActivate(anchor_);
      return true;

    case Key::kA:
      if (mods.ctrl) {
        selectAll();
        return true;
      }
      break;

    default:
      break;
  }

  // 其余按键交给业务层（Delete、Backspace 等）
  if (callbacks_.onKeyDown) callbacks_.onKeyDown(key);
  return true;
}

}  // namespace AiUI
