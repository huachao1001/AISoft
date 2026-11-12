// TreeView：自绘扁平列表 + 懒加载子节点
//
// 继承 ScrollView：滚动位置与滚动条由基类管，这里只管行数据与绘制。
// 选中、悬停这类变化只重绘受影响的几行，不整树重建。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/scroll_view.h"
#include "AiUI/theme.h"

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
  // 颜色：kNoColor 表示跟随主题
  Color background = kNoColor;
  Color hoverBg = kNoColor;
  Color selBg = kNoColor;
  Color text = kNoColor;
  Color textActive = kNoColor;
  Color iconColor = kNoColor;
  Color iconActive = kNoColor;
  Color divider = kNoColor;
};

using TreeChildrenLoader = std::function<std::vector<std::unique_ptr<TreeNode>>(TreeNode*)>;
using TreeSelectHandler = std::function<void(TreeNode*)>;

class TreeView : public ScrollView {
 public:
  TreeView();
  ~TreeView() override;

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

  // ── ScrollView ──
  int contentHeight() const override;
  int contentWidth() const override;
  int viewportHeight() const override { return clientRect().height(); }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onScrollChanged() override;
  Color backgroundColor() const override;
  // 一格滚三行，与列表手感一致
  int wheelStep() const override { return style_.rowHeight * 3; }

 private:
  int rowHeight() const;
  Rect rowRect(int index) const;
  int hitRow(Point pt) const;
  void loadChildren(TreeNode* node);
  TreeNode* syncTo(TreeNode* node, const std::wstring& target);
  void scrollToActive();
  // 只重绘一行；悬停、选中这类局部变化用
  void invalidateRow(int index);
  // 节点当前对应的可见行号；不在可见区返回 -1
  int indexOfNode(const TreeNode* node) const;

  TreeStyle style_{};
  TreeChildrenLoader loader_;
  TreeSelectHandler onSelect_;
  std::unique_ptr<TreeNode> root_;
  std::vector<TreeNode*> rows_;
  // 最长行宽度；逐行测量开销大，行数据或样式变化时置脏
  mutable int contentWidthCache_ = 0;
  mutable bool widthDirty_ = true;
  int hot_ = -1;
  TreeNode* active_ = nullptr;
};

}  // namespace AiUI
