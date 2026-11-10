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

// ── 适配器 ──

int ListRowSource::rowCount() const { return count_ ? count_() : 0; }

void ListRowSource::bindRow(int row, ListRow& out) const {
  if (binder_) binder_(row, out);
}

int ListRowSource::tagAt(int row) const { return tag_ ? tag_(row) : row; }

void ListArrayAdapter::bindRow(int row, ListRow& out) const {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  const ListRow& src = rows_[static_cast<size_t>(row)];
  // 就地覆盖，保留 out 里原有的字符串容量
  out.cells.resize(src.cells.size());
  for (size_t i = 0; i < src.cells.size(); ++i) {
    out.cells[i].text = src.cells[i].text;
    out.cells[i].textColor = src.cells[i].textColor;
  }
  out.icon = src.icon;
  out.tag = src.tag;
  out.dimmed = src.dimmed;
}

int ListArrayAdapter::tagAt(int row) const {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
  return rows_[static_cast<size_t>(row)].tag;
}

// ── ListView ──

ListView::ListView() { setFocusable(true); }

ListView::~ListView() = default;

void ListView::setStyle(const ListStyle& style) {
  style_ = style;
  layoutColumns();
  invalidate();
}

void ListView::setColumns(std::vector<ListColumn> columns) {
  columns_ = std::move(columns);
  colWidth_.assign(columns_.size(), 0);
  userWidth_.assign(columns_.size(), 0);
  if (sortColumn_ >= static_cast<int>(columns_.size())) sortColumn_ = 0;
  // 列数可能变了，已绑的行容器要按新列重绑
  ++generation_;
  layoutColumns();
  invalidate();
}

void ListView::setCallbacks(ListCallbacks callbacks) { callbacks_ = std::move(callbacks); }

void ListView::setAdapter(std::shared_ptr<ListAdapter> adapter) {
  adapter_ = std::move(adapter);
  // 换了数据源，行容器与选中状态都作废
  recycleHolders();
  ++generation_;
  selected_.assign(static_cast<size_t>(rowCount()), false);
  anchor_ = -1;
  hot_ = -1;
  setScrollOffset(0);
  layoutColumns();
  invalidate();
}

void ListView::notifyDataSetChanged() {
  const int count = rowCount();
  // 顺序和内容都可能整体变了，按行号保留选中没有意义
  selected_.assign(static_cast<size_t>(count), false);
  anchor_ = -1;
  hot_ = -1;
  ++generation_;
  for (ListViewHolder& holder : holderPool_) holder.row = -1;
  // 行数缩水后滚动位置要夹回范围内
  setScrollOffset(scrollOffset());
  invalidate();
}

void ListView::notifyRowsChanged(int firstRow, int lastRow) {
  const int count = rowCount();
  if (count <= 0) return;
  const int low = std::clamp(firstRow, 0, count - 1);
  const int high = std::clamp(lastRow < firstRow ? firstRow : lastRow, low, count - 1);
  // 行号没变，只让这几行的容器重新绑一次，其他行保持原样
  for (ListViewHolder& holder : holderPool_) {
    if (holder.row < low || holder.row > high) continue;
    const int row = holder.row;
    holder.row = -1;
    invalidateRow(row);
  }
}

int ListView::rowCount() const { return adapter_ ? adapter_->rowCount() : 0; }

int ListView::rowTag(int row) const {
  if (row < 0 || row >= rowCount()) return -1;
  return adapter_->tagAt(row);
}

int ListView::headHeight() const { return style_.headHeight * dpi() / 96; }
int ListView::rowHeight() const { return style_.rowHeight * dpi() / 96; }
int ListView::pageHeight() const { return (std::max)(0, viewportHeight()); }

// 行区在表头下方滚动，所以只把行算进内容高度
int ListView::contentHeight() const { return rowCount() * rowHeight(); }

int ListView::viewportHeight() const {
  return (std::max)(0, clientRect().height() - headHeight());
}

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
    if (selected_[static_cast<size_t>(i)]) out.push_back(i);
  }
  return out;
}

int ListView::columnWidth(int column) const {
  if (column < 0 || column >= static_cast<int>(userWidth_.size())) return 0;
  return userWidth_[static_cast<size_t>(column)];
}

int ListView::cellLeft(int column) const {
  int x = 0;
  for (int i = 0; i < column && i < static_cast<int>(colWidth_.size()); ++i) x += colWidth_[i];
  return x;
}

Rect ListView::rowRect(int row) const {
  // 行从表头下沿开始，滚动只影响行区
  const int top = headHeight() + row * rowHeight() - scrollOffset();
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
  if (pt.y < headHeight()) return -1;
  const int index = (pt.y - headHeight() + scrollOffset()) / rowHeight();
  if (index < 0 || index >= rowCount()) return -1;
  return index;
}

// ── 行容器复用 ──

void ListView::bindHolder(ListViewHolder* holder, int row) {
  if (!holder) return;
  // 行号和版本都没变，上次绑的内容仍然有效
  if (holder->row == row && holder->generation == generation_) return;
  holder->row = row;
  holder->generation = generation_;
  // 保留字符串容量，只清内容，避免整行重新分配
  if (holder->data.cells.size() < columns_.size()) holder->data.cells.resize(columns_.size());
  for (ListCell& cell : holder->data.cells) {
    cell.text.clear();
    cell.textColor = kNoColor;
  }
  holder->data.icon = ListIcon{};
  holder->data.tag = -1;
  holder->data.dimmed = false;
  if (adapter_) adapter_->bindRow(row, holder->data);
}

ListViewHolder* ListView::acquireHolder(int row) {
  // 已绑到这一行且内容未过期，直接复用
  for (ListViewHolder& holder : holderPool_) {
    if (holder.row != row) continue;
    if (holder.generation != generation_) bindHolder(&holder, row);
    return &holder;
  }
  // 再找空闲槽位
  for (ListViewHolder& holder : holderPool_) {
    if (holder.row < 0) {
      bindHolder(&holder, row);
      return &holder;
    }
  }
  // 池子还没长到一屏，才新增容器
  holderPool_.emplace_back();
  bindHolder(&holderPool_.back(), row);
  return &holderPool_.back();
}

void ListView::recycleHolders() {
  for (ListViewHolder& holder : holderPool_) holder.row = -1;
}

void ListView::layoutColumns() {
  if (columns_.empty()) return;
  // 右侧留出滚动条的位置，列不会压到滑块下面
  const int available = clientRect().right - scrollBarReserve();
  if (available <= 0) return;

  const int d = dpi();
  int fixed = 0;
  int stretchIndex = -1;
  for (int i = 0; i < static_cast<int>(columns_.size()); ++i) {
    if (columns_[static_cast<size_t>(i)].stretch) {
      stretchIndex = i;
      continue;
    }
    // 用户拖过的宽度优先于设计值
    colWidth_[static_cast<size_t>(i)] = userWidth_[static_cast<size_t>(i)] > 0
                                            ? userWidth_[static_cast<size_t>(i)]
                                            : columns_[static_cast<size_t>(i)].width * d / 96;
    fixed += colWidth_[static_cast<size_t>(i)];
  }
  if (stretchIndex >= 0) {
    // 拉伸列被用户拖过就尊重用户宽度，不再自动铺满
    colWidth_[static_cast<size_t>(stretchIndex)] =
        userWidth_[static_cast<size_t>(stretchIndex)] > 0
            ? userWidth_[static_cast<size_t>(stretchIndex)]
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
  selected_.assign(static_cast<size_t>(count), false);
  selected_[static_cast<size_t>(row)] = true;
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
    for (int i = low; i <= high && i < count; ++i) selected_[static_cast<size_t>(i)] = true;
  } else if (ctrl) {
    selected_[static_cast<size_t>(row)] = !selected_[static_cast<size_t>(row)];
    anchor_ = row;
  } else {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[static_cast<size_t>(row)] = true;
    anchor_ = row;
  }
  if (callbacks_.onSelectionChanged) callbacks_.onSelectionChanged();
  invalidateContent();
}

void ListView::invalidateContent() {
  // 选中变化只影响行区，表头不必重画
  const Rect rc = clientRect();
  const int top = headHeight();
  if (rc.bottom <= top) return;
  invalidateRect(Rect{0, top, rc.right, rc.bottom});
}

void ListView::invalidateRow(int row) {
  if (row < 0 || row >= rowCount()) return;
  Rect rc = rowRect(row);
  // 滚出可视区的行不必重画
  const int top = headHeight();
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
  userWidth_[static_cast<size_t>(column)] = physicalWidth > 0 ? physicalWidth : 0;
  layoutColumns();
  invalidate();
}





void ListView::ensureRowVisible(int row) {
  const int page = pageHeight();
  if (page <= 0) return;
  const int top = row * rowHeight();
  int target = scrollOffset();
  if (top < target) {
    target = top;
  } else if (top + rowHeight() > target + page) {
    target = top + rowHeight() - page;
  }
  setScrollOffset(target);
}

void ListView::paintHeader(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Rect headRect{0, 0, rect.right, headHeight()};
  painter.fillRect(headRect, style_.headBg);

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = cellLeft(col);
    if (left >= rect.right) break;

    const Rect cell{left, 0, left + colWidth_[static_cast<size_t>(col)], headHeight()};
    Rect textRect = cell;
    textRect.left += style_.padX * d / 96;
    textRect.right -= style_.padX * d / 96;
    if (col == sortColumn_) textRect.right -= kSortArrowArea * d / 96;

    const TextAlign align =
        columns_[static_cast<size_t>(col)].align == ListAlign::kRight ? TextAlign::kRight
                                                                     : TextAlign::kLeft;
    painter.drawText(textRect, columns_[static_cast<size_t>(col)].title, style_.headText,
                     fonts.ui, align);

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

void ListView::paintRow(Painter& painter, const ListRow& data, int row, const Rect& rowRect) {
  const int d = dpi();
  const bool selected =
      (row < static_cast<int>(selected_.size())) && selected_[static_cast<size_t>(row)];
  const bool hot = (row == hot_) && !selected;
  painter.fillRect(rowRect, selected ? style_.selBg : (hot ? style_.hoverBg : style_.background));

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = cellLeft(col);
    if (left >= rowRect.right) break;

    Rect textRect{left + style_.padX * d / 96, rowRect.top,
                  left + colWidth_[static_cast<size_t>(col)] - style_.padX * d / 96,
                  rowRect.bottom};

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
    const ListCell& cell = data.cells[static_cast<size_t>(col)];
    if (cell.text.empty()) continue;

    const Color color =
        cell.textColor != kNoColor ? cell.textColor : (data.dimmed ? style_.headText : style_.text);
    if (columns_[static_cast<size_t>(col)].align == ListAlign::kRight) {
      painter.drawText(textRect, cell.text, color, fonts.ui, TextAlign::kRight);
    } else {
      painter.drawTextEllipsis(textRect, cell.text, color, fonts.ui);
    }
  }

  painter.drawHLine(rowRect.left, rowRect.right, rowRect.bottom - 1, style_.rowLine);
}

void ListView::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const int top = headHeight();
  // 行区裁剪在表头之下；表头最后画，盖住滚上来的行
  painter.save();
  painter.clipRect(Rect{rect.left, top, rect.right, rect.bottom});

  const int count = rowCount();
  if (count > 0 && top < rect.bottom) {
    const int first = scrollOffset() / rowHeight();
    const int last = (std::min)(count - 1, first + pageHeight() / rowHeight() + 1);

    // 池子按可视行数封顶；滚出视野的行容器放回池子，供本次可视行换绑
    const int visible = (std::max)(1, pageHeight() / rowHeight() + 2);
    if (static_cast<int>(holderPool_.size()) < visible) {
      holderPool_.resize(static_cast<size_t>(visible));
    }
    for (ListViewHolder& holder : holderPool_) {
      if (holder.row < first || holder.row > last) holder.row = -1;
    }

    for (int row = first; row <= last; ++row) {
      const Rect rr = rowRect(row);
      if (rr.bottom <= top) continue;
      if (rr.top >= rect.bottom) break;
      ListViewHolder* holder = acquireHolder(row);
      if (holder) paintRow(painter, holder->data, row, rr);
    }
  }
  painter.restore();

  paintHeader(painter, rect);
}

bool ListView::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  const int d = dpi();

  // 先给滚动条：点在滑块或轨道上就由基类处理
  if (ScrollView::onMouseDown(e)) return true;

  if (e.position.y < headHeight()) {
    const int col = columnAtX(e.position.x);
    if (col < 0) return true;
    const int right = cellLeft(col) + colWidth_[static_cast<size_t>(col)];
    if (e.position.x > right - kResizeEdge * d / 96) {
      // 开始拖列宽
      dragColumn_ = col;
      dragStartX_ = e.position.x;
      dragStartWidth_ = colWidth_[static_cast<size_t>(col)];
      captureMouse();
    } else {
      // 点表头排序：同列再次点击切换方向
      const bool ascending = (col == sortColumn_) ? !sortAscending_ : true;
      sortColumn_ = col;
      sortAscending_ = ascending;
      if (callbacks_.onSort) callbacks_.onSort(col, ascending);
      invalidate();
    }
    return true;
  }

  setFocus();
  const int row = hitRow(e.position);
  if (row < 0) {
    clearSelection();
    return true;
  }
  syncSelection(row, e.modifiers.ctrl, e.modifiers.shift);
  return true;
}

void ListView::onMouseUp(const MouseEvent& e) {
  // 滚动条滑块可能正在拖动，先让基类收尾
  ScrollView::onMouseUp(e);
  if (dragColumn_ < 0) return;
  // 拖动期间只改 colWidth_，松手才落进 userWidth_ 并通知持久化
  userWidth_[static_cast<size_t>(dragColumn_)] = colWidth_[static_cast<size_t>(dragColumn_)];
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
    colWidth_[static_cast<size_t>(dragColumn_)] = (std::max)(kMinColumnWidth * d / 96, next);
    invalidate();
    return;
  }

  // 滚动条的悬停高亮要先更新
  ScrollView::onMouseMove(e);

  if (e.position.y < headHeight()) {
    // 表头：靠近列右边界显示左右调整光标
    const int col = columnAtX(e.position.x);
    bool onEdge = false;
    if (col >= 0) {
      const int right = cellLeft(col) + colWidth_[static_cast<size_t>(col)];
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
  ScrollView::onMouseLeave();
  if (hot_ == -1) return;
  const int previous = hot_;
  hot_ = -1;
  invalidateRow(previous);
}

void ListView::onDoubleClick(const MouseEvent& e) {
  if (e.position.y < headHeight()) return;
  const int row = hitRow(e.position);
  if (row >= 0 && callbacks_.onActivate) callbacks_.onActivate(row);
}

void ListView::onContextMenu(const MouseEvent& e) {
  (void)e;
  if (callbacks_.onContextMenu) callbacks_.onContextMenu();
}

void ListView::onScrollChanged() {
  // 整行平移，重画行区即可；表头不动
  invalidateContent();
}

void ListView::onLayout(const Rect& frame) {
  ScrollView::onLayout(frame);
  // 宽度变了，名称列要重新吃掉剩余空间
  layoutColumns();
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
