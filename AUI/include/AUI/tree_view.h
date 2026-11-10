// 树形视图：自绘扁平列表 + 懒加载子节点
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AUI/icon.h"
#include "AUI/theme.h"
#include "AUI/widget.h"

namespace AUI {

// 树的节点：只存展示所需的数据，子节点由外部通过 Loader 懒加载
struct TreeNode {
  std::wstring label;
  std::wstring path;  // 业务标识：文件系统完整路径，或包内相对路径
  int depth = 0;
  bool expanded = false;
  bool loaded = false;
  bool hasChildren = true;
  bool isRoot = false;  // 根节点用 home 图标
  IconId icon = IconId::kFolder;
  TreeNode* parent = nullptr;
  std::vector<std::unique_ptr<TreeNode>> children;
};

struct TreeStyle {
  int rowHeight = 30;
  int indent = 18;
  int padLeft = 14;
  int iconSize = 16;
  int iconGap = 6;
  int pillInsetX = 6;
  int pillInsetY = 2;
  int pillRadius = 5;
  COLORREF bg = kPaneBg;
  COLORREF hoverBg = kHoverBg;
  COLORREF selBg = kSelBg;
  COLORREF text = kText;
  COLORREF textActive = kActiveInk;
  COLORREF iconColor = RGB(0x6b, 0x72, 0x80);
  COLORREF iconActive = RGB(0x25, 0x63, 0xeb);
  COLORREF divider = kLine;
};

// 懒加载回调：给定节点，产出它的直接子节点
using TreeChildrenLoader = std::function<std::vector<std::unique_ptr<TreeNode>>(TreeNode*)>;

// 选中变化回调
using TreeSelectHandler = std::function<void(TreeNode*)>;

class TreeView : public Widget {
 public:
  void SetStyle(const TreeStyle& style);
  // 构建根节点（不显示）；root 的子节点就是可见的第一层
  void SetRoot(std::unique_ptr<TreeNode> root);
  void SetChildrenLoader(TreeChildrenLoader loader) { loader_ = std::move(loader); }
  void SetSelectHandler(TreeSelectHandler fn) { onSelect_ = std::move(fn); }

  // 重建可见行；根节点与展开状态变化后必须调用
  void Rebuild();
  // 展开到指定 path 并选中；找不到时清除选中
  void SelectPath(const std::wstring& path);
  TreeNode* Active() const { return active_; }
  std::wstring ActivePath() const { return active_ ? active_->path : std::wstring(); }

  // 折叠当前选中节点（若可折叠）
  bool CollapseActive();

 protected:
  void OnSize(int cx, int cy) override;
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;
  LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

 private:
  int RowHeight() const { return px(style_.rowHeight); }
  RECT RowRect(int index, int cx) const;
  int HitRow(POINT pt) const;
  void LoadChildren(TreeNode* n);
  TreeNode* SyncTo(TreeNode* n, const std::wstring& target);
  void ScrollToActive();
  void UpdateScrollBar();
  void EnsureChildVisible(TreeNode* n);

  TreeStyle style_{};
  TreeChildrenLoader loader_;
  TreeSelectHandler onSelect_;
  std::unique_ptr<TreeNode> root_;
  std::vector<TreeNode*> rows_;
  int scroll_ = 0;
  int hot_ = -1;
  TreeNode* active_ = nullptr;
  bool tracking_ = false;
};

}  // namespace AUI
