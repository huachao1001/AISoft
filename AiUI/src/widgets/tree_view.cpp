// TreeView 实现
#include "AiUI/widgets/tree_view.h"

#include "AiUI/font.h"

#include <windows.h>

#include <algorithm>

namespace AiUI {
namespace {

// path 是否等于 target 或位于其下；按分隔符边界判断，避免 "a2" 误配 "a"
bool isAncestorOrSelf(const std::wstring& path, const std::wstring& target) {
  if (path.empty()) return true;
  if (target.size() < path.size()) return false;
  if (::CompareStringOrdinal(target.c_str(), static_cast<int>(path.size()), path.c_str(),
                             static_cast<int>(path.size()), TRUE) != CSTR_EQUAL) {
    return false;
  }
  if (target.size() == path.size()) return true;
  if (path.back() == L'\\' || path.back() == L'/') return true;
  return target[path.size()] == L'\\' || target[path.size()] == L'/';
}

}  // namespace

TreeView::TreeView() { setFocusable(true); }

TreeView::~TreeView() = default;

void TreeView::setStyle(const TreeStyle& style) {
  style_ = style;
  invalidate();
}

void TreeView::setRoot(std::unique_ptr<TreeNode> root) {
  root_ = std::move(root);
  active_ = nullptr;
  hot_ = -1;
  setScrollOffset(0);
  rebuild();
}

void TreeView::setChildrenLoader(TreeChildrenLoader loader) { loader_ = std::move(loader); }

void TreeView::setSelectHandler(TreeSelectHandler fn) { onSelect_ = std::move(fn); }

int TreeView::rowHeight() const { return style_.rowHeight * dpi() / 96; }

int TreeView::contentHeight() const {
  return static_cast<int>(rows_.size()) * rowHeight();
}

Size TreeView::onMeasure(const Size& available) { return available; }

void TreeView::loadChildren(TreeNode* node) {
  if (!node || node->loaded) return;
  node->loaded = true;
  node->children.clear();
  if (loader_) node->children = loader_(node);
  node->hasChildren = !node->children.empty();
}

void TreeView::rebuild() {
  rows_.clear();
  if (!root_) {
    setScrollOffset(0);
    invalidate();
    return;
  }
  // 只收集展开节点的子树
  std::function<void(TreeNode*)> flatten = [&](TreeNode* node) {
    for (auto& child : node->children) {
      rows_.push_back(child.get());
      if (child->expanded) flatten(child.get());
    }
  };
  flatten(root_.get());
  // 行数变了，滚动位置可能越界，交给基类夹紧
  setScrollOffset(scrollOffset());
  invalidate();
}

TreeNode* TreeView::syncTo(TreeNode* node, const std::wstring& target) {
  if (!node) return nullptr;
  if (!isAncestorOrSelf(node->path, target)) return nullptr;

  loadChildren(node);
  node->expanded = true;
  for (auto& child : node->children) {
    if (TreeNode* deeper = syncTo(child.get(), target)) return deeper;
  }
  return node;
}

void TreeView::selectPath(const std::wstring& path) {
  active_ = nullptr;
  if (!root_) return;
  for (auto& child : root_->children) {
    if (TreeNode* found = syncTo(child.get(), path)) {
      active_ = found;
      break;
    }
  }
  rebuild();
  scrollToActive();
  invalidate();
}

bool TreeView::collapseActive() {
  if (!active_ || !active_->hasChildren || !active_->expanded) return false;
  active_->expanded = false;
  rebuild();
  return true;
}

Rect TreeView::rowRect(int index) const {
  const int d = dpi();
  const int height = rowHeight();
  const int top = index * height - scrollOffset();
  const Rect rc = clientRect();
  return Rect{style_.padLeft * d / 96 - 4 * d / 96, top,
              rc.right - style_.padLeft * d / 96 + 4 * d / 96, top + height};
}

int TreeView::hitRow(Point pt) const {
  if (pt.x < 0) return -1;
  const int index = (pt.y + scrollOffset()) / rowHeight();
  if (index < 0 || index >= static_cast<int>(rows_.size())) return -1;
  return index;
}

void TreeView::scrollToActive() {
  if (!active_) return;
  const auto it = std::find(rows_.begin(), rows_.end(), active_);
  if (it == rows_.end()) return;

  const int index = static_cast<int>(it - rows_.begin());
  const int top = index * rowHeight();
  const int page = viewportHeight();
  int target = scrollOffset();
  if (top < target) {
    target = top;
  } else if (top + rowHeight() > target + page) {
    target = top + rowHeight() - page;
  }
  setScrollOffset(target);
}

void TreeView::invalidateRow(int index) {
  if (index < 0 || index >= static_cast<int>(rows_.size())) return;
  const Rect rc = clientRect();
  Rect row = rowRect(index);
  if (row.bottom <= 0 || row.top >= rc.bottom) return;
  row.top = (std::max)(row.top, 0);
  row.bottom = (std::min)(row.bottom, rc.bottom);
  invalidateRect(row);
}

int TreeView::indexOfNode(const TreeNode* node) const {
  if (!node) return -1;
  const auto it = std::find(rows_.begin(), rows_.end(), node);
  if (it == rows_.end()) return -1;
  return static_cast<int>(it - rows_.begin());
}

void TreeView::onScrollChanged() {
  // 整行平移，重画即可；滚动条由基类前景层补画
  invalidate();
}

void TreeView::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const int height = rowHeight();

  // 只画与脏区相交的行
  const int first = (std::max)(0, scrollOffset() / height);
  const int last = (std::min)(static_cast<int>(rows_.size()) - 1,
                              (scrollOffset() + rect.bottom) / height);

  for (int i = first; i <= last; ++i) {
    TreeNode* node = rows_[static_cast<size_t>(i)];
    const Rect rr = rowRect(i);
    if (rr.bottom <= rect.top) continue;
    if (rr.top >= rect.bottom) break;

    const bool active = (node == active_);
    const bool hot = (hot_ == i);

    // 选中/悬停都用圆角药丸，比整行铺色轻
    Rect pill = rr;
    pill.left += style_.pillInsetX * d / 96;
    pill.right -= style_.pillInsetX * d / 96;
    pill.top += style_.pillInsetY * d / 96;
    pill.bottom -= style_.pillInsetY * d / 96;
    if (active) {
      painter.fillRoundRect(pill, style_.selBg, style_.pillRadius * d / 96);
    } else if (hot) {
      painter.fillRoundRect(pill, style_.hoverBg, style_.pillRadius * d / 96);
    }

    int x = rect.left + style_.padLeft * d / 96 + node->depth * style_.indent * d / 96;
    const int iconSize = style_.iconSize * d / 96;
    const Rect iconRect{x, rr.center().y - iconSize / 2, x + iconSize,
                        rr.center().y + iconSize / 2};
    painter.drawIcon(node->icon, iconRect, active ? style_.iconActive : style_.iconColor);
    x = iconRect.right + style_.iconGap * d / 96;

    const Rect textRect{x, rr.top, rr.right - 8 * d / 96, rr.bottom};
    painter.drawTextEllipsis(textRect, node->label, active ? style_.textActive : style_.text,
                             fonts.ui);
  }

  painter.drawVLine(rect.top, rect.bottom, rect.right - 1, style_.divider);
}

bool TreeView::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;

  // 先给滚动条：点在滑块或轨道上就由基类处理
  if (ScrollView::onMouseDown(e)) return true;
  setFocus();

  const int hit = hitRow(e.position);
  if (hit < 0) return true;

  TreeNode* node = rows_[static_cast<size_t>(hit)];
  // 点击已展开的节点即折叠，其余情况选中并展开
  if (node == active_ && node->hasChildren && node->expanded) {
    node->expanded = false;
    rebuild();
    return true;
  }

  const int previousIndex = indexOfNode(active_);
  active_ = node;
  if (node->hasChildren && !node->expanded) {
    node->expanded = true;
    loadChildren(node);
    rebuild();
  } else {
    // 只换了选中项：重绘旧选中行与新选中行，不整控件重画
    invalidateRow(previousIndex);
    invalidateRow(hit);
  }
  if (onSelect_) onSelect_(node);
  return true;
}

void TreeView::onMouseUp(const MouseEvent& e) {
  // 滚动条拖动结束后由基类收尾并重画滑块
  ScrollView::onMouseUp(e);
}

void TreeView::onMouseMove(const MouseEvent& e) {
  ScrollView::onMouseMove(e);

  const int hit = hitRow(e.position);
  if (hit != hot_) {
    const int previous = hot_;
    hot_ = hit;
    invalidateRow(previous);
    invalidateRow(hot_);
  }
  setCursor(hit >= 0 ? CursorShape::kHand : CursorShape::kArrow);
}

void TreeView::onMouseLeave() {
  ScrollView::onMouseLeave();
  if (hot_ == -1) return;
  const int previous = hot_;
  hot_ = -1;
  invalidateRow(previous);
}

}  // namespace AiUI
