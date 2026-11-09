// 左侧文件夹树
#pragma once

#include <windows.h>

namespace ui {

HWND CreateTreeView(HWND parent, HINSTANCE inst, int id);
void PopulateTree(HWND tree);
void SyncTreeToCurrentDir();
LRESULT HandleTreeNotify(HWND mainWnd, NMHDR* hdr);

}  // namespace ui
