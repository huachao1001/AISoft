// 左侧文件夹树：数据来自磁盘或压缩包，视图由 TreeView 负责
#pragma once

#include <string>

namespace AiUI {
class TreeView;
}

namespace ui {

void AttachSideTree(AiUI::TreeView* tree);
// 重建整棵树并选中当前目录：只在根结构变化时用
void RebuildSideTree();
// 按情况选择：根变了才重建，否则只同步选中
void SyncSideTree();
// 只同步选中到当前目录，不重建节点：导航时用，保留展开状态与滚动位置
void SyncSideTreeSelection();
// 树当前高亮节点：文件系统为完整路径，压缩包内为包内相对路径（空=根）
std::wstring TreeActivePath();
// 树节点能否作为解压目标：只有包内目录可以
bool TreeActiveCanExtract();

}  // namespace ui
