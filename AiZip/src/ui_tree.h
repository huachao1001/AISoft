// 左侧文件夹树：自绘，展开状态与当前目录同步
#pragma once

#include <windows.h>

#include <string>

namespace ui {

HWND CreateSideTree(HWND parent, HINSTANCE inst, int id);
// 视图切换后重建整棵树并选中当前目录
void RefreshSideTree();
// 压缩包模式下树内容不同，需要重排宽度时调用
void LayoutSideTree();
// 树当前高亮节点：文件系统为完整路径，压缩包内为包内相对路径（空=根）
std::wstring TreeActivePath();
// 树节点能否作为解压目标：只有包内目录可以，文件系统的目录没有解压语义
bool TreeActiveCanExtract();

}  // namespace ui
