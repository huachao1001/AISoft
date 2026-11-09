// 底部状态栏：左「位置 + 格式标签 + 大小」，右「条目统计」
#pragma once

#include <windows.h>

namespace ui {

HWND CreateStatusBar(HWND parent, HINSTANCE inst, int id);
void RefreshStatusBarData();
void InvalidateStatusBar();

}  // namespace ui
