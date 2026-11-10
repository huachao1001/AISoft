// 底部状态栏：自绘，左边位置/格式/大小，右边条目统计
#pragma once

#include <windows.h>

namespace ui {

HWND CreateStatusBar(HWND parent, HINSTANCE inst, int id);
void RefreshStatusBarData();
void InvalidateStatusBar();

}  // namespace ui
