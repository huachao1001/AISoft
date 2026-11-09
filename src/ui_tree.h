// 左侧文件夹树：自绘，展开状态与当前目录同步
#pragma once

#include <windows.h>

namespace ui {

HWND CreateSideTree(HWND parent, HINSTANCE inst, int id);
// 视图切换后重建整棵树并选中当前目录
void RefreshSideTree();
// 压缩包模式下树内容不同，需要重排宽度时调用
void LayoutSideTree();

}  // namespace ui
