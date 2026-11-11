// 左侧树模型：把磁盘或压缩包翻译成节点树
//
// 只产出 TreeNode，不持有控件。TreeView 通过 loader 回调按需索要子节点，
// 展开哪个分支才枚举哪个目录。
#pragma once

#include "AiUI/widgets/tree_view.h"

#include <memory>
#include <string>
#include <vector>

namespace ui {

class TreeModel {
 public:
  // 根节点结构标识：文件系统用 "fs"，压缩包用包路径。
  // 只有它变了才需要重建根节点，否则保留展开状态与滚动位置。
  static std::wstring syncKey();

  // 根：压缩包模式只有包本身；文件系统放桌面、盘符、用户目录
  static std::unique_ptr<AiUI::TreeNode> buildRoot();

  // 节点子项：压缩包内从条目取同层目录，文件系统则枚举磁盘
  static std::vector<std::unique_ptr<AiUI::TreeNode>> loadChildren(AiUI::TreeNode* node);
};

}  // namespace ui
