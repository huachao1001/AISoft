// TreeView：自绘扁平列表 + 懒加载子节点
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AiUI {

// 树节点：只存展示数据，子节点由外部懒加载
struct TreeNode {
  std::wstring label;
  std::wstring path;  // 业务标识：文件系统完整路径，或包内相对路径
  int depth = 0;
  bool expanded = false;
  bool loaded = false;
  bool hasChildren = true;
  bool isRoot = false;
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
  Color background = kColorPane;
  Color hoverBg = kColorHover;
  Color selBg = kColorSelBg;
  Color text = kColorText;
  Color textActive = kColorActiveInk;
  Color iconColor = makeColor(0x6b, 0x72, 0x80);
  Color iconActive = kColorLink;
  Color divider = kColorLine;
};

using TreeChildrenLoader = std::function<std::vector<std::unique_ptr<TreeNode>>(TreeNode*)>;
using TreeSelectHandler = std::function<void(TreeNode*)>;

class TreeView : public Widget {
 public:
  TreeView() { className_ = L"AiUITreeView"; }

  void setStyle(const TreeStyle& style);
  void setRoot(std::unique_ptr<TreeNode> root);
  void setChildrenLoader(TreeChildrenLoader loader);
  void setSelectHandler(TreeSelectHandler fn);

  // 重建可见行；根节点或展开状态变化后必须调用
  void rebuild();
  // 展开到指定 path 并选中
  void selectPath(const std::wstring& path);
  TreeNode* active() const { return active_; }
  std::wstring activePath() const { return active_ ? active_->path : std::wstring(); }
  bool collapseActive();

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onMouseWheel(const MouseEvent& e) override;
  void onScroll(const ScrollEvent& e) override;
  Color backgroundColor() const override { return style_.background; }

 private:
  int rowHeight() const;
  Rect rowRect(int index) const;
  int hitRow(Point pt) const;
  void loadChildren(TreeNode* node);
  TreeNode* syncTo(TreeNode* node, const std::wstring& target);
  void scrollToActive();
  void updateScrollBar();
  void scrollBy(int delta);
  void setScroll(int position);
  int maxScroll() const;

  TreeStyle style_{};
  TreeChildrenLoader loader_;
  TreeSelectHandler onSelect_;
  std::unique_ptr<TreeNode> root_;
  std::vector<TreeNode*> rows_;
  int scroll_ = 0;
  int hot_ = -1;
  TreeNode* active_ = nullptr;
};

}  // namespace AiUI
