// 左侧文件夹树：数据来自磁盘或压缩包，视图由 TreeView 负责
#pragma once

#include <string>

namespace AiUI {
class TreeView;
}

namespace ui {

void AttachSideTree(AiUI::TreeView* tree);
// 视图切换后重建整棵树并选中当前目录
void RefreshSideTree();
// 树当前高亮节点：文件系统为完整路径，压缩包内为包内相对路径（空=根）
std::wstring TreeActivePath();
// 树节点能否作为解压目标：只有包内目录可以
bool TreeActiveCanExtract();

}  // namespace ui
