#include "AUI/list_view.h"

#include "AUI/font.h"

#include <windowsx.h>

#include <algorithm>

namespace AUI {
namespace {

// 表头右侧留给排序箭头的宽度
constexpr int kSortArrowArea = 18;
// 命中列边界的容差，用于拖列宽
constexpr int kResizeEdge = 4;

}  // namespace

void ListView::SetStyle(const ListStyle& style) {
  style_ = style;
  LayoutColumns();
  UpdateScrollBar();
  Invalidate();
}

void ListView::SetColumns(std::vector<ListColumn> columns) {
  columns_ = std::move(columns);
  colWidth_.assign(columns_.size(), 0);
  userWidth_.assign(columns_.size(), 0);
  if (sortColumn_ >= static_cast<int>(columns_.size())) sortColumn_ = 0;
  LayoutColumns();
  UpdateScrollBar();
  Invalidate();
}

void ListView::SetCallbacks(ListCallbacks cb) {
  cb_ = std::move(cb);
  Reload();
}

int ListView::RowCount() const { return cb_.rowCount ? cb_.rowCount() : 0; }

int ListView::selectedCount() const {
  int n = 0;
  for (bool s : selected_) {
    if (s) ++n;
  }
  return n;
}

std::vector<int> ListView::SelectedRows() const {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(selected_.size()); ++i) {
    if (selected_[i]) out.push_back(i);
  }
  return out;
}

int ListView::ColumnWidth(int column) const {
  if (column < 0 || column >= static_cast<int>(userWidth_.size())) return 0;
  return userWidth_[column];
}

int ListView::CellLeft(int column) const {
  int x = 0;
  for (int i = 0; i < column && i < static_cast<int>(colWidth_.size()); ++i) x += colWidth_[i];
  return x;
}

RECT ListView::RowRect(int row) const {
  const int top = ContentTop() + row * RowHeight() - scroll_;
  return RECT{0, top, ClientRect().right, top + RowHeight()};
}

int ListView::ColumnAtX(int x) const {
  int acc = 0;
  for (int i = 0; i < static_cast<int>(colWidth_.size()); ++i) {
    acc += colWidth_[i];
    if (x < acc) return i;
  }
  return -1;
}

int ListView::HitRow(POINT pt) const {
  if (pt.y < ContentTop()) return -1;
  const int idx = (pt.y - ContentTop() + scroll_) / RowHeight();
  if (idx < 0 || idx >= RowCount()) return -1;
  return idx;
}

void ListView::LayoutColumns() {
  if (columns_.empty()) return;
  const int avail = ClientRect().right;
  if (avail <= 0) return;

  int fixed = 0;
  int stretchIndex = -1;
  for (int i = 0; i < static_cast<int>(columns_.size()); ++i) {
    if (columns_[i].stretch) {
      stretchIndex = i;
      continue;
    }
    // 用户拖过的宽度优先于设计值
    colWidth_[i] = userWidth_[i] > 0 ? userWidth_[i] : px(columns_[i].width);
    fixed += colWidth_[i];
  }
  if (stretchIndex >= 0) {
    // 拉伸列被用户拖过就尊重用户宽度，不再自动铺满
    colWidth_[stretchIndex] = userWidth_[stretchIndex] > 0
                                  ? userWidth_[stretchIndex]
                                  : (std::max)(px(180), avail - fixed);
  }
}

void ListView::ClearSelection() {
  std::fill(selected_.begin(), selected_.end(), false);
  anchor_ = -1;
  if (cb_.onSelectionChanged) cb_.onSelectionChanged();
  Invalidate();
}

void ListView::SelectAll() {
  std::fill(selected_.begin(), selected_.end(), true);
  if (!selected_.empty() && anchor_ < 0) anchor_ = 0;
  if (cb_.onSelectionChanged) cb_.onSelectionChanged();
  Invalidate();
}

void ListView::SelectRow(int row, bool scrollIntoView) {
  const int n = RowCount();
  if (row < 0 || row >= n) return;
  selected_.assign(n, 0);
  selected_[row] = true;
  anchor_ = row;
  if (scrollIntoView) EnsureRowVisible(row);
  if (cb_.onSelectionChanged) cb_.onSelectionChanged();
  Invalidate();
}

void ListView::SyncSelection(int row, bool ctrl, bool shift) {
  const int n = RowCount();
  if (row < 0 || row >= n) return;

  if (shift && anchor_ >= 0) {
    const int lo = (std::min)(anchor_, row);
    const int hi = (std::max)(anchor_, row);
    if (!ctrl) std::fill(selected_.begin(), selected_.end(), false);
    for (int i = lo; i <= hi && i < n; ++i) selected_[i] = true;
  } else if (ctrl) {
    selected_[row] = !selected_[row];
    anchor_ = row;
  } else {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[row] = true;
    anchor_ = row;
  }
  if (cb_.onSelectionChanged) cb_.onSelectionChanged();
  Invalidate();
}

void ListView::Reload() {
  const int n = RowCount();
  // 行内容可能整体变化，保留原来的勾选模式
  std::vector<bool> next(n, false);
  for (int i = 0; i < n && i < static_cast<int>(selected_.size()); ++i) next[i] = selected_[i];
  selected_.swap(next);
  if (anchor_ >= n) anchor_ = n - 1;
  if (scroll_ < 0) scroll_ = 0;
  UpdateScrollBar();
  Invalidate();
}

void ListView::SetSort(int column, bool asc) {
  sortColumn_ = column;
  sortAsc_ = asc;
  Invalidate(true);
}

void ListView::SetUserWidth(int column, int physicalWidth) {
  if (column < 0 || column >= static_cast<int>(userWidth_.size())) return;
  userWidth_[column] = physicalWidth > 0 ? physicalWidth : 0;
  LayoutColumns();
  Invalidate();
}

void ListView::UpdateScrollBar() {
  if (!hwnd_) return;
  const int total = RowCount() * RowHeight();
  const int page = PageHeight();
  SCROLLINFO si{sizeof(si)};
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = total > 0 ? total - 1 : 0;
  si.nPage = page > 0 ? static_cast<UINT>(page) : 0;
  si.nPos = scroll_;
  ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void ListView::ScrollTo(int pos) {
  const int total = RowCount() * RowHeight();
  const int maxScroll = (std::max)(0, total - PageHeight());
  scroll_ = (std::max)(0, (std::min)(pos, maxScroll));
  UpdateScrollBar();
  Invalidate();
}

void ListView::EnsureRowVisible(int row) {
  const int page = PageHeight();
  if (page <= 0) return;
  const int top = row * RowHeight();
  if (top < scroll_) {
    scroll_ = top;
  } else if (top + RowHeight() > scroll_ + page) {
    scroll_ = top + RowHeight() - page;
  }
}

void ListView::OnSize(int cx, int cy) {
  (void)cx;
  (void)cy;
  LayoutColumns();
  UpdateScrollBar();
  Invalidate();
}

void ListView::PaintHeader(Painter& p) {
  const RECT rc = ClientRect();
  const RECT headRc{0, 0, rc.right, HeadHeight()};
  p.Fill(headRc, style_.headBg);

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = CellLeft(col);
    if (left >= rc.right) break;
    RECT cell{left, 0, left + colWidth_[col], HeadHeight()};

    const bool isSort = (col == sortColumn_);
    RECT tr = cell;
    tr.left += px(style_.padX);
    tr.right -= px(style_.padX);
    if (isSort) tr.right -= px(kSortArrowArea);

    const std::wstring& title = columns_[col].title;
    if (columns_[col].align == ListAlign::kRight) {
      p.TextRight(tr, title, style_.headText, fonts.ui);
    } else {
      p.TextLeft(tr, title, style_.headText, fonts.ui);
    }

    if (isSort) {
      const int cs = px(11);
      const int cx = cell.right - px(kSortArrowArea);
      const int cy = HeadHeight() / 2;
      RECT cr{cx - cs / 2, cy - cs / 2, cx + cs / 2, cy + cs / 2};
      p.Icon(sortAsc_ ? IconId::kChevronUp : IconId::kChevronDown, cr, kTextMuted);
    }
  }

  p.HLine(0, rc.right, HeadHeight() - 1, style_.headLine);
}

void ListView::PaintRow(Painter& p, int row, const RECT& rr) {
  const bool sel = (row < static_cast<int>(selected_.size())) && selected_[row] != 0;
  const bool hot = (row == hot_) && !sel;
  p.Fill(rr, sel ? style_.selBg : (hot ? style_.hotBg : style_.bg));

  for (int col = 0; col < static_cast<int>(columns_.size()); ++col) {
    const int left = CellLeft(col);
    if (left >= rr.right) break;

    RECT tr{left + px(style_.padX), rr.top, left + colWidth_[col] - px(style_.padX), rr.bottom};

    if (col == 0 && cb_.drawIcon) {
      const int iconSide = px(style_.iconSize);
      const int iconTop = (rr.top + rr.bottom - iconSide) / 2;
      RECT ir{left + px(style_.padX), iconTop, left + px(style_.padX) + iconSide,
              iconTop + iconSide};
      cb_.drawIcon(p, row, ir);
      tr.left = ir.right + px(style_.iconGap);
    }

    const std::wstring text = cb_.text ? cb_.text(row, col) : std::wstring();
    if (text.empty()) continue;
    if (columns_[col].align == ListAlign::kRight) {
      p.TextRight(tr, text, style_.text, fonts.ui);
    } else {
      p.TextEllipsis(tr, text, style_.text, fonts.ui);
    }
  }

  p.HLine(rr.left, rr.right, rr.bottom - 1, style_.rowLine);
}

void ListView::OnPaint() {
  const RECT rc = ClientRect();
  if (rc.right <= 0 || rc.bottom <= 0) return;

  // 整块离屏绘制：行与表头一次贴回，避免滚动/悬停时出现半帧
  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, style_.bg);

  // 行区裁剪在表头之下
  const int top = ContentTop();
  ::IntersectClipRect(canvas.dc(), 0, top, rc.right, rc.bottom);

  const int first = scroll_ / RowHeight();
  const int last = (std::min)(RowCount() - 1, first + PageHeight() / RowHeight() + 1);
  for (int row = first; row <= last; ++row) {
    const RECT rr = RowRect(row);
    if (rr.bottom <= top) continue;
    if (rr.top >= rc.bottom) break;
    PaintRow(p, row, rr);
  }

  // 恢复裁剪再画表头
  ::SelectClipRgn(canvas.dc(), nullptr);
  PaintHeader(p);

  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}

bool ListView::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, style_.bg);
  return true;
}

LRESULT ListView::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMOVE: {
      const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

      // 正在拖列宽
      if (dragCol_ >= 0) {
        const int next = dragStartW_ + (pt.x - dragStartX_);
        colWidth_[dragCol_] = (std::max)(px(48), next);
        Invalidate();
        return 0;
      }

      if (pt.y < HeadHeight()) {
        // 表头：靠近列右边界显示左右调整光标
        const int col = ColumnAtX(pt.x);
        bool onEdge = false;
        if (col >= 0) {
          const int right = CellLeft(col) + colWidth_[col];
          onEdge = (pt.x > right - px(kResizeEdge));
        }
        ::SetCursor(::LoadCursorW(nullptr, onEdge ? IDC_SIZEWE : IDC_ARROW));
        if (hot_ != -1) {
          hot_ = -1;
          Invalidate();
        }
        return 0;
      }

      const int row = HitRow(pt);
      if (row != hot_) {
        hot_ = row;
        Invalidate();
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        if (!tracking_) {
          tracking_ = true;
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
          ::TrackMouseEvent(&tme);
        }
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hot_ != -1) {
        hot_ = -1;
        Invalidate();
      }
      return 0;

    case WM_LBUTTONDOWN: {
      const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

      if (pt.y < HeadHeight()) {
        const int col = ColumnAtX(pt.x);
        if (col < 0) return 0;
        const int right = CellLeft(col) + colWidth_[col];
        if (pt.x > right - px(kResizeEdge)) {
          // 开始拖列宽
          dragCol_ = col;
          dragStartX_ = pt.x;
          dragStartW_ = colWidth_[col];
          ::SetCapture(hwnd_);
        } else {
          // 点表头排序：同列再次点击切换方向
          const bool asc = (col == sortColumn_) ? !sortAsc_ : true;
          sortColumn_ = col;
          sortAsc_ = asc;
          if (cb_.onSort) cb_.onSort(col, asc);
          Invalidate(true);
        }
        return 0;
      }

      ::SetFocus(hwnd_);
      const int row = HitRow(pt);
      if (row < 0) {
        ClearSelection();
        return 0;
      }
      SyncSelection(row, ::GetKeyState(VK_CONTROL) < 0, ::GetKeyState(VK_SHIFT) < 0);
      return 0;
    }

    case WM_LBUTTONUP:
      if (dragCol_ >= 0) {
        userWidth_[dragCol_] = colWidth_[dragCol_];
        dragCol_ = -1;
        ::ReleaseCapture();
        LayoutColumns();
        Invalidate();
        if (cb_.onColumnResized) cb_.onColumnResized();
        return 0;
      }
      return 0;

    case WM_LBUTTONDBLCLK: {
      const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (pt.y < HeadHeight()) return 0;
      const int row = HitRow(pt);
      if (row >= 0 && cb_.onActivate) cb_.onActivate(row);
      return 0;
    }

    case WM_MOUSEWHEEL:
      ScrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * RowHeight() / WHEEL_DELTA * 3);
      return 0;

    case WM_VSCROLL: {
      SCROLLINFO si{sizeof(si)};
      si.fMask = SIF_ALL;
      ::GetScrollInfo(hwnd_, SB_VERT, &si);
      int pos = scroll_;
      switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= RowHeight(); break;
        case SB_LINEDOWN: pos += RowHeight(); break;
        case SB_PAGEUP: pos -= PageHeight(); break;
        case SB_PAGEDOWN: pos += PageHeight(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
      }
      ScrollTo(pos);
      return 0;
    }

    case WM_KEYDOWN: {
      const int n = RowCount();
      const bool ctrl = (::GetKeyState(VK_CONTROL) < 0);
      const bool shift = (::GetKeyState(VK_SHIFT) < 0);

      switch (wParam) {
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT: {
          if (n <= 0) return 0;
          const int page = (std::max)(1, PageHeight() / RowHeight());
          int target = anchor_ < 0 ? 0 : anchor_;
          if (wParam == VK_UP) target = (std::max)(0, target - 1);
          if (wParam == VK_DOWN) target = (std::min)(n - 1, target + 1);
          if (wParam == VK_HOME) target = 0;
          if (wParam == VK_END) target = n - 1;
          if (wParam == VK_PRIOR) target = (std::max)(0, target - page);
          if (wParam == VK_NEXT) target = (std::min)(n - 1, target + page);
          SyncSelection(target, false, shift);
          EnsureRowVisible(target);
          UpdateScrollBar();
          Invalidate();
          return 0;
        }

        case VK_RETURN:
          if (anchor_ >= 0 && cb_.onActivate) cb_.onActivate(anchor_);
          return 0;

        case 'A':
          if (ctrl) {
            SelectAll();
            return 0;
          }
          break;

        default:
          break;
      }

      if (cb_.onKeyDown) cb_.onKeyDown(static_cast<int>(wParam));
      return 0;
    }

    case WM_CONTEXTMENU:
      if (cb_.onContextMenu) cb_.onContextMenu();
      return 0;

    default:
      break;
  }
  return Widget::OnMessage(msg, wParam, lParam);
}

}  // namespace AUI
