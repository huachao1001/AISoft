#include "AUI/tree_view.h"

#include "AUI/font.h"
#include "AUI/painter.h"

#include <windowsx.h>

#include <algorithm>

namespace AUI {
namespace {

// path 是否等于 target 或位于其下；按分隔符边界判断，避免 "chaohua2" 误配 "chaohua"
bool IsAncestorOrSelf(const std::wstring& p, const std::wstring& target) {
  if (p.empty()) return true;
  if (target.size() < p.size()) return false;
  if (::CompareStringOrdinal(target.c_str(), static_cast<int>(p.size()), p.c_str(),
                             static_cast<int>(p.size()), TRUE) != CSTR_EQUAL) {
    return false;
  }
  if (target.size() == p.size()) return true;
  if (p.back() == L'\\' || p.back() == L'/') return true;
  return target[p.size()] == L'\\' || target[p.size()] == L'/';
}

}  // namespace

void TreeView::SetStyle(const TreeStyle& style) {
  style_ = style;
  UpdateScrollBar();
  Invalidate();
}

void TreeView::SetRoot(std::unique_ptr<TreeNode> root) {
  root_ = std::move(root);
  active_ = nullptr;
  scroll_ = 0;
  hot_ = -1;
  Rebuild();
}

void TreeView::LoadChildren(TreeNode* n) {
  if (!n || n->loaded) return;
  n->loaded = true;
  n->children.clear();
  if (loader_) n->children = loader_(n);
  n->hasChildren = !n->children.empty();
}

void TreeView::Rebuild() {
  rows_.clear();
  if (!root_) return;
  // 递归展开：只收展开节点的子树
  std::function<void(TreeNode*)> flatten = [&](TreeNode* n) {
    for (auto& c : n->children) {
      rows_.push_back(c.get());
      if (c->expanded) flatten(c.get());
    }
  };
  flatten(root_.get());
  UpdateScrollBar();
}

TreeNode* TreeView::SyncTo(TreeNode* n, const std::wstring& target) {
  if (!n) return nullptr;
  if (!IsAncestorOrSelf(n->path, target)) return nullptr;

  LoadChildren(n);
  n->expanded = true;
  for (auto& c : n->children) {
    TreeNode* deeper = SyncTo(c.get(), target);
    if (deeper) return deeper;
  }
  return n;
}

void TreeView::SelectPath(const std::wstring& path) {
  active_ = nullptr;
  if (!root_) return;
  for (auto& c : root_->children) {
    TreeNode* found = SyncTo(c.get(), path);
    if (found) {
      active_ = found;
      break;
    }
  }
  Rebuild();
  ScrollToActive();
  UpdateScrollBar();
  Invalidate();
}

bool TreeView::CollapseActive() {
  if (!active_ || !active_->hasChildren || !active_->expanded) return false;
  active_->expanded = false;
  Rebuild();
  Invalidate();
  return true;
}

RECT TreeView::RowRect(int index, int cx) const {
  const int h = RowHeight();
  const int top = index * h - scroll_;
  return RECT{px(style_.padLeft) - px(4), top, cx - px(style_.padLeft) + px(4), top + h};
}

void TreeView::ScrollToActive() {
  if (!active_) return;
  const auto it = std::find(rows_.begin(), rows_.end(), active_);
  if (it == rows_.end()) return;
  const int index = static_cast<int>(it - rows_.begin());
  const RECT rc = ClientRect();
  if (index * RowHeight() < scroll_) {
    scroll_ = index * RowHeight();
  } else if ((index + 1) * RowHeight() > scroll_ + rc.bottom) {
    scroll_ = (index + 1) * RowHeight() - rc.bottom;
  }
  if (scroll_ < 0) scroll_ = 0;
}

void TreeView::UpdateScrollBar() {
  if (!hwnd_) return;
  const RECT rc = ClientRect();
  const int total = static_cast<int>(rows_.size()) * RowHeight();
  SCROLLINFO si{sizeof(si)};
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = total > 0 ? total - 1 : 0;
  si.nPage = rc.bottom;
  si.nPos = scroll_;
  ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

int TreeView::HitRow(POINT pt) const {
  if (pt.x < 0) return -1;
  const int idx = (pt.y + scroll_) / RowHeight();
  if (idx < 0 || idx >= static_cast<int>(rows_.size())) return -1;
  return idx;
}

void TreeView::EnsureChildVisible(TreeNode* n) {
  (void)n;
  ScrollToActive();
}

void TreeView::OnSize(int cx, int cy) {
  (void)cx;
  (void)cy;
  UpdateScrollBar();
  Invalidate();
}

void TreeView::OnPaint() {
  const RECT rc = ClientRect();
  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, style_.bg);

  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    TreeNode* n = rows_[i];
    RECT rr = RowRect(i, rc.right);
    if (rr.bottom < 0) continue;
    if (rr.top > rc.bottom) break;

    const bool active = (n == active_);
    const bool hot = (hot_ == i);
    RECT pill = rr;
    pill.left += px(style_.pillInsetX);
    pill.right -= px(style_.pillInsetX);
    ::InflateRect(&pill, 0, -px(style_.pillInsetY));
    if (active) {
      p.FillRound(pill, style_.selBg, px(style_.pillRadius));
    } else if (hot) {
      p.FillRound(pill, style_.hoverBg, px(style_.pillRadius));
    }

    int x = px(style_.padLeft) + n->depth * px(style_.indent);
    const int cy = (rr.top + rr.bottom) / 2;

    const int is = px(style_.iconSize);
    RECT ir{x, cy - is / 2, x + is, cy + is / 2};
    p.Icon(n->icon, ir, active ? style_.iconActive : style_.iconColor);
    x = ir.right + px(style_.iconGap);

    RECT tr{x, rr.top, rr.right - px(8), rr.bottom};
    p.TextEllipsis(tr, n->label, active ? style_.textActive : style_.text, fonts.ui);
  }

  p.VLine(0, rc.bottom, rc.right - 1, style_.divider);
  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}

bool TreeView::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, style_.bg);
  return true;
}

LRESULT TreeView::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitRow(pt);
      if (hit != hot_) {
        hot_ = hit;
        Invalidate();
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
      hot_ = -1;
      Invalidate();
      return 0;

    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitRow(pt);
      if (hit < 0) return 0;
      TreeNode* n = rows_[hit];

      // 单击已选中节点即折叠，其余情况选中并展开
      if (n == active_ && n->hasChildren && n->expanded) {
        n->expanded = false;
        Rebuild();
        Invalidate();
        return 0;
      }
      active_ = n;
      if (n->hasChildren && !n->expanded) {
        n->expanded = true;
        LoadChildren(n);
        Rebuild();
      }
      Invalidate();
      if (onSelect_) onSelect_(n);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      const RECT rc = ClientRect();
      const int total = static_cast<int>(rows_.size()) * RowHeight();
      const int maxScroll = (std::max)(0, total - static_cast<int>(rc.bottom));
      scroll_ -= GET_WHEEL_DELTA_WPARAM(wParam) * RowHeight() / WHEEL_DELTA * 3;
      if (scroll_ < 0) scroll_ = 0;
      if (scroll_ > maxScroll) scroll_ = maxScroll;
      UpdateScrollBar();
      Invalidate();
      return 0;
    }

    case WM_VSCROLL: {
      const RECT rc = ClientRect();
      const int total = static_cast<int>(rows_.size()) * RowHeight();
      const int maxScroll = (std::max)(0, total - static_cast<int>(rc.bottom));
      SCROLLINFO si{sizeof(si)};
      si.fMask = SIF_ALL;
      ::GetScrollInfo(hwnd_, SB_VERT, &si);
      int pos = scroll_;
      switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= RowHeight(); break;
        case SB_LINEDOWN: pos += RowHeight(); break;
        case SB_PAGEUP: pos -= rc.bottom; break;
        case SB_PAGEDOWN: pos += rc.bottom; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
      }
      if (pos < 0) pos = 0;
      if (pos > maxScroll) pos = maxScroll;
      scroll_ = pos;
      UpdateScrollBar();
      Invalidate();
      return 0;
    }

    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
      return TRUE;

    default:
      break;
  }
  return Widget::OnMessage(msg, wParam, lParam);
}

}  // namespace AUI
